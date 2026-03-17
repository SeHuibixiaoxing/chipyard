#ifndef MUDNACSIM_THREAD_QOS_PIPELINE_H
#define MUDNACSIM_THREAD_QOS_PIPELINE_H


#include "runtime.h"
#include "thread_run_pipeline.h"


namespace mudnac {

    class ThreadQoSPipeline : public Thread {
    public:
        static const uint STATE_FETCH_NEXT_TASK = 200;
        static const uint STATE_WAIT_UNTIL_DISPATCHED = 300;
        static const uint STATE_RESOURCE_ALLOC = 400;
        static const uint STATE_RESOURCE_WAIT = 500;
        static const uint STATE_CREATE_PIPELINE_THREAD = 600;

        std::deque<Model *> taskModelQueue;
        uint numTasks;
        uint64 threadStartCycles;
        uint64 threadEndCycles;
        uint64 waitingCycles;
        uint64 waitAccWaitTimer;
        std::deque<uint> runTaskList;

        uint cur_seg_idx;

        uint *curModelIdxPtr;
        uint *curSubgraphIdxPtr;
        uint *qos_curTaskIdxPtr;
        uint *qos_nextTaskIdxPtr;

        std::unique_ptr<ThreadRunPipeline> threadRunPipelinePtr;

        ThreadQoSPipeline(uint tid_,
                  const std::deque<Model *> &taskModelQueue_,
                  uint numTasks_);

        void updateResourceRecord(uint new_acc_idx, uint new_spm_idx);
        bool tick();
    };


}


#endif //MUDNACSIM_THREAD_QOS_PIPELINE_H
