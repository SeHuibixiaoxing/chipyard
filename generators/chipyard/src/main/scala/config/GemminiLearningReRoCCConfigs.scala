package chipyard

import org.chipsalliance.cde.config.Config
import freechips.rocketchip.tile.OpcodeSet

import constellation.channel._
import constellation.routing._
import constellation.topology._

import scala.collection.immutable.ListMap

class GemminiLearningConfigSpadReRoCCNoCParametric(
  numCores: Int,
  cpuX: Int,
  cpuY: Int,
  numGemmini: Int,
  gemminiX: Int,
  gemminiY: Int,
  numDMA: Int,
  dmaX: Int,
  dmaY: Int,
  sbusWidthBits: Int,
  nMemoryChannels: Int,
  freqMHz: Double,
  meshRows: Int = 8,
  meshColumns: Int = 8,
  useGlobalNoC: Boolean = false
) extends Config({
  require(numCores > 0, s"numCores must be > 0, got $numCores")
  require(cpuX > 0 && cpuY > 0, s"cpuX and cpuY must be > 0, got cpuX=$cpuX cpuY=$cpuY")
  require(numCores <= cpuX * cpuY, s"numCores must be <= cpuX*cpuY, got numCores=$numCores cpuX*cpuY=${cpuX * cpuY}")

  require(numGemmini >= 0, s"numGemmini must be >= 0, got $numGemmini")
  require(gemminiX > 0 && gemminiY > 0, s"gemminiX and gemminiY must be > 0, got gemminiX=$gemminiX gemminiY=$gemminiY")
  require(numGemmini <= gemminiX * gemminiY, s"numGemmini must be <= gemminiX*gemminiY, got numGemmini=$numGemmini gemminiX*gemminiY=${gemminiX * gemminiY}")

  require(numDMA >= 0, s"numDMA must be >= 0, got $numDMA")
  require(dmaX > 0 && dmaY > 0, s"dmaX and dmaY must be > 0, got dmaX=$dmaX dmaY=$dmaY")
  require(numDMA <= dmaX * dmaY, s"numDMA must be <= dmaX*dmaY, got numDMA=$numDMA dmaX*dmaY=${dmaX * dmaY}")

  require(sbusWidthBits > 0 && sbusWidthBits % 8 == 0, s"sbusWidthBits must be a positive multiple of 8, got $sbusWidthBits")
  require(nMemoryChannels > 0, s"nMemoryChannels must be > 0, got $nMemoryChannels")
  require(freqMHz > 0.0, s"freqMHz must be > 0, got $freqMHz")
  require(meshRows > 0 && meshColumns > 0, s"meshRows and meshColumns must be > 0, got meshRows=$meshRows meshColumns=$meshColumns")

  val gemminiBeatBytes = sbusWidthBits / 8
  require((gemminiBeatBytes & (gemminiBeatBytes - 1)) == 0, s"sbusWidthBits/8 must be a power of 2 for shared scratchpad, got $gemminiBeatBytes")

  val totalManagers = numGemmini + numDMA

  val nocCols = Seq(cpuX, gemminiX, dmaX, nMemoryChannels + 1).max
  val infraRows = (nMemoryChannels + 1 + nocCols - 1) / nocCols
  val cpuRowBase = 0
  val gemminiRowBase = cpuRowBase + cpuY
  val dmaRowBase = gemminiRowBase + gemminiY
  val infraRowBase = dmaRowBase + dmaY
  val nocRows = infraRowBase + infraRows

  val cpuNodes = (0 until numCores).map { i =>
    val row = cpuRowBase + (i / cpuX)
    val col = i % cpuX
    row * nocCols + col
  }

  val gemminiNodes = (0 until numGemmini).map { i =>
    val row = gemminiRowBase + (i / gemminiX)
    val col = i % gemminiX
    row * nocCols + col
  }

  val dmaNodes = (0 until numDMA).map { i =>
    val row = dmaRowBase + (i / dmaX)
    val col = i % dmaX
    row * nocCols + col
  }

  val managerNodes = gemminiNodes ++ dmaNodes

  val infraNodes = (0 until (infraRows * nocCols)).map { i =>
    val row = infraRowBase + (i / nocCols)
    val col = i % nocCols
    row * nocCols + col
  }
  val systemNodes = infraNodes.take(nMemoryChannels)
  val pbusNode = infraNodes(nMemoryChannels)

  val sbusInNodeMapping = ListMap((
    cpuNodes.zipWithIndex.map { case (node, i) => s"Core $i " -> node } ++
      managerNodes.zipWithIndex.flatMap { case (node, i) =>
        Seq(
          s"ReRoCC $i DCache " -> node,
          s"port_named_rerocc_${i}[" -> node
        )
      } ++
      Seq("serial_tl" -> pbusNode)
  ): _*)

  val sbusOutNodeMapping = ListMap((
    managerNodes.zipWithIndex.map { case (node, i) => s"sport_named_rerocc_${i}[" -> node } ++
      // Match shared-scratchpad TL slave banks exported by Gemmini managers.
      // Use "Gemmini<i>-" to keep matching unique for i vs i0-style IDs.
      gemminiNodes.zipWithIndex.map { case (node, i) => s"Gemmini${i}-" -> node } ++
      systemNodes.zipWithIndex.map { case (node, i) => s"system[$i]" -> node } ++
      Seq("pbus" -> pbusNode)
  ): _*)

  val sbusNodeMapping = constellation.protocol.DiplomaticNetworkNodeMapping(
    inNodeMapping = sbusInNodeMapping,
    outNodeMapping = sbusOutNodeMapping
  )

  val reroccTileClientMapping = ListMap((0 until numCores).map { i =>
    i -> cpuNodes(i)
  }: _*)

  val reroccManagerMapping = ListMap((0 until totalManagers).map { i =>
    i -> managerNodes(i)
  }: _*)

  val sbusNoCParams = constellation.noc.NoCParams(
    topology = TerminalRouter(Mesh2D(nocCols, nocRows)),
    channelParamGen = (a, b) => UserChannelParams(Seq.fill(5) { UserVirtualChannelParams(8) }),
    routingRelation = BlockingVirtualSubnetworksRouting(TerminalRouterRouting(Mesh2DEscapeRouting()), 5, 1)
  )

  // The shared global fabric carries both TL (5 vnets) and ReRoCC (2 vnets),
  // so provision >=7 virtual channels and keep one escape subnet for progress.
  val globalNoCParams = constellation.noc.NoCParams(
    topology = TerminalRouter(Mesh2D(nocCols, nocRows)),
    channelParamGen = (a, b) => UserChannelParams(Seq.fill(7) { UserVirtualChannelParams(8) }),
    routingRelation = BlockingVirtualSubnetworksRouting(
      TerminalRouterRouting(Mesh2DEscapeRouting()), 7, 1
    ),
    skipValidationChecks = true
  )

  val reroccNoCParams = rerocc.bus.ReRoCCNoCParams(
    tileClientMapping = reroccTileClientMapping,
    managerMapping = reroccManagerMapping,
    nocParams = constellation.noc.NoCParams(
      topology = TerminalRouter(Mesh2D(nocCols, nocRows)),
      channelParamGen = (a, b) => UserChannelParams(Seq.fill(2) { UserVirtualChannelParams(8) }),
      // ReRoCC responses are FIFO-sensitive; use plain deterministic DOR to
      // guarantee a single next hop for FIFO flows.
      routingRelation = TerminalRouterRouting(Mesh2DDimensionOrderedRouting())
    ),
    useGlobalNoC = useGlobalNoC
  )

  val sbusAndGlobalNoCConfig =
    if (useGlobalNoC) {
      new constellation.soc.WithGlobalNoC(
        constellation.soc.GlobalNoCParams(globalNoCParams)
      ) ++ new constellation.soc.WithSbusNoC(
        constellation.protocol.GlobalTLNoCParams(sbusNodeMapping)
      )
    } else {
      new constellation.soc.WithSbusNoC(
        constellation.protocol.SimpleTLNoCParams(
          sbusNodeMapping,
          sbusNoCParams
        )
      )
    }

  val managerGemminiConfig = gemmini.GemminiConfigs.defaultConfig.copy(
    opcodes = OpcodeSet.custom3,
    meshRows = meshRows,
    meshColumns = meshColumns,
    dma_buswidth = sbusWidthBits,
    shared_scratchpad_config = gemmini.SharedScratchpadConfig(
      enable = true,
      global_base_addr = BigInt("40000000", 16),
      local_size_bytes = 1024 * 1024,
      local_banks = 1,
      local_bank_interleaved_bytes = gemminiBeatBytes.max(64),
      local_bank_beat_bytes = gemminiBeatBytes
    )
  )

  (new freechips.rocketchip.subsystem.WithoutTLMonitors
    ++ sbusAndGlobalNoCConfig
    ++ new rerocc.WithReRoCCNoC(reroccNoCParams)
    ++ new testchipip.soc.WithNoScratchpads()
    ++ new rerocc.WithReRoCC()
    ++ new chipyard.config.WithReRoCCDirectDMAManagers(numDMA = numDMA)
    ++ new chipyard.config.WithReRoCCGemminiManagers(
      numGemmini = numGemmini,
      gemminiIdBase = 0
    )(managerGemminiConfig)
    ++ new chipyard.config.WithInheritBusFrequencyAssignments
    ++ new chipyard.config.WithUniformBusFrequencies(freqMHz)
    ++ new chipyard.config.WithTileFrequency(freqMHz)
    ++ new freechips.rocketchip.rocket.WithNBigCores(numCores)
    ++ new freechips.rocketchip.subsystem.WithNBanks(nMemoryChannels)
    ++ new chipyard.config.WithBroadcastManager
    ++ new chipyard.config.WithSystemBusWidth(sbusWidthBits)
    ++ new freechips.rocketchip.subsystem.WithNMemoryChannels(nMemoryChannels)
    ++ new chipyard.config.AbstractConfig)
})

