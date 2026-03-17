#include "runtime.h"


namespace mudnac {

    PipelineRuntime RT;
    Runtime::Runtime() {
        PID = getpid();
        system = NULL;
    }

    void Runtime::setSystem(mudnac::BaseSystem &system_) {
        system = &system_;
        hw.init(system_);
        gv.init(hw.numAccels, hw.nPages, hw.nBanks, system->config.getPagesPerBank(), system->config.spmPageBytes);
    }

    void Runtime::setSystem(mudnac::BaseSystem *system_) {
        setSystem(*system_);
    }
    
/**
 * Executes the main runtime loop for the simulation.
 * 
 * This function continuously runs until all threads in the simulation have
 * completed their tasks. It invokes the `tick` function on each thread and 
 * the system to progress the simulation state.
 */
    void Runtime::run() {
        auto timeStart = std::chrono::high_resolution_clock::now();

        uint finished = 0;
        while (finished < threads.size()) {
            if (getCycles() % 10000000 == 0) {
                auto timeCurrent = std::chrono::high_resolution_clock::now();
                std::chrono::duration<double> elapsedSecs = timeCurrent - timeStart;
                double elapsedMins = elapsedSecs.count() / 60;
                double speed = (getCycles() / 1000000) / elapsedMins;
                uint progress = 0;
                for (auto t: threads) {
                    progress += t->progress;
                }
                progress /= threads.size();
                printf("MudnacSim | PID: %lu | Cycle: %lum | "
                       "Elapsed: %u min | Speed: %.3lfm cyc/min | Progress: %u%%\n",
                       PID, getCycles() / 1000000,
                       (uint) elapsedMins, speed, progress);
            }

            finished = 0;
            for (auto t: threads) {
                finished += t->tick();
            }
            system->tick();
        }

        auto timeEnd = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> elapsedSecs = timeEnd - timeStart;
        runElapsedSeconds = elapsedSecs.count();
    }

    bool Runtime::isSPMSystem() { 
        return system->isSpmSystem(); 
    }

    void Runtime::addCmd(uint accId, AccelTiledCMD *cmd) { 
        system->addCmd(accId, cmd); 
    }

    AccelTiledCMD *Runtime::getFinishedCmd(uint accId, bool is_pipeline = false) { 
        return system->getFinishedCmd(accId, is_pipeline); 
    }

    AccelTiledCMD *Runtime::getFinishedCmd(const uint accId, const uint globalId, bool is_pipeline = false) {
        return system->getFinishedCmd(accId, globalId, is_pipeline);
    }

    uint64 Runtime::getCycles() { 
        return system->cycles; 
    }

    void Runtime::setAccMemEpochRate(uint accId, uint64 epoch, uint64 maxReqs) {
        if (isSPMSystem()) {
            (dynamic_cast<BaseSPMSystem *>(system))->accels[accId].setMemAccessCtrl(epoch, maxReqs);
        } else {
            (dynamic_cast<BaseCacheSystem *>(system))->accels[accId].setMemAccessCtrl(epoch, maxReqs);
        }
    }

    void Runtime::setAccPageForBypass(uint accId, uint pageForBypass) {
        (dynamic_cast<BaseSPMSystem *>(system))->accels[accId].pageTable.pageForBypass = pageForBypass;
    }

    void Runtime::setAccPageForBypass(uint accId) {
        uint pageForBypass =
                (system->config.accelSpmAddrType == AccelConfig::SPM_ADDR_TYPE_BLOCK_INTERLEAVED) ?
                0 :
                (system->config.accelSpmAddrType == AccelConfig::SPM_ADDR_TYPE_PAGE_INTERLEAVED) ?
                (accId / system->config.getAccelsPerBank()) :
                ((accId / system->config.getAccelsPerBank()) * system->config.getPagesPerBank());
        setAccPageForBypass(accId, pageForBypass);
    }

    std::string Runtime::getFormatedRunElapsedTime() {
        int hours = runElapsedSeconds / 3600;
        int minutes = (runElapsedSeconds - hours * 3600) / 60;
        int seconds = runElapsedSeconds - hours * 3600 - minutes * 60;
        return std::to_string(hours) + "h" + std::to_string(minutes) + "m" + std::to_string(seconds) + "s";
    }

    /**
     * @brief Acquires the specified number of accelerators for a given thread.
     *
     * @param tid The thread ID for which to acquire accelerators.
     * @param numAccels The number of accelerators to acquire.
     *
     * @return True if successful, false otherwise.
     *
     * The function will fail and return false if the number of accelerators
     * requested is greater than the number available. It will also fail if the
     * thread is not at the front of the acquisition queue for accelerators.
     *
     * If the accelerator allocation strategy is set to naive(enableThreadAccelAffiliation=false),
     * it will obtain numAccels of accelerators from accelAcquireQueue heads and assign them to
     * tid. If not(enableThreadAccelAffiliation=true), It picks up accelerators from threadAccelAlloc
     * and assigns the available accelerators to tid.
     */
    bool Runtime::acquireAccel(uint tid, uint numAccels) {
        assert(tid < GlobalVariables::MAX_THREADS);
        if (not gv.accelAcquireDict.count(tid)) {
            gv.accelAcquireQueue.push_back(tid);
        }
        gv.accelAcquireDict[tid] = numAccels;
        if (gv.accelAcquireQueue.front() != tid) {
            return false;
        }
        if (numAccels > gv.availableAccelSet.size()) {
            return false;
        } else {
            auto &alloc = gv.threadAccelAlloc[tid];
            if (gv.enableThreadAccelAffiliation) {
                assert(tid < gv.threadAccelAffiliation.size());
                auto &taa = gv.threadAccelAffiliation[tid]; // Gets the accelerator to which the thread tid is allocated
                uint availableAccelNum = 0;
                for (uint accId: taa) {
                    if (gv.availableAccelSet.count(accId)) {
                        availableAccelNum++;
                    }
                }
                if (availableAccelNum >= numAccels) {
                    uint allocatedAccelCount = 0;
                    for (uint accId: taa) {
                        if (allocatedAccelCount == numAccels) {
                            break;
                        }
                        if (gv.availableAccelSet.count(accId)) {
                            gv.availableAccelSet.erase(accId);
                            alloc.push_back(accId);
                            allocatedAccelCount++;
                        }
                    }
                    assert(allocatedAccelCount == numAccels);
                    gv.accelAcquireDict.erase(tid);
                    gv.accelAcquireQueue.pop_front();
                    return true;
                } else {
                    return false;
                }
            } else {
                for (uint i = 0; i < numAccels; i++) {
                    uint accId = *gv.availableAccelSet.begin();
                    gv.availableAccelSet.erase(accId);
                    alloc.push_back(accId);
                }
                gv.accelAcquireDict.erase(tid);
                gv.accelAcquireQueue.pop_front();
                return true;
            }
        }
    }
    void Runtime::releaseAccel(uint tid, uint numAccels) {
        assert(tid < GlobalVariables::MAX_THREADS);
        auto &alloc = gv.threadAccelAlloc[tid];
        numAccels = std::min(numAccels, (uint) alloc.size());
        for (uint i = 0; i < numAccels; i++) {
            uint accId = alloc.back();
            alloc.pop_back();
            gv.availableAccelSet.insert(accId);
        }
    }

    uint Runtime::getAvailableAccel() { 
        return gv.availableAccelSet.size(); 
    }


    /**
     * @brief Acquire virtual pages of SPM for a given thread
     * @param tid thread ID
     * @param numVPages number of virtual pages to acquire
     * @return true if acquisition is successful, false otherwise
     *
     * This function acquires virtual pages of SPM for a given thread. If the
     * number of available virtual pages is not sufficient, it returns false.
     * Otherwise, it allocates the virtual pages and sets the page table for
     * each accelerator allocated to the thread.
     *
     * If the page allocation strategy is naive, the function allocates the
     * virtual pages in the order of the available physical pages.
     *
     * If the page allocation strategy is ordered, the function allocates the
     * virtual pages in the order of the hops from the accelerator to the SPM.
     */
    bool Runtime::acquireSPM(uint tid, uint numVPages) {
        assert(tid < GlobalVariables::MAX_THREADS);
        if (not gv.spmAcquireDict.count(tid)) {
            gv.spmAcquireQueue.push_back(tid);
        }
        gv.spmAcquireDict[tid] = numVPages;
        if (gv.spmAcquireQueue.front() != tid) {
            return false;
        }
        if (numVPages > gv.availableSpmPageSet.size()) {
            return false;
        } else {
            auto &alloc = gv.threadSpmPageAlloc[tid];
            if(gv.SPM_PAGE_ALLOC_STRATEGY_AFFILIATION == gv.spmPageAllocStrategy) {
                assert(tid < gv.threadSPMAffiliation.size());
                auto tsa = gv.threadSPMAffiliation[tid]; // Gets the SPM to which the thread tid is allocated
                uint availableSpmPageNum = 0;
                for (uint pPage: tsa) {
                    if (gv.availableSpmPageSet.count(pPage)) {
                        availableSpmPageNum++;
                    }
                }
                if (availableSpmPageNum >= numVPages) {
                    uint allocatedSpmPageCount = 0;
                    for (uint pPage: tsa) {
                        if (allocatedSpmPageCount == numVPages) {
                            break;
                        }
                        if (gv.availableSpmPageSet.count(pPage)) {
                            gv.availableSpmPageSet.erase(pPage);
                            alloc.push_back(pPage);
                            allocatedSpmPageCount++;
                        }
                        if (isSPMSystem()) {
                            for(uint vPage = 0;vPage < alloc.size();++ vPage) {
                                for (auto accId: gv.threadAccelAlloc[tid]) {
                                    (dynamic_cast<BaseSPMSystem *>(system))->accels[accId].setPageTable(vPage, pPage, true);
                                }
                            }
                        }
                    }
                    assert(allocatedSpmPageCount == numVPages);
                    gv.spmAcquireDict.erase(tid);
                    gv.spmAcquireQueue.pop_front();
                    return true;
                } else {
                    return false;
                }

            } else if (gv.spmPageAllocStrategy == gv.SPM_PAGE_ALLOC_STRATEGY_NAIVE) {
                for (uint i = 0; i < numVPages; i++) {
                    uint pPage = *gv.availableSpmPageSet.begin();
                    gv.availableSpmPageSet.erase(pPage);
                    alloc.push_back(pPage);
                    uint vPage = alloc.size() - 1;
                    if (isSPMSystem()) {
                        for (auto accId: gv.threadAccelAlloc[tid]) {
                            (dynamic_cast<BaseSPMSystem *>(system))->accels[accId].setPageTable(vPage, pPage, true);
                        }
                    }
                }
            } else if (gv.spmPageAllocStrategy == gv.SPM_PAGE_ALLOC_STRATEGY_ORDERED) {
                assert(gv.threadAccelAlloc[tid].size() > 0);
                std::deque<uint> bankIdOrder;
                for (uint i = 0; i < hw.nBanks; i++) {
                    for (uint accId: gv.threadAccelAlloc[tid]) {
                        bankIdOrder.push_back(gv.accelSpmBankOrder[accId][i]);
                    }
                }
                uint orderIdx = 0;
                uint pPage = bankIdOrder[orderIdx] * hw.nPagesPerBank;
                for (uint i = 0; i < numVPages; i++) {
                    while (not gv.availableSpmPageSet.count(pPage)) {
                        if (((pPage + 1) % hw.nPagesPerBank) == 0) {
                            assert(orderIdx + 1 < bankIdOrder.size());
                            orderIdx++;
                            pPage = bankIdOrder[orderIdx] * hw.nPagesPerBank;
                        } else {
                            pPage++;
                        }
                    }
                    gv.availableSpmPageSet.erase(pPage);
                    alloc.push_back(pPage);
                    uint vPage = alloc.size() - 1;
                    if (isSPMSystem()) {
                        for (auto accId: gv.threadAccelAlloc[tid]) {
                            (dynamic_cast<BaseSPMSystem *>(system))->accels[accId].setPageTable(vPage, pPage, true);
                        }
                    }
                }
            } else {
                assert(false);
            }
//            printf("acquireSPM: tid=%u, pageId=%s\n", tid, toString(alloc).c_str());
            gv.spmAcquireDict.erase(tid);
            gv.spmAcquireQueue.pop_front();
            return true;
        }
    }

    void Runtime::releaseSPM(uint tid, uint numVPages) {
        assert(tid < GlobalVariables::MAX_THREADS);
        auto &alloc = gv.threadSpmPageAlloc[tid];
        numVPages = std::min(numVPages, (uint) alloc.size());
        for (uint i = 0; i < numVPages; i++) {
            uint vPage = alloc.size() - 1;
            uint pPage = alloc.back();
            alloc.pop_back();
            if (isSPMSystem()) {
                for (auto accId: gv.threadAccelAlloc[tid]) {
                    (dynamic_cast<BaseSPMSystem *>(system))->accels[accId].setPageTable(vPage, pPage, false);
                }
            }
            gv.availableSpmPageSet.insert(pPage);
        }
    }

    inline uint Runtime::getAvailableSPM() { 
        return gv.availableSpmPageSet.size(); 
    }

    void Runtime::releaseAll(uint tid) {
        setMemEpochRate(tid, UINT64_MAX, UINT64_MAX);
        releaseAccel(tid, UINT32_MAX);
        releaseSPM(tid, UINT32_MAX);
    }

    void Runtime::setMemEpochRate(uint tid, uint64 epoch, uint64 maxReqs) {
        for (uint accId: gv.threadAccelAlloc[tid]) {
            setAccMemEpochRate(accId, epoch, maxReqs);
        }
    }

    void Runtime::setPageForBypass(uint tid, uint pageForBypass) {
        if (isSPMSystem()) {
            for (auto accId : gv.threadAccelAlloc[tid]) {
                setAccPageForBypass(accId, pageForBypass);
            }
        }
    }

