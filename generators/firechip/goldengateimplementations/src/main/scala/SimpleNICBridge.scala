// See LICENSE for license details

package firechip.goldengateimplementations

import chisel3._
import chisel3.util._

import org.chipsalliance.cde.config.{Parameters, Field}
import freechips.rocketchip.util._

import midas.widgets._
import firesim.lib.bridgeutils._

import firechip.bridgeinterfaces._

object TokenQueueConsts {
  val TOKENS_PER_BIG_TOKEN = 7
  val BIG_TOKEN_WIDTH = (TOKENS_PER_BIG_TOKEN + 1) * 64
  val TOKEN_QUEUE_DEPTH = 3072
}
import TokenQueueConsts._

case object LoopbackNIC extends Field[Boolean](false)

/* on a NIC token transaction:
 * 1) simulation driver feeds an empty token to start:
 *  data_in is garbage or real value (if exists)
 *  data_in_valid is 0 or 1 respectively
 *  data_out_ready is true (say host can always accept)
 *
 * 2) target responds:
 *  data_out garbage or real value (if exists)
 *  data_out_valid 0 or 1 respectively
 *  data_in_ready would be 1, so driver knows how to construct the next token if there was data to send
 *
 *  repeat
 */

class ReadyValidLast extends Bundle {
  val data_last = Bool()
  val ready = Bool()
  val valid = Bool()
}

class BIGToken extends Bundle {
  val data = Vec(7, UInt(64.W))
  val rvls = Vec(7, new ReadyValidLast())
  val pad = UInt(43.W)
}

class HostToNICToken extends Bundle {
  val data_in = new StreamChannel(64)
  val data_in_valid = Bool()
  val data_out_ready = Bool()
}

class NICToHostToken extends Bundle {
  val data_out = new StreamChannel(64)
  val data_out_valid = Bool()
  val data_in_ready = Bool()
}

class BigTokenToNICTokenAdapter extends Module {
  val io = IO(new Bundle {
    val htnt = DecoupledIO(new HostToNICToken)
    val pcie_in = Flipped(DecoupledIO(UInt(512.W)))
    val debug_loop_iter = Output(UInt(32.W))
    val debug_latched_valid = Output(Bool())
    val debug_accept_count = Output(UInt(32.W))
    val debug_emit_count = Output(UInt(32.W))
    val debug_payload_emit_count = Output(UInt(32.W))
    val debug_pcie_valid_no_ready_count = Output(UInt(32.W))
    val debug_pcie_ready_no_valid_count = Output(UInt(32.W))
    val debug_htnt_valid_no_ready_count = Output(UInt(32.W))
    val debug_htnt_ready_no_valid_count = Output(UInt(32.W))
    val debug_current_slot_meta = Output(UInt(32.W))
    val debug_current_slot_data = Output(UInt(64.W))
    val debug_latched_valid_mask = Output(UInt(7.W))
    val debug_latched_ready_mask = Output(UInt(7.W))
    val debug_latched_last_mask = Output(UInt(7.W))
    val debug_last_emit_slot_mask = Output(UInt(7.W))
    val debug_ever_emit_slot_mask = Output(UInt(7.W))
    val debug_ever_payload_emit_slot_mask = Output(UInt(7.W))
    val debug_last_latched_word0 = Output(UInt(64.W))
    val debug_last_latched_word1 = Output(UInt(64.W))
    val debug_last_latched_word2 = Output(UInt(64.W))
    val debug_last_emit_data = Output(UInt(64.W))
    val debug_last_emit_meta = Output(UInt(32.W))
  })

  val latchedBits = RegInit(0.U(512.W))
  val latchedValid = RegInit(false.B)
  val loopIter = RegInit(0.U(32.W))
  val acceptCount = RegInit(0.U(32.W))
  val emitCount = RegInit(0.U(32.W))
  val payloadEmitCount = RegInit(0.U(32.W))
  val pcieValidNoReadyCount = RegInit(0.U(32.W))
  val pcieReadyNoValidCount = RegInit(0.U(32.W))
  val htntValidNoReadyCount = RegInit(0.U(32.W))
  val htntReadyNoValidCount = RegInit(0.U(32.W))
  val lastLatchedWord0 = RegInit(0.U(64.W))
  val lastLatchedWord1 = RegInit(0.U(64.W))
  val lastLatchedWord2 = RegInit(0.U(64.W))
  val lastEmitData = RegInit(0.U(64.W))
  val lastEmitMeta = RegInit(0.U(32.W))
  val lastEmitSlotMask = RegInit(0.U(7.W))
  val everEmitSlotMask = RegInit(0.U(7.W))
  val everPayloadEmitSlotMask = RegInit(0.U(7.W))

  val pcieBundled = latchedBits.asTypeOf(new BIGToken)
  val selectedData = pcieBundled.data(loopIter)
  val selectedRVL = pcieBundled.rvls(loopIter)
  val lastSlot = loopIter === 6.U
  val accept = io.pcie_in.fire
  val emit = io.htnt.fire
  val emitSlotMask = UIntToOH(loopIter, 7)

  io.pcie_in.ready := !latchedValid
  io.htnt.valid := latchedValid
  io.htnt.bits.data_in.data := selectedData
  io.htnt.bits.data_in.keep := 0xFF.U
  io.htnt.bits.data_in.last := selectedRVL.data_last
  io.htnt.bits.data_in_valid := selectedRVL.valid
  io.htnt.bits.data_out_ready := selectedRVL.ready

  when (accept) {
    latchedBits := io.pcie_in.bits
    latchedValid := true.B
    loopIter := 0.U
    acceptCount := acceptCount + 1.U
    lastLatchedWord0 := io.pcie_in.bits(63, 0)
    lastLatchedWord1 := io.pcie_in.bits(127, 64)
    lastLatchedWord2 := io.pcie_in.bits(191, 128)
  }
  when (emit) {
    emitCount := emitCount + 1.U
    lastEmitSlotMask := emitSlotMask
    everEmitSlotMask := everEmitSlotMask | emitSlotMask
    lastEmitData := selectedData
    lastEmitMeta := Cat(
      0.U(20.W),
      accept,
      emit,
      io.htnt.ready,
      io.htnt.valid,
      selectedRVL.ready,
      selectedRVL.valid,
      selectedRVL.data_last,
      loopIter(4, 0),
    )
    when (selectedRVL.valid) {
      payloadEmitCount := payloadEmitCount + 1.U
      everPayloadEmitSlotMask := everPayloadEmitSlotMask | emitSlotMask
    }
    when (lastSlot) {
      latchedValid := false.B
      loopIter := 0.U
    } .otherwise {
      loopIter := loopIter + 1.U
    }
  }
  when (io.pcie_in.valid && !io.pcie_in.ready) {
    pcieValidNoReadyCount := pcieValidNoReadyCount + 1.U
  }
  when (io.pcie_in.ready && !io.pcie_in.valid) {
    pcieReadyNoValidCount := pcieReadyNoValidCount + 1.U
  }
  when (io.htnt.valid && !io.htnt.ready) {
    htntValidNoReadyCount := htntValidNoReadyCount + 1.U
  }
  when (io.htnt.ready && !io.htnt.valid) {
    htntReadyNoValidCount := htntReadyNoValidCount + 1.U
  }

  io.debug_loop_iter := loopIter
  io.debug_latched_valid := latchedValid
  io.debug_accept_count := acceptCount
  io.debug_emit_count := emitCount
  io.debug_payload_emit_count := payloadEmitCount
  io.debug_pcie_valid_no_ready_count := pcieValidNoReadyCount
  io.debug_pcie_ready_no_valid_count := pcieReadyNoValidCount
  io.debug_htnt_valid_no_ready_count := htntValidNoReadyCount
  io.debug_htnt_ready_no_valid_count := htntReadyNoValidCount
  io.debug_current_slot_data := selectedData
  io.debug_latched_valid_mask := Cat((0 until 7).reverse.map(i => pcieBundled.rvls(i).valid))
  io.debug_latched_ready_mask := Cat((0 until 7).reverse.map(i => pcieBundled.rvls(i).ready))
  io.debug_latched_last_mask := Cat((0 until 7).reverse.map(i => pcieBundled.rvls(i).data_last))
  io.debug_last_emit_slot_mask := lastEmitSlotMask
  io.debug_ever_emit_slot_mask := everEmitSlotMask
  io.debug_ever_payload_emit_slot_mask := everPayloadEmitSlotMask
  io.debug_last_latched_word0 := lastLatchedWord0
  io.debug_last_latched_word1 := lastLatchedWord1
  io.debug_last_latched_word2 := lastLatchedWord2
  io.debug_last_emit_data := lastEmitData
  io.debug_last_emit_meta := lastEmitMeta
  io.debug_current_slot_meta := Cat(
    0.U(16.W),
    latchedValid,
    io.pcie_in.ready,
    io.pcie_in.valid,
    io.htnt.ready,
    io.htnt.valid,
    selectedRVL.ready,
    selectedRVL.valid,
    selectedRVL.data_last,
    0.U(3.W),
    loopIter(4, 0),
  )
}

class NICTokenToBigTokenAdapter extends Module {
  val io = IO(new Bundle {
    val ntht = Flipped(DecoupledIO(new NICToHostToken))
    val pcie_out = DecoupledIO(UInt(512.W))
    val debug_special_counter = Output(UInt(32.W))
    val debug_full_flush_count = Output(UInt(32.W))
    val debug_partial_flush_count = Output(UInt(32.W))
    val debug_last_flush_slot = Output(UInt(32.W))
    val debug_pcie_out_fire_count = Output(UInt(32.W))
    val debug_pcie_out_enq_count = Output(UInt(32.W))
    val debug_output_queue_count = Output(UInt(32.W))
    val debug_ntht_fire_count = Output(UInt(32.W))
    val debug_ntht_valid_no_ready_count = Output(UInt(32.W))
    val debug_ntht_ready_no_valid_count = Output(UInt(32.W))
    val debug_pcie_out_valid_no_ready_count = Output(UInt(32.W))
    val debug_pcie_out_ready_no_valid_count = Output(UInt(32.W))
    val debug_pcie_out_enq_valid_no_ready_count = Output(UInt(32.W))
    val debug_flush_candidate = Output(Bool())
    val debug_flush_blocked = Output(Bool())
    val debug_last_ntht_data = Output(UInt(64.W))
    val debug_last_ntht_meta = Output(UInt(32.W))
    val debug_last_pcie_out_enq_words = Output(Vec(8, UInt(64.W)))
    val debug_last_pcie_out_deq_words = Output(Vec(8, UInt(64.W)))
  })

  // step one, buffer 7 elems into registers. note that the 7th element is here
  // just for convenience. in reality, it is not used since we're bypassing to
  // remove a cycle of latency
  val NTHT_BUF = Reg(Vec(7, new NICToHostToken))
  val specialCounter = RegInit(0.U(32.W))
  val fullFlushCount = RegInit(0.U(32.W))
  val partialFlushCount = RegInit(0.U(32.W))
  val lastFlushSlot = RegInit(0.U(32.W))
  val pcieOutFireCount = RegInit(0.U(32.W))
  val pcieOutEnqCount = RegInit(0.U(32.W))
  val nthtFireCount = RegInit(0.U(32.W))
  val nthtValidNoReadyCount = RegInit(0.U(32.W))
  val nthtReadyNoValidCount = RegInit(0.U(32.W))
  val pcieOutValidNoReadyCount = RegInit(0.U(32.W))
  val pcieOutReadyNoValidCount = RegInit(0.U(32.W))
  val pcieOutEnqValidNoReadyCount = RegInit(0.U(32.W))
  val lastNthtData = RegInit(0.U(64.W))
  val lastNthtMeta = RegInit(0.U(32.W))
  val lastPcieOutEnqWords = RegInit(VecInit(Seq.fill(8)(0.U(64.W))))
  val lastPcieOutDeqWords = RegInit(VecInit(Seq.fill(8)(0.U(64.W))))
  io.debug_special_counter := specialCounter
  io.debug_full_flush_count := fullFlushCount
  io.debug_partial_flush_count := partialFlushCount
  io.debug_last_flush_slot := lastFlushSlot
  io.debug_pcie_out_fire_count := pcieOutFireCount
  io.debug_pcie_out_enq_count := pcieOutEnqCount
  io.debug_ntht_fire_count := nthtFireCount
  io.debug_ntht_valid_no_ready_count := nthtValidNoReadyCount
  io.debug_ntht_ready_no_valid_count := nthtReadyNoValidCount
  io.debug_pcie_out_valid_no_ready_count := pcieOutValidNoReadyCount
  io.debug_pcie_out_ready_no_valid_count := pcieOutReadyNoValidCount
  io.debug_pcie_out_enq_valid_no_ready_count := pcieOutEnqValidNoReadyCount
  io.debug_last_ntht_data := lastNthtData
  io.debug_last_ntht_meta := lastNthtMeta
  io.debug_last_pcie_out_enq_words := lastPcieOutEnqWords
  io.debug_last_pcie_out_deq_words := lastPcieOutDeqWords

