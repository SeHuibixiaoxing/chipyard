#ifndef MUDNACSIM_SPM_H
#define MUDNACSIM_SPM_H


#include "tools.h"
#include "mem_ctrl.h"


namespace mudnac {
    static const uint SPMMSG_FETCH = 10; // read from dram to spm
    static const uint SPMMSG_FETCH_LOAD = 11; // read from dram, store in spm, and send to accel
    static const uint SPMMSG_FLUSH = 20; // write from spm to dram
    static const uint SPMMSG_LOAD = 30; // load from spm to accel
    static const uint SPMMSG_LOAD_BYPASS = 31; // load from dram to accel, not store in spm first
    static const uint SPMMSG_STORE = 40; // store to spm
    static const uint SPMMSG_STORE_BYPASS = 41; // store to dram, not store in spm first
    static const uint SPMMSG_SEND = 50; // send inst to bank
    static const uint SPMMSG_S2S = 60; // send from one bank to another


    class SPMConfig {
    public:
        uint nBanks, lineBytes;
        uint upstreamBeatBytes, downstreamBeatBytes;
        uint memCtrlChannels;

        uint totalBytes, pageBytes, numPages;

        bool multicast;

        SPMConfig(uint nBanks_, uint lineBytes_,
                  uint upstreamBeatBytes_, uint downstreamBeatBytes_,
                  uint totalBytes_, uint pageBytes_) {
            nBanks = nBanks_;
            lineBytes = lineBytes_;
            upstreamBeatBytes = upstreamBeatBytes_;
            downstreamBeatBytes = downstreamBeatBytes_;
            memCtrlChannels = 1;

            assert(totalBytes_ % (nBanks_ * lineBytes_) == 0);
            assert(totalBytes_ % pageBytes_ == 0);
            assert(pageBytes_ % lineBytes_ == 0);

            totalBytes = totalBytes_;
            pageBytes = pageBytes_;
            numPages = totalBytes / pageBytes;

            multicast = true;
        }

        SPMConfig(uint nBanks_, uint lineBytes_,
                  uint upstreamBeatBytes_, uint downstreamBeatBytes_,
                  uint totalBytes_, uint pageBytes_, uint memCtrlChannels_,
                  uint arbitrationMode_, uint numArbGroups_) :
                SPMConfig(nBanks_, lineBytes_,
                          upstreamBeatBytes_, downstreamBeatBytes_,
                          totalBytes_, pageBytes_) {
            memCtrlChannels = memCtrlChannels_;
        }

        SPMConfig(uint nBanks_, uint lineBytes_,
                  uint upstreamBeatBytes_, uint downstreamBeatBytes_,
                  uint totalBytes_, uint pageBytes_, uint memCtrlChannels_) :
                SPMConfig(nBanks_, lineBytes_,
                          upstreamBeatBytes_, downstreamBeatBytes_,
                          totalBytes_, pageBytes_) {
            memCtrlChannels = memCtrlChannels_;
        }

        SPMConfig(uint nBanks_, uint lineBytes_,
                  uint upstreamBeatBytes_, uint downstreamBeatBytes_)
                : SPMConfig(nBanks_, lineBytes_, upstreamBeatBytes_, downstreamBeatBytes_,
                            32 * 1024, 32 * 1024) {}

        SPMConfig() : SPMConfig(1, 64,
                                8, 8) {}
    };


    class SPMMessage : public Message {
    public:
        uint type;
        uint64 addr;
        uint64 size;
        uint page;
        uint64 groupId;
        uint groupSize;

        // for SPM Send,  master id is accel, slave id is spm(src), dst spm id is spm(dst) slave id
        // for SPM S2S, master id is spm(src), dst spm id is spm(dst), dst spm id is accel master id
        uint dstSpmIdSPMSlave; 

        // for SPM Send Msg
        uint64 s2sSize;

        SPMMessage(uint type_, uint page_, uint size_, uint addr_, uint64 groupId_, uint groupSize_) : Message(Message::META_TYPE_SPM) {
            type = type_, addr = addr_, size = size_, page = page_, groupId = groupId_, groupSize = groupSize_;
        }

        SPMMessage(uint type_, uint page_, uint size_, uint addr_)
                : SPMMessage(type_, page_, size_, addr_, 0, 1) {}

        SPMMessage() : SPMMessage(0, 0, 0, 0, 0, 1) {}

        inline bool isFetch() { return type == SPMMSG_FETCH; }

        inline bool isFetchLoad() { return type == SPMMSG_FETCH_LOAD; }

