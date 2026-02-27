package chipyard.config

import chisel3._

import org.chipsalliance.cde.config.{Field, Parameters, Config}
import freechips.rocketchip.tile._
import freechips.rocketchip.diplomacy._

import gemmini._

import chipyard.{TestSuitesKey, TestSuiteHelper}

/**
 * Map from a tileId to a particular RoCC accelerator
 */
case object MultiRoCCKey extends Field[Map[Int, Seq[Parameters => LazyRoCC]]](Map.empty[Int, Seq[Parameters => LazyRoCC]])

/**
 * Config fragment to enable different RoCCs based on the tileId
 */
class WithMultiRoCC extends Config((site, here, up) => {
  case BuildRoCC => site(MultiRoCCKey).getOrElse(site(TileKey).tileId, Nil)
})

/**
 * Assigns what was previously in the BuildRoCC key to specific harts with MultiRoCCKey
 * Must be paired with WithMultiRoCC
 */
class WithMultiRoCCFromBuildRoCC(harts: Int*) extends Config((site, here, up) => {
  case BuildRoCC => Nil
  case MultiRoCCKey => up(MultiRoCCKey, site) ++ harts.distinct.map { i =>
    (i -> up(BuildRoCC, site))
  }
})

/** Config fragment to add DirectDMA to MultiRoCCKey for specific harts */
class WithMultiRoCCDirectDMA(harts: Int*) extends Config((site, here, up) => {
  case MultiRoCCKey => up(MultiRoCCKey, site) ++ harts.distinct.map { i =>
    (i -> (up(MultiRoCCKey, site).getOrElse(i, Nil) :+ ((p: Parameters) => {
      implicit val q = p
      val dma = LazyModule(new gemmini.GemminiDirectDMA(OpcodeSet.custom2)(p))
      dma
    })))
  }
})

class WithMultiRoCCGemmini[T <: Data : Arithmetic, U <: Data, V <: Data](
  harts: Int*)(gemminiConfig: GemminiArrayConfig[T,U,V] = GemminiConfigs.defaultConfig) extends Config((site, here, up) => {
  case MultiRoCCKey => up(MultiRoCCKey, site) ++ harts.distinct.map { i =>
    val existingRoCCs = up(MultiRoCCKey, site).getOrElse(i, Nil)
    val newGemmini = (p: Parameters) => {
      implicit val q = p
      LazyModule(new Gemmini(gemminiConfig.copy(
          gemmini_id = i
      )))
    }
    (i -> (existingRoCCs :+ newGemmini))
  }
})

class WithAccumulatorRoCC(op: OpcodeSet = OpcodeSet.custom1) extends Config((site, here, up) => {
  case BuildRoCC => up(BuildRoCC) ++ Seq((p: Parameters) => {
    val accumulator = LazyModule(new AccumulatorExample(op, n = 4)(p))
    accumulator
  })
})

class WithCharacterCountRoCC(op: OpcodeSet = OpcodeSet.custom2) extends Config((site, here, up) => {
  case BuildRoCC => up(BuildRoCC) ++ Seq((p: Parameters) => {
    val counter = LazyModule(new CharacterCountExample(op)(p))
    counter
  })
})
