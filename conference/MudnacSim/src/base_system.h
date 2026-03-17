#ifndef MUDNACSIM_BASE_SYSTEM_H
#define MUDNACSIM_BASE_SYSTEM_H


#include "base_accel.h"
#include "system_config.h"


namespace mudnac {


    class BaseSystem {
    public:
        static const uint64 TYPE_CACHE = 0;
        static const uint64 TYPE_SPM = 1;

        SystemConfig config;

        uint64 type = TYPE_CACHE;
        uint64 numAccels = 0;
        uint64 cycles = 0;
        MemController memCtrl;

    protected:
        std::vector<TickModule *> tickModuleList;

        std::vector<std::deque<AccelTiledCMD *>> cmdInput;
        std::vector<std::deque<AccelTiledCMD *>> cmdOutput;

        Port cmdPort;

    public:
        BaseSystem(uint64 type_, SystemConfig &config_)
                : config(config_),
                  type(type_),
                  numAccels(config_.accelNum),
                  memCtrl(config_.getMemCtrlConfig()),
                  cmdInput(numAccels),
                  cmdOutput(numAccels) {
            tickModuleList.push_back(&memCtrl);
        }

        virtual ~BaseSystem() = default;

        inline bool isCacheSystem() { return type == TYPE_CACHE; }

        inline bool isSpmSystem() { return type == TYPE_SPM; }

        inline void tick() {
            for (auto m: tickModuleList) { m->tick(); }
            cycles++;
        }

        inline void addCmd(uint accId, AccelTiledCMD *cmd) {
            assert(accId < cmdInput.size());
            cmdInput[accId].push_back(cmd);
        };

        /**
         * @brief Get and pop the finished command from the output queue.
         *
         * @param accId The id of the accelerator.
         * @return The finished command. If there is no finished command, return NULL.
         */
        AccelTiledCMD *getFinishedCmd(uint accId, bool is_pipeline = false) {
            assert(accId < cmdOutput.size());
            AccelTiledCMD *cmd = NULL;
            auto &fifo = cmdOutput[accId];
            if (not fifo.empty()) {
                cmd = fifo.front();
                fifo.pop_front();
            }
            for (auto it = fifo.cbegin(); it != fifo.cend(); ++it) {
                if (is_pipeline == (*it)->is_pipeline_cmd) {
                    cmd = *it;
                    fifo.erase(it);
                    break;
                }
            }
            return cmd;
        };
        AccelTiledCMD *getFinishedCmd(const uint accId, const uint globalCmdId, bool is_pipeline = false) {
            assert(accId < cmdOutput.size());
            AccelTiledCMD *cmd = NULL;
            auto &fifo = cmdOutput[accId];
            for (auto it = fifo.cbegin(); it != fifo.cend(); ++it) {
                if (is_pipeline == (*it)->is_pipeline_cmd && (*it)->globalCmdId == globalCmdId) {
                    cmd = *it;
                    fifo.erase(it);
                    break;
                }
            }
            return cmd;
        }
 
        virtual std::string getPerformanceInfo() = 0;

        virtual uint getAccDis(uint accIdSrc, uint accIdDst) = 0;
    };


} // mudnac

#endif //MUDNACSIM_BASE_SYSTEM_H