  val slotIsFull = specialCounter === 6.U
  val flushOnLast = io.ntht.bits.data_out.last
  val flushThisToken = slotIsFull || (io.ntht.valid && flushOnLast)

  when (io.ntht.fire && !flushThisToken) {
    NTHT_BUF(specialCounter) := io.ntht.bits
  }

  when (io.ntht.fire) {
    nthtFireCount := nthtFireCount + 1.U
    lastNthtData := io.ntht.bits.data_out.data
    lastNthtMeta := Cat(
      0.U(21.W),
      io.ntht.bits.data_in_ready,
      io.ntht.bits.data_out_valid,
      io.ntht.bits.data_out.last,
      io.ntht.bits.data_out.keep,
    )
    when (flushThisToken) {
      specialCounter := 0.U
      lastFlushSlot := specialCounter
      when (slotIsFull) {
        fullFlushCount := fullFlushCount + 1.U
      } .otherwise {
        partialFlushCount := partialFlushCount + 1.U
      }
    } .otherwise {
      specialCounter := specialCounter + 1.U
    }
  }
  when (io.ntht.valid && !io.ntht.ready) {
    nthtValidNoReadyCount := nthtValidNoReadyCount + 1.U
  }
  when (io.ntht.ready && !io.ntht.valid) {
    nthtReadyNoValidCount := nthtReadyNoValidCount + 1.U
  }
  // step two, connect 6 elems + latest one to output (7 items)
  // TODO: attach pcie_out to data

  // debug check to help check we're not losing tokens somewhere
  val token_trace_counter = RegInit(0.U(43.W))
  when (io.pcie_out.fire) {
    token_trace_counter := token_trace_counter + 1.U
  } .otherwise {
    token_trace_counter := token_trace_counter
  }

  val invalidToken = Wire(new NICToHostToken)
  invalidToken := 0.U.asTypeOf(new NICToHostToken)
  invalidToken.data_in_ready := true.B

  val outTokens = Wire(Vec(7, new NICToHostToken))
  for (i <- 0 until 7) {
    outTokens(i) := Mux(i.U < specialCounter,
      NTHT_BUF(i),
      Mux(i.U === specialCounter, io.ntht.bits, invalidToken))
  }

  val out = Wire(new BIGToken)
  for (i <- 0 until 7) {
    out.data(i) := outTokens(i).data_out.data
    out.rvls(i).data_last := outTokens(i).data_out.last
    out.rvls(i).ready := outTokens(i).data_in_ready
    out.rvls(i).valid := outTokens(i).data_out_valid
  }
  out.pad := token_trace_counter

  val outUInt = out.asUInt
  val pcieOutQ = Module(new Queue(UInt(512.W), 2))
  pcieOutQ.io.enq.valid := io.ntht.valid && flushThisToken
  pcieOutQ.io.enq.bits := outUInt
  io.ntht.ready := !flushThisToken || pcieOutQ.io.enq.ready
  io.pcie_out <> pcieOutQ.io.deq
  io.debug_output_queue_count := pcieOutQ.io.count
  io.debug_flush_candidate := pcieOutQ.io.enq.valid
  io.debug_flush_blocked := pcieOutQ.io.enq.valid && !pcieOutQ.io.enq.ready

  when (io.pcie_out.fire) {
    pcieOutFireCount := pcieOutFireCount + 1.U
    for (i <- 0 until 8) {
      lastPcieOutDeqWords(i) := io.pcie_out.bits((i + 1) * 64 - 1, i * 64)
    }
  }
  when (io.pcie_out.valid && !io.pcie_out.ready) {
    pcieOutValidNoReadyCount := pcieOutValidNoReadyCount + 1.U
  }
  when (io.pcie_out.ready && !io.pcie_out.valid) {
    pcieOutReadyNoValidCount := pcieOutReadyNoValidCount + 1.U
  }
  when (pcieOutQ.io.enq.fire) {
    pcieOutEnqCount := pcieOutEnqCount + 1.U
    for (i <- 0 until 8) {
      lastPcieOutEnqWords(i) := outUInt((i + 1) * 64 - 1, i * 64)
    }
  }
  when (pcieOutQ.io.enq.valid && !pcieOutQ.io.enq.ready) {
    pcieOutEnqValidNoReadyCount := pcieOutEnqValidNoReadyCount + 1.U
  }
}

class HostToNICTokenGenerator(nTokens: Int)(implicit p: Parameters) extends Module {
  val io = IO(new Bundle {
    val out = Decoupled(new HostToNICToken)
    val in = Flipped(Decoupled(new NICToHostToken))
  })

  val s_init :: s_seed :: s_forward :: Nil = Enum(3)
  val state = RegInit(s_init)

  val (_, seedDone) = Counter(state === s_seed && io.out.fire, nTokens)

  io.out.valid := state === s_seed || (state === s_forward && io.in.valid)
  io.out.bits.data_in_valid := state === s_forward && io.in.bits.data_out_valid
  io.out.bits.data_in := io.in.bits.data_out
  io.out.bits.data_out_ready := state === s_seed || io.in.bits.data_in_ready
  io.in.ready := state === s_forward && io.out.ready

  when (state === s_init) { state := s_seed }
  when (seedDone) { state := s_forward }
}

class SimpleNICBridgeHostIO(private val targetIO: NICBridgeTargetIO = new NICBridgeTargetIO)
    extends Bundle
    with ChannelizedHostPortIO {
  def targetClockRef = targetIO.clock

  val nicOut = InputChannel(targetIO.nic.out)
  val nicIn  = OutputChannel(targetIO.nic.in)
  val macAddr = OutputChannel(targetIO.nic.macAddr)
  val rlimit = OutputChannel(targetIO.nic.rlimit)
  val pauser = OutputChannel(targetIO.nic.pauser)
}

