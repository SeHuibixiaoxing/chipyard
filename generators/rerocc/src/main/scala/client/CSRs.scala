package rerocc.client

import chisel3._
import chisel3.util._
import freechips.rocketchip.tile.{CustomCSR}


object ReRoCCCSRs {
  val MAX_CFGS = 32
  private val cfgSelectWidth = log2Ceil(MAX_CFGS)
  private val cfgCSRWidth = 9
  private val cfgsPerBank = 16
  private val cfgBankStride = 0x10
  private val cfgBaseAddr = 0x810

  val rropc0 = (0x800, cfgSelectWidth)
  val rropc1 = (0x801, cfgSelectWidth)
  val rropc2 = (0x802, cfgSelectWidth)
  val rropc3 = (0x803, cfgSelectWidth)
  val rrbar = (0x804, cfgSelectWidth)

  private def rrcfg(idx: Int): (Int, Int) = {
    require(idx >= 0 && idx < MAX_CFGS)
    val bank = idx / cfgsPerBank
    val offset = idx % cfgsPerBank
    (cfgBaseAddr + (bank * cfgBankStride) + offset, cfgCSRWidth)
  }

  def customCSRs(nCfgs: Int) = {
    require(nCfgs <= MAX_CFGS)
    (Seq(
    rropc0, rropc1, rropc2, rropc3, rrbar,
    ) ++ (0 until nCfgs).map(rrcfg)
    ).map { case (csr, sz) => CustomCSR(csr, (BigInt(1) << sz) - 1, Some(0)) }
  }
}

class ReRoCCCfg extends Bundle {
  val acq = Bool()
  val mgr = UInt(8.W)
}
