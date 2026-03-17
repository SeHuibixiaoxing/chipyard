#ifndef MUDNACSIM_THREAD_RUN_MODEL_H
#define MUDNACSIM_THREAD_RUN_MODEL_H


#include "runtime.h"


namespace mudnac {


    class ThreadRunModel : public Thread {
    public:
        uint maxRun;
        uint64 maxCycles;

        uint64 modelStartCycles;
        uint64 modelEndCycles;
        uint64 layerStartCycles;
        std::deque<uint64> modelCyclesList;
        std::deque<std::deque<uint64>> layerCyclesList;

        uint startLayerIdx, endLayerIdx;

        ThreadRunModel(uint tid_, Model *model_, uint modelType,
                       uint staticAccAllocIdx, uint staticSpmAllocIdx,
                       uint startLayerIdx_ = 0, uint maxRun_ = 1, uint64 maxCycles_ = UINT64_MAX,
                       int endLayerIdx_ = -1)
                : Thread(tid_) {
            assert(model_ != NULL);
            assert(maxRun_ >= 1);
            assert(RT.gv.accelAllocMode == RT.gv.ACCEL_ALLOC_MODE_STATIC);
            assert(RT.gv.memoryAccessMode == RT.gv.MEMORY_ACCESS_MODE_UNLIMITED);
            maxRun = maxRun_;
            maxCycles = maxCycles_;
            modelStartCycles = RT.getCycles();
            layerStartCycles = RT.getCycles();
            layerCyclesList.emplace_back();

            model = model_;
            layer = model->layers[startLayerIdx_ % model->numLayers];

            RT.gv.threadAccelAllocIdx[tid] = staticAccAllocIdx;
            RT.gv.threadSpmAllocIdx[tid] = staticSpmAllocIdx;
            RT.gv.threadCurrentModelType[tid] = modelType;
            RT.gv.threadCurrentLayerIdx[tid] = startLayerIdx_ % model->numLayers;

            std::cout << "RT.gv.threadAccelAllocIdx=" << staticAccAllocIdx << std::endl;
            std::cout << "RT.gv.threadSpmAllocIdx=" << staticSpmAllocIdx << std::endl;

            this->startLayerIdx = startLayerIdx_;
            this->endLayerIdx = endLayerIdx_ < 0 ? model->layers.size() - 1 : endLayerIdx_;
            
            assert(endLayerIdx >= startLayerIdx_);
        }

        bool tick() {
            switch (state) {
                case STATE_RUN: {
                    bool done = layer->tick();
                    if (done) {  // layer inference is done
                        layer->resetAllState();
                        uint &layerIdx = RT.gv.threadCurrentLayerIdx[tid];
                        layerCyclesList.back().push_back(RT.getCycles() - layerStartCycles);
#ifdef THREAD_DBG
                        printf("%lu tid=%u: layer done, latency=%lu, progress=%u/%u\n",
                               RT.getCycles(), tid, layerCyclesList.back().back(),
                               layerIdx - startLayerIdx + 1, endLayerIdx - startLayerIdx + 1);
#endif
                        if (layerIdx == endLayerIdx) {  // the last layer is done
                            modelCyclesList.push_back(RT.getCycles() - modelStartCycles);
                            modelEndCycles = RT.getCycles();  
                            RT.releaseAll(tid);
                            if (RT.model_has_run_once.size() > 0) {
                                RT.model_has_run_once[RT.gv.threadCurrentModelType[tid]] = 1;
                                std::cout << "model_has_run_once: " << mudnac::toString(RT.model_has_run_once) << std::endl;
                            }
#ifdef THREAD_DBG
                            printf("%lu tid=%u: model done, latency=%lu, count=%u\n",
                                   RT.getCycles(), tid, modelCyclesList.back(), (uint) modelCyclesList.size());
#endif
                            bool other_ok = RT.model_has_run_once.size() > 0 ? true : false;
                            for (uint i = 0;i < RT.model_has_run_once.size();++ i) {
                                if (RT.model_has_run_once[i] == 0) {
                                    other_ok = false;
                                    break;
                                }
                            }
                            if ((modelCyclesList.size() >= maxRun) or (RT.getCycles() >= maxCycles) or other_ok) {
                                // meet terminate condition
#ifdef THREAD_DBG
                                printf("%lu tid=%u: terminate\n", RT.getCycles(), tid);
#endif
                                state = STATE_DONE;
                            } else {  // start a new round of model inference
                                layerIdx = 0;
                                layer = model->layers[layerIdx];
                                modelStartCycles = layerStartCycles = RT.getCycles();
                                layerCyclesList.emplace_back();
                            }
                        } else {  // has next layer
                            if (RT.getCycles() >= maxCycles) {
                                // meet terminate condition
#ifdef THREAD_DBG
                                printf("%lu tid=%u: terminate\n", RT.getCycles(), tid);
#endif
                                RT.releaseAll(tid);
                                state = STATE_DONE;
                                layerCyclesList.pop_back();
                            } else {  // start next layer
                                layerIdx++;
                                layer = model->layers[layerIdx];
                                layerStartCycles = RT.getCycles();
                            }
                        }
                        progress = std::max(static_cast<uint64>((100 * (layerIdx + 1)) / ((endLayerIdx + 1) * maxRun)),
                                            (100 * RT.getCycles()) / maxCycles);
                    }
                    break;
                }
                case STATE_INIT: {
                    state = STATE_RUN;
                    break;
                }
                case STATE_DONE: {
                    break;
                }
                default: {
                    assert(false);
                }
            }
            return state == STATE_DONE;
        }

        void popFirstRunRecord() {
            if (modelCyclesList.size() > 0) {
                modelCyclesList.pop_front();
            }
            if (layerCyclesList.size() > 0) {
                layerCyclesList.pop_front();
            }
        }

        uint getNumRuns() { return modelCyclesList.size(); }

        uint64 getTotalModelCycles() {
            uint64 cycles = 0;
            for (auto c: modelCyclesList) {
                cycles += c;
            }
            return cycles;
        }

        uint64 getAvgModelCycles() {
            uint numRuns = getNumRuns();
            if (numRuns == 0) {
                return 0;
            }
            return getTotalModelCycles() / numRuns;
        }

        uint64 getMaxModelCycles() {
            uint64 cycles = 0;
            for (auto c: modelCyclesList) {
                cycles = std::max(cycles, c);
            }
            return cycles;
        }
    };


} // mudnac


#endif //MUDNACSIM_THREAD_RUN_MODEL_H
