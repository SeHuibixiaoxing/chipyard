#ifndef MUDNACSIM_CACHE_H
#define MUDNACSIM_CACHE_H


#include "tools.h"
#include "mem_ctrl.h"


namespace mudnac {


    class CacheConfig {
    public:
        uint nSets, nWays, nBanks, lineBytes;
        uint upstreamBeatBytes, downstreamBeatBytes;
        uint memCtrlChannels;

        bool writeThrough;
        bool writeAllocation;

        CacheConfig(uint nSets_, uint nWays_, uint nBanks_, uint lineBytes_,
                    uint upstreamBeatBytes_, uint downstreamBeatBytes_, uint memCtrlChannels_,
                    bool writeThrough_, bool writeAllocation_) :
                nSets(nSets_), nWays(nWays_), nBanks(nBanks_), lineBytes(lineBytes_),
                upstreamBeatBytes(upstreamBeatBytes_), downstreamBeatBytes(downstreamBeatBytes_),
                memCtrlChannels(memCtrlChannels_),
                writeThrough(writeThrough_), writeAllocation(writeAllocation_) {}

        CacheConfig(uint nSets_, uint nWays_, uint nBanks_, uint lineBytes_,
                    uint upstreamBeatBytes_, uint downstreamBeatBytes_, uint memCtrlChannels_)
                : nSets(nSets_), nWays(nWays_), nBanks(nBanks_), lineBytes(lineBytes_),
                  upstreamBeatBytes(upstreamBeatBytes_), downstreamBeatBytes(downstreamBeatBytes_),
                  memCtrlChannels(memCtrlChannels_),
                  writeThrough(false), writeAllocation(true) {}

        CacheConfig(uint nSets_, uint nWays_, uint nBanks_, uint lineBytes_,
                    uint upstreamBeatBytes_, uint downstreamBeatBytes_)
                : nSets(nSets_), nWays(nWays_), nBanks(nBanks_), lineBytes(lineBytes_),
                  upstreamBeatBytes(upstreamBeatBytes_), downstreamBeatBytes(downstreamBeatBytes_),
                  memCtrlChannels(1),
                  writeThrough(false), writeAllocation(true) {}

        CacheConfig() : CacheConfig(0, 0, 0, 0,
                                    0, 0) {}

        uint getTotalCapacity() { return nSets * nWays * nBanks * lineBytes; }
    };


    class CacheMessage : public Message {
    public:        
        uint64 addr;
        uint64 size;
        bool isWrite;
        bool flush;

        CacheMessage(uint64 addr_, uint64 size_, bool isWrite_) : Message(META_TYPE_CACHE) {
            addr = addr_, size = size_, isWrite = isWrite_, flush = false;
        }

        CacheMessage() : CacheMessage(0, 0, false) {}

        static CacheMessage getFlushMsg(uint64 addr = 0) {
            CacheMessage cm;
            cm.addr = addr;
            cm.flush = true;
            return cm;
        }
    };


    class CacheTagSet {
    public:
        class CacheTag {
        public:
            uint64 value;
            bool valid;
            bool dirty;

            CacheTag() : value(0), valid(false), dirty(false) {}
        };

        CacheTagSet(uint nWays);

        bool check(uint64 tagVal, bool isWrite, bool writeThrough);

        bool victim(uint &idx, uint64 &dirtyTagVal);

        void update(uint64 tagVal, uint idx);

        bool flush(uint idx, uint64 &tagVal);

    private:
        uint nWays_;
        std::vector<CacheTag> tags_;

        // for finding recordHit tag faster
        std::map<uint64, uint> tagVal2TagIdxMap_;

        // LRU implementation
        std::list<uint> victimTagIdxList_;
        std::vector<std::list<uint>::iterator> victimListIterVec_;
    };


    class CacheBank : public TickModule {
    public:
        class MSHR {
        public:
            uint victimTagIdx_;
            std::deque<CacheMessage *> miss_;
        };

        class Counter {
        public:
            // counters
            uint64 readHit, readMiss, writeHit, writeMiss;
            uint64 readBytes, writeBytes, fetchBytes, flushBytes;
            uint64 cycles;

            Counter() {
                readHit = 0, readMiss = 0, writeHit = 0, writeMiss = 0;
                readBytes = 0, writeBytes = 0, flushBytes = 0, fetchBytes = 0;
                cycles = 0;
            }

            inline void recordHit(bool isWrite) {
                if (isWrite) { writeHit++; } else { readHit++; }
            }

            inline void recordMiss(bool isWrite) {
                if (isWrite) { writeMiss++; } else { readMiss++; }
            }

            inline void recordUpstream(bool isWrite, uint64 size) {
                if (isWrite) { writeBytes += size; } else { readBytes += size; }
            }

            inline void recordDownstream(bool isWrite, uint64 size) {
                if (isWrite) { flushBytes += size; } else { fetchBytes += size; }
            }

            inline void tick() { cycles++; }

            uint64 totalAccess() { return readHit + readMiss + writeHit + writeMiss; }

            uint64 totalAccessBytes() { return readBytes + writeBytes; }

            uint64 totalMemAccessBytes() { return flushBytes + fetchBytes; }

            double hitRate() { return (double) (readHit + writeHit) / totalAccess(); }

            double lineUtil(uint64 lineBytes) { return (double) totalAccessBytes() / (totalAccess() * lineBytes); }

            double avgBW() { return (double) totalAccessBytes() / cycles; }