    /**
     * @brief Repartition SPM for a given thread
     * @param tid thread ID
     * @param threshold reference to a variable to store the threshold of SPM repartition
     *
     * This function repartitions the SPM for a given thread. It first checks if the
     * thread is during inter-layer reusing and it is not the head of the layer group,
     * if so, it returns immediately. Then it checks if the SPM allocation mode is 
     * dynamic or fixed. 
     * 
     * If it is dynamic, it estimates the number of pages available for this thread, and 
     * selects the mapping candidate with the minimum SPM utilization that does not exceed
     * the estimated number of pages available. 
     * 
     * If it is fixed, it sets the threshold to be the average maximum number of pages, 
     * and selects the mapping candidate with the minimum SPM utilization that does 
     * not exceed the average maximum number of pages.
     * 
     * For inter-layer reusing, only the head layer in the layer group is repartitioned.
     * It use the mapping candidate at the last index of the mapping table in the head layer.
     */
    void Runtime::spmRepartition(uint tid, uint64 &threshold) {
        uint accAllocIdx = gv.threadAccelAllocIdx[tid];
        uint modelIdx = gv.threadCurrentModelType[tid];
        uint layerIdx = gv.threadCurrentLayerIdx[tid];
        Model *model = threads[tid]->model;
        Layer *layer = threads[tid]->layer;
        auto &mappingList = layer->mappingTable[accAllocIdx];

        // if it's during inter-layer reusing, no SPM repartition.
        if (gv.spm_threadInterLayerReuse[tid]) {
            assert(layer->lgTypeIsInter() or layer->lgTypeIsTail());
#ifdef RUNTIME_DBG
            printf("%lu Runtime::spmRepartition: during inter-layer reusing, "
                       "tid=%u, modelIdx=%u, layerIdx=%u\n",
                       getCycles(), tid, modelIdx, layerIdx);
#endif
            return;
        }

        if (gv.spmAllocMode == gv.SPM_ALLOC_MODE_DYNAMIC) {
            assert(gv.pf_doneProfiling);
            if (isSPMSystem() and layer->lgTypeIsHead()) {
                // is head layer of the layer group, check if inter-layer reuse is doable

                // compute threshold
                uint idx = layerIdx;
                threshold = getCycles();
                while (true) {
                    threshold += gv.pf_modelLayerCycles[accAllocIdx][modelIdx][idx] * gv.spm_beta;
                    idx++;
                    if (model->layers[idx - 1]->lgTypeIsTail()) { break; }
                }

                // compute pages available for this thread
                int pageEstimated = getAvailableSPM();
                for (uint tid_ = 0; tid_ < threads.size(); tid_++) {
                    if (tid_ == tid) { continue; }
                    if (gv.spmAcquireDict.count(tid_)) {  // tid_ is waiting for SPM before tid
                        pageEstimated -= gv.spmAcquireDict[tid_];
                    } else {  // tid_ contains a running layer
                        if (gv.spm_threadEstimatedReleasingTime[tid_] <= threshold) {
                            pageEstimated += gv.threadSpmPageAlloc[tid_].size() -
                                             gv.spm_alpha * gv.spm_threadEstimatedAcquiredPages[tid_];
                        }
                    }
                }
                pageEstimated = std::max(pageEstimated, 0);

#ifdef RUNTIME_DBG
                printf("%lu Runtime::spmRepartition: check inter-layer reusing, "
                       "tid=%u, modelIdx=%u, layerIdx=%u, threshold=%lu, pageEst=%d, pageReq=%d\n",
                       getCycles(), tid, modelIdx, layerIdx,
                       threshold, pageEstimated, divCeil(mappingList.back().spmCapaUtilized, hw.pageBytes));
#endif

                // check if inter-layer reuse is doable
                if (mappingList.back().lgSpmUtil <= pageEstimated * hw.pageBytes) {
                    gv.spm_threadInterLayerReuse[tid] = true;
                    gv.threadSpmAllocIdx[tid] = mappingList.size() - 1;
                    return;
                }
            }

            // compute threshold
            threshold = getCycles() + gv.pf_modelLayerCycles[accAllocIdx][modelIdx][layerIdx] * gv.spm_beta;

            // compute pages available for this thread (this part should be identical to the upper one)
            int pageEstimated = getAvailableSPM();
            for (uint tid_ = 0; tid_ < threads.size(); tid_++) {
                if (tid_ == tid) { continue; }
                if (gv.spmAcquireDict.count(tid_)) {  // tid_ is waiting for SPM before tid
                    pageEstimated -= gv.spmAcquireDict[tid_];
                } else {  // tid_ contains a running layer
                    if (gv.spm_threadEstimatedReleasingTime[tid_] <= threshold) {
                        pageEstimated += gv.threadSpmPageAlloc[tid_].size() -
                                         gv.spm_alpha * gv.spm_threadEstimatedAcquiredPages[tid_];
                    }
                }
            }
            pageEstimated = std::max(pageEstimated, 0);

            // get mapping candidates
            uint spmAllocIdx = mappingList.size() - 2;
            while (mappingList[spmAllocIdx].spmUtil > pageEstimated * hw.pageBytes) {
                assert(spmAllocIdx > 0);
                spmAllocIdx--;
            }
#ifdef RUNTIME_DBG
            printf("%lu Runtime::spmRepartition: select single layer candidate, "
                   "tid=%u, modelIdx=%u, layerIdx=%u, threshold=%lu, pageEst=%d, pageReq=%d, spmAllocIdx=%u\n",
                   getCycles(), tid, modelIdx, layerIdx,
                   threshold, pageEstimated, divCeil(mappingList[spmAllocIdx].spmCapaUtilized, hw.pageBytes),
                   spmAllocIdx);
#endif
            gv.spm_threadInterLayerReuse[tid] = false;
            gv.threadSpmAllocIdx[tid] = spmAllocIdx;

        } else if (gv.spmAllocMode == gv.SPM_ALLOC_MODE_FIX) {
            uint maxPages = divFloor(gv.numPages, threads.size());
            threshold = UINT64_MAX;

            if (spm_pages_alloc_per_model.size() > 0) {
                maxPages = spm_pages_alloc_per_model[tid];
            }

            std::cout << "wzydebug: maxPages in spm alloc: " << maxPages << ", threads.size()=" << threads.size() << ", gv.numPages=" << gv.numPages << std::endl;

            if (isSPMSystem() and layer->lgTypeIsHead()) {
                if (mappingList.back().lgSpmUtil <= maxPages * hw.pageBytes) {
                    gv.spm_threadInterLayerReuse[tid] = true;
                    gv.threadSpmAllocIdx[tid] = mappingList.size() - 1;
                    return;
                }
            }

            uint spmAllocIdx = mappingList.size() - 2;
            while (mappingList[spmAllocIdx].spmUtil > maxPages * hw.pageBytes) {
                assert(spmAllocIdx > 0);
                spmAllocIdx--;
            }
            gv.spm_threadInterLayerReuse[tid] = false;
            gv.threadSpmAllocIdx[tid] = spmAllocIdx;
        }
    }

    void Runtime::spmRepartitionReduce(uint tid, uint64 &threshold) {
        uint accAllocIdx = gv.threadAccelAllocIdx[tid];
        uint modelIdx = gv.threadCurrentModelType[tid];
        uint layerIdx = gv.threadCurrentLayerIdx[tid];
        Layer *layer = threads[tid]->layer;
        auto &mappingList = layer->mappingTable[accAllocIdx];

        uint spmAllocIdx = gv.threadSpmAllocIdx[tid];
        uint currentSpmUsage = 0;
        if (gv.spm_threadInterLayerReuse[tid]) {
            assert(spmAllocIdx == mappingList.size() - 1);
            currentSpmUsage = mappingList.back().lgSpmUtil;
        } else {
            assert(spmAllocIdx < mappingList.size() - 1);
            currentSpmUsage = mappingList[spmAllocIdx].spmUtil;
        }
        if (currentSpmUsage == 0) { return; }

        assert(spmAllocIdx > 0);
        spmAllocIdx--;
        while (mappingList[spmAllocIdx].spmUtil >= currentSpmUsage) {
            assert(spmAllocIdx > 0);
            spmAllocIdx--;
        }
#ifdef RUNTIME_DBG
        printf("%lu Runtime::spmRepartitionReduce: tid=%u, pageReq=%d, spmAllocIdx=%u\n",
               getCycles(), tid, divCeil(mappingList[spmAllocIdx].spmCapaUtilized, hw.pageBytes), spmAllocIdx);
#endif

        gv.spm_threadInterLayerReuse[tid] = false;
        gv.threadSpmAllocIdx[tid] = spmAllocIdx;
        threshold = getCycles() + gv.pf_modelLayerCycles[accAllocIdx][modelIdx][layerIdx] * gv.spm_beta;
    }

    void Runtime::computeRepartition(uint tid) {
        // NOTE: closely follows compute_repartition (gemmini.h) in Aurora project
//        printf("COMPUTE: begin, tid=%u\n", tid);
        assert(gv.NUM_ACCEL_ALLOCS == 3);
        assert(gv.pf_doneProfiling);

        uint taskIdx = gv.qos_threadCurrentTaskIdx[tid];
        uint modelIdx = gv.qos_taskModelTypeQueue[taskIdx];
        uint layerIdx = gv.threadCurrentLayerIdx[tid];
        uint accAllocIdx = gv.threadAccelAllocIdx[tid];
        uint numIdleArray = getAvailableAccel();

        uint64 old = getCycles() - gv.qos_taskDispatchTime[taskIdx];
        int64 slack = gv.qos_modelTargetCycles[modelIdx] - old;
        int64 score = (int64) (100 * slack) / (int64) gv.pf_modelRemainingCycles[accAllocIdx][modelIdx][layerIdx];
        gv.qos_threadCurrentScore[tid] = score;
//        printf("COMPUTE: tid=%u, modelIdx=%u, layerIdx=%u, accAllocIdx=%u, slack=%ld, score=%ld, numIdleArray=%u\n",
//               tid, modelIdx, layerIdx, accAllocIdx, slack, score, numIdleArray);

        if (layerIdx == 0) {
            accAllocIdx = 1;
            if (numIdleArray <= 1) {
                accAllocIdx = 0;
            }
            score = (int64) (100 * slack) / (int64) gv.pf_modelRemainingCycles[accAllocIdx][modelIdx][layerIdx];
            gv.qos_threadCurrentScore[tid] = score;
            gv.threadAccelAllocIdx[tid] = accAllocIdx;
//            printf("COMPUTE: end, tid=%u\n", tid);
            return;
        }

        bool needRelease = false;
//        if (accAllocIdx >= 1 and score > 200) {  // backup4
        if (accAllocIdx >= 1 and score > 100) {
            if (numIdleArray <= 4 and score > 400) {
                needRelease = true;
            } else {
                needRelease = false;
                for (int t = 0; t < threads.size(); t++) {
                    if (t != tid) {
                        int64 otherScore = gv.qos_threadCurrentScore[t];
                        uint otherAccAllocIdx = gv.threadAccelAllocIdx[t];
//                        if (otherScore < 100 and otherScore > 30 and otherAccAllocIdx <= 1) {  // backup4
                        if (score > 2 * otherScore and otherScore > 25 and otherAccAllocIdx <= 1) {
                            needRelease = true;
                        } else if (otherScore > score and otherAccAllocIdx >= 1) {
                            needRelease = false;
                            break;
                        }
                    }
                }
            }
        }

        bool needAcquire = false;
//        if (accAllocIdx <= 1 and score > 30 and score < 100) {  // backup4
        if (accAllocIdx <= 1 and score > 25) {
            if ((accAllocIdx == 0 and numIdleArray <= 1) or (accAllocIdx == 1 and numIdleArray <= 2)) {
                needAcquire = false;
            } else {
                needAcquire = true;
                for (int t = 0; t < threads.size(); t++) {
                    if (t != tid) {
                        int64 otherScore = gv.qos_threadCurrentScore[t];
                        uint otherAccAllocIdx = gv.threadAccelAllocIdx[t];
                        if (score > otherScore and otherScore > 25 and otherAccAllocIdx <= 1) {
                            needAcquire = false;
                            break;
                        }
                    }
                }
            }
        }

        if (needRelease) {
            accAllocIdx -= 1;
        } else if (needAcquire) {
            accAllocIdx += 1;
        }
        if (needRelease or needAcquire) {
            score = (int64) (100 * slack) / (int64) gv.pf_modelRemainingCycles[accAllocIdx][modelIdx][layerIdx];
            gv.qos_threadCurrentScore[tid] = score;
            gv.threadAccelAllocIdx[tid] = accAllocIdx;
//            printf("\tneedRelease=%d, needAcquire=%d, score=%ld, accAllocIdx=%u\n",
//                   needRelease, needAcquire, score, accAllocIdx);
        }
//        printf("COMPUTE: end, tid=%u\n", tid);
    }

