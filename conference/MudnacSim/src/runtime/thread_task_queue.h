#ifndef MUDNACSIM_THREAD_TASK_QUEUE_H
#define MUDNACSIM_THREAD_TASK_QUEUE_H


#include "runtime.h"


namespace mudnac {


    class ThreadTaskQueue : public Thread {
    public:
        std::deque<Model *> modelList;
        std::deque<uint> taskModelIdxQueue;
        uint taskIdx;

        uint64 modelStartCycles;
        uint64 layerStartCycles;
        uint64 threadStartCycles;
        uint64 threadEndCycles;
        std::deque<uint64> modelCyclesList;
        std::deque<std::deque<uint64>> layerCyclesList;

        uint *curModelIdxPtr;
        uint *curLayerIdxPtr;

        ThreadTaskQueue(uint tid_,
                        const std::deque<Model *> &modelList_,
                        const std::deque<uint> &taskModelIdxQueue_,
                        uint staticAccAllocIdx,
                        uint staticSpmAllocIdx)
                : Thread(tid_) {
            assert(taskModelIdxQueue_.size() > 0);
            for (auto idx: taskModelIdxQueue_) {
                assert(idx < modelList_.size());
            }

            modelList = modelList_;
            taskModelIdxQueue = taskModelIdxQueue_;
            taskIdx = 0;
            modelStartCycles = RT.getCycles();
            layerStartCycles = RT.getCycles();
            threadStartCycles = RT.getCycles();
            threadEndCycles = 0;
            layerCyclesList.emplace_back();

            model = modelList[taskModelIdxQueue[taskIdx]];
            layer = model->layers[0];

            RT.gv.threadAccelAllocIdx[tid] = staticAccAllocIdx;
            RT.gv.threadSpmAllocIdx[tid] = staticSpmAllocIdx;
            RT.gv.threadCurrentModelType[tid] = taskModelIdxQueue[taskIdx];
            RT.gv.threadCurrentLayerIdx[tid] = 0;
            curModelIdxPtr = &RT.gv.threadCurrentModelType[tid];
            curLayerIdxPtr = &RT.gv.threadCurrentLayerIdx[tid];
        }

        bool tick() {
            switch (state) {
                case STATE_RUN: {
                    bool done = layer->tick();
                    if (done) {  // the layer is done
                        layer->resetAllState();
                        layerCyclesList.back().push_back(RT.getCycles() - layerStartCycles);
#ifdef THREAD_DBG
                        printf("%lu tid=%u: layer done, latency=%lu, progress=%u/%u, taskIdx=%u, modelIdx=%u\n",
                               RT.getCycles(), tid, layerCyclesList.back().back(),
                               (*curLayerIdxPtr) + 1, (uint) model->layers.size(), taskIdx, *curModelIdxPtr);
#endif
                        if ((*curLayerIdxPtr) == model->layers.size() - 1) {  // the last layer is done
                            progress = 100 * (taskIdx + 1) / taskModelIdxQueue.size();
                            modelCyclesList.push_back(RT.getCycles() - modelStartCycles);
                            RT.releaseAll(tid);
#ifdef THREAD_DBG
                            printf("%lu tid=%u: model done, latency=%lu, taskIdx=%u, modelIdx=%u\n",
                                   RT.getCycles(), tid, modelCyclesList.back(), taskIdx, *curModelIdxPtr);
#endif
                            if (taskIdx == taskModelIdxQueue.size() - 1) {  // the last model is done
#ifdef THREAD_DBG
                                printf("%lu tid=%u: terminate\n", RT.getCycles(), tid);
#endif
                                threadEndCycles = RT.getCycles();
                                state = STATE_DONE;
                            } else {  // has next model
                                taskIdx++;
                                *curLayerIdxPtr = 0;
                                *curModelIdxPtr = taskModelIdxQueue[taskIdx];
                                model = modelList[*curModelIdxPtr];
                                layer = model->layers[0];
                                modelStartCycles = layerStartCycles = RT.getCycles();
                                layerCyclesList.emplace_back();
                            }
                        } else {  // has next layer
                            (*curLayerIdxPtr)++;
                            layer = model->layers[*curLayerIdxPtr];
                            layerStartCycles = RT.getCycles();
                        }
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

        uint64 getModelCyclesByModelIdx(uint modelIdx, uint times=0) {
            assert(modelIdx < modelList.size());
            uint count = 0;
            for (uint i = 0; i < taskModelIdxQueue.size(); i++) {
                uint taskModelIdx = taskModelIdxQueue[i];
                if (taskModelIdx == modelIdx) {
                    if (count >= times) {
                        return modelCyclesList[i];
                    } else {
                        count++;
                    }
                }
            }
            assert(false);
        }

        void getModelCyclesListByModelIdx(uint modelIdx, std::deque<uint64> &cyclesList) {
            assert(modelIdx < modelList.size());
            for (uint i = 0; i < taskModelIdxQueue.size(); i++) {
                uint taskModelIdx = taskModelIdxQueue[i];
                if (taskModelIdx == modelIdx) {
                    cyclesList.push_back(modelCyclesList[i]);
                }
            }
        }

    };


}


#endif //MUDNACSIM_THREAD_TASK_QUEUE_H
