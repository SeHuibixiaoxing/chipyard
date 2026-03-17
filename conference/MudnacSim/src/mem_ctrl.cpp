#include "mem_ctrl.h"


namespace mudnac {


    MemCtrlChannel::MemCtrlChannel(const mudnac::MemCtrlConfig &config_, uint chIdx_) :
            config(config_) {
        channelIdx = chIdx_;
        channelIdxMask = config.channels - 1;
        channelIdxWidth = log2(config.channels);
        highBitsMask = UINT64_MAX << (channelIdxWidth + MemCtrlConfig::DRAM_CHANNEL_IDX_OFFSET);

        std::string dramsimOutputPath = config.dramsimOutputPath;
        if (dramsimOutputPath.back() != '/') {
            dramsimOutputPath += '/';
        }
        dramsimOutputPath += std::to_string(chIdx_);
        int r = system(("mkdir -p " + dramsimOutputPath).c_str());
        if(r != 0) {
            std::cerr << "Failed to create directory " << dramsimOutputPath << std::endl;
            exit(1);
        }

        memSys = dramsim3::GetMemorySystem(config.dramsimConfigPath, dramsimOutputPath,
                                           std::bind(&MemCtrlChannel::dramsim3Callback, this, std::placeholders::_1, false),
                                           std::bind(&MemCtrlChannel::dramsim3Callback, this, std::placeholders::_1, true));
        dramBeatBytes = memSys->GetBusBits() / 8 * 2;
        assert((dramBeatBytes * memSys->GetBurstLength() / 2) == MemCtrlConfig::DRAM_BURST_BYTES && "DRAM burst size mismatch");

        ioTimeIncr = memSys->GetTCK() / (1000.0 / config.freqMHz); // tck ns per cycle
        ioTimeRemain = 0;

        slavePort = NULL;

        dramCurBurstMsg = NULL;
        dramCurBurstBytes = 0;
        dramCurLocalAddr = 0;
    }

    /**
     * @brief Simulates the memory controller channel for one tick.
     * A tick is a period of time equal to the memory controller frequency.
     * The memory controller channel is responsible for tracking the state of
     * the memory system, including the current row and column addresses,
     * the current burst length and bytes remaining in the burst, and the
     * current message being sent to or received from the memory system.
     * The memory controller channel also keeps track of the number of cycles
     * with no requests.
     */
    inline void MemCtrlChannel::tick() {
        ++ mem_channel_cycles_;

        double ioTime = 1.0 + ioTimeRemain;
        double ioTimeCost = 0;

        // ###wzy
        static bool tag = true;
        if (DebugLogger::isEnabled(MEMCTRL_DEBUG_FLAG) && tag) {
            LOG(MEMCTRL_DEBUG_FLAG) << "mem_channel_cycles_=" << mem_channel_cycles_ << ", dramMessageMap.size()=" << dramMessageMap.size() << std::endl;
            tag = false;
        }

        if (slavePort->input->empty()) {
            counter.noReqCycleCount.add(1);
        }

        while (ioTimeCost + ioTimeIncr <= ioTime) {
            if ((dramCurBurstMsg == NULL) and (not slavePort->input->empty())) {
                dramCurBurstBytes = 0;
                dramCurBurstMsg = dynamic_cast<MemCtrlMessage*>(slavePort->input->front());
                slavePort->input->pop_front();
                dramCurLocalAddr = getLocalAddr(dramCurBurstMsg->addr);
            }
            
            if (dramCurBurstMsg != NULL) {
                // 如果是读请求，且不是第一个读请求
                if (!dramCurBurstMsg->isWrite && dramMessageMap.count({dramCurLocalAddr, dramCurBurstMsg->isWrite})) {
                    dramMessageMap.insert({{dramCurLocalAddr, dramCurBurstMsg->isWrite}, dramCurBurstMsg});
                    dramCurBurstMsg = NULL;
                }
                // 如果是写请求且没有相同地址的读请求，或是第一个读请求且没有相同地址的写请求，准备发送新事务
                else if ((!dramMessageMap.count({dramCurLocalAddr, !dramCurBurstMsg->isWrite})) && memSys->WillAcceptTransaction(dramCurLocalAddr, dramCurBurstMsg->isWrite)) {
                    if (dramCurBurstMsg->isWrite and
                        (dramCurBurstBytes + dramBeatBytes < MemCtrlConfig::DRAM_BURST_BYTES)) {
                        dramCurBurstBytes += dramBeatBytes;
                    } else {

                        LOGF(MEMCTRL_DEBUG_FLAG, "%lu MemCtrlChannel %lu: globalAddr=%lx, localAddr=%lx, isWrite=%u\n",
                               counter.cycles, channelIdx, dramCurBurstMsg->addr, dramCurLocalAddr,
                               dramCurBurstMsg->isWrite);
                               
                        memSys->AddTransaction(dramCurLocalAddr, dramCurBurstMsg->isWrite);
                        dramMessageMap.insert({{dramCurLocalAddr, dramCurBurstMsg->isWrite}, dramCurBurstMsg});
                         
                        dramCurBurstMsg = NULL;
                    }
                }
            }

            memSys->ClockTick();

            ioTimeCost += ioTimeIncr;
        }

        ioTimeRemain = ioTime - ioTimeCost;
        counter.tick();
    }


    MemController::MemController(const mudnac::MemCtrlConfig &config_)
            : config(config_) {
        assert(config.freqMHz > 0);
        assert(config.slavePortBeatBytes > 0);
        assert(config.channels > 0 and isPowOfTwo(config.channels));

        memCtrlChannel.reserve(config.channels);
        for (int chIdx = 0; chIdx < config.channels; chIdx++) {
            memCtrlChannel.emplace_back(config, chIdx);
        }

        printf("---- MemController ----\n"
               "\tdramsimConfigPath = %s\n"
               "\tdramsimOutputPath = %s\n"
               "\tfreqMHz = %lu\n"
               "\tslavePortBeatBytes = %lu\n"
               "\tchannels = %u\n"
               "\n",
               config.dramsimConfigPath.c_str(),
               config.dramsimOutputPath.c_str(),
               config.freqMHz,
               config.slavePortBeatBytes,
               config.channels);
    }

    void MemController::tick() {
        for (auto &ch: memCtrlChannel) {
            ch.tick();
        }
    }


} // mudnac