    void Runtime::memoryRepartition(uint tid) {
        // NOTE: closely follows memory_repartition (gemmini.h) in Aurora project
//        printf("MEMORY: begin, tid=%u\n", tid);
        assert(gv.pf_doneProfiling);
        assert(threads[tid]->layer != NULL);
        uint64 totalDramBW = 100 * hw.dramBytesPerCycles;  // 100 * 102.4 = 10240
        int64 lowerLimit = (double) (totalDramBW / threads.size()) * 0.2;

        uint accAllocIdx = gv.threadAccelAllocIdx[tid];
        uint spmAllocIdx = gv.threadSpmAllocIdx[tid];
        LayerMapping &mapping = threads[tid]->layer->mappingTable[accAllocIdx][spmAllocIdx];

        uint taskIdx = gv.qos_threadCurrentTaskIdx[tid];
        uint modelIdx = gv.qos_taskModelTypeQueue[taskIdx];
        uint layerIdx = gv.threadCurrentLayerIdx[tid];

        uint64 totalFromDram = mapping.dramAccess;
        uint64 predictionCycles = gv.pf_modelLayerCycles[accAllocIdx][modelIdx][layerIdx];
        uint64 idealDramUtil = (100 * totalFromDram) / predictionCycles;

        uint64 old = getCycles() - gv.qos_taskDispatchTime[taskIdx];
        int64 slack = gv.qos_modelTargetCycles[modelIdx] - old;
        int64 thisScore = 0;
        if (slack <= 0) {
            thisScore = 200;
        } else {
            thisScore = std::min(((int64) (100 * gv.pf_modelRemainingCycles[accAllocIdx][modelIdx][layerIdx]) / slack),
                                 (int64) 200);
        }
        gv.qos_threadCurrentMemScore[tid] = thisScore;
        gv.qos_threadDramUtil[tid] = idealDramUtil;
//        bool thisGoodSlack = thisScore < 50;
//        bool thisGoodSlack = thisScore < 100;  // backup3
        bool thisGoodSlack = true;
        if (layerIdx == 0) {
            return;
//            printf("MEMORY: end, tid=%u\n", tid);
        }

        uint64 sumDramUtil = idealDramUtil;
        for (int i = 0; i < threads.size(); i++) {
            if (i != tid) {
                sumDramUtil += gv.qos_threadDramUtil[i];
            }
        }
//        printf("MEMORY: tid=%u, modelIdx=%u, layerIdx=%u, idealDramUtil=%lu, slack=%ld, thisScore=%ld, sumDramUtil=%lu\n",
//               tid, modelIdx, layerIdx, idealDramUtil, slack, thisScore, sumDramUtil);

        bool contention = (sumDramUtil > totalDramBW);
        int64 newPredictionCycles = -1;
        if (contention) {
            uint64 excessUtil = sumDramUtil - totalDramBW;
            int64 otherScore = 0;
            int64 otherWeightedSum = 0;
            bool otherBadSlack = false;
            for (int i = 0; i < threads.size(); i++) {
                if (i != tid) {
                    otherScore += gv.qos_threadCurrentMemScore[i];
                    otherWeightedSum += gv.qos_threadCurrentMemScore[i] * gv.qos_threadDramUtil[i];
//                    otherBadSlack = otherBadSlack or (gv.qos_threadCurrentMemScore[i] > 100);  // backup3
                    otherBadSlack = true;
                }
            }
//            printf("\texcessUtil=%lu, otherScore=%ld, otherWeightedSum=%ld\n",
//                   excessUtil, otherScore, otherWeightedSum);
            if (otherScore > 0 and otherWeightedSum > 0 and otherBadSlack and thisGoodSlack) {
                int64 thisDramUtil = (int64) idealDramUtil -
                                     ((excessUtil * otherWeightedSum) / (thisScore * idealDramUtil + otherWeightedSum));
//                int64 thisDramUtil = (double) idealDramUtil * (0.3 + 0.4 * (thisScore / 100));
                if (thisDramUtil < lowerLimit and idealDramUtil >= lowerLimit) {
                    thisDramUtil = lowerLimit;
                }
                newPredictionCycles = (100 * totalFromDram) / thisDramUtil;
//                printf("\tthisDramUtil=%ld, newPredictionCycles=%lu\n", thisDramUtil, newPredictionCycles);
                gv.qos_threadDramUtil[tid] = thisDramUtil;
            } else {
                gv.qos_threadDramUtil[tid] = idealDramUtil;
            }
        } else {
            gv.qos_threadDramUtil[tid] = idealDramUtil;
        }

        if (newPredictionCycles > 0) {
            uint numTile = mapping.totalTiles;
            uint64 epoch = newPredictionCycles / (numTile / mapping.accUtil);
            uint64 maxReq = std::max((uint64) 64, totalFromDram / numTile); // no less than the size of a burst
            gv.bw_threadEpoch[tid] = epoch;
            gv.bw_threadMaxReq[tid] = maxReq;
//            printf("\tepoch=%lu, maxReq=%lu\n", epoch, maxReq);
        }
//        printf("MEMORY: end, tid=%u\n", tid);
    }

    void Runtime::setPageTable(uint accId, uint vPage, uint pPage) {
        dynamic_cast<BaseSPMSystem *>(RT.system)->accels[accId].setPageTable(vPage, pPage, true);
    }

    void Runtime::invalidPageTable(uint accId, uint vPage) {
        dynamic_cast<BaseSPMSystem *>(RT.system)->accels[accId].invalidPageTable(vPage);
    }

    PipelineRuntime::PipelineRuntime(): dram_addr_manager_(EXT_DRAM_ADDR_OFFSET, std::numeric_limits<uint32>::max()) {
        Runtime();
    }

    void PipelineRuntime::PipelineRuntimeInit(uint noc_w, uint noc_h, uint noc_bytes_per_cycle, AcceleratorManager::PipelineAccAllocStrategy acc_alloc_strategy, PipelineSpmAllocStrategy spm_alloc_strategy) {
        noc_h_ = noc_h;
        noc_w_ = noc_w;
        noc_bytes_per_cycle_ = noc_bytes_per_cycle;

        assert(system != nullptr);
        num_accels_ = hw.numAccels;
        pages_per_bank_ = hw.nPagesPerBank;
        page_bytes_size_ = hw.pageBytes;
        
        accelFetchCmdCount.resize(num_accels_, 0);
        accelSendCmdCount.resize(num_accels_, 0);
        accelFlushCmdCount.resize(num_accels_, 0);

        spm_vpage_manager_.resize(num_accels_, IntervalManager(PIPELINE_VPAGE_OFFSET, std::numeric_limits<long long>::max()));

        spm_ppage_manager_.init(num_accels_, pages_per_bank_, page_bytes_size_);
        std::cout << "wzydebug: num_accels=" << num_accels_ << ", pages_per_bank_=" << pages_per_bank_ << ", page_bytes_size_=" << page_bytes_size_ << std::endl;

        accel_manager_.init(noc_w_, noc_h_, acc_alloc_strategy);

        spm_alloc_strategy_ = spm_alloc_strategy;
    }

    void PipelineRuntime::ScheduleRuntimeInit(const std::string profiling_filepath, uint batch_size_per_task, uint num_tasks, uint num_threads, float time_out_threshold, uint schedule_partition_num, uint model_type_num, const std::string& method, ScheduleAccAllocStrategy schedule_acc_alloc_strategy, float acc_increase_factor, float spm_mindis_init_factor, float spm_mindis_decrease_factor) {
        profiling_result_.loadFromFile(profiling_filepath);
        batch_size_per_task_ = batch_size_per_task;
        num_threads_ = num_threads;
        schedule_partition_num_ = schedule_partition_num;
        method_ = method;
        schedule_acc_alloc_strategy_ = schedule_acc_alloc_strategy;
        acc_increase_factor_ = acc_increase_factor;
        spm_mindis_init_factor_ = spm_mindis_init_factor;
        spm_mindis_decrease_factor_ = spm_mindis_decrease_factor;

        qos_model_target_cycles_.resize(model_type_num, 0);

        qos_next_task_idx_ = 0;
        qos_task_model_type_queue_.resize(num_tasks, 0);
        qos_task_dispatch_time_.resize(num_tasks, 0);
        qos_task_start_time_.resize(num_tasks, 0);
        qos_task_end_time_.resize(num_tasks, 0);

        qos_task_subgraph_start_alloc_time_.resize(num_tasks, std::vector<uint64>(schedule_partition_num, 0));
        qos_task_subgraph_start_run_time_.resize(num_tasks, std::vector<uint64>(schedule_partition_num, 0));
        qos_task_subgraph_end_run_time_.resize(num_tasks, std::vector<uint64>(schedule_partition_num, 0));
        qos_task_subgraph_acc_num_.resize(num_tasks, std::vector<uint>(schedule_partition_num, 0));
        qos_task_subgraph_slack_time_.resize(num_tasks, std::vector<int>(schedule_partition_num, 0));
        qos_task_subgraph_est_time_subgraph_.resize(num_tasks, std::vector<uint64>(schedule_partition_num, 0));
        qos_task_subgraph_est_time_task_.resize(num_tasks, std::vector<uint64>(schedule_partition_num, 0));


        
        qos_thread_current_task_idx_.resize(num_threads, 0);  // current task index of each thread

        qos_thread_current_score_.resize(num_threads, 0);  // task_idx -> score for computeRepartition
        qos_thread_nxt_schedule_time_.resize(num_threads, 0);

        acc_occcupied_num_ = 0;
        thread_acc_occupied_num_.resize(num_threads_, 0);

        spm_pages_occupied_num_ = 0;
        thread_spm_occupied_num_.resize(num_threads_, 0);

        thread_acc_target_idx_.resize(num_threads_, 0);
        thread_acc_wait_target_idx_.resize(num_threads_, 0);
        thread_spm_pages_per_acc_kb_target_idx_.resize(num_threads_, 0);

        thread_acc_alloc_timeout_.resize(num_threads, 0);
        time_out_threshold_ = time_out_threshold;

        thread_current_model_type_.resize(num_threads_, 0);
        thread_current_subgraph_idx_.resize(num_threads_, 0);
    }

    uint PipelineRuntime::GetMinimumAccIdx(const std::vector<uint>& accSet, const std::vector<uint> cmdCount) {
        assert(!accSet.empty());
        uint accIdx = 0;
        for (uint i = 0;i < accSet.size();++ i) {
            if (cmdCount[accSet[i]] < cmdCount[accSet[accIdx]]) {
                accIdx = i;
            }
        }
        return accIdx;
    }

    bool PipelineRuntime::AllocAcc(std::shared_ptr<ScheduleAction> schedule_action) {
        const auto& segment_mapping = schedule_action->segment_mapping_;
        uint acc_need = segment_mapping->accUtil;
        auto acc_list = accel_manager_.AllocateAccels(acc_need);
        if (acc_list.empty()) {
            return false;
        }
        assert(acc_list.size() == acc_need);

        auto& acc_source = schedule_action->acc_source_;
        acc_source.num_acc_ = acc_need;
        acc_source.all_acc_ids_ = acc_list;
        acc_source.acc_ids_.clear();

        acc_source.acc_ids_.reserve(segment_mapping->stage_mapping_vec.size());
        auto it = acc_source.all_acc_ids_.begin();
        for (uint i = 0;i < segment_mapping->stage_mapping_vec.size();++ i) {
            uint stage_acc_need = segment_mapping->stage_mapping_vec[i].accUtil;
            acc_source.acc_ids_.push_back(std::vector<uint>(it, it + stage_acc_need));
            it += stage_acc_need;
        }
        assert(it == acc_source.all_acc_ids_.end());
        LOG(RUNTIME_SOURCE_ALLOC) << "Allocated " << acc_need << " for action " << schedule_action->action_id_ << " accelerators: " << toString(acc_list);
        return true;
    }

    bool PipelineRuntime::AllocSpm(std::shared_ptr<ScheduleAction> schedule_action) {
        if (spm_alloc_strategy_ == PipelineSpmAllocStrategy::ALL_BANK_PAGE_INTER) {
            return AllocSpmAllBankPageInter(schedule_action);
        } else if (spm_alloc_strategy_ == PipelineSpmAllocStrategy::MIN_DIS) {
            return AllocSpmMinDis(schedule_action);
        } else {
            assert(false);
        }
        return false;
    }

    std::shared_ptr<ScheduleAction> PipelineRuntime::GenerateAction(std::shared_ptr<SegmentMapping> segment_mapping, Model* model) {
        // 创建ScheduleAction对象
        auto schedule_action = std::make_shared<ScheduleAction>();
        
        // 设置segment_mapping
        schedule_action->segment_mapping_ = segment_mapping;

        schedule_action->model = model;
        
        // 分配加速器资源
        LOG(RUNTIME_SOURCE_ALLOC) << "Allocating resources for action " << schedule_action->action_id_;
        LOG(RUNTIME_SOURCE_ALLOC) << "Acc num before alloc: " << accel_manager_.GetAvailableAccelCount();
        if (!AllocAcc(schedule_action)) {
            assert(false);
            return nullptr; // 分配失败，返回空指针
        }
        LOG(RUNTIME_SOURCE_ALLOC) << "Acc num aflter alloc: " << accel_manager_.GetAvailableAccelCount();
        
        // 分配SPM资源
        LOG(RUNTIME_SOURCE_ALLOC) << "Spm num before alloc: " << spm_ppage_manager_.getTotalRemainPages();
        if (!AllocSpm(schedule_action)) {
            assert(false);
            return nullptr; // 分配失败，返回空指针
        }
        LOG(RUNTIME_SOURCE_ALLOC) << "Spm num aflter alloc: " << spm_ppage_manager_.getTotalRemainPages();
        
        // 返回创建的action
        return schedule_action;
    }

    void PipelineRuntime::ReleaseAction(std::shared_ptr<ScheduleAction> schedule_action) {
        // Release accelerators
        if (!schedule_action->acc_source_.all_acc_ids_.empty()) {
            accel_manager_.ReleaseAccels(schedule_action->acc_source_.all_acc_ids_);
            schedule_action->acc_source_.clear();
        }

        uint total_release_pages = 0;

        // Release SPM pages
        SpmSource& spm_source = schedule_action->spm_source_;
        LOG(RUNTIME_SOURCE_ALLOC) << "Releasing SPM pages for action " << schedule_action->action_id_;
        LOG(RUNTIME_SOURCE_ALLOC) << "Spm pages before release " << spm_ppage_manager_.getTotalRemainPages();
        LOG(RUNTIME_SOURCE_ALLOC) << "Spm pages wil be released: " << spm_source.num_spm_pages_;
        
        // Release weight pages
        for (auto& kv : spm_source.weight_spm_pages) {
            if (kv.second) {
                total_release_pages += kv.second->size();
                spm_ppage_manager_.freePages(kv.second);
                kv.second->clear();
            }
        }
        
        // Release in-stage pages
        for (auto& stage_pages : spm_source.in_stage_spm_pages_) {
            for (auto& kv : stage_pages) {
                for (auto& page_set : kv.second) {
                    if (page_set) {
                        total_release_pages += page_set->size();
                        spm_ppage_manager_.freePages(page_set);
                        page_set->clear();
                    }
                }
            }
        }
        
        // Release ring buffer pages
        for (auto& kv : spm_source.ring_buffer_spm_pages_) {
            for (auto& page_set : kv.second) {
                if (page_set) {
                    total_release_pages += page_set->size();
                    spm_ppage_manager_.freePages(page_set);
                    page_set->clear();
                }
            }
        }
        
        LOG(RUNTIME_SOURCE_ALLOC) << "Spm pages after release: " << spm_ppage_manager_.getTotalRemainPages();
        assert(total_release_pages == schedule_action->spm_source_.num_spm_pages_);

        // Clear the spm_source
        spm_source.clear();

    }

