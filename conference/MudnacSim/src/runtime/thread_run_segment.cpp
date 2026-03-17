#include "thread_run_segment.h"

namespace mudnac {
    ThreadRunSegment::ThreadRunSegment(uint tid, uint segment_idx, uint64 max_cycles, uint totalBatch)
                : Thread(tid), segment_idx_(segment_idx), max_cycles_(max_cycles), pipeline_(nullptr), action_(nullptr), totalBatch_(totalBatch), cycles_(0) {
                    
    }

    void ThreadRunSegment::release_schedule_action() {

    }

    void ThreadRunSegment::done_process() {}

    bool ThreadRunSegment::tick()
    {
        if(cycles_ >= max_cycles_) {
            std::cout << "thread " << tid << " threadRunSegment return true because cycles >= maxCycles" << std::endl;
            return true;
        }
        if(RT.getCycles() % 1000000 == 0 && pipeline_ != nullptr) {
            std::cout << "thread " << tid<< " cycles: " << RT.getCycles() << ", maxBatchOffset=" << pipeline_->getMaxBatchOffset() << ", noProcessBatch=" << pipeline_->getNoProcessBatch() << ", processingBatch=" << pipeline_->getBatchProcessing() << ", batchProcessed=" << pipeline_->getTotalBatchProcessed() << std::endl;
        }
        switch (state) {
            case static_cast<uint>(ThreadRunSegmentState::STATE_INIT): {
                action_ = generate_schedule_action();
                pipeline_ = std::make_unique<Pipeline>(tid, action_);
                // configure total batches from totalBatch_ (in units of full batches)
                if (pipeline_ && totalBatch_ > 0) {
                    pipeline_->addBatch(totalBatch_);
                }
                start_cycles_ = RT.getCycles();
                stateChange(ThreadRunSegmentState::STATE_RUN);
                break;
            }
            case static_cast<uint>(ThreadRunSegmentState::STATE_RUN): {
                if(pipeline_->tick()) {
                    end_cycles_ = RT.getCycles();
                    stateChange(ThreadRunSegmentState::STATE_DONE);
                    LOG(RUNTIME_SOURCE_ALLOC) << "Thread " << tid << " finished segment " << segment_idx_ << std::endl;
                    RT.ReleaseAction(action_);
                    done_process();
                }
                break;
            }
            case static_cast<uint>(ThreadRunSegmentState::STATE_DONE): {
                break;
            }
            default: {
                assert(false);
            }
        }
        return state == static_cast<uint>(ThreadRunSegmentState::STATE_DONE);
    }

    void ThreadRunSegment::stateChange(ThreadRunSegmentState state_) {
        state = static_cast<uint>(state_);
    }

    ThreadRunSegmentBase::ThreadRunSegmentBase(uint tid, uint segment_idx, Model* model, std::shared_ptr<SegmentMapping> segment_mapping, uint64 max_cycles, uint totalBatch):
        ThreadRunSegment(tid, segment_idx, max_cycles, totalBatch), model_(model), segment_mapping_(segment_mapping) {

    }

    std::shared_ptr<ScheduleAction> ThreadRunSegmentBase::generate_schedule_action() {
        schedule_action_ = RT.GenerateAction(segment_mapping_, model_);
        return schedule_action_;
    }
}