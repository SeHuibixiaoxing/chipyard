#ifndef MUDNACSIM_MEM_CTRL_H
#define MUDNACSIM_MEM_CTRL_H


#include "tools.h"
#include "dramsim3.h"


namespace mudnac {


    class MemCtrlConfig {
    public:
        static const uint64 DRAM_BURST_BYTES = 64;
        static const uint64 DRAM_BURST_MASK = DRAM_BURST_BYTES - 1;
        static const uint64 DRAM_CHANNEL_IDX_OFFSET = log2Uint(DRAM_BURST_BYTES);  // 2^6 = 64

        std::string dramsimConfigPath;
        std::string dramsimOutputPath;

        uint64 freqMHz;
        uint64 slavePortBeatBytes;
        uint channels;

        MemCtrlConfig(const std::string &dramsimConfigPath_, const std::string &dramsimOutputPath_,
                      uint64 freqMHz_, uint64 slavePortBeatBytes_, uint channels_)
                : dramsimConfigPath(dramsimConfigPath_), dramsimOutputPath(dramsimOutputPath_),
                  freqMHz(freqMHz_), slavePortBeatBytes(slavePortBeatBytes_), channels(channels_) {}

        MemCtrlConfig(const std::string &dramsimConfigPath_, const std::string &dramsimOutputPath_,
                      uint64 freqMHz_, uint64 slavePortBeatBytes_)
                : dramsimConfigPath(dramsimConfigPath_), dramsimOutputPath(dramsimOutputPath_),
                  freqMHz(freqMHz_), slavePortBeatBytes(slavePortBeatBytes_), channels(1) {}

        MemCtrlConfig() : MemCtrlConfig("", "",
                                        0, 0) {}
    };


    class MemCtrlMessage : public Message {
    public:
        uint64 addr;
        bool isWrite;

        // counter
        // uint requestCycle, sendToDramCycle, responseCycle;

        MemCtrlMessage(uint64 addr_, bool isWrite_) : Message(Message::META_TYPE_MEMCTRL) {
            addr = addr_;
            isWrite = isWrite_;
        }

        MemCtrlMessage() : MemCtrlMessage(0, false) {}

        void print(const std::string& debugFlag) override {
            Message::print(debugFlag);
            LOGF(debugFlag, "MemCtrl Message Info:\n");
            LOGF(debugFlag, "addr: %#lx, isWrite: %s\n", addr, isWrite ? "true" : "false");
        }
    };

    /**
     * @brief A memory controller counter, responsible for tracking and calculating various memory-related metrics per tick.
     * 
     */
    class MemCtrlCounter {
    public:
        uint64 burstBytes;
        uint64 cycles;
        PeriodicalCounter<uint64> readBytes, writeBytes, readTrans, writeTrans;
        PeriodicalCounter<uint64> noReqCycleCount;

        MemCtrlCounter(uint64 periodCycles = 1000, uint64 burstBytes_ = MemCtrlConfig::DRAM_BURST_BYTES)
                : readBytes(periodCycles), writeBytes(periodCycles),
                  readTrans(periodCycles), writeTrans(periodCycles),
                  noReqCycleCount(periodCycles) {
            burstBytes = burstBytes_;
            cycles = 0;
        }

        inline void record(MemCtrlMessage *msg) {
            if (msg->isWrite) {
                writeBytes.add(burstBytes);
                writeTrans.add(1);
            } else {
                readBytes.add(burstBytes);
                readTrans.add(1);
            }
        }

        inline void tick() {
            readBytes.tick();
            writeBytes.tick();
            readTrans.tick();
            writeTrans.tick();
            noReqCycleCount.tick();
            cycles++;
        }

        uint64 getReadBytes() { return readTrans.sum() * burstBytes; }

        uint64 getWriteBytes() { return writeTrans.sum() * burstBytes; }

        uint64 getTotalBytes() { return getReadBytes() + getWriteBytes(); }

        double getAvgReadBW() { return (double) getReadBytes() / cycles; }

        double getAvgWriteBW() { return (double) getWriteBytes() / cycles; }

        double getAvgBW() { return getAvgReadBW() + getAvgWriteBW(); }

        double getPeakBW() {
            double peakBW = 0;
            for (uint i = 0; i < readTrans.size(); i++) {
                uint64 bytes = (readTrans.get(i) + writeTrans.get(i)) * burstBytes;
                uint64 cyc = (i == readTrans.size() - 1) ?
                             std::max((uint64) 1, cycles % readTrans.periodCycles) :
                             readTrans.periodCycles;
                peakBW = std::max(peakBW, (double) bytes / cyc);
            }
            return peakBW;
        }

        double getAvgBWUtil() { return (double) (readBytes.sum() + writeBytes.sum()) / getTotalBytes(); }

        double getAvgNoReqRate() { return (double) noReqCycleCount.sum() / cycles; }

        double getPeakNoReqRate() {
            double peakRate = 0;
            for (uint i = 0; i < noReqCycleCount.size(); i++) {
                uint64 num = noReqCycleCount.get(i);
                uint64 cyc = (i == noReqCycleCount.size() - 1) ?
                             std::max((uint64) 1, cycles % noReqCycleCount.periodCycles) :
                             noReqCycleCount.periodCycles;
                peakRate = std::max(peakRate, (double) num / cyc);
            }
            return peakRate;
        }

