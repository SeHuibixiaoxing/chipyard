#ifndef MUDNACSIM_BASE_NOC_SYSTEM_H
#define MUDNACSIM_BASE_NOC_SYSTEM_H


#include "noc.h"
#include "system_config.h"


namespace mudnac {


    class BaseNoCSystem {
    public:
        NoC *noc0 = NULL;
        NoC *noc1 = NULL;
        bool shared;
        uint64 numNodes;

        // Node ID offsets
        uint64 memCtrlChSlaveNodeIdOffset;
        uint64 cacheBankMasterNodeIdOffset;
        uint64 cacheBankSlaveNodeIdOffset;
        uint64 accelMasterNodeIdOffset;

        // Mapping between portId and nodeId
        std::vector<uint64> portId2NodeId;
        
        BaseNoCSystem(const SystemConfig &config, bool shared_) {
            shared = shared_;
            numNodes = config.memCtrlChannels + config.cacheBanks * 2 + config.accelNum;

            // Create node IDs
            if (shared) {
                memCtrlChSlaveNodeIdOffset = 0;
                cacheBankMasterNodeIdOffset = config.memCtrlChannels;
                cacheBankSlaveNodeIdOffset = config.memCtrlChannels + config.cacheBanks;
                accelMasterNodeIdOffset = config.memCtrlChannels + config.cacheBanks * 2;
            } else {
                memCtrlChSlaveNodeIdOffset = 0;
                cacheBankMasterNodeIdOffset = config.memCtrlChannels;
                cacheBankSlaveNodeIdOffset = 0;
                accelMasterNodeIdOffset = config.cacheBanks;
            }

            printf("numNodes=%lu, shared=%d, offsets=[%lu, %lu, %lu, %lu]\n",
                   numNodes, shared,
                   memCtrlChSlaveNodeIdOffset, cacheBankMasterNodeIdOffset,
                   cacheBankSlaveNodeIdOffset, accelMasterNodeIdOffset);

            portId2NodeId.reserve(PortIDManager::getMaxPortID() + 1);

            // Create mapping between portId and nodeId
            for (uint64 i = 0; i < config.memCtrlChannels; i++) {
                portId2NodeId[PortIDManager::localToGlobalMEMSlave(i)] = memCtrlChSlaveNodeIdOffset + i;
                printf("Map Memory Channel slave port id %#lx(global_id %#x) to node id %#lx\n", i, PortIDManager::localToGlobalMEMSlave(i), memCtrlChSlaveNodeIdOffset + i);
            }
            for (uint64 i = 0; i < config.cacheBanks; i++) {
                portId2NodeId[PortIDManager::localToGlobalCacheMaster(i)] = cacheBankMasterNodeIdOffset + i;
            }
            for (uint64 i = 0; i < config.cacheBanks; i++) {
                portId2NodeId[PortIDManager::localToGlobalCacheSlave(i)] = cacheBankSlaveNodeIdOffset + i;
            }
            for (uint64 i = 0; i < config.cacheBanks; i++) {
                portId2NodeId[PortIDManager::localToGlobalSPMMaster(i)] = cacheBankMasterNodeIdOffset + i;
                // printf("Map SPM master port id %#lx(global_id %#x) to node id %#lx\n", i, PortIDManager::localToGlobalSPMMaster(i), cacheBankMasterNodeIdOffset + i);
            }
            for (uint64 i = 0; i < config.cacheBanks; i++) {
                portId2NodeId[PortIDManager::localToGlobalSPMSlave(i)] = cacheBankSlaveNodeIdOffset + i;
                // printf("Map SPM slave port id %#lx(global_id %#x) to node id %#lx\n", i, PortIDManager::localToGlobalSPMSlave(i), cacheBankSlaveNodeIdOffset + i);
            }
            for (uint64 i = 0; i < config.accelNum; i++) {
                portId2NodeId[PortIDManager::localToGlobalAccelMaster(i)] = accelMasterNodeIdOffset + i;
                // printf("Map SPM master port id %#lx(global_id %#x) to node id %#lx\n", i, PortIDManager::localToGlobalAccelMaster(i), accelMasterNodeIdOffset + i);
            }
        }

        std::string getPerformanceString() {
            std::stringstream ss;

            {
                ss << "NoC 0\n\n";
                NetworkInterfaceCounter sumRecvCounter;
                for (auto ni: noc0->nodes) {
                    sumRecvCounter.merge(ni->recvCounter);
                }
                ss << "Summary\n";
                ss << sumRecvCounter.toString();
                for (uint nodeId = 0; nodeId < noc0->nodes.size(); nodeId++) {
                    BaseNetworkInterface *ni = noc0->nodes[nodeId];
                    ss << "Interface " << nodeId << "\n";
                    ss << "  SEND:\n";
                    ss << ni->sendCounter.toString();
                    ss << "  RECV:\n";
                    ss << ni->recvCounter.toString();
                }
            }

            if (not shared) {
                ss << "\nNoC 1\n\n";
                NetworkInterfaceCounter sumRecvCounter;
                for (auto ni : noc1->nodes) {
                    sumRecvCounter.merge(ni->recvCounter);
                }
                ss << "Summary\n";
                ss << sumRecvCounter.toString();
                for (uint nodeId = 0; nodeId < noc1->nodes.size(); nodeId++) {
                    BaseNetworkInterface *ni = noc1->nodes[nodeId];
                    ss << "Interface " << nodeId << "\n";
                    ss << "  SEND:\n";
                    ss << ni->sendCounter.toString();
                    ss << "  RECV:\n";
                    ss << ni->recvCounter.toString();
                }
            }

            return ss.str();
        }
    };


}


#endif //MUDNACSIM_BASE_NOC_SYSTEM_H
