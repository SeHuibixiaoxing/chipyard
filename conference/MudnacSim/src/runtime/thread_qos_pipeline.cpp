#include "thread_qos_pipeline.h"

namespace mudnac {
    ThreadQoSPipeline::ThreadQoSPipeline(uint tid_, const std::deque<Model *> &taskModelQueue_, uint numTasks_) :
            Thread(tid_) {
        taskModelQueue = taskModelQueue_;
        numTasks = numTasks_;
        waitingCycles = 0;
        cur_seg_idx= 0;

        curModelIdxPtr = &RT.thread_current_model_type_[tid];
        curSubgraphIdxPtr = &RT.thread_current_subgraph_idx_[tid];
        qos_curTaskIdxPtr = &RT.qos_thread_current_task_idx_[tid];
        qos_nextTaskIdxPtr = &RT.qos_next_task_idx_;
                
        RT.thread_acc_target_idx_[tid] = 0;
        RT.thread_spm_pages_per_acc_kb_target_idx_[tid] = 0;

        updateResourceRecord(0, 0);
    }
    void ThreadQoSPipeline::updateResourceRecord(uint new_acc_idx, uint new_spm_idx) {
        auto target = RT.GenerateTargetFromIdx(new_acc_idx, new_spm_idx, *curModelIdxPtr);
        auto &preprocessed_entry = RT.profiling_result_.get(target);

        LOG(THREAD_QOS_PIPELINE) << "ThreadQoS tid=" << tid
                                << ": update resource record, oldAccTargetIdx=" << RT.thread_acc_target_idx_[tid]
                                << ", oldSpmTargetIdx=" << RT.thread_spm_pages_per_acc_kb_target_idx_[tid]
                                << ", newAccTargetIdx=" << new_acc_idx
                                << ", newSpmTargetIdx=" << new_spm_idx
                                << ", acc_occcupied_num_=" << RT.acc_occcupied_num_
                                << "/" << RT.num_accels_
                                << ", spm_pages_occupied_num_=" << RT.spm_pages_occupied_num_
                                << "/" << RT.num_accels_ * RT.pages_per_bank_;
        // 先释放,再占用
        RT.acc_occcupied_num_ -= RT.thread_acc_occupied_num_[tid];
        RT.spm_pages_occupied_num_ -= RT.thread_spm_occupied_num_[tid];

        RT.thread_acc_occupied_num_[tid] = RT.profiling_result_.getAvailableAccs()[new_acc_idx];
        RT.thread_spm_occupied_num_[tid] = RT.profiling_result_.getAvailableAccs()[new_acc_idx] * RT.profiling_result_.getAvailableSpmPerAccKb()[new_spm_idx] * 1024 / RT.page_bytes_size_;

        RT.acc_occcupied_num_ += RT.thread_acc_occupied_num_[tid];
        RT.spm_pages_occupied_num_ += RT.thread_spm_occupied_num_[tid];

        RT.thread_acc_target_idx_[tid] = new_acc_idx;

        LOG(THREAD_QOS_PIPELINE) << "ThreadQoS tid=" << tid
                                << ": after update resource record, acc_occcupied_num_=" << RT.acc_occcupied_num_
                                << "/" << RT.num_accels_
                                << ", spm_pages_occupied_num_=" << RT.spm_pages_occupied_num_
                                << "/" << RT.num_accels_ * RT.pages_per_bank_
                                << ",RT.thread_acc_occupied_num_[tid]=" << RT.thread_acc_occupied_num_[tid]
                                << ",RT.thread_spm_occupied_num_[tid]=" << RT.thread_spm_occupied_num_[tid];
    }