        inline bool isFlush() { return type == SPMMSG_FLUSH; }

        inline bool isLoad() { return type == SPMMSG_LOAD; }

        inline bool isLoadBypass() { return type == SPMMSG_LOAD_BYPASS; }

        inline bool isStore() { return type == SPMMSG_STORE; }

        inline bool isStoreBypass() { return type == SPMMSG_STORE_BYPASS; }

        inline bool isSend() {return type == SPMMSG_SEND;}

        inline bool isS2S() {return type == SPMMSG_S2S;}

        void print(const std::string& debugFlag) override {
            Message::print(debugFlag);
            LOGF(debugFlag, "SPM Message Info:\n");        
            LOGF(debugFlag, "type: %u\n", type);
            LOGF(debugFlag, "addr: %#lx, size: %#lx\n", addr, size);
            LOGF(debugFlag, "page: %#x\n", page);
            LOGF(debugFlag, "groupId: %lu, groupSize: %u\n", groupId, groupSize);
            LOGF(debugFlag, "dstSpmIdSPMSlave: %#x, s2sSize: %lu\n", dstSpmIdSPMSlave, s2sSize);
        }
    };


    class SPMBank : public TickModule {
    public:
        // A group of message with the same group id.
        class GroupedMessage {
        public:
            uint groupSize;
            std::deque<SPMMessage *> msgs;

            GroupedMessage() { groupSize = 0; }
            
            /**
             * @brief Retrieves the message with the smallest masterPortId from the group.
             *
             * @return The SPMMessage pointer with the smallest masterPortId.
             * @note The function asserts that the deque of messages is not empty.
             */
            SPMMessage *getOne() {
                assert(msgs.size() > 0);
                SPMMessage *msg = msgs[0];
                for (auto m: msgs) {
                    if (m->masterPortId < msg->masterPortId) {
                        msg = m;
                    }
                }
                return msg;
            }
        };

        class Counter {
        public:
            uint64 load, loadOnly, fetchLoadOnly, store, fetch, flush, sendFrom, sendTo;
            uint64 loadBytes, loadOnlyBytes, fetchLoadOnlyBytes, storeBytes, fetchBytes, flushBytes, sendFromBytes, sendToBytes;
            uint64 spmReadBytesFromLoad, spmReadBytesFromLoadOnly, spmReadBytesFromFetchLoadOnly,spmReadBytesFromFlush, spmReadBytesFromSend;
            uint64 spmWriteBytesFromFetchLoad, spmWriteBytesFromStore, spmWriteBytesFromFetch, spmWriteBytesFromSend;
            uint64 dramReadBytes, dramReadBytesOnlyFetch, dramReadBytesOnlyFetchLoad, dramReadBytesOnlyLoadBypass;
            uint64 dramWriteBytes, dramWriteBytesOnlyStore, dramWriteBytesOnlyFlush;
            uint64 noReqCycles;
            uint64 cycles;

            Counter() {
                load = 0, loadOnly = 0, fetchLoadOnly = 0, store = 0, fetch = 0, flush = 0, sendFrom = 0, sendTo = 0;
                loadBytes = 0, loadOnlyBytes = 0, fetchLoadOnlyBytes = 0, storeBytes = 0, fetchBytes = 0, flushBytes = 0, sendFromBytes = 0, sendToBytes = 0;
                
                spmReadBytesFromLoad = 0, spmReadBytesFromLoadOnly = 0, spmReadBytesFromFetchLoadOnly = 0, spmReadBytesFromFlush = 0, spmReadBytesFromSend = 0;
                spmWriteBytesFromFetchLoad = 0, spmWriteBytesFromStore = 0, spmWriteBytesFromFetch = 0, spmWriteBytesFromSend = 0;

                dramReadBytes = 0, dramReadBytesOnlyFetch = 0, dramReadBytesOnlyFetchLoad = 0, dramReadBytesOnlyLoadBypass = 0;
                dramWriteBytes = 0, dramWriteBytesOnlyStore = 0, dramWriteBytesOnlyFlush = 0;

                noReqCycles = 0;
                cycles = 0;
            }
            inline void recordReqFromSlaveInpuePort(SPMMessage *msg) {
                if(msg->isLoad() or msg->isLoadBypass() or msg->isFetchLoad()) {
                    load++;
                    loadBytes += msg->size;
                    if(msg->isFetchLoad()) {
                        fetchLoadOnly ++;
                        fetchLoadOnlyBytes += msg->size;
                    } else {
                        loadOnly ++;
                        loadOnlyBytes += msg->size;
                    }
                } else if(msg->isStore() or msg->isStoreBypass()) {
                    store++;
                    storeBytes += msg->size;
                } else if(msg->isFetch()) {
                    fetch ++;
                    fetchBytes += msg->size;
                } else if(msg->isFlush()) {
                    flush++;
                    flushBytes += msg->size;
                } else if(msg->isSend()) {
                    sendFrom ++;
                    sendFromBytes += msg->size;
                } else if(msg->isS2S()) {
                    sendTo ++;
                    sendToBytes += msg->size;
                } else {
                    assert(false);
                }
            }
            inline void recordReadQueueMsg(SPMMessage *msg) {
                if(msg->isLoad() or msg->isFetchLoad()) {
                    spmReadBytesFromLoad += msg->size;
                    if(msg->isFetchLoad()) {
                        spmReadBytesFromFetchLoadOnly += msg->size;
                    } else {
                        spmReadBytesFromLoadOnly += msg->size;
                    }
                } else if(msg->isFlush()) {
                    spmReadBytesFromFlush += msg->size;
                } else if(msg->isSend()) {
                    spmReadBytesFromSend += msg->size;
                } else {
                    assert(false);
                }
            }

