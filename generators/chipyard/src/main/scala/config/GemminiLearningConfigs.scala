package chipyard

import org.chipsalliance.cde.config.Config
import freechips.rocketchip.subsystem.{MBUS, SBUS}
import freechips.rocketchip.diplomacy.BufferParams

import constellation.channel._
import constellation.routing._
import constellation.router._
import constellation.topology._

import scala.collection.immutable.ListMap


class GemminiLearningConfigBasic extends Config(
  // Improve sim speed by removing TileLink monitors
  new freechips.rocketchip.subsystem.WithoutTLMonitors ++

  new gemmini.DefaultGemminiConfig(
    gemmini.GemminiConfigs.defaultConfig.copy(
      // meshRows = 16,
      meshRows = 64,
      // meshColumns = 16,
      meshColumns = 64,
      // dma_buswidth = 4 * 8,
      // dma_buswidth = 16 * 8,
      dma_buswidth = 64 * 8,
      shared_scratchpad_config = gemmini.SharedScratchpadConfig(
        // enable = false,
        enable = true,
        global_base_addr = BigInt("40000000", 16),
        local_size_bytes = 256 * 1024,
        local_banks = 1,
        // local_banks = 4,
        local_bank_interleaved_bytes = 64,
        // local_bank_interleaved_bytes = 64 * 1024,
        // local_bank_beat_bytes = 8,
        // local_bank_beat_bytes = 16,
        local_bank_beat_bytes = 64,
      )
    )
  ) ++

  new freechips.rocketchip.rocket.WithNBigCores(1) ++
  new chipyard.config.WithSystemBusWidth(16 * 8) ++
  new chipyard.config.AbstractConfig
)


class GemminiLearningConfigSpad extends Config(
  // Improve sim speed by removing TileLink monitors
  new freechips.rocketchip.subsystem.WithoutTLMonitors ++

  // Add a Scratchpad to system bus
  new testchipip.soc.WithScratchpad(
    busWhere = SBUS,
    base = 0x70000000L, // max 256MB
    size = 1 << 20,  // 1MB
    banks = 4,
  ) ++
  // Add a Scratchpad to memory bus
  new testchipip.soc.WithScratchpad(
    busWhere = MBUS,
    base = 0x60000000L, // max 256MB
    size = 1 << 20,  // 1MB
    banks = 4,
  ) ++
  // Remove the default Scratchpad in `AbstractConfig`
  new testchipip.soc.WithNoScratchpads() ++

  // Select a set of tileId/hardId and instantiate one gemmini to each of them.
  new chipyard.config.WithMultiRoCCGemmini(
    // Select a set of tileId.
    0, 1, 2, 3
  )(
    gemmini.GemminiConfigs.dummyConfig.copy(
    // gemmini.GemminiConfigs.defaultConfig.copy(
      // meshRows = 16,
      meshRows = 64,
      // meshColumns = 16,
      meshColumns = 64,
      // dma_buswidth = 4 * 8,
      // dma_buswidth = 8 * 8,
      // dma_buswidth = 16 * 8,
      // dma_buswidth = 32 * 8,
      dma_buswidth = 64 * 8,
      shared_scratchpad_config = gemmini.SharedScratchpadConfig(
        // enable = false,
        enable = true,
        global_base_addr = BigInt("40000000", 16), // max 512MB
        local_size_bytes = 1024 * 1024,
        local_banks = 1,
        // local_banks = 4,
        // local_bank_interleaved_bytes = 64,
        // local_bank_interleaved_bytes = 256 * 1024,
        // local_bank_beat_bytes = 16,
        local_bank_beat_bytes = 64,
      )
    )
  ) ++
  // Enable different RoCCs based on the tileId
  new chipyard.config.WithMultiRoCC ++

  // Set all major bus domains to 1 GHz
  new chipyard.config.WithInheritBusFrequencyAssignments ++
  new chipyard.config.WithUniformBusFrequencies(1000.0) ++

  // Set tile (chip core) frequency to 1 GHz
  new chipyard.config.WithTileFrequency(1000.0) ++

  // Set CPU cores
  new freechips.rocketchip.rocket.WithNBigCores(4) ++
  // This will set the banking factor of L2 cache.
  new freechips.rocketchip.subsystem.WithNBanks(4) ++
  // Set L2 cache.
  new freechips.rocketchip.subsystem.WithInclusiveCache(
    nWays = 16,
    capacityKB = 64,
  ) ++
  // Set the width of system bus
  new chipyard.config.WithSystemBusWidth(64 * 8) ++
  // Set number of memory channels.
  new freechips.rocketchip.subsystem.WithNMemoryChannels(4) ++
  new chipyard.config.AbstractConfig
)

