#ifndef MUDNACSIM_CACHE_ACCEL_H
#define MUDNACSIM_CACHE_ACCEL_H


#include "tools.h"
#include "base_accel.h"
#include "cache.h"


namespace mudnac {


    class CacheAccel : public BaseAccel {
    public:
        CacheAccel(const AccelConfig &config_);

        ~CacheAccel() = default;

        void assignMasterPort(Port &masterPort_) {
            this->masterPort = &masterPort_;
        }

        void connectCPUPort(std::deque<AccelTiledCMD *> *input, std::deque<AccelTiledCMD *> *output) {
            assert(input != NULL and output != NULL);
            cpuPortInput = input;
            cpuPortOutput = output;
        }

        void tick();

    private:
        std::deque<AccelTiledCMD *> *cpuPortInput;
        std::deque<AccelTiledCMD *> *cpuPortOutput;

        Port *masterPort;

        AccelTiledCMD *computeCMD;
        AccelTiledCMD *loadCMD;
        AccelTiledCMD *storeCMD;
        AccelTiledCMD *waitedComputeCMD;
        AccelTiledCMD *waitedLoadCMD;
        AccelTiledCMD *waitedStoreCMD;

        // for cache bank indexing
        uint64 bankIdxOffset;
        uint64 bankIdxMask;

        // for load
        bool loadCacheMsgCreationDone;
        uint64 flyingLoadCacheMsgCount;

        // for store
        class FlyingCmdInStore {
        public:
            AccelTiledCMD *cmd = NULL;
            uint count = 0;
        };

        bool storeCacheMsgCreationDone;
        std::map<CacheMessage *, FlyingCmdInStore *> flyingStoreCacheMsgCount;

        // for compute
        uint64 computeCurrentCycles;
        uint64 computeTotalCycles;

        // for burst to master port
        std::deque<CacheMessage *> masterPortBurstFifo;

        // for cache flushing
        AccelTiledCMD *flushCMD;
        std::map<CacheMessage *, AccelTiledCMD *> flushCmdMap;

        // for performance counter
        Counter::TimeSlice ts;
    };


} // mudnac


#endif //MUDNACSIM_CACHE_ACCEL_H