            inline void recordWriteQueueMsg(SPMMessage *msg) {
                if(msg->isFetchLoad()) {
                    spmWriteBytesFromFetchLoad += msg->size;
                } else if(msg->isStore()) {
                    spmWriteBytesFromStore += msg->size;
                } else if(msg->isFetch()) {
                    spmWriteBytesFromFetch += msg->size;
                } else if(msg->isS2S()) {
                    spmWriteBytesFromSend += msg->size;
                } else if(msg->isSend()) {
                    // send self
                    spmWriteBytesFromSend += msg->size;
                } else {
                    assert(false);
                }        
            }
            inline void recordDRAMMsg(SPMMessage *msg, MemCtrlMessage* memMsg) {
                if(msg->isLoadBypass() or msg->isFetchLoad()) {
                    dramReadBytes += msg->size;
                    if(msg->isLoadBypass()) {
                        dramReadBytesOnlyLoadBypass += msg->size;
                    } 
                    else {
                        dramReadBytesOnlyFetchLoad += msg->size;
                    }
                } else if(msg->isStoreBypass()) {
                    dramWriteBytes += msg->size;
                    dramWriteBytesOnlyStore += msg->size;
                } else if(msg->isFetch()) {
                    dramReadBytes += msg->size;
                    dramReadBytesOnlyFetch += msg->size;
                } else if(msg->isFlush()) {
                    dramWriteBytes += msg->size;
                    dramWriteBytesOnlyFlush += msg->size;
                } else {
                    assert(false);
                }
            }

            inline void tick() { cycles++; }

            void add(Counter &c) {
                load += c.load, loadOnly += c.loadOnly, fetchLoadOnly += c.fetchLoadOnly, store += c.store, fetch += c.fetch, flush += c.flush, sendFrom += c.sendFrom, sendTo += c.sendTo;
                loadBytes += c.loadBytes, loadOnlyBytes += c.loadOnlyBytes, fetchLoadOnlyBytes += c.fetchLoadOnlyBytes, storeBytes += c.storeBytes, fetchBytes += c.fetchBytes, flushBytes += c.flushBytes, sendFromBytes += c.sendFromBytes, sendToBytes += c.sendToBytes;
                spmReadBytesFromLoad += c.spmReadBytesFromLoad, spmReadBytesFromLoadOnly += spmReadBytesFromLoadOnly, spmReadBytesFromFetchLoadOnly += c.spmReadBytesFromFetchLoadOnly, spmReadBytesFromFlush += c.spmReadBytesFromFlush, spmReadBytesFromSend += c.spmReadBytesFromSend;
                spmWriteBytesFromFetchLoad += c.spmWriteBytesFromFetchLoad, spmWriteBytesFromStore += c.spmWriteBytesFromStore, spmWriteBytesFromFetch += c.spmWriteBytesFromFetch, spmWriteBytesFromSend += c.spmWriteBytesFromSend;
                dramReadBytes += c.dramReadBytes, dramReadBytesOnlyFetch += dramReadBytesOnlyFetch, dramReadBytesOnlyFetchLoad += dramReadBytesOnlyFetchLoad, dramReadBytesOnlyLoadBypass += dramReadBytesOnlyLoadBypass;
                dramWriteBytes += dramWriteBytes, dramWriteBytesOnlyStore += dramWriteBytesOnlyStore, dramWriteBytesOnlyFlush += dramWriteBytesOnlyFlush;

                noReqCycles += c.noReqCycles;
                cycles = std::max(cycles, c.cycles);
            }

