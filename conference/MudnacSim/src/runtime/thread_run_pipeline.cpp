#include "thread_run_pipeline.h"
#include "tools.h"

namespace mudnac {

    ThreadRunPipeline::ThreadRunPipeline(uint tid, Model* model, std::shared_ptr<PipelineMapping> pipeline_mapping, uint64 max_cycles, uint totalBatch)
            : Thread(tid), pipeline_mapping_(pipeline_mapping), current_segment_idx_(0), max_cycles_(max_cycles), totalBatch_(totalBatch), model_(model) {
        // Create a ThreadRunSegmentBase for each segment candidate
        if (pipeline_mapping_) {
            for (uint i = 0;i < pipeline_mapping_->segmentCandidates.size();++ i) {
                const auto& cand = pipeline_mapping_->segmentCandidates[i];
                auto seg = std::make_shared<SegmentMapping>(cand);
                segments_.push_back(std::make_unique<ThreadRunSegmentBase>(tid, i, model_, seg, max_cycles_, totalBatch_));
            }
        }
        state = Thread::STATE_INIT;
        progress = 0;

        // Log creation using DebugLogger/LOG macro.
        LOG(THREAD_PIPELINE) << "ThreadRunPipeline(tid=" << tid
                     << ") created with " << segments_.size() << " segments.";
    }

    bool ThreadRunPipeline::tick() {
        if (state == Thread::STATE_DONE) return true;

        if (state == Thread::STATE_INIT) {
            start_cycles = RT.getCycles();
            if (segments_.empty()) {
                state = Thread::STATE_DONE;
                progress = 100;
                end_cycles = RT.getCycles();
                return true;
            }
            state = Thread::STATE_RUN;
        }

        // Run current segment
        if (current_segment_idx_ < segments_.size()) {
            if (RT.getCycles() % 1000000 == 0) {
                std::cout << "Cycles " << RT.getCycles() << ": Thread " << tid << " running segment " << current_segment_idx_ << " / " << (segments_.size()-1) << std::endl;
            }
            bool seg_done = segments_[current_segment_idx_]->tick();

            // update progress as percent of segments completed
            progress = (uint)((current_segment_idx_ * 100) / segments_.size());

            if (seg_done) {
                std::cout << "Thread " << tid << " completed segment " << current_segment_idx_ << std::endl;

                current_segment_idx_++;
                // reset progress for next segment (will be updated on next tick)
                if (current_segment_idx_ >= segments_.size()) {
                    state = Thread::STATE_DONE;
                    progress = 100;
                    std::cout << "Thread " << tid << " all segments done" << std::endl;
                    end_cycles = RT.getCycles();
                    return true;
                }
            }
            return state == Thread::STATE_DONE;
        }

        state = Thread::STATE_DONE;
        end_cycles = RT.getCycles();
        progress = 100;
        return true;
    }
}
