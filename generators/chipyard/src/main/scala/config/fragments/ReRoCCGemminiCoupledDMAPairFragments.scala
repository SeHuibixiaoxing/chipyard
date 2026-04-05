package chipyard.config

import chisel3._

import org.chipsalliance.cde.config.{Config, Parameters}
import freechips.rocketchip.diplomacy.LazyModule
import freechips.rocketchip.tile.BuildRoCC

import gemmini._

class WithReRoCCGemminiCoupledDMAPairManagers[T <: Data : Arithmetic, U <: Data, V <: Data](
  numPairs: Int,
  gemminiIdBase: Int = 0,
  sharedScratchpadConfig: SharedScratchpadConfig = SharedScratchpadConfig()
)(
  gemminiConfig: GemminiArrayConfig[T, U, V] = GemminiConfigs.defaultConfig
) extends Config((site, here, up) => {
  case BuildRoCC =>
    require(numPairs >= 0, s"numPairs must be >= 0, got $numPairs")
    require(gemminiIdBase >= 0, s"gemminiIdBase must be >= 0, got $gemminiIdBase")
    up(BuildRoCC) ++ Seq.tabulate(numPairs) { i =>
      (p: Parameters) => {
        implicit val q = p
        LazyModule(new GemminiCoupledDMAPairWrapper(
          gemminiConfig,
          gemminiIdBase + i,
          sharedScratchpadConfig
        ))
      }
    }
})
