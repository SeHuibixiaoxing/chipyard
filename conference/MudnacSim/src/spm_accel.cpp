#include "spm_accel.h"


namespace mudnac {
    bool SPMAccel::print_info = true;

    SPMAccel::SPMAccel(const AccelConfig &config_)
            : BaseAccel(config_),
              pageTable(log2(config_.pageBytes), config.accId),
              queueWithArbitor(QueueWithArbitor<SPMMessage*>::ArbitorType::RRArbitor,8) {
        cpuPortInput = NULL;
        cpuPortOutput = NULL;
        masterPort = NULL;

        computeCMD = NULL;
        loadCMD = NULL;
        storeCMD = NULL;
        waitedComputeCMD = NULL;
        waitedLoadCMD = NULL;
        waitedStoreCMD = NULL;

        bankIdxOffset = log2(config.maxBurstBytes);
        bankIdxMask = config.nBanks - 1;

        loadSpmMsgCreationDone = false;
        flyingLoadSpmMsgCount = 0;

        storeSpmMsgCreationDone = false;
        flyingStoreSpmMsgCount = 0;

        spmSendCmdCreationDone = false;
        flyingSendSpmMsgCount = 0;

        spmFetchCmdCreationDone = false;
        flyingFetchSpmMsgCount = 0;

        spmFlushCmdCreationDone = false;
        flyingFlushSpmMsgCount = 0;

        computeCurrentCycles = 0;
        computeTotalCycles = 0;

        ptCMD = NULL;

        sendCMD = NULL;
        fetchCMD = NULL;
        flushCMD = NULL;

        ts.startCycles = UINT64_MAX;

        assert((config.nPages % config.nBanks) == 0);
        pagesPerBank = config.nPages / config.nBanks;
        assert((config.numAccels % config.nBanks) == 0);
        accelsPerBank = config.numAccels / config.nBanks;
        
        if (print_info) {
            printf("---- SPMAccel ----\n"
                "\taccId = %u\n"
                "\tarray = %u x %u\n"
                "\tbeatBytes = %u\n"
                "\tmaxBurstBytes = %u\n"
                "\tnumPages = %u\n"
                "\tpageBytes = %u\n"
                "\tpagesPerBank = %u\n"
                "\taccelsPerBank = %u\n"
                "\tpageForBypass = %u\n"
                "\n",
                config.accId, config.arrayH, config.arrayW, config.beatBytes, config.maxBurstBytes,
                config.nPages, config.pageBytes, pagesPerBank, accelsPerBank, pageTable.pageForBypass);
            
            print_info = false;
        }
    }

