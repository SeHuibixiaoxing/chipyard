package chipyard

import org.chipsalliance.cde.config.Config
import freechips.rocketchip.tile.OpcodeSet

import constellation.channel._
import constellation.routing._
import constellation.topology._

import scala.collection.immutable.ListMap

class GemminiLearningConfigSpadReRoCCNoCPairManagerParametric(
  numCores: Int,
  cpuX: Int,
  cpuY: Int,
  numPairs: Int,
  pairX: Int,
  pairY: Int,
  sbusWidthBits: Int,
  nMemoryChannels: Int,
  freqMHz: Double,
  meshRows: Int = 8,
  meshColumns: Int = 8,
  useGlobalNoC: Boolean = false,
  useDeterministicGlobalNoCRouting: Boolean = false,
  useDummyGemmini: Boolean = false,
  filterDmaVisibleManagers: Boolean = false,
  connectSbusSlaveToStl: Boolean = false,
  sharedSpadBytes: Int = 1024 * 1024,
  useCompactPairManagerLayout: Boolean = false,
  globalNoCVirtualChannelDepth: Int = 8
) extends Config({
  require(numCores > 0, s"numCores must be > 0, got $numCores")
  require(cpuX > 0 && cpuY > 0, s"cpuX and cpuY must be > 0, got cpuX=$cpuX cpuY=$cpuY")
  require(numCores <= cpuX * cpuY, s"numCores must be <= cpuX*cpuY, got numCores=$numCores cpuX*cpuY=${cpuX * cpuY}")

  require(numPairs >= 0, s"numPairs must be >= 0, got $numPairs")
  require(pairX > 0 && pairY > 0, s"pairX and pairY must be > 0, got pairX=$pairX pairY=$pairY")
  require(numPairs <= pairX * pairY, s"numPairs must be <= pairX*pairY, got numPairs=$numPairs pairX*pairY=${pairX * pairY}")

  require(sbusWidthBits > 0 && sbusWidthBits % 8 == 0, s"sbusWidthBits must be a positive multiple of 8, got $sbusWidthBits")
  require(nMemoryChannels > 0, s"nMemoryChannels must be > 0, got $nMemoryChannels")
  require(freqMHz > 0.0, s"freqMHz must be > 0, got $freqMHz")
  require(sharedSpadBytes > 0, s"sharedSpadBytes must be > 0, got $sharedSpadBytes")
  require(globalNoCVirtualChannelDepth > 0, s"globalNoCVirtualChannelDepth must be > 0, got $globalNoCVirtualChannelDepth")

  val gemminiBeatBytes = sbusWidthBits / 8
  require((gemminiBeatBytes & (gemminiBeatBytes - 1)) == 0,
    s"sbusWidthBits/8 must be a power of 2 for shared scratchpad, got $gemminiBeatBytes")

  val layout = GemminiLearningReRoCCCoupledDMAConfigHelpers.buildPairLayout(
    numCores = numCores,
    cpuX = cpuX,
    cpuY = cpuY,
    numPairs = numPairs,
    pairX = pairX,
    pairY = pairY,
    nMemoryChannels = nMemoryChannels,
    extraInfraEndpoints = 2,
    useCompactPairManagerLayout = useCompactPairManagerLayout
  )

  val nocCols = layout.nocCols
  val nocRows = layout.nocRows
  val cpuNodes = layout.cpuNodes
  val pairNodes = layout.managerNodes
  val infraNodes = layout.infraNodes
  val systemNodes = infraNodes.take(nMemoryChannels)
  val pbusNode = infraNodes(nMemoryChannels)
  val serialTlNode = infraNodes(nMemoryChannels + 1)
  val activeNodes = cpuNodes ++ pairNodes ++ systemNodes ++ Seq(pbusNode, serialTlNode)

  require(activeNodes.distinct.size == activeNodes.size, "NoC node assignments must be unique")
  require(activeNodes.forall(node => node >= 0 && node < nocCols * nocRows),
    s"NoC node assignments must fit within ${nocCols}x${nocRows} mesh")

  val sbusInNodeMapping = ListMap((
    cpuNodes.zipWithIndex.map { case (node, i) => s"Core $i " -> node } ++
      pairNodes.zipWithIndex.flatMap { case (node, i) =>
        Seq(
          s"ReRoCC $i DCache " -> node,
          s"port_named_rerocc_$i[" -> node
        )
      } ++
      Seq("serial_tl" -> serialTlNode)
  ): _*)

  val sbusOutNodeMapping = ListMap((
    pairNodes.zipWithIndex.map { case (node, i) => s"sport_named_rerocc_${i}[" -> node } ++
      (if (connectSbusSlaveToStl) {
        pairNodes.zipWithIndex.map { case (node, i) => s"sport_named_rerocc_sbus_${i}[" -> node }
      } else {
        Seq.empty
      }) ++
      pairNodes.zipWithIndex.map { case (node, i) => s"Gemmini${i}-" -> node } ++
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

  val reroccManagerMapping = ListMap((0 until numPairs).map { i =>
    i -> pairNodes(i)
  }: _*)

  val sbusNoCParams = constellation.noc.NoCParams(
    topology = TerminalRouter(Mesh2D(nocCols, nocRows)),
    channelParamGen = (a, b) => UserChannelParams(Seq.fill(5) { UserVirtualChannelParams(8) }),
    routingRelation = BlockingVirtualSubnetworksRouting(TerminalRouterRouting(Mesh2DEscapeRouting()), 5, 1)
  )

  val globalNoCRoutingRelation =
    if (useDeterministicGlobalNoCRouting) {
      TerminalRouterRouting(Mesh2DDimensionOrderedRouting())
    } else {
      BlockingVirtualSubnetworksRouting(
        TerminalRouterRouting(Mesh2DEscapeRouting()), 7, 1
      )
    }

  val globalNoCParams = constellation.noc.NoCParams(
    topology = TerminalRouter(Mesh2D(nocCols, nocRows)),
    channelParamGen = (a, b) => UserChannelParams(Seq.fill(7) { UserVirtualChannelParams(globalNoCVirtualChannelDepth) }),
    routingRelation = globalNoCRoutingRelation,
    skipValidationChecks = true
  )

  val reroccNoCParams = rerocc.bus.ReRoCCNoCParams(
    tileClientMapping = reroccTileClientMapping,
    managerMapping = reroccManagerMapping,
    nocParams = constellation.noc.NoCParams(
      topology = TerminalRouter(Mesh2D(nocCols, nocRows)),
      channelParamGen = (a, b) => UserChannelParams(Seq.fill(2) { UserVirtualChannelParams(8) }),
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

  val sharedScratchpadConfig = gemmini.SharedScratchpadConfig(
    enable = true,
    global_base_addr = BigInt("40000000", 16),
    local_size_bytes = sharedSpadBytes,
    local_banks = 1,
    local_bank_interleaved_bytes = gemminiBeatBytes.max(64),
    local_bank_beat_bytes = gemminiBeatBytes,
    use_page_table_xlate = true,
    share_xlate_with_coupled_dma = true
  )

  val pairManagerConfig =
    if (useDummyGemmini) {
      new chipyard.config.WithReRoCCGemminiCoupledDMAPairManagers(
        numPairs = numPairs,
        gemminiIdBase = 0,
        sharedScratchpadConfig = sharedScratchpadConfig
      )(
        gemmini.GemminiConfigs.dummyConfig.copy(
          opcodes = OpcodeSet.custom3,
          meshRows = meshRows,
          meshColumns = meshColumns,
          dma_buswidth = sbusWidthBits,
          shared_scratchpad_config = sharedScratchpadConfig
        )
      )
    } else {
      new chipyard.config.WithReRoCCGemminiCoupledDMAPairManagers(
        numPairs = numPairs,
        gemminiIdBase = 0,
        sharedScratchpadConfig = sharedScratchpadConfig
      )(
        gemmini.GemminiConfigs.defaultConfig.copy(
          opcodes = OpcodeSet.custom3,
          meshRows = meshRows,
          meshColumns = meshColumns,
          dma_buswidth = sbusWidthBits,
          shared_scratchpad_config = sharedScratchpadConfig
        )
      )
    }

  (new freechips.rocketchip.subsystem.WithoutTLMonitors
    ++ sbusAndGlobalNoCConfig
    ++ new rerocc.WithReRoCCNoC(reroccNoCParams)
    ++ new testchipip.soc.WithNoScratchpads()
    ++ new rerocc.WithReRoCC(
      reRoCCManagerParams = rerocc.manager.ReRoCCTileParams(
        filterDmaVisibleManagers = filterDmaVisibleManagers,
        connectSbusSlaveToStl = connectSbusSlaveToStl,
        preserveIncomingOpcode = true
      )
    )
    ++ pairManagerConfig
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

class GemminiLearningConfigSpadReRoCCGlobalNoC2C1x2P2x1x2CoupledDMAPairManager
  extends GemminiLearningConfigSpadReRoCCNoCPairManagerParametric(
    numCores = 2,
    cpuX = 1,
    cpuY = 2,
    numPairs = 2,
    pairX = 1,
    pairY = 2,
    sbusWidthBits = 64 * 8,
    nMemoryChannels = 2,
    freqMHz = 1000.0,
    meshRows = 8,
    meshColumns = 8,
    useGlobalNoC = true,
    useDeterministicGlobalNoCRouting = false,
    useDummyGemmini = false,
    filterDmaVisibleManagers = true,
    connectSbusSlaveToStl = true,
    sharedSpadBytes = 1024 * 1024,
    useCompactPairManagerLayout = false,
    globalNoCVirtualChannelDepth = 8
  )