    std::shared_ptr<PipelineMapping> PipelineRuntime::GetPipelineMapping(Model* model, const std::string &mapping_path) {
        auto pipeline_mapping = std::make_shared<PipelineMapping>(PipelineMappingParser::pipelineCandidateToPipelineMapping(model, mapping_path));
        return pipeline_mapping;
    }

    PipelineProfilingResult::Target PipelineRuntime::GenerateTarget(uint target_acc, uint target_spm_per_acc_kb, uint model_idx) {
        auto target = PipelineProfilingResult::Target();
        target.acc = target_acc;
        target.batch = RT.batch_size_per_task_;
        target.spm_per_acc_kb = target_spm_per_acc_kb;
        target.noc_bw = RT.noc_bytes_per_cycle_;
        target.dram_bw = 19;
        target.partition_num = RT.schedule_partition_num_;
        target.model_idx = model_idx;

        return target;
    }

    PipelineProfilingResult::Target PipelineRuntime::GenerateTargetFromIdx(uint target_acc_idx, uint target_spm_per_acc_kb_idx, uint model_idx) {
        uint acc = profiling_result_.getAvailableAccs()[target_acc_idx];
        uint spm_per_acc_kb = profiling_result_.getAvailableSpmPerAccKb()[target_spm_per_acc_kb_idx];
        return GenerateTarget(acc, spm_per_acc_kb, model_idx);
    }

    bool PipelineRuntime::AllocSpmAllBankPageInter(std::shared_ptr<ScheduleAction> schedule_action) {
        const auto& segment_mapping = schedule_action->segment_mapping_;
        uint num_stage = segment_mapping->stage_mapping_vec.size();
        
        // Step 1. 计算总共需要的页数
        uint total_pages_needed = segment_mapping->totalSpmUtil;        
        LOG(RUNTIME_SOURCE_ALLOC) << "Allocated " << total_pages_needed << " for action " << schedule_action->action_id_ << " spm pages";

        // Step 2. 从SPM管理器分配物理页
        auto [page_set, failed_page_num] = spm_ppage_manager_.allocateAllBankInterlace(total_pages_needed);
        if (failed_page_num != 0) {
            // 分配失败，释放已分配的页并返回false
            if (page_set && !page_set->empty()) {
                spm_ppage_manager_.freePages(page_set);
            }
            LOG(RUNTIME_SOURCE_ALLOC) << "Failed to allocate " << total_pages_needed << " for action " << schedule_action->action_id_ << ".failed " << failed_page_num << " spm pages";
            return false;
        }
        
        // Step 3. 初始化SpmSource
        SpmSource& spm_source = schedule_action->spm_source_;
        spm_source.clear();
        spm_source.num_spm_pages_ = total_pages_needed;
        spm_source.in_stage_spm_pages_.resize(num_stage);
        
        // Step 4. 分配物理页
        auto page_it = page_set->begin();

        // 分配weight
        for (auto [tensor_id, weight_spm_need]: segment_mapping->tensorSpmUtilWeight) {
            assert(static_cast<uint>(page_set->end() - page_it) >= weight_spm_need);
            spm_source.weight_spm_pages[tensor_id] = std::make_shared<PageSet>(page_it, page_it + weight_spm_need);
            page_it += weight_spm_need;
        }
        

        // 分配in stage spm pages
        for (uint stage_idx = 0; stage_idx < num_stage; ++stage_idx) { 
            const auto& stage_mapping = segment_mapping->stage_mapping_vec[stage_idx];
            for (auto [tensor_id, in_stage_spm_need_total]: segment_mapping->tensorSpmUtilInStage[stage_idx]) {
                uint buffer_num = stage_mapping.isDoubleBuffer(tensor_id) ? 2 : 1;
                uint spm_need_per = in_stage_spm_need_total / buffer_num;
                assert(static_cast<uint>(page_set->end() - page_it) >= in_stage_spm_need_total);
                assert(in_stage_spm_need_total == buffer_num * spm_need_per);

                // 为每个buffer分配页
                for (uint i = 0; i < buffer_num; ++i) {
                    PageSetShared buffer_pages = std::make_shared<PageSet>(page_it, page_it + spm_need_per);
                    spm_source.in_stage_spm_pages_[stage_idx][tensor_id].push_back(buffer_pages);
                    page_it += spm_need_per;
                }
            }
        }

        // 分配shared tensors
        for (auto [tensor_id, in_stage_spm_need_total]: segment_mapping->tensorSpmUtilShared) {
            const auto& stage_idx_vec = segment_mapping->entryTensorToStageIdx[tensor_id];
            assert(!stage_idx_vec.empty());

            // 为shared tensor分配页，所有stage共享相同的页
            std::vector<PageSetShared> shared_page_sets;
            
            {
                const auto& stage_mapping = segment_mapping->stage_mapping_vec[stage_idx_vec[0]];
                uint buffer_num = stage_mapping.isDoubleBuffer(tensor_id) ? 2 : 1;
                uint spm_need_per = in_stage_spm_need_total / buffer_num;

                assert(static_cast<uint>(page_set->end() - page_it) >= in_stage_spm_need_total);
                assert(in_stage_spm_need_total == buffer_num * spm_need_per);

                for (uint i = 0; i < buffer_num; ++i) {
                    PageSetShared buffer_pages = std::make_shared<PageSet>(page_it, page_it + spm_need_per);
                    shared_page_sets.push_back(buffer_pages);
                    page_it += spm_need_per;
                }
            }
            
            // 将相同的页分配给所有使用此tensor的stage
            for (uint stage_idx : segment_mapping->entryTensorToStageIdx[tensor_id]) {
                spm_source.in_stage_spm_pages_[stage_idx][tensor_id] = shared_page_sets;
            }
            for (uint stage_idx : segment_mapping->exportTensorToStageIdx[tensor_id]) {
                spm_source.in_stage_spm_pages_[stage_idx][tensor_id] = shared_page_sets;
            }
        }
        
        // 分配ring buffer
        for (const auto& [tensor_id, ringbuffer_config] : segment_mapping->ring_buffer_config_) {
            if (!ringbuffer_config.use_) {
                continue;
            }

            bool use_max_size_in_ringbuffer = false;
            bool is_dram_depen = false;
            
            // 检查tensor类型
            std::vector<uint> all_stage_idx;
            if (segment_mapping->entryTensorToStageIdx.find(tensor_id) != segment_mapping->entryTensorToStageIdx.end()) {
                all_stage_idx.insert(all_stage_idx.end(),
                                     segment_mapping->entryTensorToStageIdx.at(tensor_id).begin(),
                                     segment_mapping->entryTensorToStageIdx.at(tensor_id).end());
            }
            if (segment_mapping->exportTensorToStageIdx.find(tensor_id) != segment_mapping->exportTensorToStageIdx.end()) {
                all_stage_idx.insert(all_stage_idx.end(),
                                     segment_mapping->exportTensorToStageIdx.at(tensor_id).begin(),
                                     segment_mapping->exportTensorToStageIdx.at(tensor_id).end());
            }

            for (uint stage_idx: all_stage_idx) {
                const auto& stage_mapping = segment_mapping->stage_mapping_vec[stage_idx];
                if (stage_mapping.isAllRingbufferPipeBuffer(tensor_id) || stage_mapping.isSharedPipeBuffer(tensor_id)) {
                    use_max_size_in_ringbuffer = true;
                    break;
                }
                if (stage_mapping.isDRAMDepenPipeBuffer(tensor_id)) {
                    is_dram_depen = true;
                    break;
                }
            }

            if (is_dram_depen) {
                // DRAM dependent tensor不分配物理页
                continue;
            }
            
            uint spm_need = use_max_size_in_ringbuffer ? 
                segment_mapping->tensor_id_to_max_spm_page.at(tensor_id) : 
                segment_mapping->tensor_id_to_min_spm_page.at(tensor_id);

            assert(ringbuffer_config.spm_util_per_ == spm_need);
            
            uint buffer_num = ringbuffer_config.size_;
            
            assert(spm_need > 0);
            assert(static_cast<uint>(page_set->end() - page_it) >= spm_need * buffer_num);
            
            // 为ring buffer分配页
            spm_source.ring_buffer_spm_pages_[tensor_id].reserve(buffer_num);
            for (uint i = 0; i < buffer_num; ++i) {
                PageSetShared buffer_pages = std::make_shared<PageSet>(page_it, page_it + spm_need);
                spm_source.ring_buffer_spm_pages_[tensor_id].push_back(buffer_pages);
                page_it += spm_need;
            }
        }
        
        assert(page_it == page_set->end());
        return true;
    }

