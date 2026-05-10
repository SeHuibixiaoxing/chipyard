package rerocc.manager

import chisel3._
import chisel3.util._
import org.chipsalliance.cde.config._
import freechips.rocketchip.diplomacy._
import freechips.rocketchip.tile._
import freechips.rocketchip.tilelink._
import freechips.rocketchip.rocket._
import freechips.rocketchip.util._
import freechips.rocketchip.prci._
import freechips.rocketchip.subsystem._
import midas.targetutils.{PerfCounter, SynthesizePrintf}

import rerocc.bus._

case class ReRoCCManagerParams(
  managerId: Int,
)

case object ReRoCCManagerControlAddress extends Field[BigInt](0x20000)

// For local PTW
class MiniDCache(reRoCCId: Int, crossing: ClockCrossingType)(implicit p: Parameters) extends DCache(0, crossing)(p) {
  override def cacheClientParameters = Seq(TLMasterParameters.v1(
    name          = s"ReRoCC ${reRoCCId} DCache",
    sourceId      = IdRange(0, 1),
    supportsProbe = TransferSizes(cfg.blockBytes, cfg.blockBytes)))
  override def mmioClientParameters = Seq(TLMasterParameters.v1(
    name          = s"ReRoCC ${reRoCCId} DCache MMIO",
    sourceId      = IdRange(firstMMIO, firstMMIO + cfg.nMMIOs),
    requestFifo   = true))
}

class ReRoCCManager(reRoCCTileParams: ReRoCCTileParams, supportedOpcodes: Seq[UInt])(implicit p: Parameters) extends LazyModule {
  require(supportedOpcodes.nonEmpty, "ReRoCCManager requires at least one supported opcode")
  val node = ReRoCCManagerNode(ReRoCCManagerParams(reRoCCTileParams.reroccId))
  val ibufEntries = p(ReRoCCIBufEntriesKey)
  private val defaultOpcode = supportedOpcodes.head
  override lazy val module = new Impl
  class Impl extends LazyModuleImp(this) {
    val io = IO(new Bundle {
      val manager_id = Input(UInt(log2Ceil(p(ReRoCCTileKey).size).W))
      val cmd = Decoupled(new RoCCCommand)
      val resp = Flipped(Decoupled(new RoCCResponse))
      val busy = Input(Bool())
      val ptw = Flipped(new DatapathPTWIO)
    })

    val (rerocc, edge) = node.in(0)
    val s_idle :: s_active :: s_rel_wait :: s_sfence :: s_unbusy :: Nil = Enum(5)

    def ageWhile(active: Bool, width: Int = 32): UInt = {
      val age = RegInit(0.U(width.W))
      when (active) {
        when (!age.andR) { age := age + 1.U }
      } .otherwise {
        age := 0.U
      }
      age
    }

    def debugStuckPrint(age: UInt): Bool =
      age === 1024.U || (age > 1024.U && age(11, 0) === 0.U)

    val numClients = edge.cParams.clients.map(_.nCfgs).sum

    val client = Reg(UInt(log2Ceil(numClients).W))
    val status = Reg(new MStatus)
    val ptbr = Reg(new PTBR)
    val state = RegInit(s_idle)

    io.ptw.ptbr := ptbr
    io.ptw.hgatp := 0.U.asTypeOf(new PTBR)
    io.ptw.vsatp := 0.U.asTypeOf(new PTBR)
    io.ptw.sfence.valid := state === s_sfence
    io.ptw.sfence.bits.rs1 := false.B
    io.ptw.sfence.bits.rs2 := false.B
    io.ptw.sfence.bits.addr := 0.U
    io.ptw.sfence.bits.asid := 0.U
    io.ptw.sfence.bits.hv := false.B
    io.ptw.sfence.bits.hg := false.B

    io.ptw.status := status
    io.ptw.hstatus := 0.U.asTypeOf(new HStatus)
    io.ptw.gstatus := 0.U.asTypeOf(new MStatus)
    io.ptw.pmp.foreach(_ := 0.U.asTypeOf(new PMP))

    val rr_req = Queue(rerocc.req)
    val (req_first, req_last, req_beat) = ReRoCCMsgFirstLast(rr_req, true)
    val rr_resp = rerocc.resp