class GemminiLearningConfigSpadReRoCCNoC4C2x2G4_2x2D4_2x2 extends GemminiLearningConfigSpadReRoCCNoCParametric(
  numCores = 4,
  cpuX = 2,
  cpuY = 2,
  numGemmini = 4,
  gemminiX = 2,
  gemminiY = 2,
  numDMA = 4,
  dmaX = 2,
  dmaY = 2,
  sbusWidthBits = 64 * 8,
  nMemoryChannels = 4,
  freqMHz = 1000.0,
  meshRows = 8,
  meshColumns = 8
)

class GemminiLearningConfigSpadReRoCCNoC4C2x2G4x2x2D4x2x2 extends GemminiLearningConfigSpadReRoCCNoCParametric(
  numCores = 4,
  cpuX = 2,
  cpuY = 2,
  numGemmini = 4,
  gemminiX = 2,
  gemminiY = 2,
  numDMA = 4,
  dmaX = 2,
  dmaY = 2,
  sbusWidthBits = 64 * 8,
  nMemoryChannels = 4,
  freqMHz = 1000.0,
  meshRows = 8,
  meshColumns = 8
)

class GemminiLearningConfigSpadReRoCCNoC2C1x2G2_1x2D2_1x2 extends GemminiLearningConfigSpadReRoCCNoCParametric(
  numCores = 2,
  cpuX = 1,
  cpuY = 2,
  numGemmini = 2,
  gemminiX = 1,
  gemminiY = 2,
  numDMA = 2,
  dmaX = 1,
  dmaY = 2,
  sbusWidthBits = 64 * 8,
  nMemoryChannels = 2,
  freqMHz = 1000.0,
  meshRows = 8,
  meshColumns = 8
)