    /**
     * SPMAccel's tick function.
     *
     * This function is the main entry of SPMAccel's cycle-accurate simulation.
     * It has 8 parts:
     * 1. fetch next cmd
     * 2. load
     * 3. store
     * 4. handle returned spm messages
     * 5. compute
     * 6. synchronized update
     * 7. burst downstream
     * 8. set page table
     *
     * * cmd synchronization:
     * load: cmd3, compute: cmd2, store: cmd1
     * after fetch cmd: waited compute = cmd4
     * after load: waited compute = cmd3
     * after compute: waited store = cmd2
     * ========barrier===================
     * finish: cmd1
     * load: cmd4
     * compute: cmd3
     * store: cmd2
     * 
     * @return void
     */
    void SPMAccel::tick() {
//        if (counter.cycles % 10000 == 0) {
//            printf("%lu SPMAccel::tick %d: waiting tiles = %zu, state = [%d,%d,%d]\n",
//                   counter.cycles, config.accId, cpuPortInput->size(),
//                   loadCMD == NULL, computeCMD == NULL, storeCMD == NULL);
//        }

        // part 1: fetch next cmd
        cmdFetch();
        
        // part 2: update cmd cycle counts
        updateCmdCountr();

        // part 3. loadCMD
        if(loadCMD != NULL) {
            bool loadCMDFinish = tickCMD(loadCMD, loadSpmMsgCreationDone, flyingLoadSpmMsgCount, FROM_LOAD_CMD, this->counter.idealLoadBytes, this->counter.realLoadBytes);
            if(loadCMDFinish) {
                waitedComputeCMD = loadCMD; 
                loadCMD = NULL;
            }
#ifdef SPM_ACCEL_DBG
            printf("loadCMD: finish=%u, loadSpmMsgCreationDone=%u, flyingLoadSpmMsgCount=%lu\n", loadCMDFinish, (uint)loadSpmMsgCreationDone, flyingLoadSpmMsgCount);
            printf("\n");
#endif
        }

        // part 4. fetchCMD
        if(fetchCMD != NULL) {
            bool fetchCMDFinish = tickCMD(fetchCMD, spmFetchCmdCreationDone, flyingFetchSpmMsgCount, FROM_FETCH_CMD, this->counter.idealFetchBytes, this->counter.realFetchBytes);

            if(fetchCMDFinish) {          
                cpuPortOutput->push_back(fetchCMD);
                fetchTs.endCycles = counter.cycles;
                counter.fetchQueue.push_back(fetchTs);
                fetchTs.clear();
                fetchCMD = NULL;
            }
#ifdef SPM_ACCEL_DBG
            printf("fetchCMD: finish=%u, loadSpmMsgCreationDone=%u, flyingLoadSpmMsgCount=%lu\n", fetchCMDFinish, (uint)spmFetchCmdCreationDone, flyingFetchSpmMsgCount);
            printf("\n");
#endif
        }

        // part 5. storeCMD
        if(storeCMD != NULL) {
            bool storeCMDFinish = tickCMD(storeCMD, storeSpmMsgCreationDone, flyingStoreSpmMsgCount, FROM_STORE_CMD, this->counter.idealStoreBytes, this->counter.realStoreBytes);            
            if(storeCMDFinish) {
                cpuPortOutput->push_back(storeCMD);
                storeCMD = NULL;
            }
#ifdef SPM_ACCEL_DBG
            printf("storeCMD: finish=%u, storeSpmMsgCreationDone=%u, flyingStoreSpmMsgCount=%lu\n", storeCMDFinish, (uint)storeSpmMsgCreationDone, flyingStoreSpmMsgCount);
            printf("\n");
#endif
        }

        // part 6. flushCMD
        if(flushCMD != NULL) {
            bool flushCMDFinish = tickCMD(flushCMD, spmFlushCmdCreationDone, flyingFlushSpmMsgCount, FROM_FLUSH_CMD, this->counter.idealFlushBytes, this->counter.realFlushBytes);
            if(flushCMDFinish) {
                cpuPortOutput->push_back(flushCMD);
                flushTs.endCycles = counter.cycles;
                counter.sendFlushQueue.push_back(fetchTs);
                flushTs.clear();
                flushCMD = NULL;
            }
#ifdef SPM_ACCEL_DBG
        printf("flushCMD: finish=%u, spmFlushCmdCreationDone=%u, flyingFlushSpmMsgCount=%lu\n", flushCMDFinish, (uint)spmFlushCmdCreationDone, flyingFlushSpmMsgCount);
        printf("\n");
#endif
        }
        // part 7. sendCMD
        if(sendCMD != NULL) {
            bool sendCMDFinish = tickCMD(sendCMD, spmSendCmdCreationDone, flyingSendSpmMsgCount, FROM_SEND_CMD, this->counter.idealSendBytes, this->counter.realSendBytes);
            if(sendCMDFinish) {
#ifdef SPM_ACCEL_DBG
                printf("%lu SPMAccel::tick %u, finish sendCMD, cycles=%lu\n",
                            counter.cycles, config.accId, computeTotalCycles);          
                printf("\n");
#endif
                cpuPortOutput->push_back(sendCMD);
                sendTs.endCycles = counter.cycles;
                counter.sendFlushQueue.push_back(sendTs);
                sendTs.clear();
                sendCMD = NULL;
            }
#ifdef SPM_ACCEL_DBG
            printf("sendCMDFinish: finish=%u, spmSendCmdCreationDone=%u, flyingSendSpmMsgCount=%lu\n", sendCMDFinish, (uint)spmSendCmdCreationDone, flyingSendSpmMsgCount);
            printf("\n");
#endif
        }

        // part 8. handle returned spm messages from master input port
        if (not masterPort->input->empty()) {
            auto msg = dynamic_cast<SPMMessage*>(masterPort->input->front());
#ifdef SPM_ACCEL_DBG
                printf("%lu SPMAccel::tick %u, receive a msg, cycles=%lu\n",
                       counter.cycles, config.accId, computeTotalCycles);                
                msg->print();
                printf("flyingStoreSpmMsgCount:%lu, flyingLoadSpmMsgCount:%lu, flyingFetchSpmMsgCount:%lu, flyingSendSpmMsgCount:%lu, flyingFlushSpmMsgCount:%lu", flyingStoreSpmMsgCount, flyingLoadSpmMsgCount, flyingFetchSpmMsgCount, flyingSendSpmMsgCount, flyingFlushSpmMsgCount);
#endif
            masterPort->input->pop_front();
            if (msg->isStore() or msg->isStoreBypass()) {  // store
                assert((storeCMD != NULL) and (flyingStoreSpmMsgCount > 0));
                -- flyingStoreSpmMsgCount;
            } else if(msg->isLoad() or msg->isLoadBypass() or msg->isFetchLoad()) {  // load
                assert((loadCMD != NULL) and (flyingLoadSpmMsgCount > 0));
                --flyingLoadSpmMsgCount;
            } else if(msg->isFetch()) {
                assert((fetchCMD != NULL) and (flyingFetchSpmMsgCount > 0));
                --flyingFetchSpmMsgCount;
            } else if(msg->isSend()) {
                assert((sendCMD != NULL) and (flyingSendSpmMsgCount > 0));
                --flyingSendSpmMsgCount;
            } else if(msg->isFlush()) {
                assert((flushCMD != NULL) and (flyingFlushSpmMsgCount > 0));
                --flyingFlushSpmMsgCount;
            } else {
                assert(false);
            }
            delete msg;
#ifdef SPM_ACCEL_DBG
            printf("flyingStoreSpmMsgCount:%lu, flyingLoadSpmMsgCount:%lu, flyingFetchSpmMsgCount:%lu, flyingSendSpmMsgCount:%lu, flyingFlushSpmMsgCount:%lu", flyingStoreSpmMsgCount, flyingLoadSpmMsgCount, flyingFetchSpmMsgCount, flyingSendSpmMsgCount, flyingFlushSpmMsgCount);
            printf("\n");
#endif
        }

        // part 9: compute
        if (computeCMD != NULL) {
            if (computeTotalCycles == 0) {  // a new compute is arrived
                computeTotalCycles = std::max((uint64) 1, computeCMD->compute(config));
#ifdef SPM_ACCEL_DBG
                printf("%lu SPMAccel::tick %u, start compute, cycles=%lu\n",
                       counter.cycles, config.accId, computeTotalCycles);
                printf("\n");
#endif
            } else {  // wait until compute is done
                if (computeCurrentCycles == computeTotalCycles - 1) {  // compute is done
                    waitedStoreCMD = computeCMD;
                    computeCMD = NULL;
                    computeTotalCycles = 0;
                    computeCurrentCycles = 0;
#ifdef SPM_ACCEL_DBG
                    printf("%lu SPMAccel::tick %u, end compute\n", counter.cycles, config.accId);
                    printf("\n");
#endif
                } else {
                    computeCurrentCycles++;
                }
            }
        }

        // part 10: synchronized update
        if (loadCMD == NULL and computeCMD == NULL and storeCMD == NULL) {
            // save time slice
            if (ts.startCycles != UINT64_MAX) {
                ts.endCycles = counter.cycles;
                counter.tsQueue.push_back(ts);
                ts.startCycles = UINT64_MAX;
            }
            // update CMDs
            computeCMD = waitedComputeCMD;
            loadCMD = waitedLoadCMD;
            storeCMD = waitedStoreCMD;
            waitedComputeCMD = NULL;
            waitedLoadCMD = NULL;
            waitedStoreCMD = NULL;
            // update time slice
            if (computeCMD != NULL or loadCMD != NULL or storeCMD != NULL) {
                ts.clear();
                ts.startCycles = counter.cycles;
            }
#ifdef SPM_ACCEL_DBG
            if (loadCMD != NULL or computeCMD != NULL or storeCMD != NULL) {
                printf("%lu SPMAccel::tick %u, synchronized update, load=%u, compute=%u, store=%u\n",
                       counter.cycles, config.accId, loadCMD != NULL, computeCMD != NULL, storeCMD != NULL);
                printf("\n");
            }
#endif
        }

        // part 11: burst master output
        if(queueWithArbitor.canPop()) {
            auto [msg, fifoType] = queueWithArbitor.pop();
            assert(fifoType != -1);
#ifdef SPM_ACCEL_DBG
            printf("queueWithArbitor: fifoType=%u\n", fifoType);
            msg->print();
            printf("\n");
#endif
            masterPort->output->push_back(msg);
            if(msg->isLoadBypass() or msg->isStoreBypass() or msg->isFetchLoad() or msg->isFetch() or msg->isFlush()) {
                memAccessCtrlBytesCounter += msg->size;
            }

        }


        // part 12: set page table
        if (ptCMD != NULL) {
            auto *cmd = dynamic_cast<AccelSetPageTable *>(ptCMD);
            pageTable.set(cmd->vPage, cmd->pPage, cmd->valid);
            cpuPortOutput->push_back(ptCMD);
#ifdef SPM_ACCEL_DBG
            printf("%lu SPMAccel::tick %u, set page table, vpage=%#x, ppage=%#x, valid=%u\n",
                   counter.cycles, config.accId, cmd->vPage, cmd->pPage, cmd->valid);
            printf("\n");
#endif
            ptCMD = NULL;
        }

        // part 13: for MoCA
        if (memAccessCtrlCycleCounter + 1 >= memAccessCtrlEpoch) {
            memAccessCtrlCycleCounter = 0;
            memAccessCtrlBytesCounter = 0;

            queueWithArbitor.enable(MASTERPORT_FIFO_LOAD_BYPASS);
            queueWithArbitor.enable(MASTERPORT_FIFO_STORE_BYPASS);
            queueWithArbitor.enable(MASTERPORT_FIFO_FETCH_LOAD);
            queueWithArbitor.enable(MASTERPORT_FIFO_FLUSH);
            queueWithArbitor.enable(MASTERPORT_FIFO_FETCH);            
        } else {
            if(memAccessCtrlBytesCounter >= memAccessCtrlMaxReqs) {
                queueWithArbitor.disable(MASTERPORT_FIFO_LOAD_BYPASS);
                queueWithArbitor.disable(MASTERPORT_FIFO_STORE_BYPASS);
                queueWithArbitor.disable(MASTERPORT_FIFO_FETCH_LOAD);
                queueWithArbitor.disable(MASTERPORT_FIFO_FLUSH);
                queueWithArbitor.disable(MASTERPORT_FIFO_FETCH);
            }
            memAccessCtrlCycleCounter++;
        }

        counter.tick();
        queueWithArbitor.tick();
    }
    
