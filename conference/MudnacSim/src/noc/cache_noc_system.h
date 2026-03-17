#ifndef MUDNACSIM_CACHE_NOC_SYSTEM_H
#define MUDNACSIM_CACHE_NOC_SYSTEM_H


#include "base_noc_system.h"
#include "cache_system.h"


namespace mudnac {


    class BaseCacheNoCSystem : public BaseCacheSystem, public BaseNoCSystem {
    public:
        std::vector<NetworkInterface<Message>> memCtrlChSlaveNIs;
        std::vector<NetworkInterface<Message>> cacheBankMasterNIs;
        std::vector<NetworkInterface<Message>> cacheBankSlaveNIs;
        std::vector<NetworkInterface<Message>> accelMasterNIs;
        
        std::vector<Port> memCtrlChSlavePorts;
        std::vector<Port> cacheBankMasterPorts;
        std::vector<Port> cacheBankSlavePorts;
        std::vector<Port> accelMasterPorts;

        BaseCacheNoCSystem(SystemConfig &config,
                           const NetworkInterfaceConfig &noc0NiCfg,
                           const NetworkInterfaceConfig &noc1NiCfg,
                           bool shared)
                : BaseCacheSystem(config),
                  BaseNoCSystem(config, shared) {

            // Create network interfaces.
            memCtrlChSlaveNIs.reserve(config.memCtrlChannels);
            cacheBankMasterNIs.reserve(config.cacheBanks);
            cacheBankSlaveNIs.reserve(config.cacheBanks);
            accelMasterNIs.reserve(config.accelNum);

            memCtrlChSlavePorts.reserve(config.memCtrlChannels);
            cacheBankMasterPorts.reserve(config.cacheBanks);
            cacheBankSlavePorts.reserve(config.cacheBanks);
            accelMasterPorts.reserve(config.accelNum);

            for (uint64 i = 0; i < config.memCtrlChannels; i++)
                memCtrlChSlaveNIs.emplace_back(memCtrlChSlaveNodeIdOffset + i, noc0NiCfg, false, portId2NodeId);
            for (uint64 i = 0; i < config.cacheBanks; i++)
                cacheBankMasterNIs.emplace_back(cacheBankMasterNodeIdOffset + i, noc0NiCfg, true, portId2NodeId);
            const NetworkInterfaceConfig &niCfg = shared ? noc0NiCfg : noc1NiCfg;
            for (uint64 i = 0; i < config.cacheBanks; i++)
                cacheBankSlaveNIs.emplace_back(cacheBankSlaveNodeIdOffset + i, niCfg, false, portId2NodeId);
            for (uint64 i = 0; i < config.accelNum; i++)
                accelMasterNIs.emplace_back(accelMasterNodeIdOffset + i, niCfg, true, portId2NodeId);

            // Connect network interfaces with modules
            for (uint64 i = 0; i < config.memCtrlChannels; i++){
                memCtrlChSlavePorts[i].assign(memCtrlChSlaveNIs[i].input, memCtrlChSlaveNIs[i].output);
                memCtrl.assignPort(i, memCtrlChSlavePorts[i]);
            }
            for (uint64 i = 0; i < config.cacheBanks; i++){
                cacheBankMasterPorts[i].assign(cacheBankMasterNIs[i].input, cacheBankMasterNIs[i].output);
                cacheBankSlavePorts[i].assign(cacheBankSlaveNIs[i].input, cacheBankSlaveNIs[i].output);
                cache.assignPort(i, cacheBankMasterPorts[i], cacheBankSlavePorts[i]);
            }
            for (uint64 i = 0; i < config.accelNum; i++){
                accelMasterPorts[i].assign(accelMasterNIs[i].input, accelMasterNIs[i].output);
                accels[i].assignMasterPort(accelMasterPorts[i]);
            }
        }
    
        uint getAccDis(uint accIdSrc, uint accIdDst) = 0;
    };


    class CacheNoCSystemHomoTileMesh : public BaseCacheNoCSystem {
    protected:
        MeshNoC meshNoc0;
        MeshNoC meshNoc1;
        uint64 accelNumPerTile;
        uint64 H, W;

