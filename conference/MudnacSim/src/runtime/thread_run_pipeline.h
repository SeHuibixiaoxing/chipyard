#ifndef MUDNACSIM_THREAD_RUN_PIPELINE_H
#define MUDNACSIM_THREAD_RUN_PIPELINE_H

#include "thread_run_segment.h"
#include <vector>
#include <memory>

namespace mudnac {

    // Runs a full PipelineMapping (multiple SegmentMappings) sequentially.
    class ThreadRunPipeline : public Thread {
    public:
        ThreadRunPipeline(uint tid, Model* model, std::shared_ptr<PipelineMapping> pipeline_mapping, uint64 max_cycles = UINT64_MAX, uint totalBatch = 1);
        bool tick() override;

        uint start_cycles, end_cycles;

    protected:
        std::shared_ptr<PipelineMapping> pipeline_mapping_;
        std::vector<std::unique_ptr<ThreadRunSegmentBase>> segments_;
        size_t current_segment_idx_;
        uint64 max_cycles_;
        uint totalBatch_;
        Model* model_;
    };

}

#endif //MUDNACSIM_THREAD_RUN_PIPELINE_H