    void SPMAccel::cmdFetch()
    {
        if ((not cpuPortInput->empty()) and ptCMD == NULL) {
            auto cmd = cpuPortInput->front();
            if (typeid(*cmd) == typeid(AccelSetPageTable)) {  // page table CMD first
                ptCMD = cmd;
                cpuPortInput->pop_front();
            } else if (typeid(*cmd) == typeid(AccelSPMSend) && sendCMD == NULL) {
                sendCMD = cmd;
                sendTs.startCycles = counter.cycles;
                cpuPortInput->pop_front();
            } else if (typeid(*cmd) == typeid(AccelSPMFetch) && fetchCMD == NULL) {
                fetchCMD = cmd;
                fetchTs.startCycles = counter.cycles;
                cpuPortInput->pop_front();
            } else if (typeid(*cmd) == typeid(AccelSPMFlush) && sendCMD == NULL && flushCMD == NULL) {
                flushCMD = cmd;
                flushTs.startCycles = counter.cycles;
                cpuPortInput->pop_front();
            } else if(waitedLoadCMD == NULL) {                
                waitedLoadCMD = cmd;
                cpuPortInput->pop_front();
            } 
        }

    }

    void SPMAccel::updateCmdCountr()
    {
        if (loadCMD != NULL) {  
            ts.loadCycles++;
        }
        if (storeCMD != NULL) {
            ts.storeCycles++;
        }
        if (computeCMD != NULL) {
            ts.computeCycles++;
        }
        if (sendCMD != NULL) {
            sendTs.storeCycles++;
        } 
        if (flushCMD != NULL) {
            flushTs.storeCycles++;
        }
        if (fetchCMD != NULL) {
            fetchTs.loadCycles++;
        }
    }