class GemminiLearningConfigSpadWithDirectDMA extends Config(
  new chipyard.config.WithMultiRoCCDirectDMA(0, 1, 2, 3) ++
  new GemminiLearningConfigSpad
)

class GemminiLearningConfigSpadNoL2SmallL1 extends Config(
  // Shrink L1 caches (example: 8KB I$, 8KB D$ with 64B lines)
  new freechips.rocketchip.rocket.WithL1ICacheSets(64) ++
  new freechips.rocketchip.rocket.WithL1ICacheWays(2) ++
  new freechips.rocketchip.rocket.WithL1DCacheSets(64) ++
  new freechips.rocketchip.rocket.WithL1DCacheWays(2) ++

  // In no-L2/broadcast topology, MBUS may not exist; drop inherited MBUS scratchpad entries.
  new Config((site, here, up) => {
    case testchipip.soc.BankedScratchpadKey => up(testchipip.soc.BankedScratchpadKey).filter(_.busWhere != MBUS)
  }) ++

  // Disable LLC/L2-style inclusive cache and use broadcast coherence manager
  new freechips.rocketchip.subsystem.WithNBanks(0) ++
  new chipyard.config.WithBroadcastManager ++

  new GemminiLearningConfigSpad
)

class GemminiLearningConfigSpadNoL2SmallL1WithDirectDMA extends Config(
  new chipyard.config.WithMultiRoCCDirectDMA(0, 1, 2, 3) ++
  new GemminiLearningConfigSpadNoL2SmallL1
)

class GemminiLearningConfigSpadNoL2SmallL1WithDRAM extends Config(
  // Shrink L1 caches
  new freechips.rocketchip.rocket.WithL1ICacheSets(64) ++
  new freechips.rocketchip.rocket.WithL1ICacheWays(2) ++
  new freechips.rocketchip.rocket.WithL1DCacheSets(64) ++
  new freechips.rocketchip.rocket.WithL1DCacheWays(2) ++

  // Keep DRAM path (MBUS exists when nBanks > 0), but avoid LLC-style inclusive cache behavior.
  new freechips.rocketchip.subsystem.WithNBanks(1) ++
  new chipyard.config.WithBroadcastManager ++

  new GemminiLearningConfigSpad
)

class GemminiLearningConfigSpadNoL2SmallL1WithDRAMWithDirectDMA extends Config(
  new chipyard.config.WithMultiRoCCDirectDMA(0, 1, 2, 3) ++
  new GemminiLearningConfigSpadNoL2SmallL1WithDRAM
)