class SimpleNICBridgeModule(implicit p: Parameters)
    extends BridgeModule[SimpleNICBridgeHostIO]()(p)
    with StreamToHostCPU
    with StreamFromHostCPU {
  // Stream mixin parameters
  val fromHostCPUQueueDepth = TOKEN_QUEUE_DEPTH
  val toHostCPUQueueDepth   = TOKEN_QUEUE_DEPTH

  lazy val module = new BridgeModuleImp(this) {
    val io = IO(new WidgetIO)
    val hPort = IO(new SimpleNICBridgeHostIO)

    val htnt_queue = Module(new Queue(new HostToNICToken, 10))
    val ntht_queue = Module(new Queue(new NICToHostToken, 10))

    val bigtokenToNIC = Module(new BigTokenToNICTokenAdapter)
    val NICtokenToBig = Module(new NICTokenToBigTokenAdapter)

    val toHostChannelValid = hPort.nicOut.valid
    val toHostPayloadValid = hPort.nicOut.valid && hPort.nicOut.bits.valid
    val toHostEmptyValid = hPort.nicOut.valid && !hPort.nicOut.bits.valid
    val toHostTokenBlocked = toHostPayloadValid && !ntht_queue.io.enq.ready
    val toHostPayloadBlocked = toHostPayloadValid && !ntht_queue.io.enq.ready
    val toHostLocalReady = !toHostTokenBlocked
    val toHostValidMask = Fill(4, hPort.nicOut.valid)
    val toHostReadyMask = Fill(4, hPort.nicOut.ready)
    val toHostFireMask = Fill(4, hPort.nicOut.fire)
    val toHostFullMask = "hf".U(4.W)
    val toHostMissingValidMask = ~toHostValidMask & "hf".U(4.W)
    val toHostBlockedMask = toHostValidMask & ~toHostReadyMask
    val nicInChannelReady = hPort.nicIn.ready
    val fromHostChannelReady = nicInChannelReady
    val nicInChannelValid = hPort.nicIn.valid
    val fromHostChannelValid = nicInChannelValid
    val fromHostTokenAvailable = htnt_queue.io.deq.valid
    val fromHostPayloadTokenAvailable = fromHostTokenAvailable && htnt_queue.io.deq.bits.data_in_valid
    val fromHostEmptyTokenAvailable = fromHostTokenAvailable && !htnt_queue.io.deq.bits.data_in_valid
    val fromHostCurrentPayloadValid = RegInit(false.B)
    val fromHostCurrentPayloadBits = RegInit(0.U.asTypeOf(new StreamChannel(64)))
    val fromHostPendingPayloadValid = RegInit(false.B)
    val fromHostPendingPayloadBits = RegInit(0.U.asTypeOf(new StreamChannel(64)))
    val fromHostPendingWillFree = hPort.nicIn.fire && fromHostPendingPayloadValid
    val fromHostPayloadCaptureReady = !fromHostPendingPayloadValid || fromHostPendingWillFree
    val fromHostPayloadCapture = fromHostPayloadTokenAvailable && fromHostPayloadCaptureReady
    val fromHostEmptyDrop = fromHostEmptyTokenAvailable
    val fromHostPayloadDeq = htnt_queue.io.deq.fire && htnt_queue.io.deq.bits.data_in_valid
    val fromHostEmptyDropFire = htnt_queue.io.deq.fire && !htnt_queue.io.deq.bits.data_in_valid
    val fromHostValidDrive = true.B
    val toHostReadyDrive = Mux(toHostPayloadValid, ntht_queue.io.enq.ready, true.B)
    val toHostEmptyBypass = false.B
    val toHostEmptyEnqueue = false.B
    val localQueuesReady = toHostLocalReady && fromHostPayloadCaptureReady
    val targetCycleReady = toHostReadyDrive && fromHostChannelReady
    val toHostFire = hPort.nicOut.fire
    val fromHostFire = hPort.nicIn.fire
    val tFire = toHostFire && fromHostFire
    val fromHostGroupFire = hPort.nicIn.fire
    val fromHostGroupBlocked = hPort.nicIn.valid && !hPort.nicIn.ready
    val targetBlockedNoToHostToken = !toHostChannelValid
    val targetBlockedNoFromHostToken = false.B
    val targetBlockedFromHostChannels = fromHostTokenAvailable && !hPort.nicIn.ready
    val targetBlockedToHostPayloadQueue = toHostTokenBlocked
    val fromHostEmptyBlocked = !fromHostCurrentPayloadValid && !fromHostChannelReady
    val toHostEmptySuppressed = toHostEmptyValid && hPort.nicOut.fire
    val tFireBlockedToHostValid = !toHostChannelValid
    val tFireBlockedFromHostReady = !fromHostChannelReady
    val tFireBlockedNthtEnqReady = !ntht_queue.io.enq.ready
    val tFireBlockedHtntDeqValid = !htnt_queue.io.deq.valid

    val fromHostReadyMask = Cat(
      Fill(3, hPort.pauser.ready),
      Fill(3, hPort.rlimit.ready),
      hPort.macAddr.ready,
      Fill(4, hPort.nicIn.ready),
    )
    val fromHostValidMask = Cat(
      Fill(3, hPort.pauser.valid),
      Fill(3, hPort.rlimit.valid),
      hPort.macAddr.valid,
      Fill(4, hPort.nicIn.valid),
    )
    val fromHostFireMask = Cat(
      Fill(3, hPort.pauser.fire),
      Fill(3, hPort.rlimit.fire),
      hPort.macAddr.fire,
      Fill(4, hPort.nicIn.fire),
    )
    val fromHostLeafValidMask = Cat(
      hPort.pauser.valid,
      hPort.rlimit.valid,
      hPort.macAddr.valid,
      hPort.nicIn.valid,
    )
    val fromHostLeafReadyMask = Cat(
      hPort.pauser.ready,
      hPort.rlimit.ready,
      hPort.macAddr.ready,
      hPort.nicIn.ready,
    )
    val fromHostLeafFireMask = Cat(
      hPort.pauser.fire,
      hPort.rlimit.fire,
      hPort.macAddr.fire,
      hPort.nicIn.fire,
    )
    val fromHostLeafBlockedMask = fromHostLeafValidMask & ~fromHostLeafReadyMask
    val fromHostLeafBlocked = fromHostLeafBlockedMask.orR
    val fromHostWouldValidMask = Fill(11, fromHostTokenAvailable)
    val fromHostWouldBlockedMask = fromHostWouldValidMask & ~fromHostReadyMask
    val fromHostBlockedMask = fromHostValidMask & ~fromHostReadyMask
    val toHostPayloadFire = hPort.nicOut.fire && hPort.nicOut.bits.valid
    val toHostEmptyFire = hPort.nicOut.fire && !hPort.nicOut.bits.valid

    val tFireCount = RegInit(0.U(32.W))
    val toHostFireCount = RegInit(0.U(32.W))
    val fromHostFireCount = RegInit(0.U(32.W))
    val htntQueueEnqFireCount = RegInit(0.U(32.W))
    val htntQueueDeqFireCount = RegInit(0.U(32.W))
    val nthtQueueEnqFireCount = RegInit(0.U(32.W))
    val nthtQueueDeqFireCount = RegInit(0.U(32.W))
    val pcieInFireCount = RegInit(0.U(32.W))
    val pcieOutFireCount = RegInit(0.U(32.W))
    val toHostPayloadFireCount64 = RegInit(0.U(64.W))
    val toHostEmptyFireCount64 = RegInit(0.U(64.W))
    val toHostEmptyEnqueueCount64 = RegInit(0.U(64.W))
    val toHostPayloadBlockedCount64 = RegInit(0.U(64.W))
    val fromHostEmptyDropCount64 = RegInit(0.U(64.W))
    val fromHostPayloadCaptureCount64 = RegInit(0.U(64.W))
    val targetCycleFireCount64 = RegInit(0.U(64.W))
    val targetBlockedNoToHostTokenCount64 = RegInit(0.U(64.W))
    val targetBlockedNoFromHostTokenCount64 = RegInit(0.U(64.W))
    val targetBlockedFromHostChannelsCount64 = RegInit(0.U(64.W))
    val targetBlockedToHostPayloadQueueCount64 = RegInit(0.U(64.W))
    val targetCycleReadyCount64 = RegInit(0.U(64.W))
    val targetCycleNotReadyCount64 = RegInit(0.U(64.W))
    val toHostReadyDriveCount64 = RegInit(0.U(64.W))
    val fromHostReadyCount64 = RegInit(0.U(64.W))
    val toHostEmptySuppressedCount64 = RegInit(0.U(64.W))
    val pcieInBackpressureCount64 = RegInit(0.U(64.W))
    val pcieOutBackpressureCount64 = RegInit(0.U(64.W))
    val toHostMissingValidCount64 = RegInit(0.U(64.W))
    val fromHostGroupBlockedCount64 = RegInit(0.U(64.W))
    val fromHostWouldBlockedCount64 = RegInit(0.U(64.W))
    val fromHostLeafBlockedCount64 = RegInit(0.U(64.W))
    val nicInBlockedCount64 = RegInit(0.U(64.W))
    val macAddrBlockedCount64 = RegInit(0.U(64.W))
    val rlimitBlockedCount64 = RegInit(0.U(64.W))
    val pauserBlockedCount64 = RegInit(0.U(64.W))
    val nicInFireCount64 = RegInit(0.U(64.W))
    val macAddrFireCount64 = RegInit(0.U(64.W))
    val rlimitFireCount64 = RegInit(0.U(64.W))
    val pauserFireCount64 = RegInit(0.U(64.W))
    val debugCycle = RegInit(0.U(64.W))
    val lastTFireCycle = RegInit(0.U(64.W))
    val lastToHostFireCycle = RegInit(0.U(64.W))
    val lastFromHostFireCycle = RegInit(0.U(64.W))
    val lastPcieInFireCycle = RegInit(0.U(64.W))
    val lastPcieOutFireCycle = RegInit(0.U(64.W))
    val firstBlockedValid = RegInit(false.B)
    val firstBlockedCycle = RegInit(0.U(64.W))
    val firstBlockedReason = RegInit(0.U(32.W))
    val firstBlockedQueueSnapshot = RegInit(0.U(32.W))
    val firstBlockedAdapterSnapshot = RegInit(0.U(32.W))
    val lastBlockedCycle = RegInit(0.U(64.W))
    val lastBlockedReason = RegInit(0.U(32.W))
    val lastFromHostPayloadCaptureCycle = RegInit(0.U(64.W))
    val lastFromHostPayloadCaptureData = RegInit(0.U(64.W))
    val lastFromHostPayloadCaptureMeta = RegInit(0.U(32.W))
    val minObsBuildMarker = RegInit("h05040003".U(32.W))
    dontTouch(minObsBuildMarker)

    val firstProgressValid = RegInit(false.B)
    val firstProgressEventMask = RegInit(0.U(32.W))
    val firstProgressCycle = RegInit(0.U(64.W))
    val firstProgressQueueSnapshot = RegInit(0.U(32.W))
    val firstProgressAdapterSnapshot = RegInit(0.U(32.W))
    val progressSeenMask = RegInit(0.U(32.W))
    val lastProgressEventMask = RegInit(0.U(32.W))
    val lastProgressCycle = RegInit(0.U(64.W))
    val lastProgressQueueSnapshot = RegInit(0.U(32.W))
    val lastProgressAdapterSnapshot = RegInit(0.U(32.W))
    val blockedStreakCount64 = RegInit(0.U(64.W))
    val maxBlockedStreakCount64 = RegInit(0.U(64.W))
    val targetCycleReadyStreakCount64 = RegInit(0.U(64.W))
    val targetCycleReadyMaxStreakCount64 = RegInit(0.U(64.W))
    val targetCycleNotReadyStreakCount64 = RegInit(0.U(64.W))
    val targetCycleNotReadyMaxStreakCount64 = RegInit(0.U(64.W))
    val fromHostLeafBlockedStreakCount64 = RegInit(0.U(64.W))
    val fromHostLeafBlockedMaxStreakCount64 = RegInit(0.U(64.W))
    val toHostPayloadBlockedStreakCount64 = RegInit(0.U(64.W))
    val toHostPayloadBlockedMaxStreakCount64 = RegInit(0.U(64.W))

    val toHostEverValidMask = RegInit(0.U(4.W))
    val toHostEverReadyMask = RegInit(0.U(4.W))
    val toHostEverFireMask = RegInit(0.U(4.W))
    val toHostEverBlockedMask = RegInit(0.U(4.W))
    val fromHostEverValidMask = RegInit(0.U(11.W))
    val fromHostEverReadyMask = RegInit(0.U(11.W))
    val fromHostEverFireMask = RegInit(0.U(11.W))
    val fromHostEverBlockedMask = RegInit(0.U(11.W))

    val lastBlockedToHostValidMask = RegInit(0.U(4.W))
    val lastBlockedToHostReadyMask = RegInit(0.U(4.W))
    val lastBlockedToHostFireMask = RegInit(0.U(4.W))
    val lastBlockedToHostBlockedMask = RegInit(0.U(4.W))
    val lastBlockedFromHostValidMask = RegInit(0.U(11.W))
    val lastBlockedFromHostReadyMask = RegInit(0.U(11.W))
    val lastBlockedFromHostFireMask = RegInit(0.U(11.W))
    val lastBlockedFromHostBlockedMask = RegInit(0.U(11.W))
    val lastBlockedQueueSnapshot = RegInit(0.U(32.W))
    val lastBlockedAdapterSnapshot = RegInit(0.U(32.W))

    val firstFromHostBlockedValid = RegInit(false.B)
    val firstFromHostBlockedCycle = RegInit(0.U(64.W))
    val firstFromHostBlockedReadyMask = RegInit(0.U(11.W))
    val firstFromHostBlockedValidMask = RegInit(0.U(11.W))
    val lastFromHostBlockedCycle = RegInit(0.U(64.W))
    val lastFromHostBlockedReadyMask = RegInit(0.U(11.W))
    val lastFromHostBlockedValidMask = RegInit(0.U(11.W))

    val firstToHostBlockedValid = RegInit(false.B)
    val firstToHostBlockedCycle = RegInit(0.U(64.W))
    val firstToHostBlockedReadyMask = RegInit(0.U(4.W))
    val firstToHostBlockedValidMask = RegInit(0.U(4.W))
    val lastToHostBlockedCycle = RegInit(0.U(64.W))
    val lastToHostBlockedReadyMask = RegInit(0.U(4.W))
    val lastToHostBlockedValidMask = RegInit(0.U(4.W))

    val firstFromHostLeafBlockedValid = RegInit(false.B)
    val firstFromHostLeafBlockedCycle = RegInit(0.U(64.W))
    val firstFromHostLeafBlockedMask = RegInit(0.U(4.W))
    val lastFromHostLeafBlockedCycle = RegInit(0.U(64.W))
    val lastFromHostLeafBlockedMask = RegInit(0.U(4.W))

    val firstTargetCycleReadyValid = RegInit(false.B)
    val firstTargetCycleReadyCycle = RegInit(0.U(64.W))
    val firstTargetCycleFireValid = RegInit(false.B)
    val firstTargetCycleFireCycle = RegInit(0.U(64.W))

    val htntPayloadEnqCount64 = RegInit(0.U(64.W))
    val htntEmptyEnqCount64 = RegInit(0.U(64.W))
    val targetInPayloadFireCount64 = RegInit(0.U(64.W))
    val targetInEmptyFireCount64 = RegInit(0.U(64.W))
    val nthtPayloadEnqCount64 = RegInit(0.U(64.W))
    val nthtPayloadDeqCount64 = RegInit(0.U(64.W))

    val lastHtntEnqCycle = RegInit(0.U(64.W))
    val lastHtntEnqData = RegInit(0.U(64.W))
    val lastHtntEnqMeta = RegInit(0.U(32.W))
    val lastHtntDeqCycle = RegInit(0.U(64.W))
    val lastHtntDeqData = RegInit(0.U(64.W))
    val lastHtntDeqMeta = RegInit(0.U(32.W))
    val lastTargetInFireCycle = RegInit(0.U(64.W))
    val lastTargetInFireData = RegInit(0.U(64.W))
    val lastTargetInFireMeta = RegInit(0.U(32.W))
    val lastTargetOutFireCycle = RegInit(0.U(64.W))
    val lastTargetOutFireData = RegInit(0.U(64.W))
    val lastTargetOutFireMeta = RegInit(0.U(32.W))
    val lastNthtEnqCycle = RegInit(0.U(64.W))
    val lastNthtEnqData = RegInit(0.U(64.W))
    val lastNthtEnqMeta = RegInit(0.U(32.W))
    val lastNthtDeqCycle = RegInit(0.U(64.W))
    val lastNthtDeqData = RegInit(0.U(64.W))
    val lastNthtDeqMeta = RegInit(0.U(32.W))

    val firstHtntPayloadEnqCycle = RegInit(0.U(64.W))
    val firstTargetInPayloadFireCycle = RegInit(0.U(64.W))
    val firstTargetOutPayloadFireCycle = RegInit(0.U(64.W))
    val firstNthtPayloadEnqCycle = RegInit(0.U(64.W))
    val firstPcieOutFireCycle = RegInit(0.U(64.W))
    debugCycle := debugCycle + 1.U

    val htntSnapshotMeta = Cat(
      0.U(21.W),
      htnt_queue.io.deq.bits.data_out_ready,
      htnt_queue.io.deq.bits.data_in_valid,
      htnt_queue.io.deq.bits.data_in.last,
      htnt_queue.io.deq.bits.data_in.keep,
    )

    val htntEnqSnapshotMeta = Cat(
      0.U(21.W),
      htnt_queue.io.enq.bits.data_out_ready,
      htnt_queue.io.enq.bits.data_in_valid,
      htnt_queue.io.enq.bits.data_in.last,
      htnt_queue.io.enq.bits.data_in.keep,
    )
    val targetInSnapshotMeta = Cat(
      0.U(19.W),
      hPort.nicIn.fire,
      hPort.nicIn.ready,
      hPort.nicIn.valid,
      hPort.nicIn.bits.valid,
      hPort.nicIn.bits.bits.last,
      hPort.nicIn.bits.bits.keep,
    )
    val targetOutSnapshotMeta = Cat(
      0.U(19.W),
      hPort.nicOut.fire,
      hPort.nicOut.ready,
      hPort.nicOut.valid,
      hPort.nicOut.bits.valid,
      hPort.nicOut.bits.bits.last,
      hPort.nicOut.bits.bits.keep,
    )
    val nthtSnapshotMeta = Cat(
      0.U(21.W),
      ntht_queue.io.enq.bits.data_in_ready,
      ntht_queue.io.enq.bits.data_out_valid,
      ntht_queue.io.enq.bits.data_out.last,
      ntht_queue.io.enq.bits.data_out.keep,
    )
    val nthtDeqSnapshotMeta = Cat(
      0.U(21.W),
      ntht_queue.io.deq.bits.data_in_ready,
      ntht_queue.io.deq.bits.data_out_valid,
      ntht_queue.io.deq.bits.data_out.last,
      ntht_queue.io.deq.bits.data_out.keep,
    )
    val queueSnapshot = Cat(
      0.U(13.W),
      ntht_queue.io.deq.ready,
      ntht_queue.io.deq.valid,
      ntht_queue.io.enq.valid,
      ntht_queue.io.enq.ready,
      htnt_queue.io.deq.ready,
      htnt_queue.io.deq.valid,
      htnt_queue.io.enq.valid,
      htnt_queue.io.enq.ready,
      fromHostPendingPayloadValid,
      fromHostCurrentPayloadValid,
      fromHostPayloadCaptureReady,
      ntht_queue.io.count,
      htnt_queue.io.count,
    )
    val adapterSnapshot = Cat(
      0.U(20.W),
      NICtokenToBig.io.pcie_out.ready,
      NICtokenToBig.io.pcie_out.valid,
      NICtokenToBig.io.ntht.ready,
      NICtokenToBig.io.ntht.valid,
      bigtokenToNIC.io.pcie_in.ready,
      bigtokenToNIC.io.pcie_in.valid,
      bigtokenToNIC.io.htnt.ready,
      bigtokenToNIC.io.htnt.valid,
      NICtokenToBig.io.debug_output_queue_count(1, 0),
      bigtokenToNIC.io.debug_loop_iter(1, 0),
    )

    val progressEventMask = Cat(
      0.U(14.W),
      tFire,
      targetCycleReady,
      NICtokenToBig.io.pcie_out.fire,
      ntht_queue.io.deq.fire,
      ntht_queue.io.enq.fire,
      toHostPayloadFire,
      hPort.nicOut.fire,
      hPort.nicOut.valid,
      hPort.nicIn.fire && hPort.nicIn.bits.valid,
      hPort.nicIn.fire,
      fromHostCurrentPayloadValid,
      fromHostPayloadCapture,
      fromHostPayloadDeq,
      htnt_queue.io.deq.fire,
      htnt_queue.io.enq.fire && htnt_queue.io.enq.bits.data_in_valid,
      htnt_queue.io.enq.fire,
      bigtokenToNIC.io.pcie_in.fire,
      debugCycle === 0.U,
    )
    val anyProgressEvent = progressEventMask.orR
    val nextBlockedStreakCount64 = blockedStreakCount64 + 1.U

    val currentBlockedReason = Cat(
      (NICtokenToBig.io.pcie_out.valid && !NICtokenToBig.io.pcie_out.ready),
      (bigtokenToNIC.io.pcie_in.valid && !bigtokenToNIC.io.pcie_in.ready),
      fromHostGroupBlocked,
      toHostPayloadBlocked,
      targetBlockedNoFromHostToken,
      targetBlockedToHostPayloadQueue,
      targetBlockedFromHostChannels,
      targetBlockedNoToHostToken,
    )

    when (tFire) {
      tFireCount := tFireCount + 1.U
      targetCycleFireCount64 := targetCycleFireCount64 + 1.U
      lastTFireCycle := debugCycle
      when (!firstTargetCycleFireValid) {
        firstTargetCycleFireValid := true.B
        firstTargetCycleFireCycle := debugCycle
      }
    }
    when (anyProgressEvent) {
      progressSeenMask := progressSeenMask | progressEventMask
      lastProgressEventMask := progressEventMask
      lastProgressCycle := debugCycle
      lastProgressQueueSnapshot := queueSnapshot
      lastProgressAdapterSnapshot := adapterSnapshot
      when (!firstProgressValid) {
        firstProgressValid := true.B
        firstProgressEventMask := progressEventMask
        firstProgressCycle := debugCycle
        firstProgressQueueSnapshot := queueSnapshot
        firstProgressAdapterSnapshot := adapterSnapshot
      }
    }
    toHostEverValidMask := toHostEverValidMask | toHostValidMask
    toHostEverReadyMask := toHostEverReadyMask | toHostReadyMask
    toHostEverFireMask := toHostEverFireMask | toHostFireMask
    toHostEverBlockedMask := toHostEverBlockedMask | toHostBlockedMask
    fromHostEverValidMask := fromHostEverValidMask | fromHostValidMask
    fromHostEverReadyMask := fromHostEverReadyMask | fromHostReadyMask
    fromHostEverFireMask := fromHostEverFireMask | fromHostFireMask
    fromHostEverBlockedMask := fromHostEverBlockedMask | fromHostBlockedMask
    when (tFire) {
      blockedStreakCount64 := 0.U
    } .otherwise {
      blockedStreakCount64 := nextBlockedStreakCount64
      when (nextBlockedStreakCount64 > maxBlockedStreakCount64) {
        maxBlockedStreakCount64 := nextBlockedStreakCount64
      }
    }
    when (toHostFire) {
      toHostFireCount := toHostFireCount + 1.U
      lastToHostFireCycle := debugCycle
      lastTargetOutFireCycle := debugCycle
      lastTargetOutFireData := hPort.nicOut.bits.bits.data
      lastTargetOutFireMeta := targetOutSnapshotMeta
    }
    when (fromHostFire) {
      fromHostFireCount := fromHostFireCount + 1.U
      lastFromHostFireCycle := debugCycle
      lastTargetInFireCycle := debugCycle
      lastTargetInFireData := hPort.nicIn.bits.bits.data
      lastTargetInFireMeta := targetInSnapshotMeta
      when (hPort.nicIn.bits.valid) {
        targetInPayloadFireCount64 := targetInPayloadFireCount64 + 1.U
        when (firstTargetInPayloadFireCycle === 0.U) {
          firstTargetInPayloadFireCycle := debugCycle
        }
      } .otherwise {
        targetInEmptyFireCount64 := targetInEmptyFireCount64 + 1.U
      }
    }
    when (toHostPayloadFire) {
      toHostPayloadFireCount64 := toHostPayloadFireCount64 + 1.U
      when (firstTargetOutPayloadFireCycle === 0.U) {
        firstTargetOutPayloadFireCycle := debugCycle
      }
    }
    when (toHostEmptyFire) {
      toHostEmptyFireCount64 := toHostEmptyFireCount64 + 1.U
    }
    when (toHostEmptyEnqueue) {
      toHostEmptyEnqueueCount64 := toHostEmptyEnqueueCount64 + 1.U
    }
    when (toHostEmptySuppressed) {
      toHostEmptySuppressedCount64 := toHostEmptySuppressedCount64 + 1.U
    }
    when (toHostPayloadBlocked) {
      toHostPayloadBlockedCount64 := toHostPayloadBlockedCount64 + 1.U
    }
    when (toHostMissingValidMask.orR) {
      toHostMissingValidCount64 := toHostMissingValidCount64 + 1.U
    }
    when (targetCycleReady) {
      targetCycleReadyCount64 := targetCycleReadyCount64 + 1.U
      targetCycleNotReadyStreakCount64 := 0.U
      val nextReadyStreak = targetCycleReadyStreakCount64 + 1.U
      targetCycleReadyStreakCount64 := nextReadyStreak
      when (nextReadyStreak > targetCycleReadyMaxStreakCount64) {
        targetCycleReadyMaxStreakCount64 := nextReadyStreak
      }
      when (!firstTargetCycleReadyValid) {
        firstTargetCycleReadyValid := true.B
        firstTargetCycleReadyCycle := debugCycle
      }
    } .otherwise {
      targetCycleNotReadyCount64 := targetCycleNotReadyCount64 + 1.U
      targetCycleReadyStreakCount64 := 0.U
      val nextNotReadyStreak = targetCycleNotReadyStreakCount64 + 1.U
      targetCycleNotReadyStreakCount64 := nextNotReadyStreak
      when (nextNotReadyStreak > targetCycleNotReadyMaxStreakCount64) {
        targetCycleNotReadyMaxStreakCount64 := nextNotReadyStreak
      }
    }
    when (toHostReadyDrive) {
      toHostReadyDriveCount64 := toHostReadyDriveCount64 + 1.U
    }
    when (fromHostChannelReady) {
      fromHostReadyCount64 := fromHostReadyCount64 + 1.U
    }
    when (fromHostEmptyDropFire) {
      fromHostEmptyDropCount64 := fromHostEmptyDropCount64 + 1.U
    }
    when (fromHostPayloadDeq) {
      fromHostPayloadCaptureCount64 := fromHostPayloadCaptureCount64 + 1.U
      lastFromHostPayloadCaptureCycle := debugCycle
      lastFromHostPayloadCaptureData := htnt_queue.io.deq.bits.data_in.data
      lastFromHostPayloadCaptureMeta := htntSnapshotMeta
    }
    when (fromHostGroupBlocked) {
      fromHostGroupBlockedCount64 := fromHostGroupBlockedCount64 + 1.U
      lastFromHostBlockedCycle := debugCycle
      lastFromHostBlockedReadyMask := fromHostReadyMask
      lastFromHostBlockedValidMask := fromHostValidMask
      when (!firstFromHostBlockedValid) {
        firstFromHostBlockedValid := true.B
        firstFromHostBlockedCycle := debugCycle
        firstFromHostBlockedReadyMask := fromHostReadyMask
        firstFromHostBlockedValidMask := fromHostValidMask
      }
    }
    when (fromHostWouldBlockedMask.orR) {
      fromHostWouldBlockedCount64 := fromHostWouldBlockedCount64 + 1.U
    }
    when (fromHostLeafBlocked) {
      fromHostLeafBlockedCount64 := fromHostLeafBlockedCount64 + 1.U
      lastFromHostLeafBlockedCycle := debugCycle
      lastFromHostLeafBlockedMask := fromHostLeafBlockedMask
      val nextLeafBlockedStreak = fromHostLeafBlockedStreakCount64 + 1.U
      fromHostLeafBlockedStreakCount64 := nextLeafBlockedStreak
      when (nextLeafBlockedStreak > fromHostLeafBlockedMaxStreakCount64) {
        fromHostLeafBlockedMaxStreakCount64 := nextLeafBlockedStreak
      }
      when (!firstFromHostLeafBlockedValid) {
        firstFromHostLeafBlockedValid := true.B
        firstFromHostLeafBlockedCycle := debugCycle
        firstFromHostLeafBlockedMask := fromHostLeafBlockedMask
      }
    } .otherwise {
      fromHostLeafBlockedStreakCount64 := 0.U
    }
    when (toHostPayloadBlocked) {
      lastToHostBlockedCycle := debugCycle
      lastToHostBlockedReadyMask := toHostReadyMask
      lastToHostBlockedValidMask := toHostValidMask
      val nextToHostPayloadBlockedStreak = toHostPayloadBlockedStreakCount64 + 1.U
      toHostPayloadBlockedStreakCount64 := nextToHostPayloadBlockedStreak
      when (nextToHostPayloadBlockedStreak > toHostPayloadBlockedMaxStreakCount64) {
        toHostPayloadBlockedMaxStreakCount64 := nextToHostPayloadBlockedStreak
      }
      when (!firstToHostBlockedValid) {
        firstToHostBlockedValid := true.B
        firstToHostBlockedCycle := debugCycle
        firstToHostBlockedReadyMask := toHostReadyMask
        firstToHostBlockedValidMask := toHostValidMask
      }
    } .otherwise {
      toHostPayloadBlockedStreakCount64 := 0.U
    }
    when (hPort.nicIn.valid && !hPort.nicIn.ready) {
      nicInBlockedCount64 := nicInBlockedCount64 + 1.U
    }
    when (hPort.macAddr.valid && !hPort.macAddr.ready) {
      macAddrBlockedCount64 := macAddrBlockedCount64 + 1.U
    }
    when (hPort.rlimit.valid && !hPort.rlimit.ready) {
      rlimitBlockedCount64 := rlimitBlockedCount64 + 1.U
    }
    when (hPort.pauser.valid && !hPort.pauser.ready) {
      pauserBlockedCount64 := pauserBlockedCount64 + 1.U
    }
    when (hPort.nicIn.fire) {
      nicInFireCount64 := nicInFireCount64 + 1.U
    }
    when (hPort.macAddr.fire) {
      macAddrFireCount64 := macAddrFireCount64 + 1.U
    }
    when (hPort.rlimit.fire) {
      rlimitFireCount64 := rlimitFireCount64 + 1.U
    }
    when (hPort.pauser.fire) {
      pauserFireCount64 := pauserFireCount64 + 1.U
    }
    when (!targetCycleReady && targetBlockedNoToHostToken) {
      targetBlockedNoToHostTokenCount64 := targetBlockedNoToHostTokenCount64 + 1.U
    }
    when (!targetCycleReady && targetBlockedNoFromHostToken) {
      targetBlockedNoFromHostTokenCount64 := targetBlockedNoFromHostTokenCount64 + 1.U
    }
    when (!targetCycleReady && targetBlockedFromHostChannels) {
      targetBlockedFromHostChannelsCount64 := targetBlockedFromHostChannelsCount64 + 1.U
    }
    when (!targetCycleReady && targetBlockedToHostPayloadQueue) {
      targetBlockedToHostPayloadQueueCount64 := targetBlockedToHostPayloadQueueCount64 + 1.U
    }
    when (htnt_queue.io.enq.fire) {
      htntQueueEnqFireCount := htntQueueEnqFireCount + 1.U
      lastHtntEnqCycle := debugCycle
      lastHtntEnqData := htnt_queue.io.enq.bits.data_in.data
      lastHtntEnqMeta := htntEnqSnapshotMeta
      when (htnt_queue.io.enq.bits.data_in_valid) {
        htntPayloadEnqCount64 := htntPayloadEnqCount64 + 1.U
        when (firstHtntPayloadEnqCycle === 0.U) {
          firstHtntPayloadEnqCycle := debugCycle
        }
      } .otherwise {
        htntEmptyEnqCount64 := htntEmptyEnqCount64 + 1.U
      }
    }
    when (htnt_queue.io.deq.fire) {
      htntQueueDeqFireCount := htntQueueDeqFireCount + 1.U
      lastHtntDeqCycle := debugCycle
      lastHtntDeqData := htnt_queue.io.deq.bits.data_in.data
      lastHtntDeqMeta := htntSnapshotMeta
    }
    when (ntht_queue.io.enq.fire) {
      nthtQueueEnqFireCount := nthtQueueEnqFireCount + 1.U
      lastNthtEnqCycle := debugCycle
      lastNthtEnqData := ntht_queue.io.enq.bits.data_out.data
      lastNthtEnqMeta := nthtSnapshotMeta
      when (ntht_queue.io.enq.bits.data_out_valid) {
        nthtPayloadEnqCount64 := nthtPayloadEnqCount64 + 1.U
        when (firstNthtPayloadEnqCycle === 0.U) {
          firstNthtPayloadEnqCycle := debugCycle
        }
      }
    }
    when (ntht_queue.io.deq.fire) {
      nthtQueueDeqFireCount := nthtQueueDeqFireCount + 1.U
      lastNthtDeqCycle := debugCycle
      lastNthtDeqData := ntht_queue.io.deq.bits.data_out.data
      lastNthtDeqMeta := nthtDeqSnapshotMeta
      when (ntht_queue.io.deq.bits.data_out_valid) {
        nthtPayloadDeqCount64 := nthtPayloadDeqCount64 + 1.U
      }
    }
    when (bigtokenToNIC.io.pcie_in.fire) {
      pcieInFireCount := pcieInFireCount + 1.U
      lastPcieInFireCycle := debugCycle
    }
    when (NICtokenToBig.io.pcie_out.fire) {
      pcieOutFireCount := pcieOutFireCount + 1.U
      lastPcieOutFireCycle := debugCycle
      when (firstPcieOutFireCycle === 0.U) {
        firstPcieOutFireCycle := debugCycle
      }
    }
    when (bigtokenToNIC.io.pcie_in.valid && !bigtokenToNIC.io.pcie_in.ready) {
      pcieInBackpressureCount64 := pcieInBackpressureCount64 + 1.U
    }
    when (NICtokenToBig.io.pcie_out.valid && !NICtokenToBig.io.pcie_out.ready) {
      pcieOutBackpressureCount64 := pcieOutBackpressureCount64 + 1.U
    }
    when (!tFire) {
      lastBlockedCycle := debugCycle
      lastBlockedReason := currentBlockedReason
      lastBlockedToHostValidMask := toHostValidMask
      lastBlockedToHostReadyMask := toHostReadyMask
      lastBlockedToHostFireMask := toHostFireMask
      lastBlockedToHostBlockedMask := toHostBlockedMask
      lastBlockedFromHostValidMask := fromHostValidMask
      lastBlockedFromHostReadyMask := fromHostReadyMask
      lastBlockedFromHostFireMask := fromHostFireMask
      lastBlockedFromHostBlockedMask := fromHostBlockedMask
      lastBlockedQueueSnapshot := queueSnapshot
      lastBlockedAdapterSnapshot := adapterSnapshot
      when (!firstBlockedValid) {
        firstBlockedValid := true.B
        firstBlockedCycle := debugCycle
        firstBlockedReason := currentBlockedReason
        firstBlockedQueueSnapshot := queueSnapshot
        firstBlockedAdapterSnapshot := adapterSnapshot
      }
    }

    if (p(LoopbackNIC)) {
      val tokenGen = Module(new HostToNICTokenGenerator(10))
      htnt_queue.io.enq <> tokenGen.io.out
      tokenGen.io.in <> ntht_queue.io.deq
      NICtokenToBig.io.ntht.valid := false.B
      NICtokenToBig.io.ntht.bits := DontCare
      bigtokenToNIC.io.htnt.ready := false.B
    } else {
      NICtokenToBig.io.ntht <> ntht_queue.io.deq
      htnt_queue.io.enq <> bigtokenToNIC.io.htnt
    }

    // Host input is self-cleaning: the target always receives a stable "no
    // packet" token unless a captured payload token is ready. Empty CPU-stream
    // tokens are dropped before they can bury real host packets behind an
    // unbounded backlog of no-op network cycles.
    val nextPendingPayloadValid = WireDefault(fromHostPendingPayloadValid)
    val nextPendingPayloadBits = WireDefault(fromHostPendingPayloadBits)
    val nextCurrentPayloadValid = WireDefault(fromHostCurrentPayloadValid)
    val nextCurrentPayloadBits = WireDefault(fromHostCurrentPayloadBits)

    when (hPort.nicIn.fire) {
      nextCurrentPayloadValid := fromHostPendingPayloadValid
      nextCurrentPayloadBits := fromHostPendingPayloadBits
      nextPendingPayloadValid := false.B
    }
    when (fromHostPayloadDeq) {
      when (fromHostPendingWillFree || !fromHostPendingPayloadValid) {
        nextPendingPayloadValid := true.B
        nextPendingPayloadBits := htnt_queue.io.deq.bits.data_in
      }
    }
    fromHostCurrentPayloadValid := nextCurrentPayloadValid
    fromHostCurrentPayloadBits := nextCurrentPayloadBits
    fromHostPendingPayloadValid := nextPendingPayloadValid
    fromHostPendingPayloadBits := nextPendingPayloadBits

    hPort.nicOut.ready := toHostReadyDrive
    // The software switch only consumes data-valid flits. Empty target->host
    // tokens carry no useful payload and are extremely expensive on F2 when the
    // host driver drains sparse CPU-managed stream beats. Suppress them here;
    // the host driver publishes empty shmem rounds when the to-host stream is
    // silent, which is enough to keep the switch cooperative.
    ntht_queue.io.enq.valid := toHostPayloadValid
    ntht_queue.io.enq.bits.data_out.data := hPort.nicOut.bits.bits.data
    ntht_queue.io.enq.bits.data_out.keep := hPort.nicOut.bits.bits.keep
    ntht_queue.io.enq.bits.data_out.last := hPort.nicOut.bits.bits.last
    ntht_queue.io.enq.bits.data_out_valid := toHostPayloadValid
    ntht_queue.io.enq.bits.data_in_ready := true.B

    hPort.nicIn.valid := fromHostValidDrive
    hPort.nicIn.bits.valid := fromHostCurrentPayloadValid
    hPort.nicIn.bits.bits.data := Mux(fromHostCurrentPayloadValid, fromHostCurrentPayloadBits.data, 0.U)
    hPort.nicIn.bits.bits.keep := Mux(fromHostCurrentPayloadValid, fromHostCurrentPayloadBits.keep, 0.U)
    hPort.nicIn.bits.bits.last := fromHostCurrentPayloadValid && fromHostCurrentPayloadBits.last
    hPort.macAddr.valid := true.B
    hPort.rlimit.valid := true.B
    hPort.pauser.valid := true.B
    htnt_queue.io.deq.ready := fromHostEmptyDrop || fromHostPayloadCapture

    bigtokenToNIC.io.pcie_in <> streamDeq
    streamEnq <> NICtokenToBig.io.pcie_out

    val macAddrDrive = Wire(UInt(IceNetConsts.ETH_MAC_BITS.W))
    val rlimitDrive = Wire(new RateLimiterSettings)
    val pauserDrive = Wire(new PauserSettings)
    hPort.macAddr.bits := macAddrDrive
    hPort.rlimit.bits := rlimitDrive
    hPort.pauser.bits := pauserDrive

    if (p(LoopbackNIC)) {
      rlimitDrive.size := 8.U
      rlimitDrive.period := 0.U
      rlimitDrive.inc := 1.U
      macAddrDrive := 0.U
      pauserDrive.threshold := 0.U
      pauserDrive.quanta := 0.U
      pauserDrive.refresh := 0.U
    } else {
      val macAddrRegUpper = Reg(UInt(32.W))
      val macAddrRegLower = Reg(UInt(32.W))
      val rlimitSettings = Reg(UInt(32.W))
      val pauseThreshold = Reg(UInt(32.W))
      val pauseTimes = Reg(UInt(32.W))

      rlimitDrive := rlimitSettings.asTypeOf(new RateLimiterSettings)
      macAddrDrive := Cat(macAddrRegUpper, macAddrRegLower)
      pauserDrive.threshold := pauseThreshold(15, 0)
      pauserDrive.quanta := pauseTimes(15, 0)
      pauserDrive.refresh := pauseTimes(31, 16)

      attach(macAddrRegUpper, "macaddr_upper", WriteOnly)
      attach(macAddrRegLower, "macaddr_lower", WriteOnly)
      attach(rlimitSettings, "rlimit_settings", WriteOnly)
      attach(pauseThreshold, "pause_threshold", WriteOnly)
      attach(pauseTimes, "pause_times", WriteOnly)
    }

    def attachUInt32Chunks(value: UInt, name: String, chunks: Int): Unit = {
      for (idx <- 0 until chunks) {
        val lo = idx * 32
        val hi = lo + 31
        attach(value(hi, lo), s"${name}_${idx}", ReadOnly)
      }
    }

    // Keep the NIC debug surface low-fanout: expose control and sticky status
    // needed for cycle-0 diagnosis, but avoid wide payload-history OCL taps.
    genROReg(!(toHostFire || fromHostFire), "done")
    attach(minObsBuildMarker, "minobs_build_marker", ReadOnly)
    attach(htnt_queue.io.count, "htnt_queue_count", ReadOnly)
    attach(ntht_queue.io.count, "ntht_queue_count", ReadOnly)
    attach(tFire, "t_fire", ReadOnly)
    attach(toHostFire, "to_host_fire", ReadOnly)
    attach(fromHostFire, "from_host_fire", ReadOnly)
    attach(toHostChannelValid, "hport_to_host_valid", ReadOnly)
    attach(toHostReadyDrive, "hport_to_host_ready", ReadOnly)
    attach(fromHostChannelValid, "hport_from_host_valid", ReadOnly)
    attach(fromHostChannelReady, "hport_from_host_ready", ReadOnly)
    attach(hPort.nicIn.bits.valid, "target_in_valid", ReadOnly)
    attach(hPort.nicOut.bits.valid, "target_out_valid", ReadOnly)
    attach(targetCycleReady, "target_cycle_ready", ReadOnly)
    attach(toHostReadyDrive, "to_host_ready_drive", ReadOnly)
    attach(fromHostChannelReady, "from_host_all_ready", ReadOnly)
    attach(fromHostTokenAvailable, "from_host_token_available", ReadOnly)
    attach(fromHostPayloadTokenAvailable, "from_host_payload_token_available", ReadOnly)
    attach(fromHostEmptyTokenAvailable, "from_host_empty_token_available", ReadOnly)
    attach(fromHostEmptyDrop, "from_host_empty_drop", ReadOnly)
    attach(fromHostCurrentPayloadValid, "from_host_current_payload_valid", ReadOnly)
    attach(fromHostPendingPayloadValid, "from_host_pending_payload_valid", ReadOnly)
    attach(fromHostPayloadCapture, "from_host_payload_capture", ReadOnly)
    attach(fromHostPayloadCaptureReady, "from_host_payload_capture_ready", ReadOnly)
    attach(fromHostPayloadDeq, "from_host_payload_deq", ReadOnly)
    attach(fromHostEmptyDropFire, "from_host_empty_drop_fire", ReadOnly)
    attach(toHostPayloadValid, "to_host_payload_valid", ReadOnly)
    attach(toHostEmptyValid, "to_host_empty_valid", ReadOnly)
    attach(toHostEmptySuppressed, "to_host_empty_suppressed", ReadOnly)
    attach(toHostPayloadBlocked, "to_host_payload_blocked", ReadOnly)
    attach(htnt_queue.io.enq.valid, "htnt_queue_enq_valid", ReadOnly)
    attach(htnt_queue.io.enq.ready, "htnt_queue_enq_ready", ReadOnly)
    attach(htnt_queue.io.deq.valid, "htnt_queue_deq_valid", ReadOnly)
    attach(htnt_queue.io.deq.ready, "htnt_queue_deq_ready", ReadOnly)
    attach(ntht_queue.io.enq.valid, "ntht_queue_enq_valid", ReadOnly)
    attach(ntht_queue.io.enq.ready, "ntht_queue_enq_ready", ReadOnly)
    attach(ntht_queue.io.deq.valid, "ntht_queue_deq_valid", ReadOnly)
    attach(ntht_queue.io.deq.ready, "ntht_queue_deq_ready", ReadOnly)
    attach(bigtokenToNIC.io.pcie_in.valid, "bigtoken_pcie_in_valid", ReadOnly)
    attach(bigtokenToNIC.io.pcie_in.ready, "bigtoken_pcie_in_ready", ReadOnly)
    attach(NICtokenToBig.io.pcie_out.valid, "nicbig_pcie_out_valid", ReadOnly)
    attach(NICtokenToBig.io.pcie_out.ready, "nicbig_pcie_out_ready", ReadOnly)
    attach(NICtokenToBig.io.debug_output_queue_count, "nicbig_output_queue_count", ReadOnly)
    attach(currentBlockedReason, "current_blocked_reason", ReadOnly)
    attach(targetBlockedNoToHostToken, "target_blocked_no_to_host_token", ReadOnly)
    attach(targetBlockedFromHostChannels, "target_blocked_from_host_channels", ReadOnly)
    attach(targetBlockedToHostPayloadQueue, "target_blocked_to_host_payload_queue", ReadOnly)
    attachUInt32Chunks(debugCycle, "debug_cycle", 2)
    attachUInt32Chunks(targetCycleFireCount64, "target_cycle_fire_count64", 2)
    attachUInt32Chunks(targetCycleReadyCount64, "target_cycle_ready_count64", 2)
    attachUInt32Chunks(targetCycleNotReadyCount64, "target_cycle_not_ready_count64", 2)
    attachUInt32Chunks(fromHostPayloadCaptureCount64, "from_host_payload_capture_count64", 2)
    attachUInt32Chunks(fromHostEmptyDropCount64, "from_host_empty_drop_count64", 2)
    attachUInt32Chunks(toHostPayloadFireCount64, "to_host_payload_fire_count64", 2)
    attachUInt32Chunks(toHostEmptyFireCount64, "to_host_empty_fire_count64", 2)
    attach(pcieInFireCount, "pcie_in_fire_count", ReadOnly)
    attach(pcieOutFireCount, "pcie_out_fire_count", ReadOnly)

    // 2026-05-04 timing-recovery build: keep the previous broad debug attach
    // list type-checked but do not elaborate its OCL/CSR surface.
    if (false) {
    attach(minObsBuildMarker, "minobs_build_marker", ReadOnly)
    attach(htnt_queue.io.count, "htnt_queue_count", ReadOnly)
    attach(ntht_queue.io.count, "ntht_queue_count", ReadOnly)
    attach(tFire, "t_fire", ReadOnly)
    attach(toHostFire, "to_host_fire", ReadOnly)
    attach(fromHostFire, "from_host_fire", ReadOnly)
    attach(tFireBlockedToHostValid, "t_fire_blocked_to_host_valid", ReadOnly)
    attach(tFireBlockedFromHostReady, "t_fire_blocked_from_host_ready", ReadOnly)
    attach(tFireBlockedNthtEnqReady, "t_fire_blocked_ntht_enq_ready", ReadOnly)
    attach(tFireBlockedHtntDeqValid, "t_fire_blocked_htnt_deq_valid", ReadOnly)
    attach(toHostChannelValid, "hport_to_host_valid", ReadOnly)
    attach(toHostReadyDrive, "hport_to_host_ready", ReadOnly)
    attach(fromHostChannelValid, "hport_from_host_valid", ReadOnly)
    attach(fromHostChannelReady, "hport_from_host_ready", ReadOnly)
    attach(hPort.nicIn.bits.valid, "target_in_valid", ReadOnly)
    attach(hPort.nicOut.bits.valid, "target_out_valid", ReadOnly)
    attach(toHostValidMask, "chan_to_host_valid_mask", ReadOnly)
    attach(toHostReadyMask, "chan_to_host_ready_mask", ReadOnly)
    attach(toHostFireMask, "chan_to_host_fire_mask", ReadOnly)
    attach(toHostBlockedMask, "chan_to_host_blocked_mask", ReadOnly)
    attach(fromHostValidMask, "chan_from_host_valid_mask", ReadOnly)
    attach(fromHostReadyMask, "chan_from_host_ready_mask", ReadOnly)
    attach(fromHostFireMask, "chan_from_host_fire_mask", ReadOnly)
    attach(fromHostBlockedMask, "chan_from_host_blocked_mask", ReadOnly)
    attach(fromHostWouldValidMask, "chan_from_host_would_valid_mask", ReadOnly)
    attach(fromHostWouldBlockedMask, "chan_from_host_would_blocked_mask", ReadOnly)
    attach(fromHostLeafValidMask, "from_host_leaf_valid_mask", ReadOnly)
    attach(fromHostLeafReadyMask, "from_host_leaf_ready_mask", ReadOnly)
    attach(fromHostLeafFireMask, "from_host_leaf_fire_mask", ReadOnly)
    attach(fromHostLeafBlockedMask, "from_host_leaf_blocked_mask", ReadOnly)
    attach(bigtokenToNIC.io.htnt.valid, "bigtoken_htnt_valid", ReadOnly)
    attach(bigtokenToNIC.io.htnt.ready, "bigtoken_htnt_ready", ReadOnly)
    attach(NICtokenToBig.io.ntht.valid, "nicbig_ntht_valid", ReadOnly)
    attach(NICtokenToBig.io.ntht.ready, "nicbig_ntht_ready", ReadOnly)
    attach(bigtokenToNIC.io.pcie_in.valid, "bigtoken_pcie_in_valid", ReadOnly)
    attach(bigtokenToNIC.io.pcie_in.ready, "bigtoken_pcie_in_ready", ReadOnly)
    attach(NICtokenToBig.io.pcie_out.valid, "nicbig_pcie_out_valid", ReadOnly)
    attach(NICtokenToBig.io.pcie_out.ready, "nicbig_pcie_out_ready", ReadOnly)
    attach(bigtokenToNIC.io.debug_loop_iter, "bigtoken_loop_iter", ReadOnly)
    attach(bigtokenToNIC.io.debug_latched_valid, "bigtoken_latched_valid", ReadOnly)
    attach(bigtokenToNIC.io.debug_accept_count, "bigtoken_accept_count", ReadOnly)
    attach(bigtokenToNIC.io.debug_emit_count, "bigtoken_emit_count", ReadOnly)
    attach(bigtokenToNIC.io.debug_payload_emit_count, "bigtoken_payload_emit_count", ReadOnly)
    attach(bigtokenToNIC.io.debug_pcie_valid_no_ready_count, "bigtoken_pcie_valid_no_ready_count", ReadOnly)
    attach(bigtokenToNIC.io.debug_pcie_ready_no_valid_count, "bigtoken_pcie_ready_no_valid_count", ReadOnly)
    attach(bigtokenToNIC.io.debug_htnt_valid_no_ready_count, "bigtoken_htnt_valid_no_ready_count", ReadOnly)
    attach(bigtokenToNIC.io.debug_htnt_ready_no_valid_count, "bigtoken_htnt_ready_no_valid_count", ReadOnly)
    attach(bigtokenToNIC.io.debug_current_slot_meta, "bigtoken_current_slot_meta", ReadOnly)
    attachUInt32Chunks(bigtokenToNIC.io.debug_current_slot_data, "bigtoken_current_slot_data", 2)
    attach(bigtokenToNIC.io.debug_latched_valid_mask, "bigtoken_latched_valid_mask", ReadOnly)
    attach(bigtokenToNIC.io.debug_latched_ready_mask, "bigtoken_latched_ready_mask", ReadOnly)
    attach(bigtokenToNIC.io.debug_latched_last_mask, "bigtoken_latched_last_mask", ReadOnly)
    attach(bigtokenToNIC.io.debug_last_emit_slot_mask, "bigtoken_last_emit_slot_mask", ReadOnly)
    attach(bigtokenToNIC.io.debug_ever_emit_slot_mask, "bigtoken_ever_emit_slot_mask", ReadOnly)
    attach(bigtokenToNIC.io.debug_ever_payload_emit_slot_mask, "bigtoken_ever_payload_emit_slot_mask", ReadOnly)
    attachUInt32Chunks(bigtokenToNIC.io.debug_last_latched_word0, "bigtoken_last_latched_word0", 2)
    attachUInt32Chunks(bigtokenToNIC.io.debug_last_latched_word1, "bigtoken_last_latched_word1", 2)
    attachUInt32Chunks(bigtokenToNIC.io.debug_last_latched_word2, "bigtoken_last_latched_word2", 2)
    attachUInt32Chunks(bigtokenToNIC.io.debug_last_emit_data, "bigtoken_last_emit_data", 2)
    attach(bigtokenToNIC.io.debug_last_emit_meta, "bigtoken_last_emit_meta", ReadOnly)
    attach(NICtokenToBig.io.debug_special_counter, "nicbig_special_counter", ReadOnly)
    attach(NICtokenToBig.io.debug_full_flush_count, "nicbig_full_flush_count", ReadOnly)
    attach(NICtokenToBig.io.debug_partial_flush_count, "nicbig_partial_flush_count", ReadOnly)
    attach(NICtokenToBig.io.debug_last_flush_slot, "nicbig_last_flush_slot", ReadOnly)
    attach(NICtokenToBig.io.debug_pcie_out_fire_count, "nicbig_pcie_out_fire_count", ReadOnly)
    attach(NICtokenToBig.io.debug_pcie_out_enq_count, "nicbig_pcie_out_enq_count", ReadOnly)
    attach(NICtokenToBig.io.debug_output_queue_count, "nicbig_output_queue_count", ReadOnly)
    attach(NICtokenToBig.io.debug_ntht_fire_count, "nicbig_ntht_fire_count", ReadOnly)
    attach(NICtokenToBig.io.debug_ntht_valid_no_ready_count, "nicbig_ntht_valid_no_ready_count", ReadOnly)
    attach(NICtokenToBig.io.debug_ntht_ready_no_valid_count, "nicbig_ntht_ready_no_valid_count", ReadOnly)
    attach(NICtokenToBig.io.debug_pcie_out_valid_no_ready_count, "nicbig_pcie_out_valid_no_ready_count", ReadOnly)
    attach(NICtokenToBig.io.debug_pcie_out_ready_no_valid_count, "nicbig_pcie_out_ready_no_valid_count", ReadOnly)
    attach(NICtokenToBig.io.debug_pcie_out_enq_valid_no_ready_count, "nicbig_pcie_out_enq_valid_no_ready_count", ReadOnly)
    attach(NICtokenToBig.io.debug_flush_candidate, "nicbig_flush_candidate", ReadOnly)
    attach(NICtokenToBig.io.debug_flush_blocked, "nicbig_flush_blocked", ReadOnly)
    attachUInt32Chunks(NICtokenToBig.io.debug_last_ntht_data, "nicbig_last_ntht_data", 2)
    attach(NICtokenToBig.io.debug_last_ntht_meta, "nicbig_last_ntht_meta", ReadOnly)
    for (idx <- 0 until 8) {
      attachUInt32Chunks(NICtokenToBig.io.debug_last_pcie_out_enq_words(idx), s"nicbig_last_pcie_out_enq_word${idx}", 2)
    }
    for (idx <- 0 until 8) {
      attachUInt32Chunks(NICtokenToBig.io.debug_last_pcie_out_deq_words(idx), s"nicbig_last_pcie_out_deq_word${idx}", 2)
    }

    val toHostPacketSeq = RegInit(0.U(64.W))
    val toHostPacketFlitIdx = RegInit(0.U(16.W))
    when (hPort.nicOut.fire && hPort.nicOut.bits.valid) {
      when (hPort.nicOut.bits.bits.last) {
        toHostPacketSeq := toHostPacketSeq + 1.U
        toHostPacketFlitIdx := 0.U
      } .otherwise {
        toHostPacketFlitIdx := toHostPacketFlitIdx + 1.U
      }
    }

    attachUInt32Chunks(toHostPacketSeq, "to_host_packet_seq", 2)
    attach(toHostPacketFlitIdx, "to_host_packet_flit_idx", ReadOnly)

    attach(tFireCount, "t_fire_count", ReadOnly)
    attach(toHostFireCount, "to_host_fire_count", ReadOnly)
    attach(fromHostFireCount, "from_host_fire_count", ReadOnly)
    attach(htntQueueEnqFireCount, "htnt_queue_enq_fire_count", ReadOnly)
    attach(htntQueueDeqFireCount, "htnt_queue_deq_fire_count", ReadOnly)
    attach(nthtQueueEnqFireCount, "ntht_queue_enq_fire_count", ReadOnly)
    attach(nthtQueueDeqFireCount, "ntht_queue_deq_fire_count", ReadOnly)
    attach(pcieInFireCount, "pcie_in_fire_count", ReadOnly)
    attach(pcieOutFireCount, "pcie_out_fire_count", ReadOnly)
    attach(currentBlockedReason, "current_blocked_reason", ReadOnly)
    attach(firstBlockedValid, "first_blocked_valid", ReadOnly)
    attach(firstBlockedReason, "first_blocked_reason", ReadOnly)
    attach(firstBlockedQueueSnapshot, "first_blocked_queue_snapshot", ReadOnly)
    attach(firstBlockedAdapterSnapshot, "first_blocked_adapter_snapshot", ReadOnly)
    attach(lastBlockedReason, "last_blocked_reason", ReadOnly)
    attachUInt32Chunks(debugCycle, "debug_cycle", 2)
    attachUInt32Chunks(lastTFireCycle, "last_t_fire_cycle", 2)
    attachUInt32Chunks(lastToHostFireCycle, "last_to_host_fire_cycle", 2)
    attachUInt32Chunks(lastFromHostFireCycle, "last_from_host_fire_cycle", 2)
    attachUInt32Chunks(lastPcieInFireCycle, "last_pcie_in_fire_cycle", 2)
    attachUInt32Chunks(lastPcieOutFireCycle, "last_pcie_out_fire_cycle", 2)
    attachUInt32Chunks(firstBlockedCycle, "first_blocked_cycle", 2)
    attachUInt32Chunks(lastBlockedCycle, "last_blocked_cycle", 2)
    attach(localQueuesReady, "local_queues_ready", ReadOnly)
    attach(targetCycleReady, "target_cycle_ready", ReadOnly)
    attach(toHostReadyDrive, "to_host_ready_drive", ReadOnly)
    attach(fromHostValidDrive, "from_host_valid_drive", ReadOnly)
    attach(fromHostChannelReady, "from_host_all_ready", ReadOnly)
    attach(fromHostGroupFire, "from_host_group_fire", ReadOnly)
    attach(fromHostGroupBlocked, "from_host_group_blocked", ReadOnly)
    attach(fromHostTokenAvailable, "from_host_token_available", ReadOnly)
    attach(fromHostPayloadTokenAvailable, "from_host_payload_token_available", ReadOnly)
    attach(fromHostEmptyTokenAvailable, "from_host_empty_token_available", ReadOnly)
    attach(fromHostEmptyDrop, "from_host_empty_drop", ReadOnly)
    attach(toHostPayloadValid, "to_host_payload_valid", ReadOnly)
    attach(toHostEmptyValid, "to_host_empty_valid", ReadOnly)
    attach(toHostEmptyBypass, "to_host_empty_bypass", ReadOnly)
    attach(toHostEmptyEnqueue, "to_host_empty_enqueue", ReadOnly)
    attach(toHostPayloadBlocked, "to_host_payload_blocked", ReadOnly)
    attach(htnt_queue.io.enq.ready, "htnt_queue_enq_ready", ReadOnly)
    attach(htnt_queue.io.enq.valid, "htnt_queue_enq_valid", ReadOnly)
    attach(htnt_queue.io.deq.valid, "htnt_queue_deq_valid", ReadOnly)
    attach(htnt_queue.io.deq.ready, "htnt_queue_deq_ready", ReadOnly)
    attach(ntht_queue.io.enq.ready, "ntht_queue_enq_ready", ReadOnly)
    attach(ntht_queue.io.enq.valid, "ntht_queue_enq_valid", ReadOnly)
    attach(ntht_queue.io.deq.valid, "ntht_queue_deq_valid", ReadOnly)
    attach(ntht_queue.io.deq.ready, "ntht_queue_deq_ready", ReadOnly)
    attach(hPort.nicIn.bits.bits.data(31, 0), "target_in_bits_data_lo", ReadOnly)
    attach(hPort.nicIn.bits.bits.data(63, 32), "target_in_bits_data_hi", ReadOnly)
    attach(hPort.nicIn.bits.bits.keep, "target_in_bits_keep", ReadOnly)
    attach(hPort.nicIn.bits.bits.last, "target_in_bits_last", ReadOnly)
    attach(hPort.nicOut.bits.bits.data(31, 0), "target_out_bits_data_lo", ReadOnly)
    attach(hPort.nicOut.bits.bits.data(63, 32), "target_out_bits_data_hi", ReadOnly)
    attach(hPort.nicOut.bits.bits.keep, "target_out_bits_keep", ReadOnly)
    attach(hPort.nicOut.bits.bits.last, "target_out_bits_last", ReadOnly)
    attach(htnt_queue.io.deq.bits.data_in.data(31, 0), "htnt_deq_data_lo", ReadOnly)
    attach(htnt_queue.io.deq.bits.data_in.data(63, 32), "htnt_deq_data_hi", ReadOnly)
    attach(htnt_queue.io.deq.bits.data_in.keep, "htnt_deq_keep", ReadOnly)
    attach(htnt_queue.io.deq.bits.data_in.last, "htnt_deq_last", ReadOnly)
    attach(htnt_queue.io.deq.bits.data_in_valid, "htnt_deq_data_in_valid", ReadOnly)
    attach(htnt_queue.io.deq.bits.data_out_ready, "htnt_deq_data_out_ready", ReadOnly)
    attach(ntht_queue.io.enq.bits.data_out.data(31, 0), "ntht_enq_data_lo", ReadOnly)
    attach(ntht_queue.io.enq.bits.data_out.data(63, 32), "ntht_enq_data_hi", ReadOnly)
    attach(ntht_queue.io.enq.bits.data_out.keep, "ntht_enq_keep", ReadOnly)
    attach(ntht_queue.io.enq.bits.data_out.last, "ntht_enq_last", ReadOnly)
    attach(ntht_queue.io.enq.bits.data_out_valid, "ntht_enq_data_out_valid", ReadOnly)
    attach(ntht_queue.io.enq.bits.data_in_ready, "ntht_enq_data_in_ready", ReadOnly)
    attach(fromHostCurrentPayloadValid, "from_host_current_payload_valid", ReadOnly)
    attach(fromHostPendingPayloadValid, "from_host_pending_payload_valid", ReadOnly)
    attach(fromHostPayloadCapture, "from_host_payload_capture", ReadOnly)
    attach(fromHostPayloadCaptureReady, "from_host_payload_capture_ready", ReadOnly)
    attach(fromHostPayloadDeq, "from_host_payload_deq", ReadOnly)
    attach(fromHostEmptyDropFire, "from_host_empty_drop_fire", ReadOnly)
    attach(fromHostCurrentPayloadBits.data(31, 0), "from_host_current_payload_data_lo", ReadOnly)
    attach(fromHostCurrentPayloadBits.data(63, 32), "from_host_current_payload_data_hi", ReadOnly)
    attach(fromHostCurrentPayloadBits.keep, "from_host_current_payload_keep", ReadOnly)
    attach(fromHostCurrentPayloadBits.last, "from_host_current_payload_last", ReadOnly)
    attach(fromHostPendingPayloadBits.data(31, 0), "from_host_pending_payload_data_lo", ReadOnly)
    attach(fromHostPendingPayloadBits.data(63, 32), "from_host_pending_payload_data_hi", ReadOnly)
    attach(fromHostPendingPayloadBits.keep, "from_host_pending_payload_keep", ReadOnly)
    attach(fromHostPendingPayloadBits.last, "from_host_pending_payload_last", ReadOnly)
    attachUInt32Chunks(fromHostPayloadCaptureCount64, "from_host_payload_capture_count64", 2)
    attachUInt32Chunks(lastFromHostPayloadCaptureCycle, "last_from_host_payload_capture_cycle", 2)
    attachUInt32Chunks(lastFromHostPayloadCaptureData, "last_from_host_payload_capture_data", 2)
    attach(lastFromHostPayloadCaptureMeta, "last_from_host_payload_capture_meta", ReadOnly)
    attachUInt32Chunks(targetCycleFireCount64, "target_cycle_fire_count64", 2)
    attachUInt32Chunks(targetBlockedNoToHostTokenCount64, "target_blocked_no_to_host_token_count64", 2)
    attachUInt32Chunks(targetBlockedFromHostChannelsCount64, "target_blocked_from_host_channels_count64", 2)
    attachUInt32Chunks(targetBlockedToHostPayloadQueueCount64, "target_blocked_to_host_payload_queue_count64", 2)
    attachUInt32Chunks(targetCycleReadyCount64, "target_cycle_ready_count64", 2)
    attachUInt32Chunks(targetCycleNotReadyCount64, "target_cycle_not_ready_count64", 2)
    attach(firstTargetCycleReadyValid, "first_target_cycle_ready_valid", ReadOnly)
    attachUInt32Chunks(firstTargetCycleReadyCycle, "first_target_cycle_ready_cycle", 2)
    attach(firstTargetCycleFireValid, "first_target_cycle_fire_valid", ReadOnly)
    attachUInt32Chunks(firstTargetCycleFireCycle, "first_target_cycle_fire_cycle", 2)
    attachUInt32Chunks(targetCycleReadyStreakCount64, "target_cycle_ready_streak_count64", 2)
    attachUInt32Chunks(targetCycleReadyMaxStreakCount64, "target_cycle_ready_max_streak_count64", 2)
    attachUInt32Chunks(targetCycleNotReadyStreakCount64, "target_cycle_not_ready_streak_count64", 2)
    attachUInt32Chunks(targetCycleNotReadyMaxStreakCount64, "target_cycle_not_ready_max_streak_count64", 2)
    attachUInt32Chunks(toHostReadyDriveCount64, "to_host_ready_drive_count64", 2)
    attachUInt32Chunks(fromHostReadyCount64, "from_host_ready_count64", 2)
    attach(targetBlockedNoToHostToken, "target_blocked_no_to_host_token", ReadOnly)
    attachUInt32Chunks(targetBlockedNoFromHostTokenCount64, "target_blocked_no_from_host_token_count64", 2)
    attach(targetBlockedFromHostChannels, "target_blocked_from_host_channels", ReadOnly)
    attach(targetBlockedToHostPayloadQueue, "target_blocked_to_host_payload_queue", ReadOnly)
    attach(targetBlockedNoFromHostToken, "target_blocked_no_from_host_token", ReadOnly)
    attach(toHostEmptySuppressed, "to_host_empty_suppressed", ReadOnly)
    attachUInt32Chunks(toHostPayloadFireCount64, "to_host_payload_fire_count64", 2)
    attachUInt32Chunks(toHostEmptyFireCount64, "to_host_empty_fire_count64", 2)
    attachUInt32Chunks(toHostEmptyEnqueueCount64, "to_host_empty_enqueue_count64", 2)
    attachUInt32Chunks(toHostEmptySuppressedCount64, "to_host_empty_suppressed_count64", 2)
    attachUInt32Chunks(fromHostEmptyDropCount64, "from_host_empty_drop_count64", 2)
    attachUInt32Chunks(pcieInBackpressureCount64, "pcie_in_backpressure_count64", 2)
    attachUInt32Chunks(pcieOutBackpressureCount64, "pcie_out_backpressure_count64", 2)
    attachUInt32Chunks(toHostMissingValidCount64, "to_host_missing_valid_count64", 2)
    attachUInt32Chunks(fromHostGroupBlockedCount64, "from_host_group_blocked_count64", 2)
    attachUInt32Chunks(fromHostWouldBlockedCount64, "from_host_would_blocked_count64", 2)
    attachUInt32Chunks(fromHostLeafBlockedCount64, "from_host_leaf_blocked_count64", 2)
    attachUInt32Chunks(nicInBlockedCount64, "nic_in_blocked_count64", 2)
    attachUInt32Chunks(macAddrBlockedCount64, "macaddr_blocked_count64", 2)
    attachUInt32Chunks(rlimitBlockedCount64, "rlimit_blocked_count64", 2)
    attachUInt32Chunks(pauserBlockedCount64, "pauser_blocked_count64", 2)
    attachUInt32Chunks(nicInFireCount64, "nic_in_fire_count64", 2)
    attachUInt32Chunks(macAddrFireCount64, "macaddr_fire_count64", 2)
    attachUInt32Chunks(rlimitFireCount64, "rlimit_fire_count64", 2)
    attachUInt32Chunks(pauserFireCount64, "pauser_fire_count64", 2)
    attach(firstProgressValid, "first_progress_valid", ReadOnly)
    attach(firstProgressEventMask, "first_progress_event_mask", ReadOnly)
    attachUInt32Chunks(firstProgressCycle, "first_progress_cycle", 2)
    attach(firstProgressQueueSnapshot, "first_progress_queue_snapshot", ReadOnly)
    attach(firstProgressAdapterSnapshot, "first_progress_adapter_snapshot", ReadOnly)
    attach(progressSeenMask, "progress_seen_mask", ReadOnly)
    attach(lastProgressEventMask, "last_progress_event_mask", ReadOnly)
    attachUInt32Chunks(lastProgressCycle, "last_progress_cycle", 2)
    attach(lastProgressQueueSnapshot, "last_progress_queue_snapshot", ReadOnly)
    attach(lastProgressAdapterSnapshot, "last_progress_adapter_snapshot", ReadOnly)
    attachUInt32Chunks(blockedStreakCount64, "blocked_streak_count64", 2)
    attachUInt32Chunks(maxBlockedStreakCount64, "max_blocked_streak_count64", 2)
    attachUInt32Chunks(fromHostLeafBlockedStreakCount64, "from_host_leaf_blocked_streak_count64", 2)
    attachUInt32Chunks(fromHostLeafBlockedMaxStreakCount64, "from_host_leaf_blocked_max_streak_count64", 2)
    attachUInt32Chunks(toHostPayloadBlockedStreakCount64, "to_host_payload_blocked_streak_count64", 2)
    attachUInt32Chunks(toHostPayloadBlockedMaxStreakCount64, "to_host_payload_blocked_max_streak_count64", 2)
    attach(toHostEverValidMask, "to_host_ever_valid_mask", ReadOnly)
    attach(toHostEverReadyMask, "to_host_ever_ready_mask", ReadOnly)
    attach(toHostEverFireMask, "to_host_ever_fire_mask", ReadOnly)
    attach(toHostEverBlockedMask, "to_host_ever_blocked_mask", ReadOnly)
    attach(fromHostEverValidMask, "from_host_ever_valid_mask", ReadOnly)
    attach(fromHostEverReadyMask, "from_host_ever_ready_mask", ReadOnly)
    attach(fromHostEverFireMask, "from_host_ever_fire_mask", ReadOnly)
    attach(fromHostEverBlockedMask, "from_host_ever_blocked_mask", ReadOnly)
    attach(lastBlockedToHostValidMask, "last_blocked_to_host_valid_mask", ReadOnly)
    attach(lastBlockedToHostReadyMask, "last_blocked_to_host_ready_mask", ReadOnly)
    attach(lastBlockedToHostFireMask, "last_blocked_to_host_fire_mask", ReadOnly)
    attach(lastBlockedToHostBlockedMask, "last_blocked_to_host_blocked_mask", ReadOnly)
    attach(lastBlockedFromHostValidMask, "last_blocked_from_host_valid_mask", ReadOnly)
    attach(lastBlockedFromHostReadyMask, "last_blocked_from_host_ready_mask", ReadOnly)
    attach(lastBlockedFromHostFireMask, "last_blocked_from_host_fire_mask", ReadOnly)
    attach(lastBlockedFromHostBlockedMask, "last_blocked_from_host_blocked_mask", ReadOnly)
    attach(lastBlockedQueueSnapshot, "last_blocked_queue_snapshot", ReadOnly)
    attach(lastBlockedAdapterSnapshot, "last_blocked_adapter_snapshot", ReadOnly)
    attach(firstFromHostBlockedValid, "first_from_host_blocked_valid", ReadOnly)
    attachUInt32Chunks(firstFromHostBlockedCycle, "first_from_host_blocked_cycle", 2)
    attach(firstFromHostBlockedReadyMask, "first_from_host_blocked_ready_mask", ReadOnly)
    attach(firstFromHostBlockedValidMask, "first_from_host_blocked_valid_mask", ReadOnly)
    attachUInt32Chunks(lastFromHostBlockedCycle, "last_from_host_blocked_cycle", 2)
    attach(lastFromHostBlockedReadyMask, "last_from_host_blocked_ready_mask", ReadOnly)
    attach(lastFromHostBlockedValidMask, "last_from_host_blocked_valid_mask", ReadOnly)
    attach(firstToHostBlockedValid, "first_to_host_blocked_valid", ReadOnly)
    attachUInt32Chunks(firstToHostBlockedCycle, "first_to_host_blocked_cycle", 2)
    attach(firstToHostBlockedReadyMask, "first_to_host_blocked_ready_mask", ReadOnly)
    attach(firstToHostBlockedValidMask, "first_to_host_blocked_valid_mask", ReadOnly)
    attachUInt32Chunks(lastToHostBlockedCycle, "last_to_host_blocked_cycle", 2)
    attach(lastToHostBlockedReadyMask, "last_to_host_blocked_ready_mask", ReadOnly)
    attach(lastToHostBlockedValidMask, "last_to_host_blocked_valid_mask", ReadOnly)
    attach(firstFromHostLeafBlockedValid, "first_from_host_leaf_blocked_valid", ReadOnly)
    attachUInt32Chunks(firstFromHostLeafBlockedCycle, "first_from_host_leaf_blocked_cycle", 2)
    attach(firstFromHostLeafBlockedMask, "first_from_host_leaf_blocked_mask", ReadOnly)
    attachUInt32Chunks(lastFromHostLeafBlockedCycle, "last_from_host_leaf_blocked_cycle", 2)
    attach(lastFromHostLeafBlockedMask, "last_from_host_leaf_blocked_mask", ReadOnly)
    attachUInt32Chunks(htntPayloadEnqCount64, "htnt_payload_enq_count64", 2)
    attachUInt32Chunks(htntEmptyEnqCount64, "htnt_empty_enq_count64", 2)
    attachUInt32Chunks(targetInPayloadFireCount64, "target_in_payload_fire_count64", 2)
    attachUInt32Chunks(targetInEmptyFireCount64, "target_in_empty_fire_count64", 2)
    attachUInt32Chunks(nthtPayloadEnqCount64, "ntht_payload_enq_count64", 2)
    attachUInt32Chunks(nthtPayloadDeqCount64, "ntht_payload_deq_count64", 2)
    attachUInt32Chunks(lastHtntEnqCycle, "last_htnt_enq_cycle", 2)
    attachUInt32Chunks(lastHtntEnqData, "last_htnt_enq_data", 2)
    attach(lastHtntEnqMeta, "last_htnt_enq_meta", ReadOnly)
    attachUInt32Chunks(lastHtntDeqCycle, "last_htnt_deq_cycle", 2)
    attachUInt32Chunks(lastHtntDeqData, "last_htnt_deq_data", 2)
    attach(lastHtntDeqMeta, "last_htnt_deq_meta", ReadOnly)
    attachUInt32Chunks(lastTargetInFireCycle, "last_target_in_fire_cycle", 2)
    attachUInt32Chunks(lastTargetInFireData, "last_target_in_fire_data", 2)
    attach(lastTargetInFireMeta, "last_target_in_fire_meta", ReadOnly)
    attachUInt32Chunks(lastTargetOutFireCycle, "last_target_out_fire_cycle", 2)
    attachUInt32Chunks(lastTargetOutFireData, "last_target_out_fire_data", 2)
    attach(lastTargetOutFireMeta, "last_target_out_fire_meta", ReadOnly)
    attachUInt32Chunks(lastNthtEnqCycle, "last_ntht_enq_cycle", 2)
    attachUInt32Chunks(lastNthtEnqData, "last_ntht_enq_data", 2)
    attach(lastNthtEnqMeta, "last_ntht_enq_meta", ReadOnly)
    attachUInt32Chunks(lastNthtDeqCycle, "last_ntht_deq_cycle", 2)
    attachUInt32Chunks(lastNthtDeqData, "last_ntht_deq_data", 2)
    attach(lastNthtDeqMeta, "last_ntht_deq_meta", ReadOnly)
    attachUInt32Chunks(firstHtntPayloadEnqCycle, "first_htnt_payload_enq_cycle", 2)
    attachUInt32Chunks(firstTargetInPayloadFireCycle, "first_target_in_payload_fire_cycle", 2)
    attachUInt32Chunks(firstTargetOutPayloadFireCycle, "first_target_out_payload_fire_cycle", 2)
    attachUInt32Chunks(firstNthtPayloadEnqCycle, "first_ntht_payload_enq_cycle", 2)
    attachUInt32Chunks(firstPcieOutFireCycle, "first_pcie_out_fire_cycle", 2)
    }
    genCRFile()

    override def genHeader(base: BigInt, memoryRegions: Map[String, BigInt], sb: StringBuilder): Unit = {
      genConstructor(
          base,
          sb,
          "simplenic_t",
          "simplenic",
          Seq(
            UInt32(toHostStreamIdx),
            UInt32(toHostCPUQueueDepth),
            UInt32(fromHostStreamIdx),
            UInt32(fromHostCPUQueueDepth),
          ),
          hasStreams = true
      )
    }
  }
}