            uint64 totalAccess() { return load + store + fetch + sendFrom + sendTo; }

            uint64 totalAccessBytes() { return loadBytes + storeBytes + fetchBytes + flushBytes + sendFromBytes + sendToBytes; }

            uint64 totalMemAccessBytes() { return dramReadBytes + dramWriteBytes; }

            // double getHitRate() { return (double) (totalAccess() - fetch) / totalAccess(); }

            // double getLineUtil(uint64 lineBytes) { return (double) totalAccessBytes() / (totalAccess() * lineBytes); }

            double getAvgBW() { return (double) totalAccessBytes() / cycles; }

            double getNoReqRate() { return (double) noReqCycles / cycles; }

            std::string getPerformanceInfo(uint64 lineBytes) {
                std::stringstream ss;
                // ss << "\tHit Rate: " << getHitRate() << "\n";
                // ss << "\tLine Utilization: " << getLineUtil(lineBytes) << "\n";
                // ss << "\tAverage Bandwidth (Bytes/Cycle): " << getAvgBW() << "\n";
                ss << "\tLoad Bytes: " << loadBytes << "\n";
                ss << "\tStore Bytes: " << storeBytes << "\n";
                ss << "\tfetch Bytes: " << fetchBytes << "\n";
                ss << "\tflush Bytes: " << flushBytes << "\n";
                ss << "\tSend Rom Bytes: " << sendFromBytes << "\n";
                ss << "\tSend To Bytes: " << sendToBytes << "\n";
                ss << "\tTotal Access Bytes: " << totalAccessBytes() << "\n";
                ss << "\tTotal Memory Access Bytes: " << totalMemAccessBytes() << "\n";
                ss << "\tNo Request Rate: " << getNoReqRate() << "\n";
#ifdef PERF_DETAIL_LOG 
                ss << "\tDetail Log:" << "\n";
                ss << "\tload=" << load << ", loadOnly=" << loadOnly << ", fetchLoadOnly=" << fetchLoadOnly << ", store=" << store << ", fetch=" << fetch << ", flush=" << flush << ", sendFrom=" << sendFrom << ", sendTo=" << sendTo << "\n";
                ss << "\tloadBytes=" << loadBytes << ", loadOnlyBytes=" << loadOnlyBytes << ", fetchLoadOnlyBytes=" << fetchLoadOnlyBytes << ", storeBytes=" << storeBytes << ", fetchBytes=" << fetchBytes << ", flushBytes=" << flushBytes << ", sendFromBytes=" << sendFromBytes << ", sendToBytes=" << sendToBytes << "\n";
                ss << "\tspmReadBytesFromLoad=" << spmReadBytesFromLoad << ", spmReadBytesFromLoadOnly=" << spmReadBytesFromLoadOnly << ", spmReadBytesFromFetchLoadOnly=" << spmReadBytesFromFetchLoadOnly << ", spmReadBytesFromFlush=" << spmReadBytesFromFlush << ", spmReadBytesFromSend=" << spmReadBytesFromSend << "\n";
                ss << "\tspmWriteBytesFromFetchLoad=" << spmWriteBytesFromFetchLoad << ", spmWriteBytesFromStore=" << spmWriteBytesFromStore << ", spmWriteBytesFromFetch=" << spmWriteBytesFromFetch << ", spmWriteBytesFromSend=" << spmWriteBytesFromSend <<"\n";
                ss << "\tdramReadBytes=" << dramReadBytes << ", dramReadBytesOnlyFetch=" << dramReadBytesOnlyFetch << ", dramReadBytesOnlyFetchLoad" << dramReadBytesOnlyFetchLoad << ", dramReadBytesOnlyLoadBypass=" << dramReadBytesOnlyLoadBypass << "\n";
                ss << "\tdramWriteBytes=" << dramWriteBytes << ", dramWriteBytesOnlyStore=" << dramWriteBytesOnlyStore << ", dramWriteBytesOnlyFlush=" << dramWriteBytesOnlyFlush << "\n";
#endif
                return ss.str();
            }
        };

        Counter counter;

        SPMBank(uint bankIdx, uint lineBytes, uint upstreamBeatBytes, uint downstreamBeatBytes,
                uint memCtrlChannels, bool multicast);

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
        uint bankIdx_;
        uint lineBytes_;
        uint upstreamBeatBytes_;
        uint downstreamBeatBytes_;
        uint64 alignedAddrMask_;
        bool multicast_;