class GemminiLearningConfigSpadReRoCCNoC2C1x2G2x1x2D2x1x2 extends GemminiLearningConfigSpadReRoCCNoCParametric(
  numCores = 2,
  cpuX = 1,
  cpuY = 2,
  numGemmini = 2,
  gemminiX = 1,
  gemminiY = 2,
  numDMA = 2,
  dmaX = 1,
  dmaY = 2,
  sbusWidthBits = 64 * 8,
  nMemoryChannels = 2,
  freqMHz = 1000.0,
  meshRows = 8,
  meshColumns = 8
)

class GemminiLearningConfigSpadReRoCCGlobalNoC4C2x2G4x2x2D4x2x2 extends GemminiLearningConfigSpadReRoCCNoCParametric(
  numCores = 4,
  cpuX = 2,
  cpuY = 2,
  numGemmini = 4,
  gemminiX = 2,
  gemminiY = 2,
  numDMA = 4,
  dmaX = 2,
  dmaY = 2,
  sbusWidthBits = 64 * 8,
  nMemoryChannels = 4,
  freqMHz = 1000.0,
  meshRows = 8,
  meshColumns = 8,
  useGlobalNoC = true
)

class GemminiLearningConfigSpadReRoCCGlobalNoC2C1x2G2x1x2D2x1x2 extends GemminiLearningConfigSpadReRoCCNoCParametric(
  numCores = 2,
  cpuX = 1,
  cpuY = 2,
  numGemmini = 2,
  gemminiX = 1,
  gemminiY = 2,
  numDMA = 2,
  dmaX = 1,
  dmaY = 2,
  sbusWidthBits = 64 * 8,
  nMemoryChannels = 2,
  freqMHz = 1000.0,
  meshRows = 8,
  meshColumns = 8,
  useGlobalNoC = true
)
