package chipyard

import org.chipsalliance.cde.config.Config
import freechips.rocketchip.tile.OpcodeSet

import constellation.channel._
import constellation.routing._
import constellation.topology._

import scala.collection.immutable.ListMap

class GemminiLearningConfigSpadReRoCCNoCCoupledDMAParametric(
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
  useGlobalNoC: Boolean = false,
  useDeterministicGlobalNoCRouting: Boolean = false,
  useDummyGemmini: Boolean = false,
  filterDmaVisibleManagers: Boolean = false,
  connectSbusSlaveToStl: Boolean = false,
  sharedSpadBytes: Int = 1024 * 1024,
  useCompactPairedManagerLayout: Boolean = false,
  globalNoCVirtualChannelDepth: Int = 8
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
  require(sharedSpadBytes > 0, s"sharedSpadBytes must be > 0, got $sharedSpadBytes")
  require(globalNoCVirtualChannelDepth > 0, s"globalNoCVirtualChannelDepth must be > 0, got $globalNoCVirtualChannelDepth")

  val gemminiBeatBytes = sbusWidthBits / 8
  require((gemminiBeatBytes & (gemminiBeatBytes - 1)) == 0, s"sbusWidthBits/8 must be a power of 2 for shared scratchpad, got $gemminiBeatBytes")

  val totalManagers = numGemmini + numDMA

  val layout = GemminiLearningReRoCCCoupledDMAConfigHelpers.buildLayout(
    numCores = numCores,
    cpuX = cpuX,
    cpuY = cpuY,
    numGemmini = numGemmini,
    gemminiX = gemminiX,
    gemminiY = gemminiY,
    numDMA = numDMA,
    dmaX = dmaX,
    dmaY = dmaY,
    nMemoryChannels = nMemoryChannels,
    extraInfraEndpoints = 2,
    useCompactPairedManagerLayout = useCompactPairedManagerLayout
  )

  val nocCols = layout.nocCols
  val nocRows = layout.nocRows
  val cpuNodes = layout.cpuNodes
  val managerNodes = layout.managerNodes
  val gemminiNodes = managerNodes.take(numGemmini)
  val dmaNodes = managerNodes.drop(numGemmini)
  val infraNodes = layout.infraNodes
  val systemNodes = infraNodes.take(nMemoryChannels)
  val pbusNode = infraNodes(nMemoryChannels)
  val serialTlNode = infraNodes(nMemoryChannels + 1)
  val activeNodes = cpuNodes ++ managerNodes ++ systemNodes ++ Seq(pbusNode, serialTlNode)

  require(activeNodes.distinct.size == activeNodes.size, "NoC node assignments must be unique")
  require(activeNodes.forall(node => node >= 0 && node < nocCols * nocRows),
    s"NoC node assignments must fit within ${nocCols}x${nocRows} mesh")

  val sbusInNodeMapping = ListMap((
    cpuNodes.zipWithIndex.map { case (node, i) => s"Core $i " -> node } ++
      managerNodes.zipWithIndex.flatMap { case (node, i) =>
        Seq(
          s"ReRoCC $i DCache " -> node,
          s"port_named_rerocc_${i}[" -> node
        )
      } ++
      Seq("serial_tl" -> serialTlNode)
  ): _*)

  val sbusOutNodeMapping = ListMap((
    managerNodes.zipWithIndex.map { case (node, i) => s"sport_named_rerocc_${i}[" -> node } ++
      (if (connectSbusSlaveToStl) {
        managerNodes.zipWithIndex.map { case (node, i) =>
          s"sport_named_rerocc_sbus_${i}[" -> node
        }
      } else {
        Seq.empty
      }) ++
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

  val gemminiManagerConfig =
    if (useDummyGemmini) {
      new chipyard.config.WithReRoCCGemminiManagers(
        numGemmini = numGemmini,
        gemminiIdBase = 0
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
      new chipyard.config.WithReRoCCGemminiManagers(
        numGemmini = numGemmini,
        gemminiIdBase = 0
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
        connectSbusSlaveToStl = connectSbusSlaveToStl
      )
    )
    ++ new chipyard.config.WithReRoCCCoupledDMAManagers(
      numDMA = numDMA,
      gemminiIdBase = 0,
      sharedScratchpadConfig = sharedScratchpadConfig
    )
    ++ gemminiManagerConfig
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

class GemminiLearningConfigSpadReRoCCGlobalNoC2C1x2G2x1x2D2x1x2CoupledDMA
  extends GemminiLearningConfigSpadReRoCCNoCCoupledDMAParametric(
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
    useGlobalNoC = true,
    filterDmaVisibleManagers = true,
    connectSbusSlaveToStl = true
  )

class GemminiLearningConfigSpadReRoCCGlobalNoC4C2x2G4x2x2D4x2x2CoupledDMA
  extends GemminiLearningConfigSpadReRoCCNoCCoupledDMAParametric(
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
    nMemoryChannels = 2,
    freqMHz = 1000.0,
    meshRows = 8,
    meshColumns = 8,
    useGlobalNoC = true,
    useDeterministicGlobalNoCRouting = true,
    filterDmaVisibleManagers = true,
    connectSbusSlaveToStl = true,
    sharedSpadBytes = 256 * 1024
  )

class GemminiLearningConfigSpadReRoCCGlobalNoC2C1x2G2x1x2D2x1x2CoupledDMAFilterOnly
  extends GemminiLearningConfigSpadReRoCCNoCCoupledDMAParametric(
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
    useGlobalNoC = true,
    filterDmaVisibleManagers = true,
    connectSbusSlaveToStl = false
  )

class GemminiLearningConfigSpadReRoCCGlobalNoC2C1x2G2x1x2D2x1x2CoupledDMAConnectOnly
  extends GemminiLearningConfigSpadReRoCCNoCCoupledDMAParametric(
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
    useGlobalNoC = true,
    filterDmaVisibleManagers = false,
    connectSbusSlaveToStl = true
  )

class GemminiLearningConfigSpadReRoCCGlobalNoC6C3x2G16x4x4D16x4x4CoupledDMADummy32x32M8
  extends GemminiLearningConfigSpadReRoCCNoCCoupledDMAParametric(
    numCores = 6,
    cpuX = 3,
    cpuY = 2,
    numGemmini = 16,
    gemminiX = 4,
    gemminiY = 4,
    numDMA = 16,
    dmaX = 4,
    dmaY = 4,
    sbusWidthBits = 64 * 8,
    nMemoryChannels = 8,
    freqMHz = 1000.0,
    meshRows = 32,
    meshColumns = 32,
    useGlobalNoC = true,
    useDeterministicGlobalNoCRouting = true,
    useDummyGemmini = true,
    filterDmaVisibleManagers = true,
    connectSbusSlaveToStl = true
  )

class GemminiLearningConfigSpadReRoCCGlobalNoC6C3x2G4x2x2D4x2x2CoupledDMADummy4x4
  extends GemminiLearningConfigSpadReRoCCNoCCoupledDMAParametric(
    numCores = 6,
    cpuX = 3,
    cpuY = 2,
    numGemmini = 4,
    gemminiX = 2,
    gemminiY = 2,
    numDMA = 4,
    dmaX = 2,
    dmaY = 2,
    sbusWidthBits = 64 * 8,
    nMemoryChannels = 2,
    freqMHz = 1000.0,
    meshRows = 4,
    meshColumns = 4,
    useGlobalNoC = true,
    useDeterministicGlobalNoCRouting = true,
    useDummyGemmini = true,
    filterDmaVisibleManagers = true,
    connectSbusSlaveToStl = true,
    sharedSpadBytes = 1024 * 1024
  )

class GemminiLearningConfigSpadReRoCCGlobalNoC8C4x2G4x2x2D4x2x2CoupledDMADummy16x16
  extends GemminiLearningConfigSpadReRoCCNoCCoupledDMAParametric(
    numCores = 8,
    cpuX = 4,
    cpuY = 2,
    numGemmini = 4,
    gemminiX = 2,
    gemminiY = 2,
    numDMA = 4,
    dmaX = 2,
    dmaY = 2,
    sbusWidthBits = 64 * 8,
    nMemoryChannels = 2,
    freqMHz = 1000.0,
    meshRows = 16,
    meshColumns = 16,
    useGlobalNoC = true,
    useDeterministicGlobalNoCRouting = true,
    useDummyGemmini = true,
    filterDmaVisibleManagers = true,
    connectSbusSlaveToStl = true,
    sharedSpadBytes = 1024 * 1024
  )

class GemminiLearningConfigSpadReRoCCGlobalNoC8C4x2G16x4x4D16x4x4CoupledDMADummy16x16
  extends GemminiLearningConfigSpadReRoCCNoCCoupledDMAParametric(
    numCores = 8,
    cpuX = 4,
    cpuY = 2,
    numGemmini = 16,
    gemminiX = 4,
    gemminiY = 4,
    numDMA = 16,
    dmaX = 4,
    dmaY = 4,
    sbusWidthBits = 64 * 8,
    nMemoryChannels = 2,
    freqMHz = 1000.0,
    meshRows = 16,
    meshColumns = 16,
    useGlobalNoC = true,
    useDeterministicGlobalNoCRouting = true,
    useDummyGemmini = true,
    filterDmaVisibleManagers = true,
    connectSbusSlaveToStl = true,
    sharedSpadBytes = 1024 * 1024
  )

class GemminiLearningConfigSpadReRoCCGlobalNoC4C2x2G16x4x4D16x4x4CoupledDMADummy16x16Sbus128
  extends GemminiLearningConfigSpadReRoCCNoCCoupledDMAParametric(
    numCores = 4,
    cpuX = 2,
    cpuY = 2,
    numGemmini = 16,
    gemminiX = 4,
    gemminiY = 4,
    numDMA = 16,
    dmaX = 4,
    dmaY = 4,
    sbusWidthBits = 16 * 8,
    nMemoryChannels = 2,
    freqMHz = 1000.0,
    meshRows = 16,
    meshColumns = 16,
    useGlobalNoC = true,
    useDeterministicGlobalNoCRouting = true,
    useDummyGemmini = true,
    filterDmaVisibleManagers = true,
    connectSbusSlaveToStl = true,
    sharedSpadBytes = 1024 * 1024,
    useCompactPairedManagerLayout = true,
    globalNoCVirtualChannelDepth = 4
  )

class GemminiLearningConfigSpadReRoCCGlobalNoC8C4x2G12x4x3D12x4x3CoupledDMADummy16x16
  extends GemminiLearningConfigSpadReRoCCNoCCoupledDMAParametric(
    numCores = 8,
    cpuX = 4,
    cpuY = 2,
    numGemmini = 12,
    gemminiX = 4,
    gemminiY = 3,
    numDMA = 12,
    dmaX = 4,
    dmaY = 3,
    sbusWidthBits = 64 * 8,
    nMemoryChannels = 2,
    freqMHz = 1000.0,
    meshRows = 16,
    meshColumns = 16,
    useGlobalNoC = true,
    useDeterministicGlobalNoCRouting = true,
    useDummyGemmini = true,
    filterDmaVisibleManagers = true,
    connectSbusSlaveToStl = true,
    sharedSpadBytes = 1024 * 1024
  )

class GemminiLearningConfigSpadReRoCCGlobalNoC4C2x2G12x4x3D12x4x3CoupledDMADummy16x16
  extends GemminiLearningConfigSpadReRoCCNoCCoupledDMAParametric(
    numCores = 4,
    cpuX = 2,
    cpuY = 2,
    numGemmini = 12,
    gemminiX = 4,
    gemminiY = 3,
    numDMA = 12,
    dmaX = 4,
    dmaY = 3,
    sbusWidthBits = 64 * 8,
    nMemoryChannels = 2,
    freqMHz = 1000.0,
    meshRows = 16,
    meshColumns = 16,
    useGlobalNoC = true,
    useDeterministicGlobalNoCRouting = true,
    useDummyGemmini = true,
    filterDmaVisibleManagers = true,
    connectSbusSlaveToStl = true,
    sharedSpadBytes = 1024 * 1024
  )

class GemminiLearningConfigSpadReRoCCGlobalNoC4C2x2G12x4x3D12x4x3CoupledDMADummy16x16Sbus256
  extends GemminiLearningConfigSpadReRoCCNoCCoupledDMAParametric(
    numCores = 4,
    cpuX = 2,
    cpuY = 2,
    numGemmini = 12,
    gemminiX = 4,
    gemminiY = 3,
    numDMA = 12,
    dmaX = 4,
    dmaY = 3,
    sbusWidthBits = 32 * 8,
    nMemoryChannels = 2,
    freqMHz = 1000.0,
    meshRows = 16,
    meshColumns = 16,
    useGlobalNoC = true,
    useDeterministicGlobalNoCRouting = true,
    useDummyGemmini = true,
    filterDmaVisibleManagers = true,
    connectSbusSlaveToStl = true,
    sharedSpadBytes = 1024 * 1024,
    useCompactPairedManagerLayout = true,
    globalNoCVirtualChannelDepth = 4
  )

class GemminiLearningConfigSpadReRoCCGlobalNoC4C2x2G12x4x3D12x4x3CoupledDMADummy16x16Sbus128
  extends GemminiLearningConfigSpadReRoCCNoCCoupledDMAParametric(
    numCores = 4,
    cpuX = 2,
    cpuY = 2,
    numGemmini = 12,
    gemminiX = 4,
    gemminiY = 3,
    numDMA = 12,
    dmaX = 4,
    dmaY = 3,
    sbusWidthBits = 16 * 8,
    nMemoryChannels = 2,
    freqMHz = 1000.0,
    meshRows = 16,
    meshColumns = 16,
    useGlobalNoC = true,
    useDeterministicGlobalNoCRouting = true,
    useDummyGemmini = true,
    filterDmaVisibleManagers = true,
    connectSbusSlaveToStl = true,
    sharedSpadBytes = 1024 * 1024,
    useCompactPairedManagerLayout = true,
    globalNoCVirtualChannelDepth = 4
  )

class GemminiLearningConfigSpadReRoCCGlobalNoC4C2x2G12x4x3D12x4x3CoupledDMADummy16x16Sbus16
  extends GemminiLearningConfigSpadReRoCCNoCCoupledDMAParametric(
    numCores = 4,
    cpuX = 2,
    cpuY = 2,
    numGemmini = 12,
    gemminiX = 4,
    gemminiY = 3,
    numDMA = 12,
    dmaX = 4,
    dmaY = 3,
    sbusWidthBits = 2 * 8,
    nMemoryChannels = 2,
    freqMHz = 1000.0,
    meshRows = 16,
    meshColumns = 16,
    useGlobalNoC = true,
    useDeterministicGlobalNoCRouting = true,
    useDummyGemmini = true,
    filterDmaVisibleManagers = true,
    connectSbusSlaveToStl = true,
    sharedSpadBytes = 1024 * 1024,
    useCompactPairedManagerLayout = true,
    globalNoCVirtualChannelDepth = 4
  )

class GemminiLearningConfigSpadReRoCCGlobalNoC4C2x2G10x5x2D10x5x2CoupledDMADummy16x16
  extends GemminiLearningConfigSpadReRoCCNoCCoupledDMAParametric(
    numCores = 4,
    cpuX = 2,
    cpuY = 2,
    numGemmini = 10,
    gemminiX = 5,
    gemminiY = 2,
    numDMA = 10,
    dmaX = 5,
    dmaY = 2,
    sbusWidthBits = 64 * 8,
    nMemoryChannels = 2,
    freqMHz = 1000.0,
    meshRows = 16,
    meshColumns = 16,
    useGlobalNoC = true,
    useDeterministicGlobalNoCRouting = true,
    useDummyGemmini = true,
    filterDmaVisibleManagers = true,
    connectSbusSlaveToStl = true,
    sharedSpadBytes = 1024 * 1024
  )

class GemminiLearningConfigSpadReRoCCGlobalNoC4C2x2G10x5x2D10x5x2CoupledDMADummy16x16Sbus256
  extends GemminiLearningConfigSpadReRoCCNoCCoupledDMAParametric(
    numCores = 4,
    cpuX = 2,
    cpuY = 2,
    numGemmini = 10,
    gemminiX = 5,
    gemminiY = 2,
    numDMA = 10,
    dmaX = 5,
    dmaY = 2,
    sbusWidthBits = 32 * 8,
    nMemoryChannels = 2,
    freqMHz = 1000.0,
    meshRows = 16,
    meshColumns = 16,
    useGlobalNoC = true,
    useDeterministicGlobalNoCRouting = true,
    useDummyGemmini = true,
    filterDmaVisibleManagers = true,
    connectSbusSlaveToStl = true,
    sharedSpadBytes = 1024 * 1024
  )

class GemminiLearningConfigSpadReRoCCGlobalNoC2C2x1G10x5x2D10x5x2CoupledDMADummy16x16
  extends GemminiLearningConfigSpadReRoCCNoCCoupledDMAParametric(
    numCores = 2,
    cpuX = 2,
    cpuY = 1,
    numGemmini = 10,
    gemminiX = 5,
    gemminiY = 2,
    numDMA = 10,
    dmaX = 5,
    dmaY = 2,
    sbusWidthBits = 64 * 8,
    nMemoryChannels = 2,
    freqMHz = 1000.0,
    meshRows = 16,
    meshColumns = 16,
    useGlobalNoC = true,
    useDeterministicGlobalNoCRouting = true,
    useDummyGemmini = true,
    filterDmaVisibleManagers = true,
    connectSbusSlaveToStl = true,
    sharedSpadBytes = 1024 * 1024
  )

class GemminiLearningConfigSpadReRoCCGlobalNoC2C2x1G10x5x2D10x5x2CoupledDMADummy16x16Sbus256
  extends GemminiLearningConfigSpadReRoCCNoCCoupledDMAParametric(
    numCores = 2,
    cpuX = 2,
    cpuY = 1,
    numGemmini = 10,
    gemminiX = 5,
    gemminiY = 2,
    numDMA = 10,
    dmaX = 5,
    dmaY = 2,
    sbusWidthBits = 32 * 8,
    nMemoryChannels = 2,
    freqMHz = 1000.0,
    meshRows = 16,
    meshColumns = 16,
    useGlobalNoC = true,
    useDeterministicGlobalNoCRouting = true,
    useDummyGemmini = true,
    filterDmaVisibleManagers = true,
    connectSbusSlaveToStl = true,
    sharedSpadBytes = 1024 * 1024
  )

class GemminiLearningConfigSpadReRoCCGlobalNoC2C2x1G8x4x2D8x4x2CoupledDMADummy16x16
  extends GemminiLearningConfigSpadReRoCCNoCCoupledDMAParametric(
    numCores = 2,
    cpuX = 2,
    cpuY = 1,
    numGemmini = 8,
    gemminiX = 4,
    gemminiY = 2,
    numDMA = 8,
    dmaX = 4,
    dmaY = 2,
    sbusWidthBits = 64 * 8,
    nMemoryChannels = 2,
    freqMHz = 1000.0,
    meshRows = 16,
    meshColumns = 16,
    useGlobalNoC = true,
    useDeterministicGlobalNoCRouting = true,
    useDummyGemmini = true,
    filterDmaVisibleManagers = true,
    connectSbusSlaveToStl = true,
    sharedSpadBytes = 1024 * 1024
  )

class GemminiLearningConfigSpadReRoCCGlobalNoC2C2x1G8x4x2D8x4x2CoupledDMADummy16x16Sbus256
  extends GemminiLearningConfigSpadReRoCCNoCCoupledDMAParametric(
    numCores = 2,
    cpuX = 2,
    cpuY = 1,
    numGemmini = 8,
    gemminiX = 4,
    gemminiY = 2,
    numDMA = 8,
    dmaX = 4,
    dmaY = 2,
    sbusWidthBits = 32 * 8,
    nMemoryChannels = 2,
    freqMHz = 1000.0,
    meshRows = 16,
    meshColumns = 16,
    useGlobalNoC = true,
    useDeterministicGlobalNoCRouting = true,
    useDummyGemmini = true,
    filterDmaVisibleManagers = true,
    connectSbusSlaveToStl = true,
    sharedSpadBytes = 1024 * 1024
  )