    bool PipelineRuntime::AllocSpmMinDis(std::shared_ptr<ScheduleAction> schedule_action) {
        const auto& segment_mapping = schedule_action->segment_mapping_;
        SpmSource& spm_source = schedule_action->spm_source_;
        assert(spm_source.num_spm_pages_ == 0);
        spm_source.clear();

        // total pages needed
        uint total_pages_needed = static_cast<uint>(segment_mapping->totalSpmUtil);
        spm_source.num_spm_pages_ = total_pages_needed;

        LOG(RUNTIME_SOURCE_ALLOC) << "Allocated " << total_pages_needed << " for action " << schedule_action->action_id_ << " spm pages";

        // helper: compute manhattan distance between two accel ids
        auto coord_of = [&](uint accId) {
            return accel_manager_.IdToCoord(accId);
        };
        auto manhattan = [&](uint a, uint b) {
            auto ca = coord_of(a);
            auto cb = coord_of(b);
            return (int) (std::abs((int)ca.first - (int)cb.first) + std::abs((int)ca.second - (int)cb.second));
        };

        // helper to flatten acc ids for given stage indices
        auto accs_for_stage = [&](const std::vector<uint>& stage_idxs) {
            std::vector<uint> accs;
            for (uint si : stage_idxs) {
                if (si < schedule_action->acc_source_.acc_ids_.size()) {
                    for (uint a : schedule_action->acc_source_.acc_ids_[si]) accs.push_back(a);
                }
            }
            // unique
            std::sort(accs.begin(), accs.end());
            accs.erase(std::unique(accs.begin(), accs.end()), accs.end());
            return accs;
        };

        // helper to allocate pages given preferred acc list and required pages
        auto allocate_for_pref = [&](const std::vector<uint>& preferred, uint need) -> std::pair<PageSetShared, uint> {
            if (need == 0) return {std::make_shared<PageSet>(), 0};

            // try preferred banks first
            if (!preferred.empty()) {
                auto [pset, failed] = spm_ppage_manager_.allocateFixedBankInterlace(preferred, need);
                if (failed == 0) return {pset, 0};
                // if partially allocated, we need to allocate the remainder
                uint allocated = need - failed;
                PageSetShared combined = std::make_shared<PageSet>();
                if (pset && !pset->empty()) {
                    combined->insert(combined->end(), pset->begin(), pset->end());
                }
                uint remaining = failed;

                // build candidate bank list: all accs excluding preferred, sorted by distance to preferred set
                std::vector<uint> candidates = schedule_action->acc_source_.all_acc_ids_;
                // remove preferred duplicates
                std::sort(candidates.begin(), candidates.end());
                candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
                // compute integer average distance to preferred set
                std::vector<std::pair<int, uint>> cand_dist;
                for (uint c : candidates) {
                    if (std::find(preferred.begin(), preferred.end(), c) != preferred.end()) continue;
                    int d_sum = 0;
                    for (uint p : preferred) d_sum += manhattan(c, p);
                    int avg = d_sum / (int)preferred.size(); // integer average
                    cand_dist.push_back({avg, c});
                }
                std::sort(cand_dist.begin(), cand_dist.end(), [](const auto&a,const auto&b){ if (a.first!=b.first) return a.first<b.first; return a.second<b.second; });

                // Group candidates by equal integer average distance so banks with
                // the same average distance are allocated in an interleaved fashion together.
                std::vector<std::vector<uint>> groups;
                bool first_group = true;
                int last_dist = 0;
                for (auto &pr : cand_dist) {
                    if (first_group) {
                        groups.push_back(std::vector<uint>{pr.second});
                        last_dist = pr.first;
                        first_group = false;
                    } else {
                        if (pr.first == last_dist) {
                            groups.back().push_back(pr.second);
                        } else {
                            groups.push_back(std::vector<uint>{pr.second});
                            last_dist = pr.first;
                        }
                    }
                }

                // try allocate progressively by adding groups from nearest to farthest
                if (!groups.empty()) {
                    for (uint g = 0; g < groups.size() && remaining > 0; ++g) {
                        // build prefix of groups 0..g
                        std::vector<uint> prefix_order;
                        for (uint gi = 0; gi <= g; ++gi) {
                            // append all banks in group gi
                            prefix_order.insert(prefix_order.end(), groups[gi].begin(), groups[gi].end());
                        }
                        auto [pset2, failed2] = spm_ppage_manager_.allocateFixedBankInterlace(prefix_order, remaining);
                        if (pset2 && !pset2->empty()) combined->insert(combined->end(), pset2->begin(), pset2->end());
                        remaining = failed2;
                        if (remaining == 0) break;
                    }
                }

                if (remaining != 0) {
                    // if still remaining, assert false
                    LOG(RUNTIME_SOURCE_ALLOC) << "After preferred allocation, remaining pages to allocate: " << remaining;
                    std::cout << "Error: " << spm_ppage_manager_.getTotalRemainPages() << " pages remaining, failed to allocate " << remaining << " pages with preferred accs " << toString(preferred) << std::endl;
                    exit(0);
                }

                return {combined, remaining};
            } else {
                // no preferred -> allocate from all banks interleaved
                return spm_ppage_manager_.allocateAllBankInterlace(need);
            }
        };

        // track allocated pages to free on failure
        std::vector<PageSetShared> allocated_collector;

        // 1) Allocate weight tensors
        for (auto &kv : segment_mapping->tensorSpmUtilWeight) {
            TensorId tid = kv.first;
            uint need = static_cast<uint>(kv.second);
            // preferred accs: stages that touch this tensor (entry/export)
            std::vector<uint> preferred;
            if (segment_mapping->entryTensorToStageIdx.find(tid) != segment_mapping->entryTensorToStageIdx.end()) {
                assert(false);
            } else if (segment_mapping->exportTensorToStageIdx.find(tid) != segment_mapping->exportTensorToStageIdx.end()) {
                assert(false);
            } else {
                // Try to locate the stage(s) that reference this weight tensor
                std::vector<uint> owner_stages_search;
                for (uint si = 0; si < segment_mapping->stage_mapping_vec.size(); ++si) {
                    const auto &sm = segment_mapping->stage_mapping_vec[si];
                    // check fixTensorDramBypassId
                    if (std::find(sm.fixTensorDramBypassId.begin(), sm.fixTensorDramBypassId.end(), tid) != sm.fixTensorDramBypassId.end()) {
                        owner_stages_search.push_back(si);
                        continue;
                    }
                }
                if (!owner_stages_search.empty()) {
                    preferred = accs_for_stage(owner_stages_search);
                } else {
                    assert(false);
                }
            }

            auto [pset, failed] = allocate_for_pref(preferred, need);
            if (failed != 0) {
                assert(false);
            }
            spm_source.weight_spm_pages[tid] = pset;
            allocated_collector.push_back(pset);
        }

        // 2) Allocate in-stage per stage
        uint num_stage = segment_mapping->stage_mapping_vec.size();
        spm_source.in_stage_spm_pages_.resize(num_stage);
        for (uint stage_idx = 0; stage_idx < num_stage; ++stage_idx) {
            for (auto &kv : segment_mapping->tensorSpmUtilInStage[stage_idx]) {
                TensorId tid = kv.first;
                uint total_need = static_cast<uint>(kv.second);
                const auto& stage_mapping = segment_mapping->stage_mapping_vec[stage_idx];
                uint buffer_num = stage_mapping.isDoubleBuffer(tid) ? 2 : 1;
                uint per_buf = total_need / buffer_num;

                // preferred: accelerators assigned to this stage
                std::vector<uint> preferred = accs_for_stage({stage_idx});

                for (uint b = 0; b < buffer_num; ++b) {
                    auto [pset, failed] = allocate_for_pref(preferred, per_buf);
                    if (failed != 0) {
                        assert(false);
                    }
                    spm_source.in_stage_spm_pages_[stage_idx][tid].push_back(pset);
                    allocated_collector.push_back(pset);
                }
            }
        }

        // 3) Allocate shared tensors
        for (auto &kv : segment_mapping->tensorSpmUtilShared) {
            TensorId tid = kv.first;
            uint total_need = static_cast<uint>(kv.second);
            // choose preferred based on sharedTensorIsReadFirst
            bool read_first = false;
            if (segment_mapping->sharedTensorIsReadFirst.find(tid) != segment_mapping->sharedTensorIsReadFirst.end())
                read_first = segment_mapping->sharedTensorIsReadFirst.at(tid);

            std::vector<uint> preferred;
            if (read_first) {
                // prefer input stage
                if (segment_mapping->entryTensorToStageIdx.find(tid) != segment_mapping->entryTensorToStageIdx.end()) {
                    preferred = accs_for_stage(segment_mapping->entryTensorToStageIdx.at(tid));
                }
            } else {
                if (segment_mapping->exportTensorToStageIdx.find(tid) != segment_mapping->exportTensorToStageIdx.end()) {
                    preferred = accs_for_stage(segment_mapping->exportTensorToStageIdx.at(tid));
                }
            }
            
            if (preferred.empty()) {
                assert(false);
            }
        
            uint buffer_num = 1;
            // determine buffer num from one of the stages that use it
            if (segment_mapping->entryTensorToStageIdx.find(tid) != segment_mapping->entryTensorToStageIdx.end()) {
                uint sidx = segment_mapping->entryTensorToStageIdx.at(tid)[0];
                buffer_num = segment_mapping->stage_mapping_vec[sidx].isDoubleBuffer(tid) ? 2 : 1;
            } else if (segment_mapping->exportTensorToStageIdx.find(tid) != segment_mapping->exportTensorToStageIdx.end()) {
                uint sidx = segment_mapping->exportTensorToStageIdx.at(tid)[0];
                buffer_num = segment_mapping->stage_mapping_vec[sidx].isDoubleBuffer(tid) ? 2 : 1;
            }
            assert(buffer_num == 2);

            uint per_buf = total_need / buffer_num;
            std::vector<PageSetShared> shared_pages;
            for (uint b = 0; b < buffer_num; ++b) {
                auto [pset, failed] = allocate_for_pref(preferred, per_buf);
                if (failed != 0) {
                    assert(false);
                }
                shared_pages.push_back(pset);
                allocated_collector.push_back(pset);
            }

            // assign shared pages to all stages that use this tensor
            if (segment_mapping->entryTensorToStageIdx.find(tid) != segment_mapping->entryTensorToStageIdx.end()) {
                for (uint sidx : segment_mapping->entryTensorToStageIdx.at(tid)) {
                    spm_source.in_stage_spm_pages_[sidx][tid] = shared_pages;
                }
            }
            if (segment_mapping->exportTensorToStageIdx.find(tid) != segment_mapping->exportTensorToStageIdx.end()) {
                for (uint sidx : segment_mapping->exportTensorToStageIdx.at(tid)) {
                    spm_source.in_stage_spm_pages_[sidx][tid] = shared_pages;
                }
            }
        }

        // 4) Allocate ring buffer
        for (const auto& [tensor_id, ringbuffer_config] : segment_mapping->ring_buffer_config_) {
            if (!ringbuffer_config.use_) continue;

            // determine stages that own this tensor: combine entry and export stage indices
            std::vector<uint> owner_stages;
            if (segment_mapping->entryTensorToStageIdx.find(tensor_id) != segment_mapping->entryTensorToStageIdx.end()) {
                owner_stages.insert(owner_stages.end(),
                                    segment_mapping->entryTensorToStageIdx.at(tensor_id).begin(),
                                    segment_mapping->entryTensorToStageIdx.at(tensor_id).end());
            }
            if (segment_mapping->exportTensorToStageIdx.find(tensor_id) != segment_mapping->exportTensorToStageIdx.end()) {
                owner_stages.insert(owner_stages.end(),
                                    segment_mapping->exportTensorToStageIdx.at(tensor_id).begin(),
                                    segment_mapping->exportTensorToStageIdx.at(tensor_id).end());
            }
            // deduplicate and sort
            std::sort(owner_stages.begin(), owner_stages.end());
            owner_stages.erase(std::unique(owner_stages.begin(), owner_stages.end()), owner_stages.end());

            std::vector<uint> preferred = accs_for_stage(owner_stages);

            bool is_all = false;
            bool is_dram_depen = false;
            for (uint stage_idx: owner_stages) {
                const auto& stage_mapping = segment_mapping->stage_mapping_vec[stage_idx];
                if (stage_mapping.isAllRingbufferPipeBuffer(tensor_id)) is_all = true;
                if (stage_mapping.isDRAMDepenPipeBuffer(tensor_id)) is_dram_depen = true;
            }
            if (is_dram_depen) continue;

            uint spm_need = is_all ? segment_mapping->tensor_id_to_max_spm_page.at(tensor_id) : segment_mapping->tensor_id_to_min_spm_page.at(tensor_id);
            uint buffer_num = ringbuffer_config.size_;

            spm_source.ring_buffer_spm_pages_[tensor_id].reserve(buffer_num);
            for (uint i = 0; i < buffer_num; ++i) {
                auto [pset, failed] = allocate_for_pref(preferred, spm_need);
                if (failed != 0) {
                    for (auto &p : allocated_collector) spm_ppage_manager_.freePages(p);
                    if (pset && !pset->empty()) spm_ppage_manager_.freePages(pset);
                    return false;
                }
                spm_source.ring_buffer_spm_pages_[tensor_id].push_back(pset);
                allocated_collector.push_back(pset);
            }
        }

        return true;
    }

    Layer::Layer(const std::string &type_, uint numTensors, uint tid_) {
        type = type_;
        tensorList.resize(numTensors);
        tensorList2.resize(numTensors);
        tensorIds.resize(numTensors);
        tid = tid_;
        lgType = "none";

        if (Layer::groupIdOffsetCounter >= 20) {
            Layer::groupIdOffsetCounter = 0; 
        }

        runtimeType = LayerRuntimeType::SINGLE_LAYER;

        resetAllState();
    }

    uint Layer::getNxtGroupIdx() {
        uint re = Layer::groupIdOffsetCounter;
        Layer::groupIdOffsetCounter ++;
        if (Layer::groupIdOffsetCounter >= (1 << 20)) {
            Layer::groupIdOffsetCounter = 1;
        }
        return re;
    }

    void Layer::setTid(uint tid_) { 
        tid = tid_; 
    }

    void Layer::resetAllState() {
        singleLayerState = SingleLayerRuntimeState::STATE_BEGINNING;

        cmdCounter = 0;
        mapping = NULL;
        
        pipelineLayerState = PipelineLayerRuntimeState::STATE_BEGINNING;

        tensorPageTableReady.clear();
    }

    void Layer::setSingleMode() {
        runtimeType = LayerRuntimeType::SINGLE_LAYER;
    }

    void Layer::setPipelineMode() {
        runtimeType = LayerRuntimeType::PIPELINE_LAYER;
    }

    bool Layer::tick(uint batch) {
        if(runtimeType == LayerRuntimeType::SINGLE_LAYER) {
            return tickSingle(batch);
        } else if(runtimeType == LayerRuntimeType::PIPELINE_LAYER) {
            return tickPipeline();
        } else {
            assert(false);
        }
        return false;
    }


    nlohmann::json Layer::toJson() const {
        nlohmann::json js;
#ifdef JSON_DBG
        js["paramList"] = paramList;
        js["type"] = type;
        js["lgType"] = lgType;
        {
            nlohmann::json tensor_js;
            for (auto &t: tensorList) {
                tensor_js.push_back(t.toJson());
            }
            js["tensor"] = tensor_js;
        }

        {
            nlohmann::json tensor2_js;
            for (auto &t: tensorList2) {
                tensor2_js.push_back(t.toJson());
            }
            js["tensor2"] = tensor2_js;
        }

        js["tensor_ids"] = tensorIds;
#endif
        return js;
    }

    std::string Layer::toString() const {
        return toJson().dump(JSON_DUMP_INDENTATION);
    }

    void Layer::loop(const std::vector<uint>& accId) {
        loop(accId, false);
    }

