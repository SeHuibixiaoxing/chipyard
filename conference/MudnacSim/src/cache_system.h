#ifndef MUDNACSIM_CACHE_SYSTEM_H
#define MUDNACSIM_CACHE_SYSTEM_H


#include "base_system.h"
#include "cache_accel.h"
#include "xbar.h"
#include "system_config.h"


namespace mudnac {


    class BaseCacheSystem : public BaseSystem {
    public:
        Cache cache;
        std::vector<CacheAccel> accels;

        BaseCacheSystem(SystemConfig &config_)
                : BaseSystem(BaseSystem::TYPE_CACHE, config_),
                  cache(config_.getCacheConfig()) {
            CacheConfig cacheCfg = config_.getCacheConfig();
            std::vector<AccelConfig> accelCfgList = config_.getAccelConfigVector();

            // Check configurations
            for (int i = 0; i < accelCfgList.size(); i++) {
                assert(cacheCfg.upstreamBeatBytes == accelCfgList[i].beatBytes);
                assert(cacheCfg.lineBytes == accelCfgList[i].maxBurstBytes);
                assert(cacheCfg.nBanks == accelCfgList[i].nBanks);
            }

            // Create TickModule objects
            accels.reserve(accelCfgList.size());
            for (int accId = 0; accId < accelCfgList.size(); accId++) {
                AccelConfig cfg = accelCfgList[accId];
                cfg.accId = accId;
                accels.emplace_back(cfg);
                accels[accId].connectCPUPort(&cmdInput[accId], &cmdOutput[accId]);
            }

            // Add modules to "tickModuleList"
            tickModuleList.push_back(&cache);
            for (auto &acc: accels) { tickModuleList.push_back(&acc); }
        }

        std::string getPerformanceInfo() {
            std::stringstream ss;
            ss << "Total Cycles: " << cycles << "\n\n";
            ss << "Memory Controller\n";
            ss << memCtrl.getPerformanceInfo() << "\n";
            ss << "Cache\n";
            ss << cache.getPerformanceInfo() << "\n";
            for (uint i = 0; i < accels.size(); i++) {
                ss << "Accel " << i << "\n";
                ss << accels[i].counter.getPerformanceInfo();
            }
            ss << "\n";
            return ss.str();
        }

        uint getAccDis(uint accIdSrc, uint accIdDst) override {
            return 1;
        }
    };


    class CacheSystem : public BaseCacheSystem {
    private:
        XBar memCtrlXbar;
        XBar cacheXbar;

        std::vector<std::deque<Message *>> memCtrlSlavePortInput; // MemCtrlMessage
        std::vector<std::deque<Message *>> memCtrlSlavePortOutput; // MemCtrlMessage
        std::vector<std::deque<Message *>> cacheMasterPortInput; // MemCtrlMessage
        std::vector<std::deque<Message *>> cacheMasterPortOutput; // MemCtrlMessage
        std::vector<std::deque<Message *>> cacheSlavePortInput; // CacheMessage
        std::vector<std::deque<Message *>> cacheSlavePortOutput; // CacheMessage
        std::vector<std::deque<Message *>> accMasterPortInput; // CacheMessage
        std::vector<std::deque<Message *>> accMasterPortOutput; // CacheMessage

        std::vector<Port> memCtrlSlavePort;
        std::vector<Port> cacheMasterPort;
        std::vector<Port> cacheSlavePort;
        std::vector<Port> accMasterPort;

    public:
        CacheSystem(SystemConfig &config)
                : BaseCacheSystem(config),
                  memCtrlSlavePortInput(config.memCtrlChannels),
                  memCtrlSlavePortOutput(config.memCtrlChannels),
                  memCtrlSlavePort(config.memCtrlChannels),
                  cacheMasterPortInput(config.cacheBanks),
                  cacheMasterPortOutput(config.cacheBanks),
                  cacheMasterPort(config.cacheBanks),
                  cacheSlavePortInput(config.cacheBanks),
                  cacheSlavePortOutput(config.cacheBanks),
                  cacheSlavePort(config.cacheBanks),
                  accMasterPortInput(config.accelNum),
                  accMasterPortOutput(config.accelNum),
                  accMasterPort(config.accelNum) {

            // Add modules to "tickModuleList"
            tickModuleList.push_back(&cacheXbar);
            tickModuleList.push_back(&memCtrlXbar);

            // Connect modules to xbars
            for (int i = 0; i < config.memCtrlChannels; i++) {
                memCtrlSlavePort[i].assign(memCtrlSlavePortInput[i], memCtrlSlavePortOutput[i]);
                memCtrl.assignPort(i, memCtrlSlavePort[i]);

                memCtrlXbar.connectPort(PortIDManager::localToGlobalMEMSlave(i), memCtrlSlavePort[i]);                
            }
            for (uint bankIdx = 0; bankIdx < config.cacheBanks; bankIdx++) {
                cacheMasterPort[bankIdx].assign(cacheMasterPortInput[bankIdx], cacheMasterPortOutput[bankIdx]);
                cacheSlavePort[bankIdx].assign(cacheSlavePortInput[bankIdx], cacheSlavePortOutput[bankIdx]);
                cache.assignPort(bankIdx, cacheMasterPort[bankIdx], cacheSlavePort[bankIdx]);

                memCtrlXbar.connectPort(PortIDManager::localToGlobalCacheMaster(bankIdx), cacheMasterPort[bankIdx]);                
                cacheXbar.connectPort(PortIDManager::localToGlobalCacheSlave(bankIdx), cacheSlavePort[bankIdx]);
            }
            for (uint accId = 0; accId < config.accelNum; accId++) {
                accMasterPort[accId].assign(accMasterPortInput[accId], accMasterPortOutput[accId]);

                cacheXbar.connectPort(PortIDManager::localToGlobalAccelMaster(accId), accMasterPort[accId]);
                accels[accId].assignMasterPort(accMasterPort[accId]);
            }
        }
    };


} // mudnac


#endif //MUDNACSIM_CACHE_SYSTEM_H
