#ifndef MUDNACSIM_SPM_SYSTEM_H
#define MUDNACSIM_SPM_SYSTEM_H


#include "base_system.h"
#include "spm_accel.h"
#include "xbar.h"
#include "system_config.h"


namespace mudnac {


    class BaseSPMSystem : public BaseSystem {
    public:
        SPM spm;
        std::vector<SPMAccel> accels;

        BaseSPMSystem(SystemConfig &config_)
                : BaseSystem(BaseSystem::TYPE_SPM, config_),
                  spm(config_.getSPMConfig()) {
            SPMConfig spmCfg = config_.getSPMConfig();
            std::vector<AccelConfig> accelCfgList = config_.getAccelConfigVector();

            // Check configurations
            for (int i = 0; i < accelCfgList.size(); i++) {
                assert(spmCfg.upstreamBeatBytes == accelCfgList[i].beatBytes);
                assert(spmCfg.lineBytes == accelCfgList[i].maxBurstBytes);
                assert(spmCfg.nBanks == accelCfgList[i].nBanks);
            }

            // Create TickModule objects
            accels.reserve(accelCfgList.size());
            for (int accId = 0; accId < accelCfgList.size(); accId++) {
                AccelConfig cfg = accelCfgList[accId];
                cfg.accId = accId;
                accels.emplace_back(cfg);
                accels.back().accel_id_ = accId;
                accels[accId].connectCPUPort(&cmdInput[accId], &cmdOutput[accId]);
            }

            // Add modules to "tickModuleList"
            tickModuleList.push_back(&spm);
            for (auto &acc: accels) { tickModuleList.push_back(&acc); }
        }

        std::string getPerformanceInfo() {
            std::stringstream ss;
            ss << "Total Cycles: " << cycles << "\n\n";
            ss << "Memory Controller\n";
            ss << memCtrl.getPerformanceInfo() << "\n";
            ss << "SPM\n";
            ss << spm.getPerformanceInfo() << "\n";
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


    class SPMSystem : public BaseSPMSystem {
    private:
        XBar sharedXbar;

        std::vector<std::deque<Message *>> memCtrlSlavePortInput;
        std::vector<std::deque<Message *>> memCtrlSlavePortOutput;
        std::vector<std::deque<Message *>> spmMasterPortInput;
        std::vector<std::deque<Message *>> spmMasterPortOutput;
        std::vector<std::deque<Message *>> spmSlavePortInput;
        std::vector<std::deque<Message *>> spmSlavePortOutput;
        std::vector<std::deque<Message *>> accMasterPortInput;
        std::vector<std::deque<Message *>> accMasterPortOutput;

        std::vector<Port> memCtrlSlavePort;
        std::vector<Port> spmMasterPort;
        std::vector<Port> spmSlavePort;
        std::vector<Port> accMasterPort;

    public:
        SPMSystem(SystemConfig &config)
                : BaseSPMSystem(config),
                  memCtrlSlavePortInput(config.memCtrlChannels),
                  memCtrlSlavePortOutput(config.memCtrlChannels),
                  memCtrlSlavePort(config.memCtrlChannels),
                  spmMasterPortInput(config.cacheBanks),
                  spmMasterPortOutput(config.cacheBanks),
                  spmMasterPort(config.cacheBanks),
                  spmSlavePortInput(config.cacheBanks),
                  spmSlavePortOutput(config.cacheBanks),
                  spmSlavePort(config.cacheBanks),
                  accMasterPortInput(config.accelNum),
                  accMasterPortOutput(config.accelNum),
                  accMasterPort(config.accelNum) {

            // Add modules to "tickModuleList"
            tickModuleList.push_back(&sharedXbar);

            // Connect modules to xbars
            for (int i = 0; i < config.memCtrlChannels; i++) {
                memCtrlSlavePort[i].assign(memCtrlSlavePortInput[i], memCtrlSlavePortOutput[i]);
                memCtrl.assignPort(i, memCtrlSlavePort[i]);

                sharedXbar.connectPort(PortIDManager::localToGlobalMEMSlave(i), memCtrlSlavePort[i]);  
            }
            for (uint bankIdx = 0; bankIdx < config.cacheBanks; bankIdx++) {
                spmMasterPort[bankIdx].assign(spmMasterPortInput[bankIdx], spmMasterPortOutput[bankIdx]);
                spmSlavePort[bankIdx].assign(spmSlavePortInput[bankIdx], spmSlavePortOutput[bankIdx]);
                spm.assignPort(bankIdx, spmMasterPort[bankIdx], spmSlavePort[bankIdx]);

                sharedXbar.connectPort(PortIDManager::localToGlobalSPMMaster(bankIdx), spmMasterPort[bankIdx]);                
                sharedXbar.connectPort(PortIDManager::localToGlobalSPMSlave(bankIdx), spmSlavePort[bankIdx]);
            }
            for (uint accId = 0; accId < config.accelNum; accId++) {
                accMasterPort[accId].assign(accMasterPortInput[accId], accMasterPortOutput[accId]);

                sharedXbar.connectPort(PortIDManager::localToGlobalAccelMaster(accId), accMasterPort[accId]);
                accels[accId].assignMasterPort(accMasterPort[accId]);
            }
        }
    };


} // mudnac


#endif //MUDNACSIM_SPM_SYSTEM_H