        void merge(MemCtrlCounter &other) {
            assert(burstBytes == other.burstBytes);
            cycles = std::max(cycles, other.cycles);
            readTrans.merge(other.readTrans);
            readBytes.merge(other.readBytes);
            writeTrans.merge(other.writeTrans);
            writeBytes.merge(other.writeBytes);
            noReqCycleCount.merge(other.noReqCycleCount);
        }

        std::string getPerformanceInfo() {
            std::stringstream ss;
            ss << "\tRead Bytes: " << getReadBytes() << "\n";
            ss << "\tWrite Bytes: " << getWriteBytes() << "\n";
            ss << "\tTotal Bytes: " << getReadBytes() + getWriteBytes() << "\n";
            ss << "\tAverage Bandwidth (Bytes/Cycle): " << getAvgBW() << "\n";
            ss << "\tPeak Bandwidth (Bytes/Cycle): " << getPeakBW() << "\n";
            ss << "\tAverage Bandwidth Utilization: " << getAvgBWUtil() << "\n";
            ss << "\tAverage No-request-cycle Rate: " << getAvgNoReqRate() << "\n";
            ss << "\tPeak No-request-cycle Rate: " << getPeakNoReqRate() << "\n";
            return ss.str();
        }
    };

    /**
     * @brief A memory controller channel, responsible for tracking and handling various memory requests and dramsim3 transactions per tick.
     * 
     */
    class MemCtrlChannel {
    private:
        uint64 mem_channel_cycles_;
    public:
        MemCtrlConfig config;
        MemCtrlCounter counter;
        uint64 channelIdx;
        uint64 channelIdxMask;
        uint64 channelIdxWidth;
        uint64 highBitsMask;

        // dramsim3
        dramsim3::MemorySystem *memSys;
        uint64 dramBeatBytes;

        //for frequency switching between DRAM IO and controller
        double ioTimeIncr; 
        double ioTimeRemain;

        // slave ports
        Port *slavePort;

        // messages accessing DRAM
        std::multimap<std::pair<uint64, bool>, MemCtrlMessage *> dramMessageMap; // (addr, isWrite)->msg*
        // 不应该有读写交错/多个写并行的情况。但为了处理这种情况，仍添加一个字段区分读写请求。

        // dram burst
        MemCtrlMessage *dramCurBurstMsg;
        uint64 dramCurBurstBytes;
        uint64 dramCurLocalAddr;

        MemCtrlChannel(const MemCtrlConfig &config_, uint chIdx_);

        ~MemCtrlChannel() {
            memSys->PrintStats();
            delete memSys;
        }

        inline void tick();

    private:
        inline uint64 getLocalAddr(uint64 globalAddr) {
            assert((globalAddr & MemCtrlConfig::DRAM_BURST_MASK) == 0);            
            assert(((globalAddr >> MemCtrlConfig::DRAM_CHANNEL_IDX_OFFSET) & channelIdxMask) == channelIdx);
            return (globalAddr & highBitsMask) >> channelIdxWidth;
        }

        inline uint64 getGlobalAddr(uint64 localAddr) {
            return (localAddr << channelIdxWidth) | (channelIdx << MemCtrlConfig::DRAM_CHANNEL_IDX_OFFSET);
        }

        /**
         * This function is called by dramsim3 when a memory access is finished.
         * It removes the corresponding message from the dramMessageMap and pushes it to the upstream output port.
         * It also records the message in the counter.
         *
         * @param addr the local address of the finished memory access
         */
        void dramsim3Callback(uint64 addr, bool isWrite) {
            auto [begin, end] = dramMessageMap.equal_range({addr, isWrite});
            assert(begin != end);
            for(auto it = begin; it != end; ++it) {
                auto msg = it->second;
                assert(getGlobalAddr(addr) == msg->addr);
                counter.record(msg);
                slavePort->output->push_back(msg);
                
                // if(isWrite) {
                //     break;
                // }
            }
            // if(isWrite) {
            //     dramMessageMap.erase(begin);
            // } else {
            //     dramMessageMap.erase(begin, end);
            // }
            dramMessageMap.erase(begin, end);
        }
    };

    /**
     * @brief Manages multiple memory channels
     * 
     */
    class MemController : public TickModule {
    public:
        MemCtrlConfig config;
        std::vector<MemCtrlChannel> memCtrlChannel;

    public:
        MemController(const MemCtrlConfig &config_);

        void assignPort(uint channelIdx, Port& port) {
            assert(channelIdx < config.channels);
            assert(memCtrlChannel[channelIdx].slavePort == NULL);
            assert(port.notNull());
            memCtrlChannel[channelIdx].slavePort = &port;
        }

        void tick();

        inline MemCtrlCounter getSummaryCounter() {
            MemCtrlCounter total;
            for (auto &ch: memCtrlChannel) {
                total.merge(ch.counter);
            }
            return total;
        }

        std::string getPerformanceInfo() {
            MemCtrlCounter counter = getSummaryCounter();
            std::stringstream ss;
            ss << "--Summary\n";
            ss << counter.getPerformanceInfo();
            for (uint i = 0; i < config.channels; i++) {
                ss << "--Channel " << i << "\n";
                ss << memCtrlChannel[i].counter.getPerformanceInfo();
            }
            return ss.str();
        }
    };


} // mudnac


#endif //MUDNACSIM_MEM_CTRL_H