    public:
        CacheNoCSystemHomoTileMesh(SystemConfig &config,
                                   const RouterConfig &noc0RouterConfig,
                                   const RouterConfig &noc1RouterConfig,
                                   const NetworkInterfaceConfig &noc0NiConfig,
                                   const NetworkInterfaceConfig &noc1NiConfig,
                                   bool shared)
                : BaseCacheNoCSystem(config, noc0NiConfig, noc1NiConfig, shared) {

            // Compute the shape of mesh
            computeMeshShape(config);
            // printf("CacheNoCSystemHomoTileMesh: H=%lu, W=%lu, accelNumPerTile=%lu\n", H, W, accelNumPerTile);

            // Setup NoC (NoCs)
            if (shared) {
                RouterConfig noc0Cfg = noc0RouterConfig;
                noc0Cfg.portNum = 4 + 2 + accelNumPerTile;
                meshNoc0.setup(H, W, noc0Cfg);
                noc0 = &meshNoc0;
            } else {
                RouterConfig noc0Cfg = noc0RouterConfig;
                noc0Cfg.portNum = 4 + 1;
                meshNoc0.setup(H, W, noc0Cfg);
                noc0 = &meshNoc0;
                RouterConfig noc1Cfg = noc1RouterConfig;
                noc1Cfg.portNum = 4 + 1 + accelNumPerTile;
                meshNoc1.setup(H, W, noc1Cfg);
                noc1 = &meshNoc1;
            }

            // Add NoC (NoCs) to "tickModuleList"
            tickModuleList.push_back(noc0);
            if (not shared) {
                tickModuleList.push_back(noc1);
            }

            // Map modules to routers
            std::vector<std::array<uint64, 2>> map;  // portId --> [h, w]
            // Memory controller channel slave-ports
            // printf("Map memory channel slave-ports\n");
            getMemCtrlChannelMapping(config.memCtrlChannels, map);
            for (uint64 i = 0; i < map.size(); i++) {
                uint64 nodeId = memCtrlChSlaveNodeIdOffset + i;
                uint64 h = map[i][0];
                uint64 w = map[i][1];
                uint64 routerId = meshNoc0.getRouterId(h, w);
                uint64 portId = 4;
                meshNoc0.addNode(memCtrlChSlaveNIs[i], nodeId, h, w, portId);
                // printf("Map port %lu (nodeId=%lu) at router (%lu,%lu) (routerId=%lu, port=%lu)\n",
                    //    i, nodeId, h, w, routerId, portId);
            }
            // Cache
            getCacheBankMapping(config.cacheBanks, map);
            // Cache bank master-ports
            // printf("Map cache bank master-ports\n");
            for (uint64 i = 0; i < map.size(); i++) {
                uint64 nodeId = cacheBankMasterNodeIdOffset + i;
                uint64 h = map[i][0];
                uint64 w = map[i][1];
                uint64 routerId = meshNoc0.getRouterId(h, w);
                uint64 portId = 4;
                meshNoc0.addNode(cacheBankMasterNIs[i], nodeId, h, w, portId);
                // printf("Map port %lu (nodeId=%lu) at router (%lu,%lu) (routerId=%lu, port=%lu)\n",
                //        i, nodeId, h, w, routerId, portId);
            }
            // Cache bank slave-ports
            // printf("Map cache bank slave-ports\n");
            for (uint64 i = 0; i < map.size(); i++) {
                uint64 nodeId = cacheBankSlaveNodeIdOffset + i;
                uint64 h = map[i][0];
                uint64 w = map[i][1];
                uint64 routerId = shared ? meshNoc0.getRouterId(h, w) : meshNoc1.getRouterId(h, w);
                uint64 portId = shared ? 5 : 4;
                if (shared) {
                    meshNoc0.addNode(cacheBankSlaveNIs[i], nodeId, h, w, portId);
                } else {
                    meshNoc1.addNode(cacheBankSlaveNIs[i], nodeId, h, w, portId);
                }
                // printf("Map port %lu (nodeId=%lu) at router (%lu,%lu) (routerId=%lu, port=%lu)\n",
                //        i, nodeId, h, w, routerId, portId);
            }
            // Accels
            // printf("Map accel master-ports\n");
            getAccelMapping(config.accelNum, map);
            getAccelMapping(config.accelNum, accelMapping);
            for (uint64 i = 0; i < map.size(); i++) {
                uint64 nodeId = accelMasterNodeIdOffset + i;
                uint64 h = map[i][0];
                uint64 w = map[i][1];
                uint64 routerId = shared ? meshNoc0.getRouterId(h, w) : meshNoc1.getRouterId(h, w);
                uint64 portId = shared ? (6 + (i % accelNumPerTile)) : (5 + (i % accelNumPerTile));
                if (shared) {
                    meshNoc0.addNode(accelMasterNIs[i], nodeId, h, w, portId);
                } else {
                    meshNoc1.addNode(accelMasterNIs[i], nodeId, h, w, portId);
                }
                // printf("Map port %lu (nodeId=%lu) at router (%lu,%lu) (routerId=%lu, port=%lu)\n",
                //        i, nodeId, h, w, routerId, portId);
            }

            // Create direct transfer connections between banks (slave-ports) and accels
            for (uint64 accId = 0; accId < config.accelNum; accId++) {
                uint64 accNodeId = accelMasterNodeIdOffset + accId;
                for (uint64 bankId = 0; bankId < config.cacheBanks; bankId++) {
                    uint64 bankNodeId = cacheBankSlaveNodeIdOffset + bankId;
                    auto &noc = shared ? noc0 : noc1;
                    if (noc->nodeIdToRouterId[accNodeId] == noc->nodeIdToRouterId[bankNodeId]) {
                        // printf("Create direct transfer connection: Acc %lu (Node %lu) <--> Bank %lu (Node %lu)\n",
                            //    accId, accNodeId, bankId, bankNodeId);
                        accelMasterNIs[accId].createDirectTransferConnection(cacheBankSlaveNIs[bankId]);
                    }
                }
            }

            // Generate NoC
            meshNoc0.generate();
            if (not shared) {
                meshNoc1.generate();
            }
        }