    rr_req.ready := false.B

    val inst_q = Module(new Queue(new RoCCCommand, ibufEntries))
    val enq_inst = Reg(new RoCCCommand)
    val next_enq_inst = WireInit(enq_inst)
    inst_q.io.enq.valid := false.B
    inst_q.io.enq.bits := next_enq_inst
    if (!reRoCCTileParams.preserveIncomingOpcode) {
      inst_q.io.enq.bits.inst.opcode := defaultOpcode
    }

    // 0 -> acquire ack
    // 1 -> inst ack
    // 2 -> writeback
    // 3 -> rel
    // 4 -> unbusyack
    val resp_arb = Module(new ReRoCCMsgArbiter(edge.bundle, 5, false))
    rr_resp <> resp_arb.io.out
    resp_arb.io.in.foreach { i => i.valid := false.B }

    val status_lower = Reg(UInt(64.W))

    when (rr_req.valid) {
      when (rr_req.bits.opcode === ReRoCCProtocol.mAcquire) {
        rr_req.ready := resp_arb.io.in(0).ready
        resp_arb.io.in(0).valid := true.B
        when (state === s_idle && rr_req.fire) {
          state := s_active
          client := rr_req.bits.client_id
        }
      } .elsewhen (rr_req.bits.opcode === ReRoCCProtocol.mUStatus) {
        rr_req.ready := !inst_q.io.deq.valid && !io.busy
        when (!inst_q.io.deq.valid && !io.busy) {
          when (req_first) { status_lower := rr_req.bits.data }
          when (req_last) { status := Cat(rr_req.bits.data, status_lower).asTypeOf(new MStatus) }
        }
      } .elsewhen (rr_req.bits.opcode === ReRoCCProtocol.mUPtbr) {
        rr_req.ready := !inst_q.io.deq.valid && !io.busy
        when (!inst_q.io.deq.valid && !io.busy) { ptbr := rr_req.bits.data.asTypeOf(new PTBR) }
      } .elsewhen (rr_req.bits.opcode === ReRoCCProtocol.mInst) {
        assert(state === s_active && inst_q.io.enq.ready)
        rr_req.ready := true.B

        when (req_beat === 0.U) {
          val inst = rr_req.bits.data.asTypeOf(new RoCCInstruction)
          val opcodeValid = supportedOpcodes.map(_ === inst.opcode).reduce(_ || _)
          assert(opcodeValid, "ReRoCC manager received an unsupported incoming opcode")
          next_enq_inst.inst := inst
          when (!inst.xs1        ) { next_enq_inst.rs1 := 0.U }
          when (!inst.xs2        ) { next_enq_inst.rs2 := 0.U }
          enq_inst := next_enq_inst
        } .otherwise {
          val enq_inst_rs1      = enq_inst.inst.xs1 && req_beat === 1.U
          val enq_inst_rs2      = enq_inst.inst.xs2 && req_beat === Mux(enq_inst.inst.xs1, 2.U, 1.U)
          when (enq_inst_rs1) { next_enq_inst.rs1 := rr_req.bits.data }
          when (enq_inst_rs2) { next_enq_inst.rs2 := rr_req.bits.data }
          enq_inst := next_enq_inst
        }
        when (req_last) {
          inst_q.io.enq.valid := true.B

          assert(inst_q.io.enq.ready)
        }
      } .elsewhen (rr_req.bits.opcode === ReRoCCProtocol.mRelease) {
        rr_req.ready := true.B
        state := s_rel_wait
      } .elsewhen (rr_req.bits.opcode === ReRoCCProtocol.mUnbusy) {
        rr_req.ready := true.B
        state := s_unbusy
      } .otherwise {
        assert(false.B)
      }
    }

    when (rr_req.fire) {
      SynthesizePrintf(printf(
        "[rrc-manager-req] mgr=%d state=%d opcode=%d client=%d beat=%d first=%d last=%d data=%x qcount=%d busy=%d\n",
        io.manager_id, state, rr_req.bits.opcode, rr_req.bits.client_id,
        req_beat, req_first.asUInt, req_last.asUInt, rr_req.bits.data,
        inst_q.io.count, io.busy.asUInt))
    }

