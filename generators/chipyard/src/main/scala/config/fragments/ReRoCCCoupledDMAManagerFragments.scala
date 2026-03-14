package chipyard.config

import org.chipsalliance.cde.config.{Config, Parameters}
import freechips.rocketchip.tile.{BuildRoCC, OpcodeSet}
import freechips.rocketchip.diplomacy.LazyModule

import gemmini.{CoupledDMAParams, SharedScratchpadConfig}

class WithReRoCCCoupledDMAManagers(
  numDMA: Int,
  gemminiIdBase: Int = 0,
  sharedScratchpadConfig: SharedScratchpadConfig = SharedScratchpadConfig()
) extends Config((site, here, up) => {
  case BuildRoCC =>
    require(numDMA >= 0, s"numDMA must be >= 0, got $numDMA")
    require(gemminiIdBase >= 0, s"gemminiIdBase must be >= 0, got $gemminiIdBase")
    up(BuildRoCC) ++ Seq.tabulate(numDMA) { i =>
      (p: Parameters) => {
        implicit val q = p
        LazyModule(new gemmini.GemminiCoupledDMA(
          OpcodeSet.custom2,
          CoupledDMAParams(
            gemmini_id = gemminiIdBase + i,
            shared_scratchpad_config = sharedScratchpadConfig
          )
        )(p))
      }
    }
})