            void add(Counter &c) {
                readHit += c.readHit;
                readMiss += c.readMiss;
                writeHit += c.writeHit;
                writeMiss += c.writeMiss;
                readBytes += c.readBytes;
                writeBytes += c.writeBytes;
                flushBytes += c.flushBytes;
                fetchBytes += c.fetchBytes;
                cycles = std::max(cycles, c.cycles);
            }

            std::string getPerformanceInfo(uint64 lineBytes) {
                std::stringstream ss;
                ss << "\tHit Rate: " << hitRate() << "\n";
                ss << "\tLine Utilization: " << lineUtil(lineBytes) << "\n";
                ss << "\tAverage Bandwidth (Bytes/Cycle): " << avgBW() << "\n";
                ss << "\tRead Bytes: " << readBytes << "\n";
                ss << "\tWrite Bytes: " << writeBytes << "\n";
                ss << "\tTotal Access Bytes: " << totalAccessBytes() << "\n";
                ss << "\tFetch Bytes: " << fetchBytes << "\n";
                ss << "\tFlush Bytes: " << flushBytes << "\n";
                ss << "\tTotal Memory Access Bytes: " << totalMemAccessBytes() << "\n";
                return ss.str();
            }
        };

        Counter counter;

        CacheBank(uint nSets, uint nWays, uint nBanks, uint lineBytes, uint bankIdx,
                  uint upstreamBeatBytes, uint downstreamBeatBytes, uint memCtrlChannels,
                  bool writeThrough, bool writeAllocation);

        void assignPort(Port &master, Port &slave) {
            assignMasterPort(master);
            assignSlavePort(slave);
        }

        void assignMasterPort(Port &port) {
            assert(port.notNull());
            masterPort = &port;
        }

        void assignSlavePort(Port &port) {
            assert(port.notNull());
            slavePort = &port;
        }

        void tick();

    private:
        uint nSets_;
        uint nWays_;
        uint nBanks_;
        uint lineBytes_;
        bool writeThrough_;
        bool writeAllocation_;

        uint bankIdx_;
        uint bankIdxOffset_;
        uint setIdxOffset_;
        uint tagOffset_;
        uint64 bankIdxMask_;
        uint64 setIdxMask_;
        uint64 alignedAddrMask_;

        uint64 memCtrlChIdxMask;
        uint64 memCtrlChIdxOffset;

        std::vector<CacheTagSet> sets_;

        Port *slavePort;
        Port *masterPort;

        std::map<uint64, MSHR> mshr_;  // emulate MSHR
        std::deque<Message *> missedCacheMsgFifo_;  // for retrying missed requests
        uint64 flyingMemCtrlMsgCount;  // for flushing

        // for cache flushing
        CacheMessage *flushMsg;
        bool flushFinishing;
        uint flushSetIdx_;
        uint flushWayIdx_;
    };


    class Cache : public TickModule {
    public:
        CacheConfig config;
        std::vector<CacheBank> banks;

        Cache(const CacheConfig &config_) {
            assert(config_.nSets > 0 and isPowOfTwo(config_.nSets));
            assert(config_.nWays > 0);
            assert(config_.nBanks > 0 and isPowOfTwo(config_.nBanks));
            assert(config_.lineBytes > 0 and isPowOfTwo(config_.lineBytes) and
                   config_.lineBytes == MemCtrlConfig::DRAM_BURST_BYTES);

            config = config_;

            banks.reserve(config.nBanks);
            for (uint bankIdx = 0; bankIdx < config.nBanks; bankIdx++) {
                banks.emplace_back(config.nSets, config.nWays, config.nBanks, config.lineBytes, bankIdx,
                                   config.upstreamBeatBytes, config.downstreamBeatBytes, config.memCtrlChannels,
                                   config.writeThrough, config.writeAllocation);
            }

            printf("---- Cache ----\n"
                   "\tnSets = %u\n"
                   "\tnWays = %u\n"
                   "\tnBanks = %u\n"
                   "\tlineBytes = %u\n"
                   "\twriteThrough = %u\n"
                   "\twriteAllocation = %u\n"
                   "\tupstreamBeatBytes = %u\n"
                   "\tdownstreamBeatBytes = %u\n"
                   "\n",
                   config.nSets, config.nWays, config.nBanks, config.lineBytes,
                   config.writeThrough, config.writeAllocation,
                   config.upstreamBeatBytes, config.downstreamBeatBytes);
        }

        void assignPort(uint bankIdx, Port &masterPort, Port &slavePort) {
            assert(bankIdx < config.nBanks);
            banks[bankIdx].assignPort(masterPort, slavePort);
        }


        inline void tick() {
            for (auto &bank: banks) {
                bank.tick();
            }
        }

        CacheBank::Counter getTotalCounter() {
            CacheBank::Counter total;
            for (auto &bank: banks) {
                total.add(bank.counter);
            }
            return total;
        }

        std::string getPerformanceInfo() {
            std::stringstream ss;
            auto counter = getTotalCounter();
            ss << "--Summary\n";
            ss << counter.getPerformanceInfo(config.lineBytes);
            for (uint i = 0; i < config.nBanks; i++) {
                ss << "--Bank " << i << "\n";
                ss << banks[i].counter.getPerformanceInfo(config.lineBytes);
            }
            return ss.str();
        }
    };


}  // mudnac


#endif //MUDNACSIM_CACHE_H
