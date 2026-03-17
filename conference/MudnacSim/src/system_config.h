#ifndef MUDNACSIM_SYSTEM_CONFIG_H
#define MUDNACSIM_SYSTEM_CONFIG_H


#include "tools.h"
#include "mem_ctrl.h"
#include "cache.h"
#include "spm.h"
#include "base_accel.h"


namespace mudnac {


    class SystemConfig {
    public:
        uint freqMHz;

        std::string dramsim3ConfigPath;
        std::string dramsim3OutputPath;
        uint memCtrlChannels;
        uint memCtrlUpstreamBeatBytes;

        bool cacheWriteThrough;
        bool cacheWriteAllocation;
        uint cacheLineBytes;
        uint cacheTotalBytes;
        uint cacheBanks;
        uint cacheWays;
        uint cacheDownstreamBeatBytes;
        uint cacheUpstreamBeatBytes;

        uint spmWays;
        uint spmPageBytes;
        bool spmMulticast;

        uint accelNum;
        uint accelArrayH, accelArrayW;
        uint accelInputBufBytes, accelWeightBufBytes, accelPsumBufBytes;
        uint accelInputWordBytes, accelPsumWordBytes;
        uint accelDownstreamBeatBytes;
        uint accelSpmAddrType;
        SystemConfig() {
            freqMHz = 1000;

            dramsim3ConfigPath = "DRAMsim3/configs/DDR4_16Gb_x16_3200_am1.ini";
            dramsim3OutputPath = "expr/output/default/dramsim3";
            memCtrlChannels = 1;
            memCtrlUpstreamBeatBytes = 64;

            cacheWriteThrough = false;
            cacheWriteAllocation = true;
            cacheLineBytes = 64;
            cacheTotalBytes = 4 * 1024 * 1024;
            cacheBanks = 8;
            cacheWays = 16;
            cacheDownstreamBeatBytes = 64;
            cacheUpstreamBeatBytes = 64;

            spmWays = 12;
            spmPageBytes = 32 * 1024;
            spmMulticast = true;

            accelNum = 8;
            accelArrayH = accelArrayW = 16;
            accelInputBufBytes = 32 * 1024;
            accelWeightBufBytes = 32 * 1024;
            accelPsumBufBytes = 64 * 1024;
            accelInputWordBytes = 1;
            accelPsumWordBytes = 4;
            accelDownstreamBeatBytes = 64;
            accelSpmAddrType = AccelConfig::SPM_ADDR_TYPE_BLOCK_INTERLEAVED;
        }

        inline uint getCacheSets() {
            assert(cacheTotalBytes % (cacheBanks * cacheWays * cacheLineBytes) == 0);
            return cacheTotalBytes / (cacheBanks * cacheWays * cacheLineBytes);
        }

        inline uint getSPMTotalBytes() {
            assert(cacheTotalBytes % cacheWays == 0);
            assert(spmWays <= cacheWays);
            return (cacheTotalBytes / cacheWays) * spmWays;
        }

        inline uint getSPMTotalPages() {
            assert(getSPMTotalBytes() % spmPageBytes == 0);
            return getSPMTotalBytes() / spmPageBytes;
        }

        inline uint getAccelsPerBank() {
            return accelNum / cacheBanks;
        }

        inline uint getPagesPerBank() {
            return getSPMTotalPages() / cacheBanks;
        }

        inline MemCtrlConfig getMemCtrlConfig() {
            return {dramsim3ConfigPath, dramsim3OutputPath,
                    freqMHz, memCtrlUpstreamBeatBytes, memCtrlChannels};
        }

        inline CacheConfig getCacheConfig() {
            return {getCacheSets(), cacheWays, cacheBanks, cacheLineBytes,
                    cacheUpstreamBeatBytes, cacheDownstreamBeatBytes,
                    memCtrlChannels, cacheWriteThrough, cacheWriteAllocation};
        }

        inline SPMConfig getSPMConfig() {
            SPMConfig cfg(cacheBanks, cacheLineBytes,
                          cacheUpstreamBeatBytes, cacheDownstreamBeatBytes,
                          getSPMTotalBytes(), spmPageBytes, memCtrlChannels);
            cfg.multicast = spmMulticast;
            return cfg;
        }

        inline AccelConfig getAccelConfig(uint accId) {
            AccelConfig cfg(accId, accelArrayH, accelArrayW,
                            accelDownstreamBeatBytes, cacheLineBytes,
                            cacheBanks, getSPMTotalPages(), spmPageBytes,
                            accelInputBufBytes, accelWeightBufBytes, accelPsumBufBytes,
                            accelInputWordBytes, accelPsumWordBytes);
            cfg.spmAddrType = accelSpmAddrType;
            cfg.numAccels = accelNum;
            return cfg;
        }

        inline std::vector<AccelConfig> getAccelConfigVector() {
            std::vector<AccelConfig> v(accelNum);
            for (uint accId = 0; accId < accelNum; accId++) {
                v[accId] = getAccelConfig(accId);
            }
            return v;
        }
    };


}


#endif //MUDNACSIM_SYSTEM_CONFIG_H
