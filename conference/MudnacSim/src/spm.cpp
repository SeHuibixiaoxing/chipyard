#include "spm.h"


namespace mudnac {


    SPMBank::SPMBank(uint bankIdx, uint lineBytes, uint upstreamBeatBytes, uint downstreamBeatBytes,
                     uint memCtrlChannels, bool multicast): 
                     sramReadFifoWithArbitor(QueueWithArbitor<SPMMessage*>::ArbitorType::RRArbitor, static_cast<int>(SramReadFifoType::COUNT)),
                     sramWriteFifoWithArbitor(QueueWithArbitor<SPMMessage*>::ArbitorType::RRArbitor, static_cast<int>(SramWriteFifoType::COUNT)),
                     memAccessFifoWithArbitor(QueueWithArbitor<SPMMessage*>::ArbitorType::RRArbitor, static_cast<int>(MemAccessFifoType::COUNT)),
                     masterPortOutputFifoWithArbitor(QueueWithArbitor<Message*>::ArbitorType::RRArbitor, static_cast<int>(MasterPortOutputFifoType::COUNT)),
                     slavePortOutputFifoWithArbitor(QueueWithArbitor<SPMMessage*>::ArbitorType::RRArbitor, static_cast<int>(SlavePortOutputFifoType::COUNT)),
                     slavePortBeforeMulticastFifoWithArbitor(QueueWithArbitor<SPMMessage*>::ArbitorType::RRArbitor, static_cast<int>(SlavePortBeforeMulticastFifoType::COUNT)) {
        bankIdx_ = bankIdx;
        lineBytes_ = lineBytes;
        upstreamBeatBytes_ = upstreamBeatBytes;
        downstreamBeatBytes_ = downstreamBeatBytes;
        alignedAddrMask_ = UINT64_MAX - (lineBytes_ - 1);
        multicast_ = multicast;

        memCtrlChIdxMask = memCtrlChannels - 1;
        memCtrlChIdxOffset = log2(lineBytes);

        slavePort = NULL;
        masterPort = NULL;

        currentSramReadMsg = NULL;
        currentSramWriteMsg = NULL;

        sramReadCurrentBytes = 0;
        sramWriteCurrentBytes = 0;

        LOGF(SPM_DEBUG_FLAG, "SPMBank::SPMBank %u, lineBytes=%u, upstreamBeatBytes=%u, downstreamBeatBytes=%u\n\n",
               bankIdx_, lineBytes_, upstreamBeatBytes_, downstreamBeatBytes_);
    }