    // TODO:强制内联以优化性能
    bool SPMAccel::tickCMD(AccelTiledCMD* cmd, bool& msgCreationDone, uint64& flyingSpmMsgCount, uint cmdSrc, uint64& idealBytes, uint64& realBytes) {
        assert(cmd != NULL);
        assert(cmdSrc == FROM_LOAD_CMD || cmdSrc == FROM_FETCH_CMD || cmdSrc == FROM_STORE_CMD || cmdSrc == FROM_FLUSH_CMD || cmdSrc == FROM_SEND_CMD); 
        bool finish = false;
        if (not msgCreationDone) { // need to create spm messages.
            // create spm messages
            assert(flyingSpmMsgCount == 0);
            bool needInput, needOutput, needBoth;
            needInput = needOutput = needBoth = false;

            if(cmdSrc == FROM_LOAD_CMD || cmdSrc == FROM_FETCH_CMD) {
                needInput = true;
            }
            else if(cmdSrc == FROM_STORE_CMD || cmdSrc == FROM_FLUSH_CMD) {
                needOutput = true;
            } else if(cmdSrc == FROM_SEND_CMD) {
                needBoth = true;
            } else {
                assert(false);
            }

            int tensorNum;
            if(needInput) {
                tensorNum = cmd->numInputs;
            } else if(needOutput) {
                tensorNum = cmd->numOutputs;
            } else if(needBoth) {
                tensorNum = cmd->numInputs;
                assert((cmd->numInputs) == (cmd->numOutputs));
            } else {
                assert(false);
            }

            for (int i = 0; i < tensorNum; i++) { // process each input
                // if reuse, not need to read from spm
                if (needInput && cmd->inputReuse[i]) { continue; } 
                if (needOutput && cmd->outputReuse[i]) { continue; }      

                SPMTensor *spmTensorPtr = NULL, *spmTensorDstPtr = NULL;
                if (needInput) spmTensorPtr = &(cmd->inputSPMTensors[i]);
                else if (needOutput) spmTensorPtr = &(cmd->outputSPMTensors[i]);
                else if(needBoth) spmTensorPtr = &(cmd->inputSPMTensors[i]), spmTensorDstPtr = &(cmd->outputSPMTensors[i]);                
                else assert(false);

                auto &spmTensor = *spmTensorPtr;
                auto &spmTensorDst = *spmTensorDstPtr;

                uint spmMsgType;
                if(cmdSrc == FROM_LOAD_CMD) {
                    spmMsgType = spmTensor.bypass ? SPMMSG_LOAD_BYPASS :
                                      (spmTensor.reuse ? SPMMSG_LOAD : SPMMSG_FETCH_LOAD);
                } else if(cmdSrc == FROM_FETCH_CMD){
                    spmMsgType = SPMMSG_FETCH;
                } else if(cmdSrc == FROM_STORE_CMD) {
                    spmMsgType = spmTensor.bypass ? SPMMSG_STORE_BYPASS : SPMMSG_STORE;
                } else if(cmdSrc == FROM_FLUSH_CMD) {
                    spmMsgType = SPMMSG_FLUSH;
                } else if(cmdSrc == FROM_SEND_CMD) {
                    spmMsgType = SPMMSG_SEND;
                } else {
                    assert(false);
                }

                Tensor::BurstQue bQue;
                std::deque<uint64> spmAddrQue, spmAddrQueDst;
                if (cmdSrc == FROM_LOAD_CMD){
                    if(spmTensor.bypass) { // if bypass, load from dram to accel directly, no need for spm address.
                        idealBytes += cmd->load(i, this->config, bQue);
                    } else {
                        idealBytes += cmd->load(i, this->config, bQue, spmAddrQue);
                    }
                } else if(cmdSrc == FROM_FETCH_CMD){
                    idealBytes += cmd->load(i, this->config, bQue, spmAddrQue);                    
                } else if(cmdSrc == FROM_STORE_CMD) {
                    if (spmTensor.bypass) {
                        idealBytes += cmd->store(i, this->config, bQue);
                    } else {
                        idealBytes += cmd->store(i, this->config, bQue, spmAddrQue);
                    }
                } else if(cmdSrc == FROM_FLUSH_CMD) {
                    idealBytes += cmd->store(i, this->config, bQue, spmAddrQue);                    
                } else if(cmdSrc == FROM_SEND_CMD) {
                    idealBytes += cmd->send(i, this->config, bQue, spmAddrQue, spmAddrQueDst);
                } else {
                    assert(false);
                }

                uint64 groupIdOffset = ((uint64) spmTensor.groupId) << 32;  // group is splited by bursts.
                for (int j = 0; j < bQue.size(); j++) {
                    uint64 addr = bQue[j][0], size = bQue[j][1];
                    uint page = 0, pSpmAddr = 0;
                    if (spmTensor.bypass and (cmdSrc == FROM_STORE_CMD or cmdSrc == FROM_LOAD_CMD)) {
                        page = this->pageTable.pageForBypass;
                        pSpmAddr = this->pageTable.concat(page, this->pageTable.getOffset(addr));
                    } else {
                        page = this->pageTable.getPPage(spmAddrQue[j]);
                        pSpmAddr = this->pageTable.concat(page, this->pageTable.getOffset(spmAddrQue[j]));
                    }
                    realBytes += size;
                    SPMMessage *msg = new SPMMessage(spmMsgType, page, size, addr, groupIdOffset + j, spmTensor.groupSize);
                    uint bankIdx = (this->config.spmAddrType == this->config.SPM_ADDR_TYPE_BLOCK_INTERLEAVED) ?
                                    ((pSpmAddr >> (this->bankIdxOffset)) & (this->bankIdxMask)) :
                                    (config.spmAddrType == config.SPM_ADDR_TYPE_PAGE_INTERLEAVED) ?
                                    (page % config.nBanks) :
                                    (page / (this->pagesPerBank));
                    uint64 s2mBytes, m2sBytes;
                    if(cmdSrc == FROM_LOAD_CMD) {
                        s2mBytes = std::max((uint64) 1, size);
                        m2sBytes = 1;
                    } 
                    else if(cmdSrc == FROM_STORE_CMD) {
                        s2mBytes = 1;
                        m2sBytes = std::max(1ul, size);
                    } else {
                        s2mBytes = 1;
                        m2sBytes = 1;
                    }
                    msg->setTrans(s2mBytes, m2sBytes, PortIDManager::localToGlobalAccelMaster(config.accId),  PortIDManager::localToGlobalSPMSlave(bankIdx));
                    if(!spmAddrQueDst.empty()) {
                        uint pageDst = this->pageTable.getPPage(spmAddrQueDst[j]);
                        uint pSpmAddrDst = this->pageTable.concat(pageDst, this->pageTable.getOffset(spmAddrQueDst[j]));
#ifdef SPM_ACCEL_DBG
                        printf("pageDst: %#x, pSpmAddrDst: %#x\n", pageDst, pSpmAddrDst);
#endif

                        uint dstBankIdx = (this->config.spmAddrType == this->config.SPM_ADDR_TYPE_BLOCK_INTERLEAVED) ?
                                    ((pSpmAddrDst >> (this->bankIdxOffset)) & (this->bankIdxMask)) :
                                    (config.spmAddrType == config.SPM_ADDR_TYPE_PAGE_INTERLEAVED) ?
                                    (pageDst % config.nBanks) :
                                    (pageDst / (this->pagesPerBank));
                        msg->dstSpmIdSPMSlave = PortIDManager::localToGlobalSPMSlave(dstBankIdx);
                        msg->s2sSize = std::max(1ul, size);
                    }

                    sendMessageToMasterOutputFifos(msg);
                    flyingSpmMsgCount++;
#ifdef SPM_ACCEL_DBG
                    printf("%lu SPMAccel::tick %u, create a SPMMessage\n", counter.cycles, config.accId);
                    printf("\n");
#endif
                }
            }
#ifdef SPM_ACCEL_DBG
            printf("%lu SPMAccel::tick %u, start cmdSrc=%u, numMsg=%zu\n",
                counter.cycles, config.accId, cmdSrc, flyingSpmMsgCount);
            printf("\n");
#endif
            if (flyingSpmMsgCount == 0) {
                finish = true;
                msgCreationDone = false;
            } else {
                msgCreationDone = true;
            }
        } else { 
            // wait until spm messages are done
            if (flyingSpmMsgCount == 0) {
#ifdef SPM_ACCEL_DBG
            printf("%lu SPMAccel::tick %u, finish flyingSpmMsgCount\n", counter.cycles, config.accId);
            printf("\n");
#endif
                finish = true;
                msgCreationDone = false;
            }
        }
        return finish;
    }