class GemminiLearningConfigSpadNoC extends Config (
  // Improve sim speed by removing TileLink monitors
  new freechips.rocketchip.subsystem.WithoutTLMonitors ++

  // System Bus NoC
  /* 0  - 1  - 2  - 3
   * 4  - 5  - 6  - 7
   * 8  - 9  - 10 - 11
   * 12 - 13 - 14 - 15
   */
  new constellation.soc.WithSbusNoC(
    constellation.protocol.SimpleTLNoCParams(
      constellation.protocol.DiplomaticNetworkNodeMapping(
        inNodeMapping = ListMap(
          "Core 0" -> 8, 
          "Core 1" -> 9,  
          "Core 2" -> 10, 
          "Core 3" -> 11,
          "Gemmini0" -> 8,
          "Gemmini1" -> 9,
          "Gemmini2" -> 10,
          "Gemmini3" -> 11,
          "[memloader][0]" -> 8,
          "[memwriter][0]" -> 8,
          "[memloader][1]" -> 9,
          "[memwriter][1]" -> 9,
          "[memloader][2]" -> 10,
          "[memwriter][2]" -> 10,
          "[memloader][3]" -> 11,
          "[memwriter][3]" -> 11,
          "serial_tl" -> 7
        ),
        outNodeMapping = ListMap(
          // Shared scratchpad in Gemmini
          "Gemmini0" -> 8,  
          "Gemmini1" -> 9,
          "Gemmini2" -> 10,
          "Gemmini3" -> 11,
          // Coherence manager bank (keep DRAM path with nBanks=1)
          "system[0]" -> 12,
          // Scratchpad banks on SBUS
          "ram[0]" -> 0,  
          "ram[1]" -> 1,
          "ram[2]" -> 2,
          "ram[3]" -> 3,
          "pbus" -> 7
        )
      ),
      constellation.noc.NoCParams(
        topology        = TerminalRouter(Mesh2D(4, 4)),
        channelParamGen = (a, b) => UserChannelParams(Seq.fill(5) { UserVirtualChannelParams(8) }),
        routingRelation = BlockingVirtualSubnetworksRouting(TerminalRouterRouting(Mesh2DEscapeRouting()), 5, 1)
      )
    )
  ) ++

  // Add a Scratchpad to system bus
  new testchipip.soc.WithScratchpad(
    busWhere = SBUS,
    base = 0x70000000L,
    size = 1 << 20,  // 1MB
    banks = 4,
  ) ++
  // Remove the default Scratchpad in `AbstractConfig`
  new testchipip.soc.WithNoScratchpads() ++

  // Select a set of tileId/hardId and instantiate one gemmini to each of them.
  // `gemmini_id`` is set inside `WithMultiRoCCGemmini`.
  new chipyard.config.WithMultiRoCCGemmini(
    // Select a set of tileId.
    0, 1, 2, 3
  )(
    gemmini.GemminiConfigs.dummyConfig.copy(
      meshRows = 64,
      meshColumns = 64,
      dma_buswidth = 64 * 8,
      shared_scratchpad_config = gemmini.SharedScratchpadConfig(
        enable = true,
        global_base_addr = BigInt("40000000", 16),
        local_size_bytes = 1024 * 1024,
        local_banks = 1,
        local_bank_beat_bytes = 64,
      )
    )
  ) ++

  // Add DirectDMA RoCC on selected tiles
  new chipyard.config.WithMultiRoCCDirectDMA(0, 1, 2, 3) ++

  // Enable different RoCCs based on the tileId
  new chipyard.config.WithMultiRoCC ++

  // Set all major bus domains to 1 GHz
  new chipyard.config.WithInheritBusFrequencyAssignments ++
  new chipyard.config.WithUniformBusFrequencies(1000.0) ++

  // Set tile (chip core) frequency to 1 GHz
  new chipyard.config.WithTileFrequency(1000.0) ++

  // Shrink L1 caches
  new freechips.rocketchip.rocket.WithL1ICacheSets(64) ++
  new freechips.rocketchip.rocket.WithL1ICacheWays(2) ++
  new freechips.rocketchip.rocket.WithL1DCacheSets(64) ++
  new freechips.rocketchip.rocket.WithL1DCacheWays(2) ++

  // Set CPU cores
  new freechips.rocketchip.rocket.WithNBigCores(4) ++

  // Keep DRAM path (MBUS exists when nBanks > 0), but avoid LLC-style inclusive cache behavior.
  new freechips.rocketchip.subsystem.WithNBanks(1) ++
  new chipyard.config.WithBroadcastManager ++

  // Set the width of system bus
  new chipyard.config.WithSystemBusWidth(64 * 8) ++
  // Set number of memory channels.
  new freechips.rocketchip.subsystem.WithNMemoryChannels(4) ++
  new chipyard.config.AbstractConfig
)

