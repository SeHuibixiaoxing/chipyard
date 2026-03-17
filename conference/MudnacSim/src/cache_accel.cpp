#include "cache_accel.h"


namespace mudnac {


    CacheAccel::CacheAccel(const AccelConfig &config_) : BaseAccel(config_) {
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

        loadCacheMsgCreationDone = false;
        flyingLoadCacheMsgCount = 0;

        storeCacheMsgCreationDone = false;

        computeCurrentCycles = 0;
        computeTotalCycles = 0;

        flushCMD = NULL;

        ts.startCycles = UINT64_MAX;

        printf("---- CacheAccel ----\n"
               "\taccId = %u\n"
               "\tarray = %u x %u\n"
               "\tbeatBytes = %u\n"
               "\tmaxBurstBytes = %u\n"
               "\n",
               config.accId, config.arrayH, config.arrayW, config.beatBytes, config.maxBurstBytes);
    }

    void CacheAccel::tick() {
        // part1: fetch next cmd
        if ((not cpuPortInput->empty()) and flushCMD == NULL) {
            auto cmd = cpuPortInput->front();
            if (typeid(*cmd) == typeid(AccelFlushCache)) {  // flush cache
                if (flushCMD == NULL) {
                    flushCMD = cmd;
                    cpuPortInput->pop_front();
                }
            } else {
                if (waitedLoadCMD == NULL and flushCmdMap.empty()) {
                    waitedLoadCMD = cmd;
                    cpuPortInput->pop_front();
#ifdef TILE_LEVEL_PROGRESS
                    printf("%lu CacheAccel::tick %d: waiting tiles = %zu\n",
                           counter.cycles, config.accId, cpuPortInput->size());
#endif
                }
            }
        }

        if (loadCMD != NULL or storeCMD != NULL) {
            ts.loadCycles++;
        }

        // load
        if (loadCMD != NULL) {
            if (not loadCacheMsgCreationDone) {
                // create load cache messages
                assert(flyingLoadCacheMsgCount == 0);
                for (int i = 0; i < loadCMD->numInputs; i++) {
                    if (not loadCMD->inputReuse[i]) {
                        Tensor::BurstQue bQue;
                        counter.idealLoadBytes += loadCMD->load(i, config, bQue);
                        for (auto &burst: bQue) {
                            uint64 addr = burst[0], size = burst[1];
                            counter.realLoadBytes += size;
                            CacheMessage *msg = new CacheMessage(addr, size, false);
                            msg->setTrans(std::max((uint64) 1, size), 1,
                                          PortIDManager::localToGlobalAccelMaster(config.accId), PortIDManager::localToGlobalCacheSlave((addr >> bankIdxOffset) & bankIdxMask));
                            masterPortBurstFifo.push_back(msg);
                            flyingLoadCacheMsgCount++;
                        }
                    }
                }
//                printf("%lu CacheAccel::tick %u: start load, numMsg=%zu\n",
//                       counter.cycles, config.accId, flyingLoadCacheMsgCount);
                if (flyingLoadCacheMsgCount == 0) {
                    // no need to load for this CMD
                    waitedComputeCMD = loadCMD;
                    loadCMD = NULL;
                    loadCacheMsgCreationDone = false;
                } else {
                    loadCacheMsgCreationDone = true;
                }
            } else {
                // wait until all cache messages are done
                if (flyingLoadCacheMsgCount == 0) {
                    waitedComputeCMD = loadCMD;
                    loadCMD = NULL;
                    loadCacheMsgCreationDone = false;
                }
            }
        }

        // store
        if (storeCMD != NULL) {
            if (not storeCacheMsgCreationDone) {
                // create store cache messages
                FlyingCmdInStore *f = new FlyingCmdInStore;
                f->cmd = storeCMD;
                f->count = 0;
                for (int i = 0; i < storeCMD->numOutputs; i++) {
                    if (not storeCMD->outputReuse[i]) {
                        Tensor::BurstQue bQue;
                        counter.idealStoreBytes += storeCMD->store(i, config, bQue);
                        for (auto &burst: bQue) {
                            uint64 addr = burst[0], size = burst[1];
                            counter.realStoreBytes += size;
                            CacheMessage *msg = new CacheMessage(addr, size, true);
                            msg->setTrans(1, std::max((uint64) 1, size),
                                          PortIDManager::localToGlobalAccelMaster(config.accId), PortIDManager::localToGlobalCacheSlave((addr >> bankIdxOffset) & bankIdxMask));
                            masterPortBurstFifo.push_back(msg);
                            assert(flyingStoreCacheMsgCount.count(msg) == 0);
                            flyingStoreCacheMsgCount[msg] = f;
                            f->count++;
                        }
                    }
                }
//                printf("%lu CacheAccel::tick %u: start store, numMsg=%u\n",
//                       counter.cycles, config.accId, f->count);
                if (f->count == 0) {
                    // no need to store for this CMD
                    cpuPortOutput->push_back(storeCMD);
                    storeCMD = NULL;
                    storeCacheMsgCreationDone = false;
                    delete f;
                } else {
                    storeCacheMsgCreationDone = true;
                }
            } else {
                // wait until all data to store are cleared out
                if (masterPortBurstFifo.empty()) {
                    storeCMD = NULL;
                    storeCacheMsgCreationDone = false;
                }
            }
        }

        // handle returned cache messages
        if (not masterPort->input->empty()) {
            auto msg = dynamic_cast<CacheMessage*>(masterPort->input->front());
            masterPort->input->pop_front();
            if (msg->isWrite) {  // store
                assert(flyingStoreCacheMsgCount.count(msg));
                auto f = flyingStoreCacheMsgCount[msg];
                assert(f->count > 0);
                f->count--;
                flyingStoreCacheMsgCount.erase(msg);
                if (f->count == 0) {
                    cpuPortOutput->push_back(f->cmd);
                    delete f;
                }
            } else {  // load
                assert((loadCMD != NULL) and (flyingLoadCacheMsgCount > 0));
                flyingLoadCacheMsgCount--;
            }
            delete msg;
        }

//        // (OLD) part2: store: create cache messages
//        if (storeCMD != NULL) {
//            ts.storeCycles++;
//            if (not loadCacheMsgCreationDone) {
//                assert(flyingLoadCacheMsgCount == 0);
//                for (int i = 0; i < storeCMD->numOutputs; i++) {
//                    if (not storeCMD->outputReuse[i]) {
//                        Tensor::BurstQue bQue;
//                        counter.idealStoreBytes += storeCMD->store(i, config, bQue);
//                        for (auto &burst: bQue) {
//                            uint64 addr = burst[0], size = burst[1];
//                            counter.realStoreBytes += size;
//                            CacheMessage *msg = new CacheMessage(addr, size, true);
//                            msg->setTrans(1, std::max((uint64) 1, size),
//                                          config.accId, (addr >> bankIdxOffset) & bankIdxMask);
//                            masterPortBurstFifo.push_back(msg);
//                        }
//                        flyingLoadCacheMsgCount += bQue.size();
//                    }
//                }
//                loadCacheMsgCreationDone = true;
//#ifdef CACHE_ACCEL_DBG
//                printf("%lu CacheAccel::tick %u: start store, numMsg=%zu\n",
//                       counter.cycles, config.accId, flyingCacheMsgCount);
//#endif
//            } else {  // wait until store cache messages are done
//                if (flyingLoadCacheMsgCount > 0) {
//                    if (not downstreamInputPort->empty()) {
//                        auto msg = downstreamInputPort->front();
//                        assert(msg->isWrite);
//                        flyingLoadCacheMsgCount--;
//                        downstreamInputPort->pop_front();
//                        delete msg;
//                    }
//                } else {
//                    cpuPortOutput->push_back(storeCMD);
//                    storeCMD = NULL;
//                    loadCacheMsgCreationDone = false;
//#ifdef CACHE_ACCEL_DBG
//                    printf("%lu CacheAccel::tick %u: end store\n", counter.cycles, config.accId);
//#endif
//                }
//            }
//        }
//
//        // (OLD) part3: load: create cache messages (after store)
//        if (storeCMD == NULL and loadCMD != NULL) {
//            ts.loadCycles++;
//            if (not loadCacheMsgCreationDone) {
//                assert(flyingLoadCacheMsgCount == 0);
//                for (int i = 0; i < loadCMD->numInputs; i++) {
//                    if (not loadCMD->inputReuse[i]) {
//                        Tensor::BurstQue bQue;
//                        counter.idealLoadBytes += loadCMD->load(i, config, bQue);
//                        for (auto &burst: bQue) {
//                            uint64 addr = burst[0], size = burst[1];
//                            counter.realLoadBytes += size;
//                            CacheMessage *msg = new CacheMessage(addr, size, false);
//                            msg->setTrans(std::max((uint64) 1, size), 1,
//                                          config.accId, (addr >> bankIdxOffset) & bankIdxMask);
//                            masterPortBurstFifo.push_back(msg);
//                        }
//                        flyingLoadCacheMsgCount += bQue.size();
//                    }
//                }
//                loadCacheMsgCreationDone = true;
//#ifdef CACHE_ACCEL_DBG
//                printf("%lu CacheAccel::tick %u: start load, numMsg=%zu\n",
//                       counter.cycles, config.accId, flyingCacheMsgCount);
//#endif
//            } else {  // wait until load cache messages are done
//                if (flyingLoadCacheMsgCount > 0) {
//                    if (not downstreamInputPort->empty()) {
//                        auto msg = downstreamInputPort->front();
//                        assert(not msg->isWrite);
//                        flyingLoadCacheMsgCount--;
//                        downstreamInputPort->pop_front();
//                        delete msg;
//                    }
//                } else {
//                    waitedComputeCMD = loadCMD;
//                    loadCMD = NULL;
//                    loadCacheMsgCreationDone = false;
//#ifdef CACHE_ACCEL_DBG
//                    printf("%lu CacheAccel::tick %u: end load\n", counter.cycles, config.accId);
//#endif
//                }
//            }
//        }

        // part4: compute
        if (computeCMD != NULL) {
            ts.computeCycles++;
            if (computeTotalCycles == 0) {  // a new compute is arrived
                computeTotalCycles = std::max((uint64_t) 1, computeCMD->compute(config));
#ifdef CACHE_ACCEL_DBG
                printf("%lu CacheAccel::tick %u: start compute, cycles=%lu\n",
                       counter.cycles, config.accId, computeTotalCycles);
#endif
            } else {  // wait until compute is done
                if (computeCurrentCycles == computeTotalCycles - 1) {  // compute is done
                    waitedStoreCMD = computeCMD;  // move cmd from compute phase to store phase
                    computeCMD = NULL;
                    computeTotalCycles = 0;
                    computeCurrentCycles = 0;
#ifdef CACHE_ACCEL_DBG
                    printf("%lu CacheAccel::tick %u: end compute\n", counter.cycles, config.accId);
#endif
                } else {
                    computeCurrentCycles++;
                }
            }
        }

        // part5: synchronized update
        if (computeCMD == NULL and loadCMD == NULL and storeCMD == NULL) {
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
                ts.startCycles = counter.cycles;
                ts.endCycles = 0;
                ts.storeCycles = 0;
                ts.loadCycles = 0;
                ts.computeCycles = 0;
            }
#ifdef CACHE_ACCEL_DBG
            if (computeCMD != NULL or loadCMD != NULL or storeCMD != NULL) {
                printf("%lu CacheAccel::tick %u: synchronized update: load=%u, compute=%u, store=%u\n",
                       counter.cycles, config.accId, loadCMD != NULL, computeCMD != NULL, storeCMD != NULL);
            }
#endif
        }