    void SPMAccel::sendMessageToMasterOutputFifos(SPMMessage *msg) {        
        const auto& spmMsgType = msg->type;
#ifdef SPM_ACCEL_DBG
        printf("%lu SPMAccel::tick %u, send msg, spmMsgType=%u\n", counter.cycles, config.accId, spmMsgType);
        printf("\n");
#endif
        if(spmMsgType == SPMMSG_LOAD)  queueWithArbitor.push(MASTERPORT_FIFO_LOAD, msg);
        else if(spmMsgType == SPMMSG_FETCH_LOAD) queueWithArbitor.push(MASTERPORT_FIFO_FETCH_LOAD, msg);
        else if(spmMsgType == SPMMSG_LOAD_BYPASS) queueWithArbitor.push(MASTERPORT_FIFO_LOAD_BYPASS, msg);
        else if(spmMsgType == SPMMSG_STORE) queueWithArbitor.push(MASTERPORT_FIFO_STORE, msg);
        else if(spmMsgType == SPMMSG_STORE_BYPASS) queueWithArbitor.push(MASTERPORT_FIFO_STORE_BYPASS, msg);
        else if(spmMsgType == SPMMSG_SEND) queueWithArbitor.push(MASTERPORT_FIFO_SEND, msg);
        else if(spmMsgType == SPMMSG_FETCH) queueWithArbitor.push(MASTERPORT_FIFO_FETCH, msg);
        else if(spmMsgType == SPMMSG_FLUSH) queueWithArbitor.push(MASTERPORT_FIFO_FLUSH, msg);
        else assert(false);
    }

} // mudnac