    bool Layer::tickSingle(uint batch) {
        //        if (RT.getCycles() % 10000 == 0) {
        //            printf("%lu Layer::tick, tid=%u, state=%u, modelIdx=%u, layerIdx=%u\n",
        //                   RT.getCycles(), tid, state, RT.gv.threadCurrentModelType[tid], RT.gv.threadCurrentLayerIdx[tid]);
        //        }
        switch (singleLayerState) {
            case SingleLayerRuntimeState::STATE_JOINING: {
                for (uint i = 0; i < mapping->accUtil; i++) {
                    uint accId = RT.gv.threadAccelAlloc[tid][i];
                    AccelTiledCMD *cmd = RT.getFinishedCmd(accId);
                    if (cmd != NULL) {
                        delete cmd;
                        cmdCounter--;
                    }
                }
                if (cmdCounter == 0) {
                    singleLayerState = SingleLayerRuntimeState::STATE_RELEASE_RESOURCE;
                }
                break;
            }

            case SingleLayerRuntimeState::STATE_ACC_ALLOC: {
                if (RT.gv.spm_threadInterLayerReuse[tid] and (not lgTypeIsHead())) {
                    // no need for accel alloc
//                    printf("%lu Layer::tick, STATE_ACC_ALLOC, interLayerReuse, tid=%u, accAllocList=%s\n",
//                           RT.getCycles(), tid, mudnac::toString(RT.gv.threadAccelAlloc[tid]).c_str());
                    assert(mapping->accUtil <= RT.gv.threadAccelAlloc[tid].size());
                    singleLayerState = SingleLayerRuntimeState::STATE_SPM_ALLOC;
                    break;
                }
                uint num = 0;
                if (RT.gv.spm_threadInterLayerReuse[tid]) {
                    num = mapping->lgAccUtil;
                } else {
                    num = mapping->accUtil;
                }
                assert(RT.gv.threadAccelAlloc[tid].size() == 0);
                bool r = RT.acquireAccel(tid, num);
                if (r) {
//                    printf("%lu Layer::tick, STATE_ACC_ALLOC, acquireAccel, tid=%u, accAllocList=%s\n",
//                           RT.getCycles(), tid, mudnac::toString(RT.gv.threadAccelAlloc[tid]).c_str());
                    singleLayerState = SingleLayerRuntimeState::STATE_SPM_ALLOC;
                } else {
//                    printf("%lu Layer::tick, waiting for accels, tid=%u, accelNum=%u\n", RT.getCycles(), tid, num);
                    RT.gv.threadWaitingAccCycles[tid]++;
                }
                break;
            }

            case SingleLayerRuntimeState::STATE_SPM_ALLOC: {
                if (RT.gv.spm_threadInterLayerReuse[tid] and (not lgTypeIsHead())) {
//                    printf("%lu Layer::tick, STATE_SPM_ALLOC, interLayerReuse, tid=%u, spmPages=%zu, "
//                           "spmAllocList=%s\n",
//                           RT.getCycles(), tid, RT.gv.threadSpmPageAlloc[tid].size(),
//                           mudnac::toString(RT.gv.threadSpmPageAlloc[tid]).c_str());
                    assert(divCeil(mapping->spmUtil, RT.hw.pageBytes) <= RT.gv.threadSpmPageAlloc[tid].size());
                    singleLayerState = SingleLayerRuntimeState::STATE_BW_ALLOC;
                    break;
                }
                uint num = 0;
                if (RT.gv.spm_threadInterLayerReuse[tid]) {
                    num = divCeil(mapping->lgSpmUtil, RT.hw.pageBytes);
                } else {
                    num = divCeil(mapping->spmUtil, RT.hw.pageBytes);
                }
                assert(RT.gv.threadSpmPageAlloc[tid].size() == 0);
                bool r = RT.acquireSPM(tid, num);
//                bool r = RT.acquireSPM(tid, std::max((uint) 1, num));
                if (r) {
//                    printf("%lu Layer::tick, STATE_SPM_ALLOC, acquireSPM, tid=%u, spmPages=%zu, "
//                           "spmAllocList=%s\n",
//                           RT.getCycles(), tid, RT.gv.threadSpmPageAlloc[tid].size(),
//                           mudnac::toString(RT.gv.threadSpmPageAlloc[tid]).c_str());
                    // setup page for bypass
                    {
                        assert(RT.gv.threadAccelAlloc[tid].size() > 0);
                        uint accId = RT.gv.threadAccelAlloc[tid][0];
                        uint pageForBypass =
                                (RT.system->config.accelSpmAddrType == AccelConfig::SPM_ADDR_TYPE_BLOCK_INTERLEAVED) ?
                                0 :
                                (RT.system->config.accelSpmAddrType == AccelConfig::SPM_ADDR_TYPE_PAGE_INTERLEAVED) ?
                                (accId / RT.system->config.getAccelsPerBank()) :
                                ((accId / RT.system->config.getAccelsPerBank()) * RT.system->config.getPagesPerBank());
                        RT.setPageForBypass(tid, pageForBypass);
                    }
                    // update spm_threadEstimatedReleasingTime, spm_threadEstimatedAcquiredPages
                    if (RT.gv.spmAllocMode == RT.gv.SPM_ALLOC_MODE_DYNAMIC) {
                        uint accAllocIdx = RT.gv.threadAccelAllocIdx[tid];
                        uint modelIdx = RT.gv.threadCurrentModelType[tid];
                        uint layerIdx = RT.gv.threadCurrentLayerIdx[tid];
                        auto &model = RT.threads[tid]->model;
                        uint64 estRelease = RT.getCycles() + RT.gv.pf_modelLayerCycles[accAllocIdx][modelIdx][layerIdx];
                        if (RT.gv.spm_threadInterLayerReuse[tid]) {
                            assert(lgTypeIsHead());
                            while (true) {
                                layerIdx++;
                                estRelease += RT.gv.pf_modelLayerCycles[accAllocIdx][modelIdx][layerIdx];
                                if (model->layers[layerIdx]->lgTypeIsTail()) { break; }
                            }
                        }
                        uint estPages = 0;
                        if (layerIdx + 1 < model->layers.size()) {
                            auto layer = model->layers[layerIdx + 1];
                            if (layer->lgTypeIsHead()) {
                                estPages = divCeil(layer->mappingTable[accAllocIdx].back().lgSpmUtil, RT.hw.pageBytes);
                            } else {
                                uint idx = mappingTable[accAllocIdx].size() - 2;
                                estPages = divCeil(layer->mappingTable[accAllocIdx][idx].spmUtil, RT.hw.pageBytes);
                            }
                        }
                        RT.gv.spm_threadEstimatedReleasingTime[tid] = estRelease;
                        RT.gv.spm_threadEstimatedAcquiredPages[tid] = estPages;
//                        printf("%lu Layer::tick, STATE_SPM_ALLOC, updateEstimated, tid=%u, estRelease=%lu, estPages=%u\n",
//                               RT.getCycles(), tid, estRelease, estPages);
                    }
                    singleLayerState = SingleLayerRuntimeState::STATE_BW_ALLOC;
                    break;
                } else {
//                    printf("%lu Layer::tick, waiting for SPM, tid=%u, pageNum=%u\n", RT.getCycles(), tid, num);
                    RT.gv.threadWaitingSpmCycles[tid]++;
                    if (RT.gv.spmAllocMode == RT.gv.SPM_ALLOC_MODE_DYNAMIC and RT.getCycles() >= spmWaitingThreshold) {
                        RT.spmRepartitionReduce(tid, spmWaitingThreshold);
                        mapping = &mappingTable[RT.gv.threadAccelAllocIdx[tid]][RT.gv.threadSpmAllocIdx[tid]];
                    }
                }
                break;
            }

            case SingleLayerRuntimeState::STATE_BW_ALLOC: {
                if (RT.gv.memoryAccessMode == RT.gv.MEMORY_ACCESS_MODE_LIMITED) {
                    RT.memoryRepartition(tid);
                    RT.setMemEpochRate(tid, RT.gv.bw_threadEpoch[tid], RT.gv.bw_threadMaxReq[tid]);
                }
                singleLayerState = SingleLayerRuntimeState::STATE_SENDING_CMD;
                break;
            }

            case SingleLayerRuntimeState::STATE_BEGINNING: {
//                printf("%lu Layer::tick, STATE_BEGINNING, tid=%u, modelIdx=%u, layerIdx=%u\n",
//                       RT.getCycles(), tid, RT.gv.threadCurrentModelType[tid], RT.gv.threadCurrentLayerIdx[tid]);
                singleLayerState = SingleLayerRuntimeState::STATE_RESOURCE_REPARTITION;
                break;
            }

            case SingleLayerRuntimeState::STATE_RESOURCE_REPARTITION: {
                if (RT.gv.accelAllocMode == RT.gv.ACCEL_ALLOC_MODE_DYNAMIC) {
                    if (not RT.gv.spm_threadInterLayerReuse[tid]) {
                        RT.computeRepartition(tid);
                    }
                }
                if (RT.gv.spmAllocMode == RT.gv.SPM_ALLOC_MODE_DYNAMIC or
                    RT.gv.spmAllocMode == RT.gv.SPM_ALLOC_MODE_FIX) {
                    RT.spmRepartition(tid, spmWaitingThreshold);
                }
//                printf("%lu Layer::tick, STATE_RESOURCE_REPARTITION, tid=%u, accAllocIdx=%u, spmAllocIdx=%u, "
//                       "interLayerReuse=%u\n",
//                       RT.getCycles(), tid, RT.gv.threadAccelAllocIdx[tid], RT.gv.threadSpmAllocIdx[tid],
//                       (uint) RT.gv.spm_threadInterLayerReuse[tid]);
                std::cout << "wzydebug: Layer::tick, STATE_RESOURCE_REPARTITION, tid=" << tid
                          << ", accAllocIdx=" << RT.gv.threadAccelAllocIdx[tid]
                          << ", spmAllocIdx=" << RT.gv.threadSpmAllocIdx[tid]
                          << ", interLayerReuse=" << (uint) RT.gv.spm_threadInterLayerReuse[tid]
                          << std::endl;
                mapping = &mappingTable[RT.gv.threadAccelAllocIdx[tid]][RT.gv.threadSpmAllocIdx[tid]];
                singleLayerState = SingleLayerRuntimeState::STATE_ACC_ALLOC;
                break;
            }

            case SingleLayerRuntimeState::STATE_SENDING_CMD: {
                for (uint i = 0;i < batch;++ i) {
                    loop(RT.gv.threadAccelAlloc[tid]);
                }
                singleLayerState = SingleLayerRuntimeState::STATE_JOINING;
                break;
            }

            case SingleLayerRuntimeState::STATE_RELEASE_RESOURCE: {
                RT.gv.qos_threadCurrentMemScore[tid] = 0;
                RT.gv.qos_threadDramUtil[tid] = 0;
                RT.gv.bw_threadEpoch[tid] = UINT64_MAX;
                RT.gv.bw_threadMaxReq[tid] = UINT64_MAX;
                RT.setMemEpochRate(tid, UINT64_MAX, UINT64_MAX);
                if ((not RT.gv.spm_threadInterLayerReuse[tid]) or lgTypeIsTail()) {
                    RT.gv.spm_threadInterLayerReuse[tid] = false;
                    RT.releaseSPM(tid, UINT32_MAX);
                    RT.gv.qos_threadCurrentScore[tid] = 400;
                    RT.releaseAccel(tid, UINT32_MAX);
                }
                singleLayerState = SingleLayerRuntimeState::STATE_ENDING;
                break;
            }

            case SingleLayerRuntimeState::STATE_ENDING: {
                break;
            }

            default: {
                assert(false);
            }
        }
        return singleLayerState == SingleLayerRuntimeState::STATE_ENDING;
    }

    LayerMapping* Layer::getLayerMapping(const PipeLayerMapperKey& key) {
        auto it = pipeLayerMappingTable.find(key);
        if (it == pipeLayerMappingTable.end()) {
            assert(false);
        }
        assert(it->second.size() == 1);
        return &(it->second.front());
    }
    bool Layer::tickPipeline() {
        switch (pipelineLayerState) {
            case PipelineLayerRuntimeState::STATE_BEGINNING: {
                if(mapping == NULL) return false;
                pipelineLayerStateChange(PipelineLayerRuntimeState::STATE_ACC_ALLOC);
                break;
            }
            case PipelineLayerRuntimeState::STATE_ACC_ALLOC: {
                if(!pipeAccAllocated) return false;
                pipelineLayerStateChange(PipelineLayerRuntimeState::STATE_SPM_ALLOC);
                break;
            }
            case PipelineLayerRuntimeState::STATE_SPM_ALLOC: {
                for(auto i: tensorPageTableReady) {
                    if(!i) return false;
                }
                for(auto accId: pipeAccAllocSet) {
                    RT.setAccPageForBypass(accId);
                }
                pipelineLayerStateChange(PipelineLayerRuntimeState::STATE_BW_ALLOC);
                break;
            }
            case PipelineLayerRuntimeState::STATE_BW_ALLOC: {
                //TODO: BW Allocation for pipeine
                for(auto accId: pipeAccAllocSet) {
                    RT.setAccMemEpochRate(accId, UINT64_MAX, UINT64_MAX);
                }
                pipelineLayerStateChange(PipelineLayerRuntimeState::STATE_SENDING_CMD);
                break;
            }
            case PipelineLayerRuntimeState::STATE_SENDING_CMD: { 
                loop(pipeAccAllocSet, true);
                pipelineLayerStateChange(PipelineLayerRuntimeState::STATE_JOINING);
                break;
            }
            case PipelineLayerRuntimeState::STATE_JOINING: {
                for (auto accId: pipeAccAllocSet) {
                    AccelTiledCMD *cmd = RT.getFinishedCmd(accId);
                    if (cmd != NULL) {
                        delete cmd;
                        cmdCounter--;
                    }
                }
                if (cmdCounter == 0) {
                    pipelineLayerStateChange(PipelineLayerRuntimeState::STATE_ENDING);
                }
                break;
            }
            case PipelineLayerRuntimeState::STATE_ENDING: {
                return true;
                break;
            }
        }
        return pipelineLayerState == PipelineLayerRuntimeState::STATE_ENDING;
    }
    
    bool Layer::lgTypeIsNone() { 
        return lgType == "none"; 
    }

    bool Layer::lgTypeIsHead() { 
        return lgType == "head"; 
    }

    bool Layer::lgTypeIsTail() { 
        return lgType == "tail"; 
    }

    bool Layer::lgTypeIsInter() { 
        return lgType == "inter"; 
    }
    void Layer::resetStateForPipelineReuse() {
        pipelineLayerState = PipelineLayerRuntimeState::STATE_BEGINNING;
    }

    void Layer::setPipeLayerMapping(LayerMapping *mapping_) {
        mapping = mapping_;
        tensorPageTableReady.clear();
        tensorPageTableReady.resize(mapping->dramBypass.size(), false);
    }

    void Layer::setPipeLayerMappingForLazyFetch(LayerMapping *mapping_) {
        mappingForLazyFetch = mapping_;
        tensorPageTableReady.clear();
        tensorPageTableReady.resize(mapping->dramBypass.size(), false);
    }

    void Layer::swapLayerMappingForLazyFetch() {
        std::swap(mapping, mappingForLazyFetch);
    }

    void Layer::setPipeAcc(const std::vector<uint> acc) {
        pipeAccAllocSet = acc;
        pipeAccAllocated = true;
    }

    void Layer::releasePipeAcc() {
        pipeAccAllocSet.clear();
        pipeAccAllocated = false;
    }

    uint Layer::getTensorPageCountUseIdx(uint tensorIdxInMapping) {
        assert(mapping != NULL);
        return mapping->spmTensorPageCount[tensorIdxInMapping];
    }
    uint Layer::getTensorPageCountUseId(TensorId tensorId) {
        assert(mapping != NULL);
        return mapping->spmTensorPageCount[getTensorIdxInLayer(tensorId)];
    }

    uint Layer::getTensorIdxInLayer(uint tensorId) {
        for(uint i = 0;i < tensorIds.size();i++) {
            if(tensorId == tensorIds[i]) {
                return i;
            }
        }
        assert(false);
        return -1;
    }

    uint Layer::getTensorId(uint tensorIdxInLayer) {
        assert(tensorIdxInLayer < tensorIds.size());
        return tensorIds[tensorIdxInLayer];
    }

    void Layer::setPipeSpmForAcc(uint tensorIdxInMapping, const std::vector<uint>& spmPPageSet) {
        assert(mapping != NULL);
        assert(!tensorPageTableReady[tensorIdxInMapping]);
        assert(spmPPageSet.size() >= getTensorPageCountUseIdx(tensorIdxInMapping));
        for(uint vPage = mapping->firstTensorPageNum[tensorIdxInMapping], pId = 0;pId < spmPPageSet.size();++ vPage, ++pId) {
            for(auto acc : pipeAccAllocSet) {
                dynamic_cast<BaseSPMSystem *>(RT.system)->accels[acc].setPageTable(vPage, spmPPageSet[pId], true);
            }
        }
        tensorPageTableReady[tensorIdxInMapping] = true;
    }
    void Layer::invalidSpmVPageForAcc(uint tensorIdxInMapping) {
        assert(mapping != NULL);
        assert(tensorPageTableReady[tensorIdxInMapping]);
        for(uint vPage = mapping->firstTensorPageNum[tensorIdxInMapping], i = 0;i < getTensorPageCountUseIdx(tensorIdxInMapping);++ vPage, ++i) {
            for(auto acc : pipeAccAllocSet) {
                dynamic_cast<BaseSPMSystem *>(RT.system)->accels[acc].invalidPageTable(vPage);
            }
        }
        tensorPageTableReady[tensorIdxInMapping] = false;
    }
    