    when (inst_q.io.enq.fire) {
      SynthesizePrintf(printf(
        "[rrc-manager-inst-enq] mgr=%d client=%d qcount=%d opc=%x funct=%x rd=%d rs1=%x rs2=%x\n",
        io.manager_id, client, inst_q.io.count, inst_q.io.enq.bits.inst.opcode,
        inst_q.io.enq.bits.inst.funct, inst_q.io.enq.bits.inst.rd,
        inst_q.io.enq.bits.rs1, inst_q.io.enq.bits.rs2))
    }

    // acquire->ack/nack
    resp_arb.io.in(0).bits.opcode := ReRoCCProtocol.sAcqResp
    resp_arb.io.in(0).bits.client_id := rr_req.bits.client_id
    resp_arb.io.in(0).bits.manager_id := io.manager_id
    resp_arb.io.in(0).bits.data := state === s_idle

    // insts -> (inst_q, inst_ack)
    io.cmd.valid := inst_q.io.deq.valid && resp_arb.io.in(1).ready
    io.cmd.bits := inst_q.io.deq.bits
    inst_q.io.deq.ready := io.cmd.ready && resp_arb.io.in(1).ready
    resp_arb.io.in(1).valid := inst_q.io.deq.valid && io.cmd.ready
    resp_arb.io.in(1).bits.opcode     := ReRoCCProtocol.sInstAck
    resp_arb.io.in(1).bits.client_id  := client
    resp_arb.io.in(1).bits.manager_id := io.manager_id
    resp_arb.io.in(1).bits.data       := 0.U

    val cmdBlockedAge = ageWhile(io.cmd.valid && !io.cmd.ready)
    when (debugStuckPrint(cmdBlockedAge)) {
      SynthesizePrintf(printf(
        "[rrc-manager-cmd-stuck] mgr=%d age=%d state=%d qcount=%d cmd_valid=%d cmd_ready=%d accel_busy=%d resp_ready=%d opc=%x funct=%x\n",
        io.manager_id, cmdBlockedAge, state, inst_q.io.count,
        io.cmd.valid.asUInt, io.cmd.ready.asUInt, io.busy.asUInt,
        resp_arb.io.in(1).ready.asUInt, io.cmd.bits.inst.opcode,
        io.cmd.bits.inst.funct))
    }

    when (io.cmd.fire) {
      SynthesizePrintf(printf(
        "[rrc-manager-cmd-fire] mgr=%d client=%d qcount=%d opc=%x funct=%x rd=%d rs1=%x rs2=%x\n",
        io.manager_id, client, inst_q.io.count, io.cmd.bits.inst.opcode,
        io.cmd.bits.inst.funct, io.cmd.bits.inst.rd, io.cmd.bits.rs1,
        io.cmd.bits.rs2))
    }

    when (resp_arb.io.in(1).fire) {
      SynthesizePrintf(printf(
        "[rrc-manager-inst-ack] mgr=%d client=%d qcount=%d\n",
        io.manager_id, client, inst_q.io.count))
    }

    // writebacks
    val resp = Queue(io.resp)
    val resp_rd = RegInit(false.B)
    resp_arb.io.in(2).valid           := resp.valid
    resp_arb.io.in(2).bits.opcode     := ReRoCCProtocol.sWrite
    resp_arb.io.in(2).bits.client_id  := client
    resp_arb.io.in(2).bits.manager_id := io.manager_id
    resp_arb.io.in(2).bits.data       := Mux(resp_rd, resp.bits.rd, resp.bits.data)
    when (resp_arb.io.in(2).fire) { resp_rd := !resp_rd }
    resp.ready := resp_arb.io.in(2).ready && resp_rd

    when (resp_arb.io.in(2).fire) {
      SynthesizePrintf(printf(
        "[rrc-manager-write] mgr=%d client=%d data=%x rd_phase=%d\n",
        io.manager_id, client, resp_arb.io.in(2).bits.data, resp_rd.asUInt))
    }

    // release
    resp_arb.io.in(3).valid           := state === s_rel_wait && !io.busy && inst_q.io.count === 0.U
    resp_arb.io.in(3).bits.opcode     := ReRoCCProtocol.sRelResp
    resp_arb.io.in(3).bits.client_id  := client
    resp_arb.io.in(3).bits.manager_id := io.manager_id
    resp_arb.io.in(3).bits.data       := 0.U

