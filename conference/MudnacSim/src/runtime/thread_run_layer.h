#ifndef MUDNACSIM_THREAD_RUN_LAYER_H
#define MUDNACSIM_THREAD_RUN_LAYER_H


#include "runtime.h"


namespace mudnac {


    class ThreadRunLayer : public Thread {
    public:
        ThreadRunLayer(uint tid_, Model *model_, uint modelType, uint layerIdx,
                       uint staticAccAllocIdx, uint staticSpmAllocIdx)
                : Thread(tid_) {
            assert(model_ != NULL);
            assert(layerIdx < model_->numLayers);
            assert(RT.gv.accelAllocMode == RT.gv.ACCEL_ALLOC_MODE_STATIC);
            assert(RT.gv.memoryAccessMode == RT.gv.MEMORY_ACCESS_MODE_UNLIMITED);
            assert(RT.gv.spmAllocMode != RT.gv.SPM_ALLOC_MODE_DYNAMIC);
            model = model_;
            layer = model->layers[layerIdx];
            RT.gv.threadAccelAllocIdx[tid] = staticAccAllocIdx;
            RT.gv.threadSpmAllocIdx[tid] = staticSpmAllocIdx;
            RT.gv.threadCurrentModelType[tid] = modelType;
            RT.gv.threadCurrentLayerIdx[tid] = layerIdx;
        }

        bool tick() {
            switch (state) {
                case STATE_RUN: {
                    bool done = layer->tick();
                    if (done) {
#ifdef THREAD_DBG
                        printf("%lu tid=%u: layer done\n", RT.getCycles(), tid);
#endif
                        RT.releaseAll(tid);
                        progress = 100;
                        state = STATE_DONE;
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
    };


}


#endif //MUDNACSIM_THREAD_RUN_LAYER_H