    bool Layer::getTensorPageMapValid(uint tensorIdxInMapping){
        assert(tensorPageTableReady.size() > tensorIdxInMapping);
        return tensorPageTableReady[tensorIdxInMapping];
    }

    void Layer::pipelineLayerStateChange(PipelineLayerRuntimeState state) {
        pipelineLayerState = state;
    }

    ScheduleAction::ScheduleAction() {
        action_id_ = ScheduleAction::max_action_id_;
        ++ ScheduleAction::max_action_id_;
    }

    nlohmann::json ScheduleAction::toJson() const {
        nlohmann::json js;
#ifdef JSON_DBG
        js["action_id"] = action_id_;

        {
            js["num_acc_"] = acc_source_.num_acc_;
            nlohmann::json acc_ids_js;
            acc_ids_js["all_acc_ids"] = acc_source_.all_acc_ids_;
            for (uint stage_idx =0 ;stage_idx < acc_source_.acc_ids_.size();++ stage_idx) {
                acc_ids_js["stage-" + std::to_string(stage_idx)] = acc_source_.acc_ids_[stage_idx];
            }
            js["acc_ids"] = acc_ids_js;
        }

        {
            js["num_spm_pages"] = spm_source_.num_spm_pages_;
            
            // Weight SPM pages
            nlohmann::json weight_spm_pages_js;
            for (const auto& [tensor_id, page_set]: spm_source_.weight_spm_pages) {
                weight_spm_pages_js[std::to_string(tensor_id)] = page_set->size();
            }
            js["weight_spm_pages"] = weight_spm_pages_js;
            
            // In stage SPM pages
            nlohmann::json in_stage_spm_pages_js;
            for (uint stage_idx = 0; stage_idx < spm_source_.in_stage_spm_pages_.size(); ++stage_idx) {
                nlohmann::json stage_js;
                for (const auto& [tensor_id, vec_pageset]: spm_source_.in_stage_spm_pages_[stage_idx]) {
                    std::vector<uint> buffer_sizes;
                    for (const auto& pageset : vec_pageset) {
                        buffer_sizes.push_back(pageset->size());
                    }
                    stage_js[std::to_string(tensor_id)] = buffer_sizes;
                }
                in_stage_spm_pages_js["stage-" + std::to_string(stage_idx)] = stage_js;
            }
            js["in_stage_spm_pages"] = in_stage_spm_pages_js;
            
            // Ring buffer SPM pages
            nlohmann::json ringbuffer_pages_js;
            for (const auto& [tensor_id, vec_pageset]: spm_source_.ring_buffer_spm_pages_) {
                std::vector<uint> ring_sizes;
                for (const auto& pageset : vec_pageset) {
                    ring_sizes.push_back(pageset->size());
                }
                ringbuffer_pages_js[std::to_string(tensor_id)] = ring_sizes;
            }
            js["ring_buffer_spm_pages"] = ringbuffer_pages_js;
        }

        // Segment mapping
        if (segment_mapping_) {
            js["segment_mapping"] = segment_mapping_->toJson();
        } else {
            js["segment_mapping"] = nullptr;
        }
#endif
        return js;
    }

    std::string ScheduleAction::toString() const {
        return toJson().dump(JSON_DUMP_INDENTATION);
    }

    void HardwareConfig::init(BaseSystem &system) {
        numAccels = system.numAccels;
        inputBufBytes = system.config.accelInputBufBytes;
        weightBufBytes = system.config.accelWeightBufBytes;
        psumBufBytes = system.config.accelPsumBufBytes;
        inputWordBytes = system.config.accelInputWordBytes;
        psumWordBytes = system.config.accelPsumWordBytes;
        nBanks = system.config.cacheBanks;
        pageBytes = system.config.spmPageBytes;
        nPages = system.config.getSPMTotalPages();
        assert(nPages % nBanks == 0);
        nPagesPerBank = nPages / nBanks;
        spmTotalBytes = nPages * pageBytes;
        assert(spmTotalBytes <= system.config.cacheTotalBytes);
        // note: assume DRAM frequency is 3200
        dramBytesPerCycles = 19 * system.config.memCtrlChannels / (system.config.freqMHz / 1000.0);
        dramChannel = system.config.memCtrlChannels;
    }

    Model::Model() {
        numLayers = 0;
        addrLo = 0;
        addrHi = 0;
        state = STATE_IDLE;
    }

    Model::~Model() {
        for (auto layer: layers) {
            delete layer;
        }
    }

    void Model::setTid(uint tid) {
        for (auto layer : layers) {
            layer->setTid(tid);
        }
    }

    nlohmann::json Model::toJson() const {
        nlohmann::json js;
#ifdef JSON_DBG
        js["path"] = path;
        js["numLayers"] = numLayers;

        {
            nlohmann::json layers_js;
            for (uint i = 0; i < numLayers; i++) {
                layers_js["layer-" + std::to_string(i)] = layers[i]->toJson();
            }
            js["layers"] = layers_js;
        }
#endif
        return js;
    }

    std::string Model::toString() const {
        return toJson().dump(JSON_DUMP_INDENTATION);
    }

    Thread::Thread(uint tid_) {
        // assert(tid_ < GlobalVariables::MAX_THREADS);
        tid = tid_;
        state = STATE_INIT;
        model = NULL;
        layer = NULL;
        progress = 0;
    }

    bool Thread::isDone() {
        return state == STATE_DONE;
    }

    bool Layer::PipeLayerMapperKey::operator==(const PipeLayerMapperKey &other) const {
        return accNum == other.accNum &&
                dramBypass == other.dramBypass &&
                spmBypass == other.spmBypass;
    }