    when (resp_arb.io.in(3).fire) {
      state := s_sfence
    }
    when (state === s_sfence) { state := s_idle }

    val relWaitAge = ageWhile(state === s_rel_wait && !(resp_arb.io.in(3).valid && resp_arb.io.in(3).ready))
    when (debugStuckPrint(relWaitAge)) {
      SynthesizePrintf(printf(
        "[rrc-manager-release-stuck] mgr=%d age=%d busy=%d qcount=%d resp_ready=%d\n",
        io.manager_id, relWaitAge, io.busy.asUInt, inst_q.io.count,
        resp_arb.io.in(3).ready.asUInt))
    }

    when (resp_arb.io.in(3).fire) {
      SynthesizePrintf(printf(
        "[rrc-manager-release-resp] mgr=%d client=%d\n",
        io.manager_id, client))
    }

    // unbusyack
    resp_arb.io.in(4).valid           := state === s_unbusy && !io.busy && inst_q.io.count === 0.U
    resp_arb.io.in(4).bits.opcode     := ReRoCCProtocol.sUnbusyAck
    resp_arb.io.in(4).bits.client_id  := client
    resp_arb.io.in(4).bits.manager_id := io.manager_id
    resp_arb.io.in(4).bits.data       := 0.U

    val unbusyWaitAge = ageWhile(state === s_unbusy && !(resp_arb.io.in(4).valid && resp_arb.io.in(4).ready))
    when (debugStuckPrint(unbusyWaitAge)) {
      SynthesizePrintf(printf(
        "[rrc-manager-unbusy-stuck] mgr=%d age=%d busy=%d qcount=%d resp_ready=%d\n",
        io.manager_id, unbusyWaitAge, io.busy.asUInt, inst_q.io.count,
        resp_arb.io.in(4).ready.asUInt))
    }

    when (resp_arb.io.in(4).fire) {
      SynthesizePrintf(printf(
        "[rrc-manager-unbusy-ack] mgr=%d client=%d\n",
        io.manager_id, client))
      state := s_active
    }

    val debugState = Cat(
      state,
      rr_req.valid,
      rr_req.ready,
      rr_req.bits.opcode,
      req_beat,
      inst_q.io.count,
      io.cmd.valid,
      io.cmd.ready,
      io.busy,
      io.resp.valid,
      io.resp.ready,
      resp_arb.io.out.valid,
      resp_arb.io.out.ready)
    PerfCounter.identity(debugState, s"rerocc_manager_${reRoCCTileParams.reroccId}_state",
      "ReRoCC manager command path state")
    PerfCounter(io.cmd.fire.asUInt, s"rerocc_manager_${reRoCCTileParams.reroccId}_cmd_fire",
      "ReRoCC manager forwarded a RoCC command")
    PerfCounter(resp_arb.io.in(1).fire.asUInt, s"rerocc_manager_${reRoCCTileParams.reroccId}_inst_ack",
      "ReRoCC manager emitted an instruction acknowledgement")
  }
}

class ReRoCCManagerTile()(implicit p: Parameters) extends LazyModule {
  val reRoCCParams = p(TileKey).asInstanceOf[ReRoCCTileParams]
  val reRoCCId = reRoCCParams.reroccId
  def this(tileParams: ReRoCCTileParams, p: Parameters) = {
    this()(p.alterMap(Map(
      TileKey -> tileParams,
      TileVisibilityNodeKey -> TLEphemeralNode()(ValName("rerocc_manager"))
    )))
  }
  val reroccManagerIdSinkNode = BundleBridgeSink[UInt]()

