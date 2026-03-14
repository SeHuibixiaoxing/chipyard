package chipyard.config

import chisel3._

import org.chipsalliance.cde.config.{Config, Parameters}
import freechips.rocketchip.tile.{BuildRoCC, OpcodeSet}
import freechips.rocketchip.diplomacy.LazyModule

import gemmini._

class WithReRoCCGemminiManagers[T <: Data : Arithmetic, U <: Data, V <: Data](
  numGemmini: Int,
  gemminiIdBase: Int = 0)(
  gemminiConfig: GemminiArrayConfig[T, U, V] = GemminiConfigs.defaultConfig
) extends Config((site, here, up) => {
  case BuildRoCC =>
    require(numGemmini >= 0, s"numGemmini must be >= 0, got $numGemmini")
    require(gemminiIdBase >= 0, s"gemminiIdBase must be >= 0, got $gemminiIdBase")
    up(BuildRoCC) ++ Seq.tabulate(numGemmini) { i =>
      (p: Parameters) => {
        implicit val q = p
        LazyModule(new Gemmini(gemminiConfig.copy(
          opcodes = OpcodeSet.custom3,
          gemmini_id = gemminiIdBase + i,
        )))
      }
    }
})

class WithReRoCCDirectDMAManagers(numDMA: Int) extends Config((site, here, up) => {
  case BuildRoCC =>
    require(numDMA >= 0, s"numDMA must be >= 0, got $numDMA")
    up(BuildRoCC) ++ Seq.fill(numDMA) {
      (p: Parameters) => {
        implicit val q = p
        LazyModule(new gemmini.GemminiDirectDMA(OpcodeSet.custom2)(p))
      }
    }
})