    bool Layer::PipeLayerMapperKey::operator<(const PipeLayerMapperKey &rhs) const {
        return std::tie(accNum, dramBypass, spmBypass) < 
            std::tie(rhs.accNum, rhs.dramBypass, rhs.spmBypass);
    }
    SpmSource::SpmSource() {
        clear();
    }
    AccSource::AccSource() {
        clear();
    }
    void SpmSource::clear() {
        num_spm_pages_ = 0;
        weight_spm_pages.clear();
        in_stage_spm_pages_.clear();
        ring_buffer_spm_pages_.clear();
    }
    void AccSource::clear() {
        num_acc_ = 0;
        acc_ids_.clear();
        all_acc_ids_.clear();
    }
    bool PipelineProfilingResult::Target::operator==(const Target &other) const {
        return acc == other.acc &&
                spm_per_acc_kb == other.spm_per_acc_kb &&
                batch == other.batch &&
                dram_bw == other.dram_bw &&
                noc_bw == other.noc_bw &&
                partition_num == other.partition_num;
    }
    nlohmann::json PipelineProfilingResult::Target::toJson() const {
        nlohmann::json js;
#ifdef JSON_DBG
        js["acc"] = acc;
        js["model_idx"] = model_idx;
        js["spm_per_acc_kb"] = spm_per_acc_kb;
        js["batch"] = batch;
        js["dram_bw"] = dram_bw;
        js["noc_bw"] = noc_bw;
        js["partition_num"] = partition_num;
#endif
        return js;
    }
    std::string PipelineProfilingResult::Target::toString() const {
        return toJson().dump(JSON_DUMP_INDENTATION);
    }
    size_t PipelineProfilingResult::Target::Hash::operator()(const Target &t) const
    {
        size_t h = 0;
        h ^= std::hash<uint>{}(t.acc) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint>{}(t.model_idx) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint>{}(t.spm_per_acc_kb) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint>{}(t.batch) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint>{}(t.dram_bw) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint>{}(t.noc_bw) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint>{}(t.partition_num) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
    void PipelineProfilingResult::loadFromFile(const std::string &filepath) {
        YAML::Node root = YAML::LoadFile(filepath);

        std::set<uint> available_accs_set;
        std::set<uint> available_spm_per_acc_kb_set;
        std::set<uint> available_batches_set;
        std::set<uint> available_dram_bw_set;
        std::set<uint> available_noc_bw_set;
        std::set<uint> available_partition_nums_set;   

        for (const auto& item : root) {
            Target t;
            t.acc = item["target"]["acc"].as<uint>();
            t.spm_per_acc_kb = item["target"]["spm_per_acc_kb"].as<uint>();
            t.batch = item["target"]["batch"].as<uint>();
            t.dram_bw = item["target"]["dram_bw"].as<uint>();
            t.noc_bw = item["target"]["noc_bw"].as<uint>();
            t.partition_num = item["target"]["partition_num"].as<uint>();
            t.model_idx = item["target"]["model_idx"].as<uint>();

            // record available parameter values
            available_accs_set.insert(t.acc);
            available_spm_per_acc_kb_set.insert(t.spm_per_acc_kb);
            available_batches_set.insert(t.batch);
            available_dram_bw_set.insert(t.dram_bw);
            available_noc_bw_set.insert(t.noc_bw);
            available_partition_nums_set.insert(t.partition_num);

            std::vector<uint> cost = item["cost"].as<std::vector<uint>>();
            std::vector<std::string> mapping_paths = item["mapping_path"].as<std::vector<std::string>>();

            assert(preprocessed_data_.find(t) == preprocessed_data_.end());
            PreprocessedEntry entry;
            entry.cost = cost;
            entry.mapping_paths = mapping_paths;
            // compute prefix sums (pre_cost) and suffix sums (remain_cost)
            size_t n = cost.size();
            entry.pre_cost.resize(n);
            entry.remain_cost.resize(n);
            if (n > 0) {
                entry.pre_cost[0] = cost[0];
                for (size_t i = 1; i < n; ++i) {
                    entry.pre_cost[i] = entry.pre_cost[i - 1] + cost[i];
                }
                entry.remain_cost[n - 1] = cost[n - 1];
                for (int i = (int)n - 2; i >= 0; --i) {
                    entry.remain_cost[i] = entry.remain_cost[i + 1] + cost[i];
                }
            }
            preprocessed_data_[t] = entry;
        }

        available_accs_ = std::vector<uint>(available_accs_set.begin(), available_accs_set.end());
        available_spm_per_acc_kb_ = std::vector<uint>(available_spm_per_acc_kb_set.begin(), available_spm_per_acc_kb_set.end());
        available_batches_ = std::vector<uint>(available_batches_set.begin(), available_batches_set.end());
        available_dram_bw_ = std::vector<uint>(available_dram_bw_set.begin(), available_dram_bw_set.end());
        available_noc_bw_ = std::vector<uint>(available_noc_bw_set.begin(), available_noc_bw_set.end());
        available_partition_nums_ = std::vector<uint>(available_partition_nums_set.begin(), available_partition_nums_set.end());
    }

    const PipelineProfilingResult::PreprocessedEntry& PipelineProfilingResult::get(const Target &target) const {
        auto it = preprocessed_data_.find(target);
        if (it != preprocessed_data_.end()) {
            return it->second;
        }
        assert(false);
    }

    const std::vector<uint>& PipelineProfilingResult::getAvailableAccs() const {
        return available_accs_;
    }

    const std::vector<uint>& PipelineProfilingResult::getAvailableSpmPerAccKb() const {
        return available_spm_per_acc_kb_;
    }

    const std::vector<uint>& PipelineProfilingResult::getAvailableBatches() const {
        return available_batches_;
    }

    const std::vector<uint>& PipelineProfilingResult::getAvailableDramBw() const {
        return available_dram_bw_;
    }

    const std::vector<uint>& PipelineProfilingResult::getAvailableNocBw() const {
        return available_noc_bw_;
    }

    const std::vector<uint>& PipelineProfilingResult::getAvailablePartitionNums() const {
        return available_partition_nums_;
    }


    void StageMappingParser::StageCandidate::load(YAML::Node &node) {
        globalStageId = node["globalStageId"].as<uint>();

        layerIdList = node["layerIdList"].as<std::vector<uint64>>();

        tensorIdList = node["tensorIdList"].as<std::vector<TensorId>>();
        entryTensorIdList = node["entryTensorIdList"].as<std::vector<TensorId>>();
        exportTensorIdList = node["exportTensorIdList"].as<std::vector<uint64>>();

        dramBypassList = node["dramBypassList"].as<std::vector<std::vector<uint64>>>();
        spmBypassList = node["spmBypassList"].as<std::vector<std::vector<uint64>>>();
        
        entryTensorTypeList = node["entryTensorTypeList"].as<std::vector<std::string>>();
        entryTensorDoubleBufferList = node["entryTensorDoubleBufferList"].as<std::vector<uint>>();
        exportTensorTypeList = node["exportTensorTypeList"].as<std::vector<std::string>>();
        exportTensorDoubleBufferList = node["exportTensorDoubleBufferList"].as<std::vector<uint>>();

        fixTensorDramBypassIdList = node["fixTensorDramBypassIdList"].as<std::vector<uint64>>();
        innerIsolateTensorId = node["innerIsolateTensorId"].as<std::vector<uint64>>();
        innerSharedTensorId = node["innerSharedTensorId"].as<std::vector<uint64>>();

        tensorUsageCountList = node["tensorUsageCountList"].as<std::vector<std::vector<uint>>>();
        tensorUseLazyFetch = node["tensorUseLazyFetch"].as<std::vector<std::vector<uint64>>>();
        
        vAccelIdxList = node["vAccIdxList"].as<std::vector<std::vector<uint>>>();
        pAccelIdxList = node["pAccIdxList"].as<std::vector<std::vector<uint>>>();
        accUtil = node["accUtil"].as<uint>();
    }
    void SegmentMappingParser::SegmentCandidate::load(YAML::Node &node) {
        segmentIdx = node["segment_idx"].as<uint>();
        startLayerIdx = node["start_layer_idx"].as<uint>();
        endLayerIdx = node["end_layer_idx"].as<uint>();

        // Parse ring buffer maps robustly (allow empty maps)
        ring_buffer_count.clear();
        if (node["ring_buffer_count"] && node["ring_buffer_count"].IsMap()) {
            for (const auto &kv : node["ring_buffer_count"]) {
                TensorId k = kv.first.as<TensorId>();
                uint v = kv.second.as<uint>();
                ring_buffer_count[k] = v;
            }
        }

        ring_buffer_size_per.clear();
        if (node["ring_buffer_size_per"] && node["ring_buffer_size_per"].IsMap()) {
            for (const auto &kv : node["ring_buffer_size_per"]) {
                TensorId k = kv.first.as<TensorId>();
                uint v = kv.second.as<uint>();
                ring_buffer_size_per[k] = v;
            }
        }

        ring_buffer_use_count.clear();
        if (node["ring_buffer_use_count"] && node["ring_buffer_use_count"].IsMap()) {
            for (const auto &kv : node["ring_buffer_use_count"]) {
                TensorId k = kv.first.as<TensorId>();
                uint v = kv.second.as<uint>();
                ring_buffer_use_count[k] = v;
            }
        }

        cost = static_cast<uint64>(node["cost"].as<double>());
        accUtil = node["acc_util"].as<uint>();

        // tensor_spm_util_in_stage is a sequence of maps: parse robustly
        tensorSpmUtilInStage.clear();
        if (node["tensor_spm_util_in_stage"] && node["tensor_spm_util_in_stage"].IsSequence()) {
            for (const auto &mapNode : node["tensor_spm_util_in_stage"]) {
                std::unordered_map<TensorId, uint64> m;
                if (mapNode && mapNode.IsMap()) {
                    for (const auto &kv : mapNode) {
                        TensorId k = kv.first.as<TensorId>();
                        uint64 v = kv.second.as<uint64>();
                        m[k] = v;
                    }
                }
                tensorSpmUtilInStage.push_back(std::move(m));
            }
        }

        tensorSpmUtilShared.clear();
        if (node["tensor_spm_util_shared"] && node["tensor_spm_util_shared"].IsMap()) {
            for (const auto &kv : node["tensor_spm_util_shared"]) {
                TensorId k = kv.first.as<TensorId>();
                uint64 v = kv.second.as<uint64>();
                tensorSpmUtilShared[k] = v;
            }
        }

        tensorSpmUtilInRingbuffer.clear();
        if (node["tensor_spm_util_in_ringbuffer"] && node["tensor_spm_util_in_ringbuffer"].IsMap()) {
            for (const auto &kv : node["tensor_spm_util_in_ringbuffer"]) {
                TensorId k = kv.first.as<TensorId>();
                uint64 v = kv.second.as<uint64>();
                tensorSpmUtilInRingbuffer[k] = v;
            }
        }

        tensorSpmUtilWeight.clear();
        if (node["tensor_spm_util_weight"] && node["tensor_spm_util_weight"].IsMap()) {
            for (const auto &kv : node["tensor_spm_util_weight"]) {
                TensorId k = kv.first.as<TensorId>();
                uint64 v = kv.second.as<uint64>();
                tensorSpmUtilWeight[k] = v;
            }
        }

        totalSpmUtil = static_cast<uint64>(node["total_spm_util"].as<double>());
        
        // Load stages
        if (node["stages"]) {
            YAML::Node stagesNode = node["stages"];
            stageCandidates.resize(stagesNode.size());
            for (uint i = 0; i < stagesNode.size(); i++) {
                // Each stage is a vector of StageCandidate
                YAML::Node stageVectorNode = stagesNode[i];
                if (stageVectorNode.size() > 0) {
                    // We only process the first element in the vector
                    YAML::Node stageNode = stageVectorNode[0];
                    stageCandidates[i].load(stageNode);
                }
            }
        }

        if (node["subBatchSize"]) {
            subBatchSize = node["subBatchSize"].as<uint>();
        } else {
            subBatchSize = 1;
        }
    }
    void PipelineMappingParser::TargetConfig::load(const YAML::Node &node) {
        numArrays = node["numArrays"].as<uint>();
        spmKB = node["spmKB"].as<uint>();
        spmKBPerArray = node["spmKB_per_array"].as<uint>();
        numMacsPerArray = node["num_macs_per_array"].as<uint>();
        dramBwPerCycle = node["dram_bw_per_cycle"].as<double>();
        nocBwPerCycle = node["noc_bw_per_cycle"].as<uint>();
        defaultBatch = node["default_batch"].as<uint>();
    }
    void PipelineMappingParser::load(const std::string &path) {
        auto root = YAML::LoadFile(path);
        
        if (root["target"]) {
            target.load(root["target"]);
        }
        
        pipelineCost = root["cost"].as<uint64>();
        
        if (root["segments"]) {
            YAML::Node segmentsNode = root["segments"];
            segments.resize(segmentsNode.size());
            for (uint i = 0; i < segments.size(); i++) {
                YAML::Node segmentNode = segmentsNode[i];
                segments[i].load(segmentNode);
            }
        }
    }
    StageMapping StageMappingParser::stageCandidateToStageMapping(Model* model, const StageCandidate &can) {
        StageMapping stage_mapping;

        stage_mapping.layerIdList = can.layerIdList; // layer Id list in stage. they are ordered by execution sequence.
        
        stage_mapping.tensorIdList = can.tensorIdList;
        stage_mapping.entryTensorId = can.entryTensorIdList;
        stage_mapping.exportTensorId = can.exportTensorIdList;
        stage_mapping.dramBypassList = can.dramBypassList;
        stage_mapping.spmBypassList = can.spmBypassList;

        stage_mapping.entryTensorType.reserve(can.entryTensorTypeList.size());
        for(auto& str: can.entryTensorTypeList) {
            stage_mapping.entryTensorType.push_back(StageMapping::stringToTensorStayType(str));
        }
        stage_mapping.entryTensorDoubleBuffer = can.entryTensorDoubleBufferList; // {0, 1} 0: single buffer, 1: double buffer

        stage_mapping.exportTensorType.reserve(can.exportTensorDoubleBufferList.size());
        for(auto& str: can.exportTensorTypeList) {
            stage_mapping.exportTensorType.push_back(StageMapping::stringToTensorStayType(str));
        }
        stage_mapping.exportTensorDoubleBuffer = can.exportTensorDoubleBufferList; // {0, 1} 0: single buffer, 1: double buffer

        stage_mapping.fixTensorDramBypassId = can.fixTensorDramBypassIdList; // 固定张量
        stage_mapping.innerIsolateTensorId = can.innerIsolateTensorId; // 内部独立张量
        stage_mapping.innerSharedTensorId = can.innerSharedTensorId; // 内部共享张量
        stage_mapping.tensorUsageCountList = can.tensorUsageCountList;
        stage_mapping.tensorUseLazyFetch = can.tensorUseLazyFetch;
        
        stage_mapping.vAccelIdx = can.vAccelIdxList;
        stage_mapping.pAccelIdx = can.pAccelIdxList;
        stage_mapping.accUtil = can.accUtil;
        
        // 导出属性，结合模型信息，从基本属性中导出。在load过程中生成。
        for(uint i = 0;i < can.layerIdList.size();++ i) {
            uint layerIdx = can.layerIdList[i];
            auto& layer = model->layers[layerIdx];
            for(uint j = 0; j < layer->tensorIds.size(); j++) {
                TensorId tensorId = layer->tensorIds[j];
                stage_mapping.tensorId2DramBaseAddr[tensorId] = {layer->tensorList[j].addr_, layer->tensorList2[j].addr_};
                stage_mapping.tensorId2Shape[tensorId] = layer->getTensorShape(tensorId);
                stage_mapping.tensorId2Strides[tensorId] = layer->getTensorStrides(tensorId);
                stage_mapping.tensorId2UseLazyFetch[tensorId] = stage_mapping.tensorUseLazyFetch[i][j];
            }
            
            for(uint j = 0;j < layer->tensorIds.size(); j++) {
                TensorId tensorId = layer->tensorIds[j];
                stage_mapping.tensorId2UsageCount[tensorId] = can.tensorUsageCountList[i][j]; // 每个张量被Stage内多少个层使用。当其减少为0时，会被释放。仅用于内部共享张量的计算。
            }

            // layerMapping
            stage_mapping.layerMapping.resize(can.layerIdList.size());
            stage_mapping.layerMappingLazyFetch.resize(can.layerIdList.size());
            for (uint li = 0; li < can.layerIdList.size(); ++li) {
                uint layerIdx = can.layerIdList[li];
                auto &layer = model->layers[layerIdx];

                // Build key from stage candidate's bypass lists and accUtil
                Layer::PipeLayerMapperKey key;
                key.accNum = can.accUtil;
                key.dramBypass = can.dramBypassList[li];
                key.spmBypass = can.spmBypassList[li];
                // Find mapping in layer's pipeLayerMappingTable
                LayerMapping *baseMappingPtr = nullptr;
                auto it = layer->pipeLayerMappingTable.find(key);
                if (it != layer->pipeLayerMappingTable.end() && !it->second.empty()) {
                    baseMappingPtr = &it->second.front();
                } else {
                    assert(false && "Mapping not found in layer's pipeLayerMappingTable");
                }

                stage_mapping.layerMapping[li] = baseMappingPtr;

                // lazyFetchLayerMapping: copy key and zero bypass for non-input/non-output tensors
                Layer::PipeLayerMapperKey keyLazy = key;
                std::unordered_set<TensorId> ioIds;
                for (auto tid : stage_mapping.entryTensorId) ioIds.insert(tid);
                for (auto tid : stage_mapping.exportTensorId) ioIds.insert(tid);
                for (size_t t = 0; t < layer->tensorIds.size(); ++t) {
                    if (ioIds.find(layer->tensorIds[t]) == ioIds.end()) {
                        keyLazy.dramBypass[t] = 0;
                        keyLazy.spmBypass[t] = 0;
                    }
                }

                LayerMapping *lazyPtr = nullptr;
                auto it2 = layer->pipeLayerMappingTable.find(keyLazy);
                if (it2 != layer->pipeLayerMappingTable.end() && !it2->second.empty()) {
                    lazyPtr = &it2->second.front();
                } else {
                    std::cout << "li=" << li << ", keyLazy: spmBypass=" << mudnac::toString(keyLazy.spmBypass) << ", dramBypass=" << mudnac::toString(keyLazy.dramBypass) << std::endl;
                    assert(false && "Lazy fetch mapping not found in layer's pipeLayerMappingTable");
                }
                stage_mapping.layerMappingLazyFetch[li] = lazyPtr;
            }
            
            // 注意，该ht仅存储单个tensor所需页数，且仅存1个bufffer的大小，即对于使用singble-buffer和double-buffer的张量，页面数相同，但double-buffer需要分配两倍大小
            for(uint j = 0;j < layer->tensorIds.size();j++) {
                TensorId tensorId = layer->tensorIds[j];
                stage_mapping.tensorId2SpmPageNums[tensorId] = stage_mapping.layerMapping[i]->spmTensorPageCount[j];
            }
        }
    
        return stage_mapping;
    }
    inline SegmentMapping SegmentMappingParser::segmentCandidateToSegmentMapping(Model* model, const SegmentCandidate& candidate) {
        SegmentMapping mapping;
        
        // 将candidate的基本属性复制到mapping
        mapping.segmentIdx = candidate.segmentIdx;
        mapping.startLayerIdx = candidate.startLayerIdx;
        mapping.endLayerIdx = candidate.endLayerIdx;
        mapping.subBatchSize = candidate.subBatchSize;


        
        for (auto [tensor_id, count]: candidate.ring_buffer_count) {
            mapping.ring_buffer_config_[tensor_id] = SegmentMapping::RingBufferConfig();
            mapping.ring_buffer_config_[tensor_id].size_ = count;
            mapping.ring_buffer_config_[tensor_id].use_ = 1;
            mapping.ring_buffer_config_[tensor_id].out_degree_ = candidate.ring_buffer_use_count.at(tensor_id);
            mapping.ring_buffer_config_[tensor_id].spm_util_per_ = candidate.ring_buffer_size_per.at(tensor_id);
        }
        
        mapping.tensorSpmUtilInStage = candidate.tensorSpmUtilInStage;
        mapping.tensorSpmUtilShared = candidate.tensorSpmUtilShared;
        mapping.tensorSpmUtilInRingbuffer = candidate.tensorSpmUtilInRingbuffer;
        mapping.tensorSpmUtilWeight = candidate.tensorSpmUtilWeight;
        
        mapping.totalSpmUtil = candidate.totalSpmUtil;
        mapping.accUtil = candidate.accUtil;
        mapping.cost = candidate.cost;
        
        mapping.stage_mapping_vec.resize(candidate.stageCandidates.size());
        for (size_t i = 0; i < candidate.stageCandidates.size(); ++i) {
            mapping.stage_mapping_vec[i] = StageMappingParser::stageCandidateToStageMapping(model, candidate.stageCandidates[i]);
        }
        
        // 调用createMap方法生成额外属性
        mapping.createMap();
        
        return mapping;
    }
    
    PipelineMapping PipelineMappingParser::pipelineCandidateToPipelineMapping(Model* model, const std::string& path) {
        PipelineMappingParser parser;
        parser.load(path);
        
        PipelineMapping pipelineMapping;
        
        // 设置Pipeline Mapping字段
        pipelineMapping.pipelineCost = parser.pipelineCost;
        
        // 设置Target相关字段
        pipelineMapping.targetNumArrays = parser.target.numArrays;
        pipelineMapping.targetSpmKB = parser.target.spmKB;
        pipelineMapping.targetSpmKBPerArray = parser.target.spmKBPerArray;
        pipelineMapping.targetNumMacsPerArray = parser.target.numMacsPerArray;
        pipelineMapping.targetDramBwPerCycle = parser.target.dramBwPerCycle;
        pipelineMapping.targetNocBwPerCycle = parser.target.nocBwPerCycle;
        pipelineMapping.targetDefaultBatch = parser.target.defaultBatch;
        
        // 转换并设置Segment Candidates
        pipelineMapping.segmentCandidates.resize(parser.segments.size());
        for (size_t i = 0; i < parser.segments.size(); ++i) {
            pipelineMapping.segmentCandidates[i] = SegmentMappingParser::segmentCandidateToSegmentMapping(model, parser.segments[i]);
            pipelineMapping.maxNumArraysSegmentUse = std::max(pipelineMapping.maxNumArraysSegmentUse, pipelineMapping.segmentCandidates[i].accUtil);
            pipelineMapping.maxSpmPageSegmentUse = std::max(pipelineMapping.maxSpmPageSegmentUse, pipelineMapping.segmentCandidates[i].totalSpmUtil);
        }
        
        return pipelineMapping;
    }
    
}