  val rocc = reRoCCParams.genRoCC.get(p)
  require(rocc.opcodes.opcodes.nonEmpty)
  val rerocc_manager = LazyModule(new ReRoCCManager(reRoCCParams, rocc.opcodes.opcodes))
  val reRoCCNode = ReRoCCIdentityNode()
  rerocc_manager.node := ReRoCCBuffer() := reRoCCNode
  val tlNode = p(TileVisibilityNodeKey) // throttle before TL Node (merged ->
  val tlXbar = TLXbar()
  val stlNode = TLIdentityNode()
  // Keep only managers that can satisfy full cache-line-sized DMA accesses.
  // Disabled by default so existing ReRoCC configs retain their original address view.
  private val minWideTransfer = TransferSizes(1, p(CacheBlockBytes))
  private val dmaVisibleFilter = TLFilter(mfilter = { m =>
    val getOk = m.supportsGet.contains(minWideTransfer)
    val putFullOk = m.supportsPutFull.contains(minWideTransfer)
    val putPartialOk = m.supportsPutPartial.contains(minWideTransfer)
    if (getOk && putFullOk && putPartialOk) Some(m) else None
  })

  tlXbar :=* rocc.atlNode
  if (reRoCCParams.mergeTLNodes) {
    tlXbar :=* rocc.tlNode
    if (reRoCCParams.filterDmaVisibleManagers) {
      tlNode :=* TLBuffer() :=* dmaVisibleFilter :=* tlXbar
    } else {
      tlNode :=* TLBuffer() :=* tlXbar
    }
  } else {
    if (reRoCCParams.filterDmaVisibleManagers) {
      tlNode :=* dmaVisibleFilter :=* rocc.tlNode
    } else {
      tlNode :=* rocc.tlNode
    }
    tlNode :=* TLBuffer() :=* tlXbar
  }

  // Keep the legacy stl path unchanged. SBUS slave attachment, when enabled,
  // is exported separately from Integration.scala to match Rocket's native RoCC hookup.
  rocc.stlNode :*= stlNode

  // minicache
  val dcache = reRoCCParams.dcacheParams.map(_ => LazyModule(new MiniDCache(reRoCCId, SynchronousCrossing())(p)))
  dcache.map(d => tlXbar := TLWidthWidget(reRoCCParams.rowBits/8) := d.node)

  val hellammio: Option[HellaMMIO] = if (!dcache.isDefined) {
    val h = LazyModule(new HellaMMIO(s"ReRoCC $reRoCCId MMIO"))
    tlXbar := h.node
    Some(h)
  } else { None }

  val ctrl = LazyModule(new ReRoCCManagerControl(reRoCCId, 8))

  override lazy val module = new LazyModuleImp(this) {
    val dcacheArb = Module(new HellaCacheArbiter(2)(p))
    dcache.map(_.module.io.cpu).getOrElse(hellammio.get.module.io) <> dcacheArb.io.mem

    val edge = dcache.map(_.node.edges.out(0)).getOrElse(hellammio.get.node.edges.out(0))

    val ptw = Module(new PTW(1 + rocc.nPTWPorts)(edge, p))

    if (dcache.isDefined) {
      dcache.get.module.io.tlb_port := DontCare
      dcache.get.module.io.tlb_port.req.valid := false.B
      ptw.io.requestor(0) <> dcache.get.module.io.ptw
    } else {
      ptw.io.requestor(0) := DontCare
      ptw.io.requestor(0).req.valid := false.B
    }
    dcacheArb.io.requestor(0) <> ptw.io.mem

    val dcIF = Module(new SimpleHellaCacheIF)
    dcIF.io.requestor <> rocc.module.io.mem
    dcacheArb.io.requestor(1) <> dcIF.io.cache

    for (i <- 0 until rocc.nPTWPorts) {
      ptw.io.requestor(1+i) <> rocc.module.io.ptw(i)
    }
    rerocc_manager.module.io.manager_id := reroccManagerIdSinkNode.bundle
    rocc.module.io.cmd <> rerocc_manager.module.io.cmd
    rerocc_manager.module.io.resp <> rocc.module.io.resp
    rerocc_manager.module.io.busy := rocc.module.io.busy

    ptw.io.dpath <> rerocc_manager.module.io.ptw

    rocc.module.io.fpu_req.ready := false.B
    assert(!rocc.module.io.fpu_req.valid)
    rocc.module.io.fpu_resp.valid := false.B
    rocc.module.io.fpu_resp.bits := DontCare

    rocc.module.io.exception := false.B

    ctrl.module.io.mgr_busy := rerocc_manager.module.io.busy
    ctrl.module.io.rocc_busy := rocc.module.io.busy
  }
}