    void SPMBank::tick() {
        // part1: receive a message from slave port input. 
        // If the message is part of a group, it is stored in a group map until all messages in the group are received.
//        const uint MAX_ISSUE_WIDTH = 1;
        const uint MAX_ISSUE_WIDTH = std::numeric_limits<uint>::max();
//        const uint MAX_ISSUE_WIDTH = 16384;


        bool noReq = true;
        if (!slavePort->input->empty()) {
            noReq = false;
        }
        for (uint i = 0; i < MAX_ISSUE_WIDTH; i++) {
            if (not slavePort->input->empty()) {
                SPMMessage *msg = dynamic_cast<SPMMessage*>(slavePort->input->front());
                counter.recordReqFromSlaveInpuePort(msg);
                slavePort->input->pop_front();
                bool canIssue = true;
                if (msg->groupSize > 1) {  // grouped message
                    assert(msg->isLoad() or msg->isLoadBypass() or msg->isSend() or msg->isFetchLoad());
                    canIssue = false;
                    if (not groupMap.count(msg->groupId)) {  // first message in this groupId
                        auto &group = groupMap[msg->groupId];
                        group.msgs.push_back(msg);
                        group.groupSize = msg->groupSize;
                        LOGF(SPM_DEBUG_FLAG, "%lu SPMBank::tick, %u, receive groupId message, groupId=%lu, groupSize=%u\n\n",
                           counter.cycles, bankIdx_, msg->groupId, msg->groupSize);
                    } else {
                        auto &group = groupMap[msg->groupId];
                        if (group.msgs.size() >= group.groupSize) {
                            std::cout << "Error: SPMBank::tick, bank " << bankIdx_ << ", groupId " << msg->groupId
                                      << " has already received all " << group.groupSize << " messages, but still receive more." << group.msgs.size()
                                      << std::endl;
                        }
                        assert(group.msgs.size() < group.groupSize);
                        if (group.groupSize != msg->groupSize) {
                            std::cout << "Error: SPMBank::tick, bank " << bankIdx_ << ", groupId " << msg->groupId
                                      << " has groupSize " << group.groupSize << ", but received message has groupSize "
                                      << msg->groupSize << std::endl;
                            msg->print(WZYDEBUG_FLAG);
                            std::cout << std::endl;
                        }
                        assert(group.groupSize == msg->groupSize);
                        group.msgs.push_back(msg);
                        LOGF(SPM_DEBUG_FLAG, "%lu SPMBank::tick, %u, receive groupId message, groupId=%lu, syn=%zu/%u\n\n",
                           counter.cycles, bankIdx_, msg->groupId, group.msgs.size(), msg->groupSize);
                        if (group.msgs.size() == group.groupSize) {
                            canIssue = true;
                            msg = group.getOne(); // chose on of the same group msg as the leader
                        }
                    }
                }
                if (canIssue) {
                    LOGF(SPM_DEBUG_FLAG, "%lu SPMBank::tick, bank %u, issue msg from slave port\n", counter.cycles, bankIdx_);
                    msg->print(SPM_DEBUG_FLAG);
                    LOGF(SPM_DEBUG_FLAG, "\n");
                    issueMsgFromSlavePort(msg);
                }
            } else {
                break;
            }
        }

        // part2: receive a message from master port input.
        for(uint i = 0;i < MAX_ISSUE_WIDTH; i++) {            
            if (not masterPort->input->empty()) {
                Message *originMsg = masterPort->input->front();
                masterPort->input->pop_front();  
                if(originMsg->metaType == Message::META_TYPE_MEMCTRL) {
                    // receive a finished mem message
                    auto memMsg = dynamic_cast<MemCtrlMessage*>(originMsg);
                    assert(flyingMemMsgMap.count(memMsg));
                    SPMMessage *msg = flyingMemMsgMap[memMsg];

                    LOGF(SPM_DEBUG_FLAG, "%lu SPMBank::tick, bank %u, receive memory msg from master port\n", counter.cycles, bankIdx_);
                    memMsg->print(SPM_DEBUG_FLAG);
                    LOGF(SPM_DEBUG_FLAG, "memMsg->msg:\n");
                    msg->print(SPM_DEBUG_FLAG);
                    LOGF(SPM_DEBUG_FLAG, "\n");

                    flyingMemMsgMap.erase(memMsg);
                    if (msg->isFetch()) {
                        sramWriteFifoWithArbitor.push(static_cast<int>(SramWriteFifoType::FETCH), msg); 
                    } else if(msg->isFetchLoad()){
                        sramWriteFifoWithArbitor.push(static_cast<int>(SramWriteFifoType::FETCH_LOAD), msg);
                    } else if(msg->isStoreBypass()) {
                        slavePortOutputFifoWithArbitor.push(static_cast<int>(SlavePortOutputFifoType::STORE_BYPASS), msg);
                    } else if(msg->isFlush()) {
                        slavePortOutputFifoWithArbitor.push(static_cast<int>(SlavePortOutputFifoType::FLUSH), msg);
                    } else if(msg->isLoadBypass()) {
                        slavePortBeforeMulticastFifoWithArbitor.push(static_cast<int>(SlavePortBeforeMulticastFifoType::LOAD_BYPASS), msg);
                    } else {
                        assert(false);
                    }
                    delete memMsg;
                } else if(originMsg->metaType == Message::META_TYPE_SPM) {
                    auto spmMsg = dynamic_cast<SPMMessage*>(originMsg);
                    assert(spmMsg->isS2S());
                    
                    LOGF(SPM_DEBUG_FLAG, "%lu SPMBank::tick, bank %u, receive spm msg from master port\n", counter.cycles, bankIdx_);
                    spmMsg->print(SPM_DEBUG_FLAG);
                    LOGF(SPM_DEBUG_FLAG, "\n");
                    
                    std::swap(spmMsg->m2sBytes, spmMsg->s2sSize);

                    auto accelId = spmMsg->dstSpmIdSPMSlave;
                    auto srcId = spmMsg->masterPortId;
                    auto dstId = spmMsg->slavePortId;

                    spmMsg->masterPortId = accelId;
                    spmMsg->slavePortId = PortIDManager::masterToSlave(srcId);
                    spmMsg->dstSpmIdSPMSlave = dstId;
                    
                    spmMsg->type = SPMMSG_SEND;
                    
                    LOGF(SPM_DEBUG_FLAG, "%lu SPMBank::tick, bank %u, translate a s2s msg from master input to send msg.\n", counter.cycles, bankIdx_);
                    spmMsg->print(SPM_DEBUG_FLAG);
                    LOGF(SPM_DEBUG_FLAG, "\n");

                    slavePortOutputFifoWithArbitor.push(static_cast<int>(SlavePortOutputFifoType::S2S_FROM_MASTER_AS_SEND_OR_SEND_SELF), spmMsg);
                }                
            } else {
                break;
            }
        }

        // part 3: break a memory accessing SPM message (into multiple dram messages)
        if (memAccessFifoWithArbitor.canPop()) {
            auto [msg, fifoType] = memAccessFifoWithArbitor.pop();
            assert(msg->type == SPMMSG_STORE_BYPASS or msg->type == SPMMSG_LOAD_BYPASS or msg->type == SPMMSG_FETCH or msg->type == SPMMSG_FETCH_LOAD or msg->type == SPMMSG_FLUSH);
            uint64 alignedAddr = msg->addr & alignedAddrMask_;
            bool isWrite = msg->isStoreBypass() or msg->isFlush();
            assert(msg->addr + msg->size <= alignedAddr + MemCtrlConfig::DRAM_BURST_BYTES);
            MemCtrlMessage *memMsg = new MemCtrlMessage(alignedAddr, isWrite);
            memMsg->setTrans(isWrite ? 1 : lineBytes_,
                             isWrite ? lineBytes_ : 1,
                             PortIDManager::localToGlobalSPMMaster(bankIdx_),
                             PortIDManager::localToGlobalMEMSlave((memMsg->addr >> memCtrlChIdxOffset) & memCtrlChIdxMask));
            counter.recordDRAMMsg(msg, memMsg);
            masterPortOutputFifoWithArbitor.push(static_cast<int>(MasterPortOutputFifoType::MEM_ACCESS), memMsg);
            flyingMemMsgMap[memMsg] = msg;
            
            LOGF(SPM_DEBUG_FLAG, "%lu SPMBank::tick, bank %u, break a memory accessing SPM message\n", counter.cycles, bankIdx_);
            msg->print(SPM_DEBUG_FLAG);
            LOGF(SPM_DEBUG_FLAG, "corresponding dram message:\n");
            memMsg->print(SPM_DEBUG_FLAG);
            LOGF(SPM_DEBUG_FLAG, "\n");
            
        }

        // part4: read SRAM
        if(currentSramReadMsg != NULL) {
            sramReadCurrentBytes += lineBytes_;
            if(sramReadCurrentBytes >= currentSramReadMsg->size) {
                
                LOGF(SPM_DEBUG_FLAG, "%lu SPMBank::tick, bank %u, finish a read sram message\n", counter.cycles, bankIdx_);
                currentSramReadMsg->print(SPM_DEBUG_FLAG);
                LOGF(SPM_DEBUG_FLAG, "\n");
                
                counter.recordReadQueueMsg(currentSramReadMsg);
                if(currentSramReadMsg->isLoad()) {
                    slavePortBeforeMulticastFifoWithArbitor.push(static_cast<int>(SlavePortBeforeMulticastFifoType::LOAD), currentSramReadMsg);
                } else if(currentSramReadMsg->isFlush()) {
                    memAccessFifoWithArbitor.push(static_cast<int>(MemAccessFifoType::FLUSH), currentSramReadMsg);
                } else if(currentSramReadMsg->isSend()) {
                    if(currentSramReadMsg->slavePortId == currentSramReadMsg->dstSpmIdSPMSlave) {
                        sramWriteFifoWithArbitor.push(static_cast<int>(SramWriteFifoType::S2S), currentSramReadMsg);
                    } else {
                        std::swap(currentSramReadMsg->m2sBytes, currentSramReadMsg->s2sSize);

                        auto accelId = currentSramReadMsg->masterPortId;
                        auto srcId = currentSramReadMsg->slavePortId;
                        auto dstId = currentSramReadMsg->dstSpmIdSPMSlave;

                        currentSramReadMsg->masterPortId = PortIDManager::slaveToMaster(srcId);
                        assert(PortIDManager::globalToLocalSPM(currentSramReadMsg->masterPortId) == this->bankIdx_);
                        currentSramReadMsg->slavePortId = dstId;
                        currentSramReadMsg->dstSpmIdSPMSlave = accelId;
                        
                        currentSramReadMsg->type = SPMMSG_S2S;
                        masterPortOutputFifoWithArbitor.push(static_cast<int>(MasterPortOutputFifoType::SRAM_READ), currentSramReadMsg);
                    }

                    LOGF(SPM_DEBUG_FLAG, "%lu SPMBank::tick, bank %u, translate a send msg after sram read to s2s msg.\n", counter.cycles, bankIdx_);
                    currentSramReadMsg->print(SPM_DEBUG_FLAG);
                    LOGF(SPM_DEBUG_FLAG, "\n");

                } else {
                    assert(false);
                }
                sramReadCurrentBytes = 0;
                currentSramReadMsg = NULL;
            }
        }
        if(currentSramReadMsg == NULL && sramReadFifoWithArbitor.canPop()) {
            auto [msg, fifoType] = sramReadFifoWithArbitor.pop();
            assert(fifoType != -1);
            currentSramReadMsg = msg;
        }

        // part5: write SRAM
        if(currentSramWriteMsg != NULL) {
            sramWriteCurrentBytes += lineBytes_;
            if(sramWriteCurrentBytes >= currentSramWriteMsg->size) {

                LOGF(SPM_DEBUG_FLAG, "%lu SPMBank::tick, bank %u, finish a write sram message\n", counter.cycles, bankIdx_);
                currentSramWriteMsg->print(SPM_DEBUG_FLAG);
                LOGF(SPM_DEBUG_FLAG, "\n");

                counter.recordWriteQueueMsg(currentSramWriteMsg);
                if(currentSramWriteMsg->isStore()) {
                    slavePortOutputFifoWithArbitor.push(static_cast<int>(SlavePortOutputFifoType::STORE), currentSramWriteMsg);
                } else if(currentSramWriteMsg->isFetch()) {
                    slavePortOutputFifoWithArbitor.push(static_cast<int>(SlavePortOutputFifoType::FETCH), currentSramWriteMsg);
                } else if(currentSramWriteMsg->isFetchLoad()) {
                    slavePortBeforeMulticastFifoWithArbitor.push(static_cast<int>(SlavePortBeforeMulticastFifoType::FETCH_LOAD), currentSramWriteMsg);                    
                } else if(currentSramWriteMsg->isS2S()) {
                    slavePortOutputFifoWithArbitor.push(static_cast<int>(SlavePortOutputFifoType::S2S_FROM_SRAM), currentSramWriteMsg);
                } else if(currentSramWriteMsg->isSend()) {
                    slavePortOutputFifoWithArbitor.push(static_cast<uint>(SlavePortOutputFifoType::S2S_FROM_MASTER_AS_SEND_OR_SEND_SELF), currentSramWriteMsg);
                } else {
                    assert(false);
                }                
                sramWriteCurrentBytes = 0;
                currentSramWriteMsg = NULL;
            }
        }      
        if(currentSramWriteMsg == NULL && sramWriteFifoWithArbitor.canPop()) {
            auto [msg, fifoType] = sramWriteFifoWithArbitor.pop();
            assert(fifoType != -1);
            currentSramWriteMsg = msg;
        }

        // part6: multicast
        if(slavePortBeforeMulticastFifoWithArbitor.canPop()) {
            auto [msg, fifoType] = slavePortBeforeMulticastFifoWithArbitor.pop();
            assert(fifoType != -1);
            assert(msg->isLoad() or msg->isLoadBypass() or msg->isFetchLoad());

            LOGF(SPM_DEBUG_FLAG, "%lu SPMBank::tick, bank %u, send a multicast message to slave port\n", counter.cycles, bankIdx_);
            msg->print(SPM_DEBUG_FLAG);
            LOGF(SPM_DEBUG_FLAG, "\n");

            std::function<void(SPMMessage*)> pushToArbitor = nullptr;            
            if(msg->isLoadBypass()) {
                pushToArbitor = [&](SPMMessage *msg_){
                    slavePortOutputFifoWithArbitor.push(static_cast<int>(SlavePortOutputFifoType::LOAD_BYPASS_AFTER_MULTICAST),msg_);
                };
            } else if(msg->isLoad()) {
                pushToArbitor = [&](SPMMessage *msg_){
                    slavePortOutputFifoWithArbitor.push(static_cast<int>(SlavePortOutputFifoType::LOAD_AFTER_MULTICAST),msg_);
                };
            } else if(msg->isFetchLoad()) {
                pushToArbitor = [&](SPMMessage *msg_){
                    slavePortOutputFifoWithArbitor.push(static_cast<int>(SlavePortOutputFifoType::FETCH_LOAD_AFTER_MULTICAST),msg_);
                };         
            } else {
                assert(false);
            }

            if (msg->groupSize > 1) {
                LOGF(SPM_DEBUG_FLAG, "%lu SPMBank::tick, %u, multicast return, type=%u, addr=%#lx, size=%#lx, groupId=%lu\n\n",
                       counter.cycles, bankIdx_, msg->type, msg->addr, msg->size, msg->groupId);
                assert(groupMap.count(msg->groupId));
                auto &group = groupMap[msg->groupId];

                if (not multicast_) {
                    for (auto m: group.msgs) {
                        pushToArbitor(m); 
                    }
                } else {
                    SPMMessage *multicastMsg = new SPMMessage();
                    multicastMsg->s2mBytes = group.msgs.front()->s2mBytes;
                    for (auto m: group.msgs) {
                        multicastMsg->multicastMsgs.push_back(m);
                    }                    
                    pushToArbitor(multicastMsg);
                }
                groupMap.erase(msg->groupId);
            } else {
                pushToArbitor(msg);
            }
        }

        // part 7: slave port output burst
        if(slavePortOutputFifoWithArbitor.canPop()) {
            auto [msg, fifoType] = slavePortOutputFifoWithArbitor.pop();
            assert(fifoType != -1);
            slavePort->output->push_back(msg);
        }

        // part 8: master port output burst
        if(masterPortOutputFifoWithArbitor.canPop()) {
            auto [msg, fifoType] = masterPortOutputFifoWithArbitor.pop();
            assert(fifoType != -1);
            masterPort->output->push_back(msg);
        }

        // part 9: update some counters
        if(noReq) {
            counter.noReqCycles++;
        }

        //part 10: tick
        counter.tick();
        sramReadFifoWithArbitor.tick();
        sramWriteFifoWithArbitor.tick();
        slavePortBeforeMulticastFifoWithArbitor.tick();
        slavePortOutputFifoWithArbitor.tick();
        masterPortOutputFifoWithArbitor.tick();
        memAccessFifoWithArbitor.tick();
    }