        uint64 memCtrlChIdxMask;
        uint64 memCtrlChIdxOffset;

        Port *slavePort;
        Port *masterPort;

        // save flying Memory Access messages
        std::unordered_map<MemCtrlMessage *, SPMMessage *> flyingMemMsgMap;

        // for groupId synchronizing
        std::unordered_map<uint64, GroupedMessage> groupMap;

        // save messages waiting for accessing SRAM 
        QueueWithArbitor<SPMMessage *> sramReadFifoWithArbitor;
        QueueWithArbitor<SPMMessage *> sramWriteFifoWithArbitor;
        SPMMessage *currentSramReadMsg;
        SPMMessage *currentSramWriteMsg;
        uint64 sramReadCurrentBytes;
        uint64 sramWriteCurrentBytes;
        
        enum class SramReadFifoType {
            LOAD, SEND, FLUSH, COUNT
        };
        enum class SramWriteFifoType {
            STORE, FETCH, FETCH_LOAD, S2S, COUNT
        };

        // save SPM messages that need to access memory
        QueueWithArbitor<SPMMessage *> memAccessFifoWithArbitor;
        enum class MemAccessFifoType {
            FETCH, FETCH_LOAD, FLUSH, LOAD_BYPASS, STORE_BYPASS, COUNT
        };

        void issueMsgFromSlavePort(SPMMessage *msg);

        // burst to master output
        QueueWithArbitor<Message *> masterPortOutputFifoWithArbitor;
        enum class MasterPortOutputFifoType {
            MEM_ACCESS, 
            SRAM_READ,  // SPM_Send->read from sram->change to SPM_S2S->send to master output to another bank
            COUNT
        };

        // burst to slave output
        QueueWithArbitor<SPMMessage *> slavePortOutputFifoWithArbitor;
        enum class SlavePortOutputFifoType {
            STORE, 
            STORE_BYPASS, 
            FETCH, 
            FLUSH, 
            S2S_FROM_SRAM, // SPM_S2S->write to sram->response to slave output to origin spm bank master input->...
            S2S_FROM_MASTER_AS_SEND_OR_SEND_SELF, //another bank slave output(master input)->write to sram->change to SPM_SEND->response to slave output to origin acc;  or this bank slave input->read from sram->write to sram->reponse to slave output to origin acc
            LOAD_BYPASS_AFTER_MULTICAST, 
            FETCH_LOAD_AFTER_MULTICAST, 
            LOAD_AFTER_MULTICAST, 
            COUNT
        };


        // burst to multicast to slave port output
        QueueWithArbitor<SPMMessage *> slavePortBeforeMulticastFifoWithArbitor;
        enum class SlavePortBeforeMulticastFifoType {
            LOAD, 
            LOAD_BYPASS, 
            FETCH_LOAD, 
            COUNT
        };
    };


    class SPM : public TickModule {
    public:
        SPMConfig config;
        std::vector<SPMBank> banks;

        SPM(const SPMConfig &config_) {
            assert(config_.nBanks > 0 and isPowOfTwo(config_.nBanks));
            assert(config_.lineBytes > 0 and isPowOfTwo(config_.lineBytes) and
                   config_.lineBytes == MemCtrlConfig::DRAM_BURST_BYTES);
            assert(config_.upstreamBeatBytes > 0);
            assert(config_.downstreamBeatBytes > 0);
            assert(config_.memCtrlChannels > 0);

            config = config_;

            banks.reserve(config.nBanks);
            for (uint bankIdx = 0; bankIdx < config.nBanks; bankIdx++) {
                banks.emplace_back(bankIdx, config.lineBytes, config.upstreamBeatBytes, config.downstreamBeatBytes,
                                   config.memCtrlChannels, config.multicast);
            }

            printf("---- SPM ----\n"
                   "\tnBanks = %u\n"
                   "\tlineBytes = %u\n"
                   "\tupstreamBeatBytes = %u\n"
                   "\tdownstreamBeatBytes = %u\n"
                   "\tpageBytes = %u\n"
                   "\n",
                   config.nBanks, config.lineBytes,
                   config.upstreamBeatBytes, config.downstreamBeatBytes,
                   config.pageBytes);
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

        SPMBank::Counter getTotalCounter() {
            SPMBank::Counter total;
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


} // mudnac


#endif //MUDNACSIM_SPM_H