        // part6: burst downstream
        if (not masterPortBurstFifo.empty()) {
            auto msg = masterPortBurstFifo.front();
            if (memAccessCtrlBytesCounter + msg->size <= memAccessCtrlMaxReqs) {
                masterPort->output->push_back(msg);
                masterPortBurstFifo.pop_front();
                memAccessCtrlBytesCounter += msg->size;
            }
        }

        // part7: flush cache
        if (flushCMD != NULL and
            computeCMD == NULL and loadCMD == NULL and storeCMD == NULL and
            waitedComputeCMD == NULL and waitedLoadCMD == NULL and waitedStoreCMD == NULL) {
            CacheMessage *msg = new CacheMessage();
            msg->addr = dynamic_cast<AccelFlushCache *>(flushCMD)->addr;
            msg->flush = true;
            msg->setTrans(1, 1,
                          PortIDManager::localToGlobalAccelMaster(config.accId), PortIDManager::localToGlobalCacheSlave((msg->addr >> bankIdxOffset) & bankIdxMask));
            masterPortBurstFifo.push_back(msg);
            assert(not flushCmdMap.count(msg));
            flushCmdMap[msg] = flushCMD;
            flushCMD = NULL;
#ifdef CACHE_ACCEL_DBG
            printf("%lu CacheAccel::tick %u: start flush, addr=%lx\n",
                   counter.cycles, config.accId, msg->addr);
#endif
        }
        if ((not flushCmdMap.empty()) and (not masterPort->input->empty())) {  // wait until flush messages are done
            auto msg = dynamic_cast<CacheMessage*>(masterPort->input->front());
            masterPort->input->pop_front();
            assert(msg->flush);
            assert(flushCmdMap.count(msg));
            auto cmd = flushCmdMap[msg];
            flushCmdMap.erase(msg);
            delete msg;
            cpuPortOutput->push_back(cmd);
#ifdef CACHE_ACCEL_DBG
            printf("%lu CacheAccel::tick %u: end flush, addr=%lx\n",
                   counter.cycles, config.accId, dynamic_cast<AccelFlushCache *>(cmd)->addr);
#endif
        }

        if (memAccessCtrlCycleCounter + 1 >= memAccessCtrlEpoch) {
            memAccessCtrlCycleCounter = 0;
            memAccessCtrlBytesCounter = 0;
        } else {
            memAccessCtrlCycleCounter++;
        }

        counter.tick();
    }


} // mudnac