    void SPMBank::issueMsgFromSlavePort(SPMMessage *msg) {
        if(msg->isLoad()) {
            sramReadFifoWithArbitor.push(static_cast<int>(SramReadFifoType::LOAD), msg);
        } else if(msg->isSend()) {
            sramReadFifoWithArbitor.push(static_cast<int>(SramReadFifoType::SEND), msg);
        } else if(msg->isFlush()) {
            sramReadFifoWithArbitor.push(static_cast<int>(SramReadFifoType::FLUSH), msg);
        } else if(msg->isStore()) {
            sramWriteFifoWithArbitor.push(static_cast<int>(SramWriteFifoType::STORE), msg);
        } else if(msg->isS2S()) {
            sramWriteFifoWithArbitor.push(static_cast<int>(SramWriteFifoType::S2S), msg);
        } else if(msg->isFetch()) {
            memAccessFifoWithArbitor.push(static_cast<int>(MemAccessFifoType::FETCH), msg);
        } else if(msg->isFetchLoad()) {
            memAccessFifoWithArbitor.push(static_cast<int>(MemAccessFifoType::FETCH_LOAD), msg);
        } else if(msg->isStoreBypass()) {
            memAccessFifoWithArbitor.push(static_cast<int>(MemAccessFifoType::STORE_BYPASS), msg);
        } else if(msg->isLoadBypass()) {
            memAccessFifoWithArbitor.push(static_cast<int>(MemAccessFifoType::LOAD_BYPASS), msg);
        } 
        else {
            assert(false);
        }
    }

} // mudnac