class GemminiLearningConfigSpadNoCNewDMA extends Config (
  // Improve sim speed by removing TileLink monitors
  new freechips.rocketchip.subsystem.WithoutTLMonitors ++

  // System Bus NoC
  /* 0  - 1  - 2  - 3
   * 4  - 5  - 6  - 7
   * 8  - 9  - 10 - 11
   * 12 - 13 - 14 - 15
   */
  new constellation.soc.WithSbusNoC(
    constellation.protocol.SimpleTLNoCParams(
      constellation.protocol.DiplomaticNetworkNodeMapping(
        inNodeMapping = ListMap(
          "Core 0" -> 8,
          "Core 1" -> 9,
          "Core 2" -> 10,
          "Core 3" -> 11,
          "Gemmini0" -> 8,
          "Gemmini1" -> 9,
          "Gemmini2" -> 10,
          "Gemmini3" -> 11,
          "[memloader][0]" -> 8,
          "[memwriter][0]" -> 8,
          "[memloader][1]" -> 9,
          "[memwriter][1]" -> 9,
          "[memloader][2]" -> 10,
          "[memwriter][2]" -> 10,
          "[memloader][3]" -> 11,
          "[memwriter][3]" -> 11,
          "serial_tl" -> 7
        ),
        outNodeMapping = ListMap(
          "Gemmini0" -> 8,
          "Gemmini1" -> 9,
          "Gemmini2" -> 10,
          "Gemmini3" -> 11,
          "system[0]" -> 12,
          "system[1]" -> 13,
          "system[2]" -> 14,
          "system[3]" -> 15,
          "ram[0]" -> 0,
          "ram[1]" -> 1,
          "ram[2]" -> 2,
          "ram[3]" -> 3,
          "pbus" -> 7
        )
      ),
      constellation.noc.NoCParams(
        topology        = TerminalRouter(Mesh2D(4, 4)),
        channelParamGen = (a, b) => UserChannelParams(Seq.fill(5) { UserVirtualChannelParams(8) }),
        routingRelation = BlockingVirtualSubnetworksRouting(TerminalRouterRouting(Mesh2DEscapeRouting()), 5, 1)
      )
    )
  ) ++

  new testchipip.soc.WithScratchpad(
    busWhere = SBUS,
    base = 0x70000000L,
    size = 1 << 20,
    banks = 4,
  ) ++
  new testchipip.soc.WithNoScratchpads() ++

  new chipyard.config.WithMultiRoCCGemmini(
    0, 1, 2, 3
  )(
    gemmini.GemminiConfigs.dummyConfig.copy(
      meshRows = 64,
      meshColumns = 64,
      dma_buswidth = 64 * 8,
      shared_scratchpad_config = gemmini.SharedScratchpadConfig(
        enable = true,
        global_base_addr = BigInt("40000000", 16),
        local_size_bytes = 1024 * 1024,
        local_banks = 1,
        local_bank_beat_bytes = 64,
      )
    )
  ) ++

  // Use the new DMA module
  new chipyard.config.WithMultiRoCCNewDirectDMA(0, 1, 2, 3) ++

  new chipyard.config.WithMultiRoCC ++
  new chipyard.config.WithInheritBusFrequencyAssignments ++
  new chipyard.config.WithUniformBusFrequencies(1000.0) ++
  new chipyard.config.WithTileFrequency(1000.0) ++
  new freechips.rocketchip.rocket.WithL1ICacheSets(64) ++
  new freechips.rocketchip.rocket.WithL1ICacheWays(2) ++
  new freechips.rocketchip.rocket.WithL1DCacheSets(64) ++
  new freechips.rocketchip.rocket.WithL1DCacheWays(2) ++
  new freechips.rocketchip.rocket.WithNBigCores(4) ++
  new freechips.rocketchip.subsystem.WithNBanks(4) ++
  new chipyard.config.WithBroadcastManager ++
  new chipyard.config.WithSystemBusWidth(64 * 8) ++
  new freechips.rocketchip.subsystem.WithNMemoryChannels(4) ++
  new chipyard.config.AbstractConfig
)