    bool ThreadQoSPipeline::tick() {
        switch (state) {
            case STATE_INIT: {
                threadStartCycles = RT.getCycles();
                state = STATE_FETCH_NEXT_TASK;
                break;
            }
            case STATE_FETCH_NEXT_TASK: {
                progress = 100 * (*qos_nextTaskIdxPtr) / numTasks;
                if ((*qos_nextTaskIdxPtr) < numTasks) {
                    *qos_curTaskIdxPtr = *qos_nextTaskIdxPtr;
                    *qos_nextTaskIdxPtr += 1;
                    *curModelIdxPtr = RT.qos_task_model_type_queue_[*qos_curTaskIdxPtr];
                    *curSubgraphIdxPtr = 0;
                    runTaskList.push_back(*qos_curTaskIdxPtr);
                    model = taskModelQueue[*qos_curTaskIdxPtr];
                    state = STATE_WAIT_UNTIL_DISPATCHED;

                    int left_time = static_cast<int>(RT.qos_model_target_cycles_[*curModelIdxPtr] +
                            RT.qos_task_dispatch_time_[*qos_curTaskIdxPtr]) -
                            static_cast<int>(RT.getCycles()); // 任务剩余时间
                    
                    auto profile_target = RT.GenerateTargetFromIdx(RT.thread_acc_target_idx_[tid],RT.thread_spm_pages_per_acc_kb_target_idx_[tid], *curModelIdxPtr);
                    auto &preprocessed_entry = RT.profiling_result_.get(profile_target);
                    auto task_remain_pred_time = preprocessed_entry.remain_cost[*curSubgraphIdxPtr]; // 任务预计剩余完成时间，包括当前子图

                    // 更新score
                    if (left_time < 0) {
                        RT.qos_thread_current_score_[tid] = 10;
                    } else {
                        RT.qos_thread_current_score_[tid] = (float)task_remain_pred_time / left_time;
                    }
                    
                    
                    printf("%lu ThreadQoS tid=%u: fetch next, taskIdx=%u, modelIdx=%u, "
                            "dispatchTime=%lu, task_remain_pred_time=%lu, left_time=%d, RT.qos_thread_current_score_[tid]=%d\n",
                            RT.getCycles(), tid, *qos_curTaskIdxPtr, *curModelIdxPtr,
                            RT.qos_task_dispatch_time_[*qos_curTaskIdxPtr], task_remain_pred_time, left_time, RT.qos_thread_current_score_[tid]);
                } else {  // all tasks are already dispatched
                    printf("%lu ThreadQoS tid=%u: thread terminate\n", RT.getCycles(), tid);
                    threadEndCycles = RT.getCycles();
                    state = STATE_DONE;
                }
                break;
            }
            case STATE_WAIT_UNTIL_DISPATCHED: {
                if (RT.getCycles() >= RT.qos_task_dispatch_time_[*qos_curTaskIdxPtr]) {
                    RT.qos_task_start_time_[*qos_curTaskIdxPtr] = RT.getCycles();
                    model->setTid(tid);
                    state = STATE_RESOURCE_ALLOC;
                    printf("%lu ThreadQoS tid=%u: task dispatched, taskIdx=%u, modelIdx=%u\n",
                            RT.getCycles(), tid, *qos_curTaskIdxPtr, *curModelIdxPtr);
                } else {
                    waitingCycles++;
                }
                break;
            }
            case STATE_RESOURCE_ALLOC: {
                RT.qos_task_subgraph_start_alloc_time_[*qos_curTaskIdxPtr][*curSubgraphIdxPtr] = RT.getCycles();
                if (RT.schedule_acc_alloc_strategy_ == PipelineRuntime::ScheduleAccAllocStrategy::STATIC_FIXED) {
                    // 静态固定分配策略，不调整资源
                    assert(RT.num_accels_  %  RT.num_threads_ == 0);
                    uint acc_per_thread = RT.num_accels_ / RT.num_threads_;
                    bool ok = false;
                    uint acc_per_thread_idx = 0;
                    for (uint i = 0;i <  RT.profiling_result_.getAvailableAccs().size();++ i) {
                        uint acc = RT.profiling_result_.getAvailableAccs()[i];
                        if (acc == acc_per_thread) {
                            ok = true;
                            acc_per_thread_idx = i;
                            break;
                        }
                    }
                    updateResourceRecord(acc_per_thread_idx, RT.thread_spm_pages_per_acc_kb_target_idx_[tid]);
                    state = STATE_CREATE_PIPELINE_THREAD;
                    break;
                }

                int left_time = static_cast<int>(RT.qos_model_target_cycles_[*curModelIdxPtr] +
                            RT.qos_task_dispatch_time_[*qos_curTaskIdxPtr]) -
                            static_cast<int>(RT.getCycles()); // 任务剩余时间
                
                auto profile_acc_list = RT.profiling_result_.getAvailableAccs();
                auto profile_spm_per_acc_kb_list = RT.profiling_result_.getAvailableSpmPerAccKb();


                uint last_acc_target_idx = RT.thread_acc_target_idx_[tid];
                uint last_spm_target_idx = RT.thread_spm_pages_per_acc_kb_target_idx_[tid];

                auto profile_target = RT.GenerateTargetFromIdx(last_acc_target_idx, last_spm_target_idx, *curModelIdxPtr);
                auto &preprocessed_entry = RT.profiling_result_.get(profile_target);
                auto task_remain_pred_time = preprocessed_entry.remain_cost[*curSubgraphIdxPtr]; // 任务预计剩余完成时间，包括当前子图
                auto subgraph_pred_exec_time = preprocessed_entry.cost[*curSubgraphIdxPtr]; // 当前子图预计执行时间
                
                uint new_acc_target_idx = last_acc_target_idx;

                LOG(THREAD_QOS_PIPELINE) << "ThreadQoS tid=" << tid
                                        << " schedule at cycle " << RT.getCycles()
                                        << ": resource alloc, taskIdx=" << *qos_curTaskIdxPtr
                                        << ", modelIdx=" << *curModelIdxPtr
                                        << ", subGraphIdx=" << *curSubgraphIdxPtr
                                        << ", leftTime=" << left_time
                                        << ", taskRemainPredTime=" << task_remain_pred_time
                                        << ", lastAccTargetIdx=" << last_acc_target_idx
                                        << ", accOccupiedNum=" << RT.acc_occcupied_num_
                                        << "/" << RT.num_accels_
                                        << ", SpmOccupiedNum=" << RT.spm_pages_occupied_num_
                                        << "/" << RT.num_accels_ * RT.pages_per_bank_
                                        << ", RT.thread_acc_occupied_num_[tid]=" << RT.thread_acc_occupied_num_[tid]
                                        << ", RT.thread_acc_target_idx_[tid]=" << RT.thread_acc_target_idx_[tid]
                                        << ", RT.thread_spm_pages_per_acc_kb_target_idx_[tid]=" << RT.thread_spm_pages_per_acc_kb_target_idx_[tid];
                
                bool do_increase = false;
                bool do_decrease = false;
                bool need_wait = false;
                // 如果任务剩余时间小于零（已经超时），或者当前资源预计剩余执行时间大于剩余时间
                if (left_time < 0 || task_remain_pred_time * RT.acc_increase_factor_ > left_time) {
                    // 没有达到最大资源，且资源剩余充足
                    if (last_acc_target_idx + 1 < profile_acc_list.size() 
                        && (static_cast<int>(RT.num_accels_) - static_cast<int>(RT.acc_occcupied_num_) + 
                            static_cast<int>(RT.thread_acc_occupied_num_[tid]) >= 
                            static_cast<int>(profile_acc_list[last_acc_target_idx + 1]))) {
                        do_increase = true;
                        new_acc_target_idx = last_acc_target_idx + 1;
                        LOG(THREAD_QOS_PIPELINE) << "ThreadQoS tid=" << tid
                                                << ": resource alloc, can increase acc directly. profile_acc_list.size()=" << profile_acc_list.size()
                                                << ", last_acc_target_idx=" << last_acc_target_idx
                                                << ", accOccupiedNum=" << RT.acc_occcupied_num_
                                                << "/" << RT.num_accels_
                                                << ", new_acc_target_idx=" << new_acc_target_idx;
                    } 
                    // else {
                    //     uint total_acc_will_release = 0;

                    //     uint can_acc_target_idx = std::min(last_acc_target_idx + 1, (uint)profile_acc_list.size() - 1);
                    //     auto can_profile_target = RT.GenerateTargetFromIdx(can_acc_target_idx, RT.thread_spm_pages_per_acc_kb_target_idx_[tid], *curModelIdxPtr);
                    //     auto& can_preprocessed_entry = RT.profiling_result_.get(can_profile_target);
                    //     auto can_subgraph_pred_exec_time = can_preprocessed_entry.cost[*curSubgraphIdxPtr];

                    //     LOG(THREAD_QOS_PIPELINE) << "ThreadQoS tid=" << tid
                    //                             << ": resource alloc, cannot increase acc directly. profile_acc_list.size()=" << profile_acc_list.size()
                    //                             << ", last_acc_target_idx=" << last_acc_target_idx
                    //                             << ", accOccupiedNum=" << RT.acc_occcupied_num_
                    //                             << "/" << RT.num_accels_
                    //                             << ", can_acc_target_idx=" << can_acc_target_idx
                    //                             << ", can_subgraph_pred_exec_time=" << can_subgraph_pred_exec_time;

                    //     for (uint i = 0;i < RT.num_threads_;++ i) {
                    //         // 该任务预计会释放资源，且预计结束时间不会太久，且拥有加速器数量大于最小值（能够释放出加速器）
                    //         LOG(THREAD_QOS_PIPELINE) << "ThreadQoS tid=" << tid
                    //                                 << ": check thread i=" << i
                    //                                 << ", qos_thread_current_score_=" << RT.qos_thread_current_score_[i]
                    //                                 << ", qos_thread_nxt_schedule_time_=" << RT.qos_thread_nxt_schedule_time_[i]
                    //                                 << ", can_subgraph_pred_exec_time=" << can_subgraph_pred_exec_time
                    //                                 << ", RT.time_out_threshold_=" << RT.time_out_threshold_
                    //                                 << ", can_subgraph_pred_exec_time * RT.time_out_threshold_=" << can_subgraph_pred_exec_time * RT.time_out_threshold_;
                    //         if (i != tid && RT.qos_thread_current_score_[i] < 0.5 && RT.qos_thread_nxt_schedule_time_[i] < can_subgraph_pred_exec_time * RT.time_out_threshold_ && RT.thread_acc_occupied_num_[tid] > RT.profiling_result_.getAvailableAccs()[0]) {
                    //             total_acc_will_release += profile_acc_list[RT.thread_acc_target_idx_[i]];
                    //         }
                    //     }
                    //     LOG(THREAD_QOS_PIPELINE) << "ThreadQoS tid=" << tid
                    //                             << ": resource alloc, cannot increase acc directly. total_acc_will_release=" << total_acc_will_release
                    //                             << ", RT.num_accels_=" << RT.num_accels_
                    //                             << ", accOccupiedNum=" << RT.acc_occcupied_num_
                    //                             << ", profile_acc_list[last_acc_target_idx + 1]=" << profile_acc_list[last_acc_target_idx + 1];
                    //     if (static_cast<int>(total_acc_will_release) + (static_cast<int>(RT.num_accels_) - static_cast<int>(RT.acc_occcupied_num_) + static_cast<int>(RT.thread_acc_occupied_num_[tid])) >= static_cast<int>(profile_acc_list[can_acc_target_idx])) {
                    //         do_increase = true;
                    //         new_acc_target_idx = can_acc_target_idx;
                    //         RT.thread_acc_alloc_timeout_[tid] = RT.time_out_threshold_ * subgraph_pred_exec_time;
                    //         waitAccWaitTimer = 0;
                    //         need_wait = true;
                    //         LOG(THREAD_QOS_PIPELINE) << "ThreadQoS tid=" << tid
                    //                                 << ": resource alloc, increase acc after wait. new_acc_target_idx=" << new_acc_target_idx
                    //                                 << ", RT.thread_acc_alloc_timeout_[tid]=" << RT.thread_acc_alloc_timeout_[tid]
                    //                                 << ", waitAccWaitTimer=" << waitAccWaitTimer
                    //                                 << ", RT.time_out_threshold_=" << RT.time_out_threshold_
                    //                                 << ", subgraph_pred_exec_time=" << subgraph_pred_exec_time;
                    //     }
                    // }
                } else if (task_remain_pred_time < left_time * 0.5) {
                    // 减少资源能完成，且有其它任务需要获取资源，则减少资源
                    bool need_decrease = false;
                    for (uint i = 0;i < RT.num_threads_;++ i) {
                        if (i != tid && RT.qos_thread_current_score_[i] * RT.acc_increase_factor_ > 1.0) {
                            need_decrease = true;
                            break;
                        }
                    }
                    if (need_decrease) {
                        do_decrease = true;
                        new_acc_target_idx = (last_acc_target_idx > 0) ? last_acc_target_idx - 1 : 0;
                        LOG(THREAD_QOS_PIPELINE) << "ThreadQoS tid=" << tid
                                                << ": resource alloc, decrease acc. profile_acc_list.size()=" << profile_acc_list.size()
                                                << ", last_acc_target_idx=" << last_acc_target_idx
                                                << ", accOccupiedNum=" << RT.acc_occcupied_num_
                                                << "/" << RT.num_accels_
                                                << ", left_time=" << left_time
                                                << ", task_remain_pred_time=" << task_remain_pred_time
                                                << ", new_acc_target_idx=" << new_acc_target_idx;   
                    }
                } else if(last_acc_target_idx + 1 < profile_acc_list.size() 
                        && (static_cast<int>(RT.num_accels_) - static_cast<int>(RT.acc_occcupied_num_) + 
                            static_cast<int>(RT.thread_acc_occupied_num_[tid]) >= 
                            static_cast<int>(profile_acc_list[last_acc_target_idx + 1]))) {
                    // 得分是最小的，且有剩余资源，则增加资源
                    do_increase = true;
                    for (uint i = 0;i < RT.num_threads_;++ i) {
                        if (i != tid && RT.qos_thread_current_score_[i] < RT.qos_thread_current_score_[tid]) {
                            do_increase = false;
                            break;
                        }
                    }
                    if (do_increase) {
                        new_acc_target_idx = std::min(last_acc_target_idx + 1, (uint)profile_acc_list.size() - 1);

                        LOG(THREAD_QOS_PIPELINE) << "ThreadQoS tid=" << tid
                                                << ": resource alloc, increase acc for best score. profile_acc_list.size()=" << profile_acc_list.size()
                                                << ", last_acc_target_idx=" << last_acc_target_idx
                                                << ", accOccupiedNum=" << RT.acc_occcupied_num_
                                                << "/" << RT.num_accels_
                                                << ", RT.qos_thread_current_score_[tid]=" << RT.qos_thread_current_score_[tid]
                                                << ", new_acc_target_idx=" << new_acc_target_idx;
                    }
                }

                if (do_increase || do_decrease) {
                    profile_target = RT.GenerateTargetFromIdx(new_acc_target_idx, RT.thread_spm_pages_per_acc_kb_target_idx_[tid], *curModelIdxPtr);
                    auto& preprocessed_entry = RT.profiling_result_.get(profile_target);
                    task_remain_pred_time = preprocessed_entry.remain_cost[*curSubgraphIdxPtr]; // 任务预计剩余完成时间，包括当前子图
                    subgraph_pred_exec_time = preprocessed_entry.cost[*curSubgraphIdxPtr]; // 当前子图预计执行时间
                    RT.qos_thread_nxt_schedule_time_[tid] = RT.getCycles() + subgraph_pred_exec_time;

                    // 更新score
                    if (left_time < 0) {
                        RT.qos_thread_current_score_[tid] = 10;
                    } else {
                        RT.qos_thread_current_score_[tid] = (float)task_remain_pred_time / left_time;
                    }

                    LOG(THREAD_QOS_PIPELINE) << "tid=" << tid << ", score=" << RT.qos_thread_current_score_[tid]
                                            << ", left_time=" << left_time
                                            << ", task_remain_pred_time=" << task_remain_pred_time;
                }

                if (need_wait) {
                    RT.thread_acc_wait_target_idx_[tid] = new_acc_target_idx;
                    RT.qos_thread_nxt_schedule_time_[tid] += RT.thread_acc_alloc_timeout_[tid];
                    state = STATE_RESOURCE_WAIT;
                } else {
                    updateResourceRecord(new_acc_target_idx, RT.thread_spm_pages_per_acc_kb_target_idx_[tid]);
                    state = STATE_CREATE_PIPELINE_THREAD;
                }

                break;
            }
            case STATE_RESOURCE_WAIT: {
                waitAccWaitTimer++;
                auto profile_acc_list = RT.profiling_result_.getAvailableAccs();
                auto profile_spm_per_acc_kb_list = RT.profiling_result_.getAvailableSpmPerAccKb();

                uint last_acc_target_idx = RT.thread_acc_wait_target_idx_[tid];
                if ((static_cast<int>(RT.num_accels_) - static_cast<int>(RT.acc_occcupied_num_) + static_cast<int>(RT.thread_acc_occupied_num_[tid])) >= static_cast<int>(profile_acc_list[last_acc_target_idx])) {
                    // 资源够了，分配后开始执行
                    updateResourceRecord(last_acc_target_idx, RT.thread_spm_pages_per_acc_kb_target_idx_[tid]);
                    auto& entry = RT.profiling_result_.get(
                            RT.GenerateTargetFromIdx(
                                last_acc_target_idx,
                                RT.thread_spm_pages_per_acc_kb_target_idx_[tid],
                                *curModelIdxPtr
                            )
                        );
                    auto task_remain_pred_time = entry.remain_cost[*curSubgraphIdxPtr];
                    RT.qos_thread_nxt_schedule_time_[tid] = RT.getCycles() + entry.cost[*curSubgraphIdxPtr];
                    uint64 left_time = static_cast<int>(RT.qos_model_target_cycles_[*curModelIdxPtr]) +
                            static_cast<int>(RT.qos_task_dispatch_time_[*qos_curTaskIdxPtr]) -
                            static_cast<int>(RT.getCycles()); // 任务剩余时间
                    // 更新score
                    if (left_time < 0) {
                        RT.qos_thread_current_score_[tid] = 10;
                    } else {
                        RT.qos_thread_current_score_[tid] = (float)task_remain_pred_time / left_time;
                    }
                    state = STATE_CREATE_PIPELINE_THREAD;
                } else if (waitAccWaitTimer >= RT.thread_acc_alloc_timeout_[tid] && last_acc_target_idx > 0) {
                    // 超时，减少资源。重设等待时间
                    uint new_acc_target_idx = last_acc_target_idx - 1;
                    RT.thread_acc_target_idx_[tid] = new_acc_target_idx;
                    RT.qos_thread_nxt_schedule_time_[tid] -= RT.thread_acc_alloc_timeout_[tid];
                    RT.thread_acc_alloc_timeout_[tid] = RT.getCycles() - RT.thread_acc_alloc_timeout_[tid] + RT.time_out_threshold_ *
                                                        RT.profiling_result_.get(
                                                                RT.GenerateTargetFromIdx(
                                                                        new_acc_target_idx,
                                                                        RT.thread_spm_pages_per_acc_kb_target_idx_[tid],
                                                                        *curModelIdxPtr
                                                                )
                                                        ).cost[*curSubgraphIdxPtr];
                    RT.qos_thread_nxt_schedule_time_[tid] += RT.thread_acc_alloc_timeout_[tid];
                }
                break;
            }
            case STATE_CREATE_PIPELINE_THREAD: {
                // uint tid, Model* model, std::shared_ptr<PipelineMapping> pipeline_mapping, uint64 max_cycles = UINT64_MAX, uint totalBatch = 1
                const auto& profiling_entry = RT.profiling_result_.get(RT.GenerateTargetFromIdx(RT.thread_acc_target_idx_[tid], RT.thread_spm_pages_per_acc_kb_target_idx_[tid], *curModelIdxPtr));
                const auto& mapping_path = profiling_entry.mapping_paths[*curSubgraphIdxPtr];
                LOG(THREAD_QOS_PIPELINE) << "ThreadQoS tid=" << tid
                                        << ": create pipeline thread, taskIdx=" << *qos_curTaskIdxPtr
                                        << ", modelIdx=" << *curModelIdxPtr
                                        << ", subGraphIdx=" << *curSubgraphIdxPtr
                                        << ", thread_acc_target_idx_=" << RT.thread_acc_target_idx_[tid]
                                        << ", thread_spm_pages_per_acc_kb=" << RT.thread_spm_pages_per_acc_kb_target_idx_[tid]
                                        << ", mapping_path=" << mapping_path;
                std::shared_ptr<PipelineMapping> pipeline_mapping = std::make_shared<PipelineMapping>(PipelineMappingParser::pipelineCandidateToPipelineMapping(model, mapping_path));
                threadRunPipelinePtr = std::make_unique<ThreadRunPipeline>(
                        tid * 1000000000 + *curModelIdxPtr * 100000 + *qos_curTaskIdxPtr * 100 + *curSubgraphIdxPtr, 
                        model, pipeline_mapping, UINT64_MAX, RT.batch_size_per_task_);
                
                int left_time = static_cast<int>(RT.qos_model_target_cycles_[*curModelIdxPtr] +
                    RT.qos_task_dispatch_time_[*qos_curTaskIdxPtr]) -
                    static_cast<int>(RT.getCycles()); // 任务剩余时间
                int task_remain_pred_time = profiling_entry.remain_cost[*curSubgraphIdxPtr]; // 任务预计剩余完成时间，包括当前子图
                
                RT.qos_task_subgraph_est_time_subgraph_[*qos_curTaskIdxPtr][*curSubgraphIdxPtr] = profiling_entry.cost[*curSubgraphIdxPtr];
                RT.qos_task_subgraph_est_time_task_[*qos_curTaskIdxPtr][*curSubgraphIdxPtr] = task_remain_pred_time;
                RT.qos_task_subgraph_slack_time_[*qos_curTaskIdxPtr][*curSubgraphIdxPtr] = left_time - task_remain_pred_time;
                RT.qos_task_subgraph_acc_num_[*qos_curTaskIdxPtr][*curSubgraphIdxPtr] = RT.thread_acc_occupied_num_[tid];
                RT.qos_task_subgraph_start_run_time_[*qos_curTaskIdxPtr][*curSubgraphIdxPtr] = RT.getCycles();
                state = STATE_RUN;
                break;
            }
            case STATE_RUN: {
                bool done = threadRunPipelinePtr->tick();
                if (done) {
                    printf("%lu ThreadQoS tid=%u: layer done, progress=%u/%u, taskIdx=%u, modelIdx=%u\n",
                            RT.getCycles(), tid, (*curSubgraphIdxPtr) + 1, RT.schedule_partition_num_,
                            *qos_curTaskIdxPtr, *curModelIdxPtr);
                    
                    RT.qos_task_subgraph_end_run_time_[*qos_curTaskIdxPtr][*curSubgraphIdxPtr] = RT.getCycles();
                    RT.qos_task_subgraph_slack_time_[*qos_curTaskIdxPtr][*curSubgraphIdxPtr] += RT.qos_model_target_cycles_[*curModelIdxPtr] +
                            RT.qos_task_dispatch_time_[*qos_curTaskIdxPtr] -
                            RT.getCycles(); // 任务剩余时间
                            
                    if ((*curSubgraphIdxPtr) == RT.schedule_partition_num_ - 1) {  // the last subgraph is done
                        RT.qos_task_end_time_[*qos_curTaskIdxPtr] = RT.getCycles();
                        
                        printf("%lu ThreadQoS tid=%u: model done, taskIdx=%u, modelIdx=%u\n",
                                RT.getCycles(), tid, *qos_curTaskIdxPtr, *curModelIdxPtr);
                        // 释放资源
                        RT.acc_occcupied_num_ -= RT.thread_acc_occupied_num_[tid];
                        RT.spm_pages_occupied_num_ -= RT.thread_spm_occupied_num_[tid];
                        RT.thread_acc_occupied_num_[tid] = 0;
                        RT.thread_spm_occupied_num_[tid] = 0;
                        RT.thread_acc_target_idx_[tid] = 0;
                        updateResourceRecord(0, 0);
                        state = STATE_FETCH_NEXT_TASK;
                    } else {  // has next subgraph
                        printf("%lu ThreadQoS tid=%u: sub-graph done, taskIdx=%u, modelIdx=%u, subGraphIdx=%u\n",
                                RT.getCycles(), tid, *qos_curTaskIdxPtr, *curModelIdxPtr, *curSubgraphIdxPtr);
                        RT.qos_task_subgraph_end_run_time_[*qos_curTaskIdxPtr][*curSubgraphIdxPtr] = RT.getCycles();
                        *curSubgraphIdxPtr += 1;
                        state = STATE_RESOURCE_ALLOC;
                    }
                }
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

}