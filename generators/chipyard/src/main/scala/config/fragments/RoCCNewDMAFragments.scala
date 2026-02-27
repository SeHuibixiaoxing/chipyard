package chipyard.config

import org.chipsalliance.cde.config.{Config, Parameters}
import freechips.rocketchip.tile.{BuildRoCC, OpcodeSet}
import freechips.rocketchip.diplomacy.LazyModule
import scala.annotation.nowarn

class WithMultiRoCCNewDirectDMA(harts: Int*) extends Config((site, here, up) => {
  case MultiRoCCKey => up(MultiRoCCKey) ++ harts.distinct.map { i =>
    (i -> (up(MultiRoCCKey).getOrElse(i, Nil) :+ ((p: Parameters) => {
      implicit val q = p
      @nowarn("cat=deprecation")
      val dma = LazyModule(new gemmini.GemminiDirectDMANew(OpcodeSet.custom2)(p))
      dma
    })))
  }
})
