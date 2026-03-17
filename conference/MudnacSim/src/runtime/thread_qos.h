#ifndef MUDNACSIM_THREAD_QOS_H
#define MUDNACSIM_THREAD_QOS_H


#include "runtime.h"


namespace mudnac {

    class ThreadQoS : public Thread {
    public:
        static const uint STATE_FETCH_NEXT_TASK = 200;
        static const uint STATE_WAIT_UNTIL_DISPATCHED = 300;

        std::deque<Model *> taskModelQueue;
        uint numTasks;
        uint64 threadStartCycles;
        uint64 threadEndCycles;
        uint64 waitingCycles;
        uint batchSize;
        std::deque<uint> runTaskList;

        uint *curModelIdxPtr;
        uint *curLayerIdxPtr;
        uint *qos_curTaskIdxPtr;
        uint *qos_nextTaskIdxPtr;

        ThreadQoS(uint tid_,
                  const std::deque<Model *> &taskModelQueue_,
                  uint numTasks_,
                  uint staticAccAllocIdx,
                  uint staticSpmAllocIdx,
                  uint batchSize_ = 1) :
                Thread(tid_) {
            taskModelQueue = taskModelQueue_;
            numTasks = numTasks_;
            waitingCycles = 0;
            batchSize = batchSize_;

            curModelIdxPtr = &RT.gv.threadCurrentModelType[tid];
            curLayerIdxPtr = &RT.gv.threadCurrentLayerIdx[tid];
            qos_curTaskIdxPtr = &RT.gv.qos_threadCurrentTaskIdx[tid];
            qos_nextTaskIdxPtr = &RT.gv.qos_nextTaskIdx;

            RT.gv.threadAccelAllocIdx[tid] = staticAccAllocIdx;
            RT.gv.threadSpmAllocIdx[tid] = staticSpmAllocIdx;
        }

        bool tick() {
            switch (state) {
                case STATE_RUN: {
                    bool done = layer->tick();
                    if (done) {  // the layer is done
                        layer->resetAllState();
#ifdef THREAD_DBG
                        printf("%lu ThreadQoS tid=%u: layer done, progress=%u/%u, taskIdx=%u, modelIdx=%u\n",
                               RT.getCycles(), tid, (*curLayerIdxPtr) + 1, (uint) model->layers.size(),
                               *qos_curTaskIdxPtr, *curModelIdxPtr);
#endif
                        if ((*curLayerIdxPtr) == model->layers.size() - 1) {  // the last layer is done
                            model->state = Model::STATE_IDLE;
                            RT.gv.qos_taskEndTime[*qos_curTaskIdxPtr] = RT.getCycles();
                            RT.releaseAll(tid);
                            printf("%lu ThreadQoS tid=%u: model done, taskIdx=%u, modelIdx=%u\n",
                                   RT.getCycles(), tid, *qos_curTaskIdxPtr, *curModelIdxPtr);
                            state = STATE_FETCH_NEXT_TASK;
                        } else {  // has next layer
                            printf("%lu ThreadQoS tid=%u: layer done, taskIdx=%u, modelIdx=%u, layerIdx=%u\n",
                                   RT.getCycles(), tid, *qos_curTaskIdxPtr, *curModelIdxPtr, *curLayerIdxPtr);
                            *curLayerIdxPtr += 1;
                            layer = model->layers[*curLayerIdxPtr];
                        }
                    }
                    break;
                }
                case STATE_WAIT_UNTIL_DISPATCHED: {
                    if (RT.getCycles() >= RT.gv.qos_taskDispatchTime[*qos_curTaskIdxPtr] and
                        model->state == Model::STATE_IDLE) {
                        RT.gv.qos_taskStartTime[*qos_curTaskIdxPtr] = RT.getCycles();
                        model->state = Model::STATE_RUNNING;
                        model->setTid(tid);
                        layer = model->layers.front();
                        state = STATE_RUN;
                        printf("%lu ThreadQoS tid=%u: task dispatched, taskIdx=%u, modelIdx=%u\n",
                               RT.getCycles(), tid, *qos_curTaskIdxPtr, *curModelIdxPtr);
#ifdef THREAD_DBG
                        printf("%lu ThreadQoS tid=%u: dispatched, taskIdx=%u, modelIdx=%u\n",
                               RT.getCycles(), tid, *qos_curTaskIdxPtr, *curModelIdxPtr);
#endif
                    } else {
                        waitingCycles++;
                    }
                    break;
                }
                case STATE_FETCH_NEXT_TASK: {
                    progress = 100 * (*qos_nextTaskIdxPtr) / numTasks;
                    if ((*qos_nextTaskIdxPtr) < numTasks) {
                        *qos_curTaskIdxPtr = *qos_nextTaskIdxPtr;
                        *qos_nextTaskIdxPtr += 1;
                        *curModelIdxPtr = RT.gv.qos_taskModelTypeQueue[*qos_curTaskIdxPtr];
                        *curLayerIdxPtr = 0;
                        runTaskList.push_back(*qos_curTaskIdxPtr);
                        model = taskModelQueue[*qos_curTaskIdxPtr];
                        state = STATE_WAIT_UNTIL_DISPATCHED;
#ifdef THREAD_DBG
                        printf("%lu ThreadQoS tid=%u: fetch next, taskIdx=%u, modelIdx=%u, "
                               "dispatchTime=%lu, modelState=%u\n",
                               RT.getCycles(), tid, *qos_curTaskIdxPtr, *curModelIdxPtr,
                               RT.gv.qos_taskDispatchTime[*qos_curTaskIdxPtr], model->state);
#endif
                    } else {  // all tasks are already dispatched
#ifdef THREAD_DBG
                        printf("%lu ThreadQoS tid=%u: thread terminate\n", RT.getCycles(), tid);
#endif
                        threadEndCycles = RT.getCycles();
                        state = STATE_DONE;
                    }
                    break;
                }
                case STATE_INIT: {
                    threadStartCycles = RT.getCycles();
                    state = STATE_FETCH_NEXT_TASK;
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
    };


}


#endif //MUDNACSIM_THREAD_QOS_H
