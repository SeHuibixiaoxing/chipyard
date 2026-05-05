// See LICENSE for license details

package firechip.bridgestubs

import chisel3._
import chisel3.util._

import org.chipsalliance.cde.config.{Parameters}

import firesim.lib.bridgeutils._

import firechip.bridgeinterfaces._

class NICBridgeHostIO(private val targetIO: NICBridgeTargetIO = new NICBridgeTargetIO)
    extends Bundle
    with ChannelizedHostPortIO {
  def targetClockRef = targetIO.clock

  val nicOut = InputChannel(targetIO.nic.out)
  val nicIn  = OutputChannel(targetIO.nic.in)
  val macAddr = OutputChannel(targetIO.nic.macAddr)
  val rlimit = OutputChannel(targetIO.nic.rlimit)
  val pauser = OutputChannel(targetIO.nic.pauser)
}

class NICBridge(implicit p: Parameters) extends BlackBox with Bridge[NICBridgeHostIO] {
  val moduleName = "firechip.goldengateimplementations.SimpleNICBridgeModule"
  val io = IO(new NICBridgeTargetIO)
  val bridgeIO = new NICBridgeHostIO(io)
  val constructorArg = None
  generateAnnotations()
}


object NICBridge {
  def apply(clock: Clock, nicIO: icenet.NICIOvonly)(implicit p: Parameters): NICBridge = {
    val ep = Module(new NICBridge)
    // TODO: Check following IOs are same size/names/etc
    ep.io.nic <> nicIO
    ep.io.clock := clock
    ep
  }
}
