#include "global_variables.h"


namespace mudnac {


    void GlobalVariables::init(uint numAccels_, uint numPages_, uint numBanks_, uint pages_per_bank, uint page_size) {
        // hardware related
        numAccels = numAccels_;
        numPages = numPages_;
        numBanks = numBanks_;

        // experiment setting
        accelAllocMode = SPM_ALLOC_MODE_STATIC;
        spmAllocMode = SPM_ALLOC_MODE_STATIC;
        memoryAccessMode = MEMORY_ACCESS_MODE_UNLIMITED;

        // Accel resource
        threadAccelAlloc.resize(MAX_THREADS);
        for (uint i = 0; i < numAccels; i++) {
            availableAccelSet.insert(i);
            
        }
        enableThreadAccelAffiliation = false;

        // SPM resource
        threadSpmPageAlloc.resize(MAX_THREADS);
        for (uint i = 0; i < numPages; i++) {
            availableSpmPageSet.insert(i);
        }
        spmPageAllocStrategy = SPM_PAGE_ALLOC_STRATEGY_NAIVE;

        // runtime updating info
        threadWaitingAccCycles.resize(MAX_THREADS, 0);
        threadWaitingSpmCycles.resize(MAX_THREADS, 0);
        threadCurrentModelType.resize(MAX_THREADS, 0);
        threadCurrentLayerIdx.resize(MAX_THREADS, 0);
        threadAccelAllocIdx.resize(MAX_THREADS, 0);
        threadSpmAllocIdx.resize(MAX_THREADS, 0);

        // profiling results
        pf_doneProfiling = false;
        pf_modelCycles.resize(NUM_ACCEL_ALLOCS);
        pf_modelLayerCycles.resize(NUM_ACCEL_ALLOCS);
        pf_modelRemainingCycles.resize(NUM_ACCEL_ALLOCS);
        for (int i = 0; i < NUM_ACCEL_ALLOCS; i++) {
            pf_modelCycles[i].resize(MAX_MODELS, 0);
            pf_modelLayerCycles[i].resize(MAX_MODELS);
            pf_modelRemainingCycles[i].resize(MAX_MODELS);
        }

        // Memory Bandwidth repartition
        bw_threadEpoch.resize(MAX_THREADS, UINT64_MAX);
        bw_threadMaxReq.resize(MAX_THREADS, UINT64_MAX);

        // SPM repartition
        spm_threadInterLayerReuse.resize(MAX_THREADS, false);
        spm_threadEstimatedReleasingTime.resize(MAX_THREADS, 0);
        spm_threadEstimatedAcquiredPages.resize(MAX_THREADS, 0);
        spm_alpha = 0.5;
        spm_beta = 0.3;

        // QoS task
        qos_modelTargetCycles.resize(MAX_MODELS, 0);
        qos_taskModelTypeQueue.resize(MAX_TASKS, 0);
        qos_nextTaskIdx = 0;
        qos_taskDispatchTime.resize(MAX_TASKS, 0);
        qos_taskStartTime.resize(MAX_TASKS, 0);
        qos_taskEndTime.resize(MAX_TASKS, 0);
        qos_threadCurrentTaskIdx.resize(MAX_THREADS, 0);
        qos_threadCurrentScore.resize(MAX_THREADS, 400);
        qos_threadCurrentMemScore.resize(MAX_THREADS, 0);
        qos_threadDramUtil.resize(MAX_THREADS, 0);
        SPMManager();
    }

    void GlobalVariables::loadProfilingResults(const std::deque<std::string> &modelNameList,
                                               uint scaleIndex,
                                               std::string scaleFolder,
                                               std::string profileRoot) {
        assert(not pf_doneProfiling);
        assert(modelNameList.size() < MAX_MODELS);
        if (profileRoot.back() != '/') {
            profileRoot += '/';
        }

        for (uint modelIdx = 0; modelIdx < modelNameList.size(); modelIdx++) {
            std::string modelName = modelNameList[modelIdx];
            std::string modelPath = profileRoot + modelName;
            std::string scalePath = profileRoot + scaleFolder + "/" + modelName;
//            printf("\nmodelIdx=%u, modelPath=%s, scalePath=%s, scaleIndex=%u\n",
//                   modelIdx, modelPath.c_str(), scalePath.c_str(), scaleIndex);

            // read scale factor
            std::ifstream ifsScale(scalePath);
            assert(ifsScale.is_open());
            double scale = 1;
            std::cout << "Read profiling scale for model " << modelName << ": path=" << scalePath << ", scaleIndex=" << scaleIndex << std::endl;
            for (uint i = 0; i <= scaleIndex; i++) {
                assert(not ifsScale.eof());
                ifsScale >> scale;
                std::cout << "modelIdx=" << modelIdx << ", i=" << i << ", scale=" << scale << std::endl;
            }
//            printf("\nmodelIdx=%u, scale=%lf\n", modelIdx, scale);
            ifsScale.close();

            // read profiling
            std::ifstream ifs(modelPath);
            assert(ifs.is_open());
            while (not ifs.eof()) {
                std::string line;
                std::getline(ifs, line);
                if (line.empty()) {
                    continue;
                }
                size_t pos = 0;
                for (int accelAllocIdx = 0; accelAllocIdx < NUM_ACCEL_ALLOCS; accelAllocIdx++) {
                    size_t end = line.find(',', pos);
                    assert(end > pos and end != line.npos);
                    uint64 cycles = (double) std::stoull(line.substr(pos, end - pos)) * scale;
                    pf_modelLayerCycles[accelAllocIdx][modelIdx].push_back(cycles);
                    pf_modelCycles[accelAllocIdx][modelIdx] += cycles;
                    pos = end + 1;
                }
            }
            ifs.close();

            for (int accelAllocIdx = 0; accelAllocIdx < NUM_ACCEL_ALLOCS; accelAllocIdx++) {
                uint64 modelCycles = pf_modelCycles[accelAllocIdx][modelIdx];
                uint layers = pf_modelLayerCycles[accelAllocIdx][modelIdx].size();
                uint64 doneCycles = 0;
                for (uint i = 0; i < layers; i++) {
                    pf_modelRemainingCycles[accelAllocIdx][modelIdx].push_back(modelCycles - doneCycles);
                    doneCycles += pf_modelLayerCycles[accelAllocIdx][modelIdx][i];
//                    printf("\tlayerIdx=%u, layerCycles=%lu, remainingCycles=%lu\n",
//                           i, pf_modelLayerCycles[accelAllocIdx][modelIdx][i],
//                           pf_modelRemainingCycles[accelAllocIdx][modelIdx][i]);
                }
            }
        }
        pf_doneProfiling = true;
    }


} // mudnac