        uint getAccDis(uint accIdSrc, uint accIdDst) override {
            return std::abs(static_cast<int>(accelMapping[accIdSrc][0]) - static_cast<int>(accelMapping[accIdDst][0])) + 
                    std::abs(static_cast<int>(accelMapping[accIdSrc][1]) - static_cast<int>(accelMapping[accIdDst][1]));
        }

    private:
        void computeMeshShape(const SystemConfig &config) {
            assert(isPowOfTwo(config.cacheBanks));
            H = 1;
            W = 1;
            while (H * W < config.cacheBanks) {
                if (H == W) {
                    W *= 2;
                } else {
                    H *= 2;
                }
            }
            assert(H * W == config.cacheBanks);
            H += 2;
            assert(config.memCtrlChannels <= W * 2);  // do not support too many memory channels
            accelNumPerTile = std::ceil((double) config.accelNum / config.cacheBanks);
        }

        void getMemCtrlChannelMapping(uint64 channels, std::vector<std::array<uint64, 2>> &map) {
            map.resize(channels);
            for (int i = 0; i < channels; i++) {
                uint64 wOffset = i / 4;
                uint64 h = ((i % 4 == 0) or (i % 4 == 2)) ? 0 : (H - 1);
                uint64 w = ((i % 4 == 0) or (i % 4 == 3)) ? wOffset : (W - 1 - wOffset);
                map[i] = {h, w};
            }
        }

        void getCacheBankMapping(uint64 cacheBanks, std::vector<std::array<uint64, 2>> &map) {
            map.resize(cacheBanks);
            for (int i = 0; i < cacheBanks; i++) {
                uint64 hOffset = i / (W * 2);
                uint64 wOffset = (i % (W * 2)) / 4;
                uint64 h = ((i % 4 == 0) or (i % 4 == 2)) ? (1 + hOffset) : (H - 2 - hOffset);
                uint64 w = ((i % 4 == 0) or (i % 4 == 3)) ? wOffset : (W - 1 - wOffset);
                map[i] = {h, w};
            }
        }

        void getAccelMapping(uint64 numAccels, std::vector<std::array<uint64, 2>> &map) {
            map.resize(numAccels);
            for (int i = 0; i < numAccels; i++) {
                int j = i / accelNumPerTile;
                uint64 h = 1 + j / W;
                uint64 w = j % W;
                map[i] = {h, w};
            }
        }
        
        std::vector<std::array<uint64, 2>> accelMapping;
    };


}


#endif //MUDNACSIM_CACHE_NOC_SYSTEM_H
