#ifndef MUDNACSIM_THREAD_RUN_SEGMENT_H
#define MUDNACSIM_THREAD_RUN_SEGMENT_H

#include "runtime.h"
#include "pipeline.h"
#include "spm_noc_system.h"
#include "model_loader.h"

namespace mudnac {
    class ThreadRunSegment : public Thread {
    public:
        enum class ThreadRunSegmentState {
            STATE_INIT,
            STATE_RUN,
            STATE_DONE,
        };

        ThreadRunSegment(uint tid, uint segment_idx, uint64 max_cycles = UINT64_MAX, uint totalBatch = 1);

        virtual std::shared_ptr<ScheduleAction> generate_schedule_action() = 0;
        virtual void release_schedule_action();
        virtual void done_process();

        bool tick();


    protected:
        uint64 max_cycles_;
        uint64 start_cycles_;
        uint64 end_cycles_;
        uint64 cycles_;
        uint totalBatch_;

        uint segment_idx_;
    
        std::unique_ptr<Pipeline> pipeline_;
        std::shared_ptr<ScheduleAction> action_;

        void stateChange(ThreadRunSegmentState state_);
    };

    class ThreadRunSegmentBase : public ThreadRunSegment {
    public: 
        ThreadRunSegmentBase(uint tid, uint segment_idx, Model* model, std::shared_ptr<SegmentMapping> segment_mapping, uint64 max_cycles = UINT64_MAX, uint totalBatch = 1);
        std::shared_ptr<ScheduleAction> generate_schedule_action() override;
    protected:
        std::shared_ptr<ScheduleAction> schedule_action_;
        std::shared_ptr<SegmentMapping> segment_mapping_;
        Model* model_;
    };

} // mudnac

#endif //MUDNACSIM_THREAD_RUN_SEGMENT_H