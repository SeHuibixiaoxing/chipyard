#include "pipeline.h"

namespace mudnac {
    void Pipeline::addBatch(uint batch) {
        maxBatchOffset += batch;
    }
    uint Pipeline::getMaxBatchOffset() {
        return maxBatchOffset;
    }
    uint Pipeline::getNoProcessBatch() {
        return maxBatchOffset - startTime.size() * getSubBatchSize();
    }
    uint Pipeline::getBatchProcessing() {
        return startTime.size() * getSubBatchSize() - getTotalBatchProcessed();
    }
    uint Pipeline::getTotalBatchProcessed() {
        uint ans = maxBatchOffset;
        for(auto [stage_idx, tensor_id]: action_->segment_mapping_->output_tensor_id_stage) {
            ans = std::min(ans, pipebuffer_set[stage_idx].at(tensor_id).subBatchOffset * getSubBatchSize());
        }
        return ans;
    }
    uint Pipeline::getSubBatchSize() {
        return action_->segment_mapping_->subBatchSize;
    }

    bool Pipeline::tick() {
        // 查死锁bug，分析卡点在哪个stage
        // {
        //     if (RT.getCycles() == 3500000) {
        //         for (uint i = 0;i < stages.size();++ i) {
        //             std::cout << "stage[" << i << "], batchProcessing=" << stages[i].getBatchProcessing() << ", batchProcessed=" << stages[i].getBatchProcessed() << std::endl;
        //         }
        //         std::cout << getAllPipeBufferStr() << std::endl;
        //         // DebugLogger::enable(SPM_DEBUG_FLAG);
        //         // DebugLogger::enable(MEMCTRL_DEBUG_FLAG);
        //         DebugLogger::enable(PIPELINE_DEADLOCK_FLAG);
        //     }
        // }
        const auto& segment_mapping = action_->segment_mapping_;
        const auto& stage_mapping_vec = segment_mapping->stage_mapping_vec;

        switch (state) {
            case PipelineState::STATE_BEGIN: {
                stateChange(PipelineState::STATE_CONFIG);
                pipelineStartTime = RT.getCycles();
                return false;
            }
            // 外部配置检查
            case PipelineState::STATE_CONFIG: {
                if(action_ == NULL) {
                    LOG(PIPELINE_DEBUG_FLAG) << "[pipeLogIdx-" << pipeLogIdx << "]" << "PipelineState::STATE_CONFIG: schedule action not config" << std::endl;
                    return false;
                }
                if (DebugLogger::isEnabled(PIPELINE_DEBUG_FLAG)) {
                    LOG(PIPELINE_DEBUG_FLAG) << "[pipeLogIdx-" << pipeLogIdx << "]" << "segment_mapping: " << action_->segment_mapping_->toString() << std::endl;
                }
                stateChange(PipelineState::STATE_PIPELINE_BUFFER_CONFIG);
                finishConfigTime = RT.getCycles();
            }
            // pipeline buffer创建
            case PipelineState::STATE_PIPELINE_BUFFER_CONFIG: {
                // 创建pipeline buffer及其监控结构
                createPipebuffer();

                // 创建ring buffer，并赋值pipebuffer中的ring buffer指针
                createRingBuffer();
                
                stateChange(PipelineState::STATE_STAGE_CONFIG);
                finishPipebufferConfigTime = RT.getCycles();
                break;
            }
            // stage创建、配置、资源分配
            case PipelineState::STATE_STAGE_CONFIG: {
                // stage创建、设置subBatch
                createStages();

                stateChange(PipelineState::STATE_STAGE_EXECUTE);
                finishStageConfigTime = RT.getCycles();
                break;
            }
            // pipeline buffer监控和stage执行
            case PipelineState::STATE_STAGE_EXECUTE: {
                // 接收已完成的指令
                waitCmd();

                // 处理入口DRAM（无ring buffer）和DRAM_DEPEN（有ring buffer）类型pipe buffer
                for(auto& [tensor_id, p]: tensorId2EntryDramOrDramDepenPipeBuffer) {
                    for (auto& [stage_idx, v]: p) {
                        auto& [pipe_buffer, buffer_idx] = v;
                        processEntryDramPipebuffer(tensor_id, stage_idx, pipe_buffer, buffer_idx);
                    }
                }
                            
                // 处理出口DRAM（无ring buffer）和DRAM_DEPEN（有ring buffer）类型pipe buffer
                for(auto& [tensor_id, p]: tensorId2ExportDramOrDramDepenPipeBuffer) {
                    assert(p.size() == 1); // 目前，同一个tensor id，只有一个stage将其作为出口张量
                    for (auto& [stage_idx, v]: p) {
                        auto& [pipe_buffer, buffer_idx] = v;
                        processExportDramPipebuffer(tensor_id, stage_idx, pipe_buffer, buffer_idx);
                    }
                }
            
                // 处理无ring buffer的ISOLATE pipebuffer            
                for(auto& [tensor_id, p]: tensorId2IsolatePipeBufferNoRingbuffer) {
                    for (auto& [nxt_stage_idx, t]: p) {
                        auto& [pre_pipe_buffer, nxt_pipe_buffer, pre_buffer_idx, nxt_buffer_idx] = t;
                        processIsolateSpmPipebufferWithoutRingbuffer(tensor_id, pre_pipe_buffer, pre_buffer_idx, nxt_stage_idx, nxt_pipe_buffer, nxt_buffer_idx);
                    }
                    processIsolateSpmPipebufferWithoutRingbufferSyn(tensor_id, p);
                }

                // 处理无ring buffer的SHARED pipebuffer
                for(auto& [tensor_id, p]: tensorId2SharedPipeBuffer) {
                    for (auto& [buffer_idx, t]: p) {
                        auto& [pre_pipe_buffer, _, pre_tag] = t.begin()->second;
                        processSharedSpmPipebufferWithoutRingbuffer(tensor_id, pre_pipe_buffer, buffer_idx, pre_tag);
                    }
                }

                // 处理有ring buffer的入口ISOLATE pipebuffer
                for(auto& [tensor_id, p]: tensorId2EntryIsolatePipeBufferWithRingbuffer) {
                    for (auto& [stage_idx, v]: p) {
                        auto& [pipe_buffer, buffer_idx] = v;
                        assert(pipe_buffer->ring_buffer_ != nullptr);
                        assert(pipe_buffer->use_ring_buffer_);
                        processEntryIsolateSpmPipebufferWithRingbuffer(tensor_id, stage_idx, pipe_buffer, buffer_idx);
                    }
                }
            
                // 处理有ring buffer的出口ISOLATE pipebuffer
                for(auto& [tensor_id, p]: tensorId2ExportIsolatePipeBufferWithRingbuffer) {
                    for (auto& [stage_idx, v]: p) {
                        auto& [pipe_buffer, buffer_idx] = v;
                        assert(pipe_buffer->ring_buffer_ != nullptr);
                        assert(pipe_buffer->use_ring_buffer_);
                        processExportIsolateSpmPipebufferWithRingbuffer(tensor_id, stage_idx, pipe_buffer, buffer_idx);
                    }
                }

                // 处理入口all ring buffer的pipebuffer
                for (auto& [tensor_id, mp]: tensorId2EntryAllRingbufferPipebuffer) {
                    for (auto& [stage_idx, p]: mp) {
                        auto& [pipe_buffer, tag] = p;
                        assert(pipe_buffer->ring_buffer_ != nullptr);
                        assert(pipe_buffer->use_ring_buffer_);
                        processEntryAllRingbuffer(tensor_id, stage_idx, pipe_buffer, tag);
                    }
                }

                // 处理出口all ring buffer的pipebuffer
                for (auto& [tensor_id, mp]: tensorId2ExportAllRingbufferPipebuffer) {
                    for (auto& [stage_idx, p]: mp) {
                        auto& [pipe_buffer, tag] = p;
                        assert(pipe_buffer->ring_buffer_ != nullptr);
                        assert(pipe_buffer->use_ring_buffer_);
                        processExportAllRingbuffer(tensor_id, stage_idx, pipe_buffer, tag);
                    }
                }

                // tick各个阶段
                uint total_batch_processed = getTotalBatchProcessed();
                if(maxBatchOffset > total_batch_processed) {
                    for(uint i = 0;i < stages.size();i++) {
                        stages[i].tick();
                    }
                } else {
                    bool tag = true;
                    
                    LOG(PIPELINE_DEBUG_FLAG) << "所有batch处理结束，停止流水线执行。maxBatchOffset=" << maxBatchOffset << ", totalBatchProcessed=" << total_batch_processed;

                    for (auto& stage: stages) {
                        stage.disableRun();
                        tag &= stage.tick();
                    }
                    if (tag) {
                        stateChange(PipelineState::STATE_END);
                        return true;
                    }
                }

                break;
            }
        }
        
        return state == PipelineState::STATE_END;
    }

    std::string Pipeline::getPerformance() {
        std::stringstream ss;
        uint subBatchNum = getTotalBatchProcessed() /  getSubBatchSize();
        ss << "pipeline performance: pipelineStartTime=" << pipelineStartTime 
            << "\n finishConfigTime=" << finishConfigTime << ", duration=" << finishConfigTime - finishConfigTime
            << "\n finishStageConfigTime=" << finishStageConfigTime << ", duration=" << finishStageConfigTime - finishConfigTime
            << "\n finishPipebufferCreateTime=" << finishPipebufferCreateTime << ", duration=" << finishPipebufferCreateTime - finishStageConfigTime
            << "\n finishPipebufferConfigTime=" << finishPipebufferConfigTime << ", duration=" << finishPipebufferConfigTime - finishPipebufferCreateTime
            << "\n";

        ss << "group by subBatch:\n";
        
        for (uint i = 0;i < subBatchNum;++ i) {
            ss << "subBatch " << i << ", stageidx-[preWait, duration, sucWait, total], pipeline:[startTime, endTime, total]\n";
            for(uint stageIdx = 0;stageIdx < stages.size();++ stageIdx) {
                auto& stage = stages[stageIdx];
                stage.generatePerformance();
                ss << stageIdx << "-[" << stage.preWait[i] << "," << stage.duration[i]  << "," << stage.sucWait[i] << "," 
                                        << stage.preWait[i] + stage.duration[i] + stage.sucWait[i] << "]\n";
            }
            ss << "pipeline:[" << startTime[i] << ", " << endTime[i] << ", " << endTime[i] - startTime[i] << "]" << "\n";
        }

        ss << "stage performance:\n";
        for (uint stageIdx = 0; stageIdx < stages.size();++ stageIdx) {
            assert(stages[stageIdx].startTime.size() == stages[stageIdx].endTime.size());
            assert(stages[stageIdx].startTime.size() == subBatchNum);
            ss << "stage[" << stageIdx << "]\n" << stages[stageIdx].getPerformance() << "\n";
        }

        return ss.str();
    }

    void Pipeline::createPipebuffer() {
        const auto& segment_mapping = action_->segment_mapping_;

        pipebuffer_set.clear();
        pipebuffer_set.resize(segment_mapping->stage_mapping_vec.size());

        for (uint stage_idx = 0;stage_idx < segment_mapping->stage_mapping_vec.size();++ stage_idx) {
            const auto& stage_mapping = segment_mapping->stage_mapping_vec[stage_idx];
            std::vector<std::pair<TensorId, bool>> tmp_vec; // tensor_id, is entry
            for (auto tensor_id: stage_mapping.entryTensorId) {
                tmp_vec.emplace_back(tensor_id, true);
            }
            for (auto tensor_id: stage_mapping.exportTensorId) {
                tmp_vec.emplace_back(tensor_id, false);
            }

            auto& tensorid_to_pipebuffer = pipebuffer_set[stage_idx];

            for (auto [tensor_id, is_entry]: tmp_vec) {
                auto tensor_type = stage_mapping.getTensorStayType(tensor_id);
                auto it = tensorid_to_pipebuffer.find(tensor_id);
                assert(it == tensorid_to_pipebuffer.end());
                tensorid_to_pipebuffer.emplace(tensor_id, PipelineBuffer(tensor_id, 
                    stage_mapping.getTensorStayType(tensor_id), 
                    stage_mapping.isDoubleBuffer(tensor_id), 
                    stage_mapping.tensorId2Strides.at(tensor_id), 
                    stage_mapping.tensorId2Shape.at(tensor_id),
                    stage_idx));
                auto& pipe_buffer = tensorid_to_pipebuffer.at(tensor_id);
                // spm pages
                {
                    auto it = action_->spm_source_.in_stage_spm_pages_[stage_idx].find(tensor_id);
                    if (tensor_type != StageMapping::TensorStayType::ALL_RINGBUFFER) {
                        assert(it != action_->spm_source_.in_stage_spm_pages_[stage_idx].end());
                        if (pipe_buffer.withDoubleBuffer) {
                            assert(it->second.size() == 2);
                            pipe_buffer.pSpmPages[1] = it->second[1];
                        } else {
                            assert(it->second.size() == 1);
                            pipe_buffer.pSpmPages[1] = std::make_shared<PageSet>();
                        }
                        pipe_buffer.pSpmPages[0] = it->second[0];
                    } else {
                        pipe_buffer.pSpmPages[0] = std::make_shared<PageSet>();
                        pipe_buffer.pSpmPages[1] = std::make_shared<PageSet>();
                    }
                }
                // dst dram addr
                {
                    if (!is_entry) {
                        if (tensor_type == StageMapping::TensorStayType::DRAM || tensor_type == StageMapping::TensorStayType::DRAM_DEPEN) {
                            pipe_buffer.dramBaseAddr = stage_mapping.tensorId2DramBaseAddr.at(tensor_id);
                        } else {
                            assert(tensor_type == StageMapping::TensorStayType::SHARED_SPM || tensor_type == StageMapping::TensorStayType::ISOLATE_SPM || tensor_type == StageMapping::TensorStayType::ALL_RINGBUFFER);
                        }
                    }
                }
            }
        }
        for (uint stage_idx = 0;stage_idx < segment_mapping->stage_mapping_vec.size();++ stage_idx) {
            const auto& stage_mapping = segment_mapping->stage_mapping_vec[stage_idx];
            std::vector<std::pair<TensorId, bool>> tmp_vec; // tensor_id, is entry
            for (auto tensor_id: stage_mapping.entryTensorId) {
                tmp_vec.emplace_back(tensor_id, true);
            }
            for (auto tensor_id: stage_mapping.exportTensorId) {
                tmp_vec.emplace_back(tensor_id, false);
            }
            // index
            for (auto [tensor_id, is_entry]: tmp_vec) {
                auto tensor_type = stage_mapping.getTensorStayType(tensor_id);
                auto& pipe_buffer = pipebuffer_set[stage_idx].at(tensor_id);
                if (tensor_type == StageMapping::TensorStayType::DRAM || tensor_type == StageMapping::TensorStayType::DRAM_DEPEN) {
                    auto& idx_mp_ptr = is_entry ? tensorId2EntryDramOrDramDepenPipeBuffer : tensorId2ExportDramOrDramDepenPipeBuffer;
                    idx_mp_ptr[tensor_id].emplace(stage_idx, std::make_pair(&pipe_buffer, 0));
                } else if (tensor_type == StageMapping::TensorStayType::ISOLATE_SPM) {
                    if (!is_entry) {
                        if (segment_mapping->ring_buffer_config_[tensor_id].use_) {
                            tensorId2ExportIsolatePipeBufferWithRingbuffer[tensor_id][stage_idx] = std::make_pair(&pipe_buffer, 0);
                        } else {
                            // 遍历每一个对应的入口张量
                            for (auto nxt_stage_idx: segment_mapping->entryTensorToStageIdx[tensor_id]) {
                                tensorId2IsolatePipeBufferNoRingbuffer[tensor_id][nxt_stage_idx] = std::make_tuple(&pipe_buffer, &(pipebuffer_set[nxt_stage_idx].at(tensor_id)), 0, 0);
                            }
                        }
                    } else {
                        if (segment_mapping->ring_buffer_config_[tensor_id].use_) {
                            tensorId2EntryIsolatePipeBufferWithRingbuffer[tensor_id][stage_idx] = std::make_pair(&pipe_buffer, 0);
                        } 
                    }
                } else if (tensor_type == StageMapping::TensorStayType::SHARED_SPM) {
                    if (!is_entry) {
                        // 遍历每一个对应的入口张量
                        for (auto nxt_stage_idx: segment_mapping->entryTensorToStageIdx[tensor_id]) {
                            for (uint i = 0;i < 2;++ i) {
                                tensorId2SharedPipeBuffer[tensor_id][i][nxt_stage_idx] = std::make_tuple(&pipe_buffer, &(pipebuffer_set[nxt_stage_idx].at(tensor_id)), 0);
                            }
                        }
                    }
                } else if (tensor_type == StageMapping::TensorStayType::ALL_RINGBUFFER) {
                    if (is_entry) {
                        tensorId2EntryAllRingbufferPipebuffer[tensor_id][stage_idx] = std::make_pair(&pipe_buffer, false);
                    } else {
                        tensorId2ExportAllRingbufferPipebuffer[tensor_id][stage_idx] = std::make_pair(&pipe_buffer, false);
                    }
                } else {
                    assert(false);
                }
            }
        }

        if (DebugLogger::isEnabled(PIPELINE_DEBUG_FLAG)) {
            LOG(PIPELINE_DEBUG_FLAG) << "[pipeLogIdx-" << pipeLogIdx << "]" << "Pipeline::CreatePipebuffer: 完成pipebuffer创建。打印pipebuffer状态。\n" << pipebufferToString();
            LOG(PIPELINE_DEBUG_FLAG) << "[pipeLogIdx-" << pipeLogIdx << "]" << "Pipeline::CreatePipebuffer: 打印pipeline buffer索引。\n" << pipebufferIndexToString(false);
        }
    }
    void Pipeline::createRingBuffer() {
        const auto& pipeline_mapping = action_->segment_mapping_;
        for (const auto& [tensor_id, ring_buffer_config]: pipeline_mapping->ring_buffer_config_) {
            if (!ring_buffer_config.use_) continue;

            if (tensorId2EntryDramOrDramDepenPipeBuffer.find(tensor_id) != tensorId2EntryDramOrDramDepenPipeBuffer.end() || 
                tensorId2ExportDramOrDramDepenPipeBuffer.find(tensor_id) != tensorId2ExportDramOrDramDepenPipeBuffer.end()) {
                // 创建dram ring buffer
                std::vector<std::pair<uint, uint>> dram_addr_range;
                for (int i = 0;i < ring_buffer_config.size_;i ++) {
                    const auto& p = RT.dram_addr_manager_.getAvailableInterval(pipeline_mapping->tensor_id_to_min_shape[tensor_id]);
                    if (p.first < 0) {
                        assert(false);
                    }
                    dram_addr_range.push_back(p);
                }
                assert(ring_buffer_.find(tensor_id) == ring_buffer_.end());
                ring_buffer_.emplace(tensor_id, RingBuffer(ring_buffer_config.size_, ring_buffer_config.out_degree_, 
                                                            pipeline_mapping->tensor_id_to_min_strides[tensor_id], pipeline_mapping->tensor_id_to_min_shape[tensor_id], dram_addr_range));

            } else if (tensorId2IsolatePipeBufferNoRingbuffer.find(tensor_id) != tensorId2IsolatePipeBufferNoRingbuffer.end() || 
                tensorId2SharedPipeBuffer.find(tensor_id) != tensorId2SharedPipeBuffer.end() || 
                tensorId2EntryIsolatePipeBufferWithRingbuffer.find(tensor_id) != tensorId2EntryIsolatePipeBufferWithRingbuffer.end() ||
                tensorId2ExportIsolatePipeBufferWithRingbuffer.find(tensor_id) != tensorId2ExportIsolatePipeBufferWithRingbuffer.end()) {
                // 创建spm ring buffer, 使用min shape
                assert(ring_buffer_.find(tensor_id) == ring_buffer_.end());
                ring_buffer_.emplace(tensor_id, RingBuffer(ring_buffer_config.size_, ring_buffer_config.out_degree_, 
                                                            pipeline_mapping->tensor_id_to_min_strides[tensor_id], pipeline_mapping->tensor_id_to_min_shape[tensor_id], action_->spm_source_.ring_buffer_spm_pages_[tensor_id]));
            
            } else if (tensorId2EntryAllRingbufferPipebuffer.find(tensor_id) != tensorId2EntryAllRingbufferPipebuffer.end() || tensorId2ExportAllRingbufferPipebuffer.find(tensor_id) != tensorId2ExportAllRingbufferPipebuffer.end()) {
                // 创建spm ring buffer，使用max shape
                assert(ring_buffer_.find(tensor_id) == ring_buffer_.end());
                ring_buffer_.emplace(tensor_id, RingBuffer(ring_buffer_config.size_, ring_buffer_config.out_degree_, 
                                                            pipeline_mapping->tensor_id_to_max_strides[tensor_id], pipeline_mapping->tensor_id_to_max_shape[tensor_id], action_->spm_source_.ring_buffer_spm_pages_[tensor_id]));
            } else  {
                // 其它情况不应该有ring buffer
                assert(false);
            }   
        }
        for (uint stage_idx = 0;stage_idx < pipebuffer_set.size();++ stage_idx) {
            for (auto& [tensor_id, pipe_buffer]: pipebuffer_set[stage_idx]) {
                assert(pipe_buffer.ring_buffer_ == nullptr);
                if (ring_buffer_.find(tensor_id) == ring_buffer_.end()) continue;
                pipe_buffer.ring_buffer_ = &(ring_buffer_.at(tensor_id));
                pipe_buffer.use_ring_buffer_ = true;
            }
        }

        if (DebugLogger::isEnabled(PIPELINE_DEBUG_FLAG)) {
            LOG(PIPELINE_DEBUG_FLAG) << "[pipeLogIdx-" << pipeLogIdx << "]" << "Pipeline::createRingBuffer: 完成ringbuffer创建。打印ringbuffer状态。\n" << ringBufferToString();
        }
    }

    void Pipeline::createStages() {
        const auto& segment_mapping = action_->segment_mapping_;
        const auto& stage_mapping_vec = segment_mapping->stage_mapping_vec;
        assert(stages.empty());
        stages.reserve(stage_mapping_vec.size());
        for(uint stage_idx = 0;stage_idx < stage_mapping_vec.size();++ stage_idx) {
            StageConfig stage_config;
            stage_config.model_ = action_->model;
            stage_config.stage_mapping_ = &stage_mapping_vec[stage_idx];
            stage_config.subbatch_size_ = segment_mapping->subBatchSize;
            stage_config.acc_ = action_->acc_source_.acc_ids_[stage_idx];
            stage_config.tensor_spm_ppages_ = action_->spm_source_.in_stage_spm_pages_[stage_idx];
            for (auto [tensor_id, weight_page_set]: action_->spm_source_.weight_spm_pages) {
                assert(stage_config.tensor_spm_ppages_[tensor_id].empty());
                stage_config.tensor_spm_ppages_[tensor_id].push_back(weight_page_set);
            }
            
            stage_config.pipeline_buffers_.clear();
            stage_config.pipeline_buffers_.reserve(pipebuffer_set[stage_idx].size());
            for (auto& [tensor_id, pipebuffer_shared]: pipebuffer_set[stage_idx]) {
                stage_config.pipeline_buffers_.push_back(&pipebuffer_shared);
            }

            stages.emplace_back(pipeLogIdx * 10000 + stage_idx, stage_config);
        }
        
        LOG(PIPELINE_DEBUG_FLAG) << "[pipeLogIdx-" << pipeLogIdx << "]" << "PipelineState::STATE_STAGE_CONFIG: stage配置完成." << std::endl;
    }

    void Pipeline::processEntryDramPipebuffer(TensorId tensor_id, uint stage_idx, PipelineBuffer* pipe_buffer, uint& buffer_idx) {
        auto print_head_to_string = [&]() {
            std::ostringstream ss;
            ss << "[pipeLogIdx-" << pipeLogIdx << "]" << "PipelineState::processEntryDramPipebuffer: 检查入口DRAM/DRAM_DEPEN类型pipe buffer.";
            return ss.str();
        };
        auto print_base_to_string = [&]() {
            std::ostringstream ss;
            ss << "totalCycles=" << RT.getCycles() 
                << ", stage_idx=" << stage_idx
                << ", tensor_id=" << tensor_id 
                << ", buffer_idx=" << buffer_idx 
                << ", subBatchOffset=" << pipe_buffer->subBatchOffset
                << ", subBatchSize=" << getSubBatchSize()
                << ", pipe_buffer->cmdRunning[buffer_idx]=" << pipe_buffer->cmdRunning[buffer_idx] 
                << ", pipe_buffer->fetchFlushSendCmdCounts[buffer_idx]=" << pipe_buffer->fetchFlushSendCmdCounts[buffer_idx];
            return ss.str();
        };
        
        if(pipe_buffer->cmdRunning[buffer_idx]) {
            // 有指令执行，检查指令计数是否为0。不为0，则继续等待指令执行完成；为0，更新pipeBuffer状态
            if(pipe_buffer->fetchFlushSendCmdCounts[buffer_idx] > 0) return;
            assert(pipe_buffer->fetchFlushSendCmdCounts[buffer_idx] == 0);

            // 执行结束，状态转移
            LOG(PIPELINE_DEBUG_FLAG) << print_head_to_string();
            pipe_buffer->cmdRunning[buffer_idx] = false;
            assert(!pipe_buffer->bufferHasFullData[buffer_idx]);
            pipe_buffer->bufferHasFullData[buffer_idx] = true;

            // 标记ring buffer使用
            if (pipe_buffer->use_ring_buffer_) {
                pipe_buffer->ring_buffer_->use(pipe_buffer->subBatchOffset);
            }

            // 增加offset
            ++ pipe_buffer->subBatchOffset;

            // 为了避免页表膨胀，手动清空页表
            for(uint vPage = pipe_buffer->cmdVPageRange[buffer_idx].first;vPage <= pipe_buffer->cmdVPageRange[buffer_idx].second;++ vPage) {
                RT.invalidPageTable(pipe_buffer->cmdAcc[buffer_idx], vPage);
            }
            LOG(PIPELINE_DEBUG_FLAG) << "入口DRAM/DRAM_Depen类型pipeBuffer有fetch指令执行结束。"
                    << print_base_to_string()
                    << ", vPage=[" << pipe_buffer->cmdVPageRange[buffer_idx].first 
                    << "," << pipe_buffer->cmdVPageRange[buffer_idx].second << "]" 
                    << ", cmdAcc=" << pipe_buffer->cmdAcc[buffer_idx];
            // 缓冲区切换
            if(pipe_buffer->withDoubleBuffer) {
                buffer_idx = (buffer_idx + 1) & 1; // 等价于(buffer_idx + 1) % 2
            }
            LOG(PIPELINE_DEBUG_FLAG) << "缓冲区切换至buffer_idx=" << buffer_idx;
            LOG(PIPELINE_DEBUG_FLAG) << "更新后pipebuffer状态：" << "buffer_idx=" << buffer_idx << ", pipeBuffer: \n" << pipe_buffer->toString() <<  std::endl;
        } else if (pipe_buffer->bufferHasFullData[buffer_idx] == false) {
            // 没有指令执行，且缓冲区为空
            uint64 dram_base_addr;
            if (pipe_buffer->tensorStayType == StageMapping::TensorStayType::DRAM) {
                // 如果是DRAM类型，等待batch足够后发起指令
                if(static_cast<int>(maxBatchOffset/getSubBatchSize()) - static_cast<int>(pipe_buffer->subBatchOffset) < 1) {
                    return;
                }
                if (startTime.size() == pipe_buffer->subBatchOffset) {
                    startTime.push_back(RT.getCycles());
                }
                dram_base_addr = pipe_buffer->dramBaseAddr[buffer_idx];
            } else if (pipe_buffer->tensorStayType == StageMapping::TensorStayType::DRAM_DEPEN) {
                // 如果是DRAM_DEPEN类型，必定有dram类型的ring buffer。通过ring buffer检查前序张量是否有效
                assert(pipe_buffer->ring_buffer_ != nullptr);
                assert(pipe_buffer->use_ring_buffer_);

                if (!pipe_buffer->ring_buffer_->hasReadyBuffer(pipe_buffer->subBatchOffset)) {
                    return;
                }

                dram_base_addr = pipe_buffer->ring_buffer_->getDramBaseAddr(pipe_buffer->subBatchOffset);
            } else {
                assert(false);
            }

            LOG(PIPELINE_DEBUG_FLAG) << print_head_to_string();

            LOG(PIPELINE_DEBUG_FLAG) << "入口DRAM/DRAM_DEPEN类型pipeBuffer有足够batch执行，准备发送指令.\n"
                        << print_base_to_string()
                        << ", dram_base_addr=" << dram_base_addr
                        << ", shape=" << mudnac::toString(pipe_buffer->shape)
                        << ", strides=" << mudnac::toString(pipe_buffer->strides);

            // 设置页表映射
            const auto& spmPages = *(pipe_buffer->pSpmPages[buffer_idx]);
            uint accId = action_->acc_source_.all_acc_ids_[RT.GetMinimumAccIdx(action_->acc_source_.all_acc_ids_, RT.accelFetchCmdCount)];
            auto [startVPage, endVPage] = RT.spm_vpage_manager_[accId].getAvailableInterval(spmPages.size());
            assert(endVPage - startVPage + 1 == spmPages.size());
            pipe_buffer->cmdAcc[buffer_idx] = accId;
            for(uint vPage = startVPage;vPage <= endVPage;++ vPage) {
                RT.setPageTable(accId, vPage, spmPages[vPage - startVPage]);
            }
            pipe_buffer->cmdVPageRange[buffer_idx] = {startVPage, endVPage};

            // 发送指令，设置指令计数，修改状态
            sendFetchCmd(accId, tensor_id, stage_idx, dram_base_addr, pipe_buffer->strides, pipe_buffer->shape, startVPage);  
            pipe_buffer->cmdRunning[buffer_idx] = true;
            assert(pipe_buffer->fetchFlushSendCmdCounts[buffer_idx] == 0); // entry dram/dram_depen类型不应该同时执行多个fetch指令
            pipe_buffer->fetchFlushSendCmdCounts[buffer_idx] += 1;

            LOG(PIPELINE_DEBUG_FLAG) << "页表设置.pipe_buffer->cmdAcc[buffer_idx]=" << pipe_buffer->cmdAcc[buffer_idx] << ", vPage=[" << startVPage << "," << endVPage << "], spmPages.front()=" << spmPages.front() << ", spmPages.back()=" << spmPages.back();
            LOG(PIPELINE_DEBUG_FLAG) << "更新后状态：" << "buffer_idx=" << buffer_idx << ", pipe_buffer: " << pipe_buffer->toString() << std::endl;
        } else {
            // 已经有数据，等待数据被消费
            return;
        }
        return;
    }
    void Pipeline::processExportDramPipebuffer(TensorId tensor_id, uint stage_idx, PipelineBuffer* pipe_buffer, uint& buffer_idx) {
        auto print_head_to_string = [&]() {
            std::ostringstream ss;
            ss << "[pipeLogIdx-" << pipeLogIdx << "]" << "PipelineState::processExportDramPipebuffer: 检查出口DRAM/DRAM_DEPEN类型pipe buffer.";
            return ss.str();
        };
        auto print_base_to_string = [&]() {
            std::ostringstream ss;
            ss << "totalCycles=" << RT.getCycles() 
                << ", stage_idx=" << stage_idx
                << ", tensor_id=" << tensor_id 
                << ", buffer_idx=" << buffer_idx 
                << ", subBatchOffset=" << pipe_buffer->subBatchOffset
                << ", subBatchSize=" << getSubBatchSize()
                << ", pipe_buffer->cmdRunning[buffer_idx]=" << pipe_buffer->cmdRunning[buffer_idx] 
                << ", pipe_buffer->fetchFlushSendCmdCounts[buffer_idx]=" << pipe_buffer->fetchFlushSendCmdCounts[buffer_idx];
            return ss.str();
        };
        
        if(pipe_buffer->cmdRunning[buffer_idx]) {
            // 有指令执行，检查指令计数是否为0
            if(pipe_buffer->fetchFlushSendCmdCounts[buffer_idx] > 0) return;
            assert(pipe_buffer->fetchFlushSendCmdCounts[buffer_idx] == 0);

            // 所有指令执行结束，状态转移
            LOG(PIPELINE_DEBUG_FLAG) << print_head_to_string();
            pipe_buffer->cmdRunning[buffer_idx] = false;
            assert(pipe_buffer->bufferHasFullData[buffer_idx]);
            pipe_buffer->bufferHasFullData[buffer_idx] = false;

            // 标记ring buffer填充完成
            if (pipe_buffer->use_ring_buffer_) {
                pipe_buffer->ring_buffer_->fill(pipe_buffer->subBatchOffset);
            }

            // 更新batch计数
            if (endTime.size() == pipe_buffer->subBatchOffset) {
                endTime.push_back(0);
            }
            endTime[pipe_buffer->subBatchOffset] = std::max<uint64>(RT.getCycles(), endTime[pipe_buffer->subBatchOffset]);
            ++ pipe_buffer->subBatchOffset;
            
            // 为了避免页表膨胀，手动清空页表
            for(uint vPage = pipe_buffer->cmdVPageRange[buffer_idx].first;vPage <= pipe_buffer->cmdVPageRange[buffer_idx].second;++ vPage) {
                RT.invalidPageTable(pipe_buffer->cmdAcc[buffer_idx], vPage);
            }

            LOG(PIPELINE_DEBUG_FLAG) << print_head_to_string();
            LOG(PIPELINE_DEBUG_FLAG) << "出口DRAMbuffer有flush指令执行结束."
                    << print_base_to_string()
                    << ", pipe_buffer->cmdAcc[buffer_idx]=" << pipe_buffer->cmdAcc[buffer_idx] 
                    << ", vPage=[" << pipe_buffer->cmdVPageRange[buffer_idx].first 
                    << "," << pipe_buffer->cmdVPageRange[buffer_idx].second << "]" ;
            // 缓冲区切换
            if(pipe_buffer->withDoubleBuffer) {
                buffer_idx = (buffer_idx + 1) & 1; // 等价于(bufferIdx + 1) % 2
            }
            LOG(PIPELINE_DEBUG_FLAG) << "缓冲区切换至buffer_idx=" << buffer_idx << std::endl;
            LOG(PIPELINE_DEBUG_FLAG) << "更新后pipebuffer状态：" << "buffer_idx=" << buffer_idx << ", pipe_buffer: \n" << pipe_buffer->toString();
        } else if(pipe_buffer->bufferHasFullData[buffer_idx]) {
            // 有完整数据可以发送
            uint64 dram_base_addr;
            if (pipe_buffer->tensorStayType == StageMapping::TensorStayType::DRAM) {
                // DRAM类型，直接flush即可
                dram_base_addr = pipe_buffer->dramBaseAddr[buffer_idx];
            } else if (pipe_buffer->tensorStayType == StageMapping::TensorStayType::DRAM_DEPEN) {
                // DRAM_DEPEN类型，需要等待ring buffer空闲
                if (!pipe_buffer->ring_buffer_->hasIdleBuffer()) {
                    return;
                }
                dram_base_addr = pipe_buffer->ring_buffer_->getDramBaseAddr(pipe_buffer->subBatchOffset);
            }
            LOG(PIPELINE_DEBUG_FLAG) << print_head_to_string();
            LOG(PIPELINE_DEBUG_FLAG) << "带ring_buffer的出口DRAM/DRAM_DEPEN类型pipeBuffer有完整数据可以发送." 
                                     << print_base_to_string()
                                     << ", dram_base_addr=" << dram_base_addr
                                     << ", strides=" << mudnac::toString(pipe_buffer->strides)
                                     << ", shape=" << mudnac::toString(pipe_buffer->shape);


            auto& spmPages = *(pipe_buffer->pSpmPages[buffer_idx]);
            uint accId = action_->acc_source_.all_acc_ids_[RT.GetMinimumAccIdx(action_->acc_source_.all_acc_ids_, RT.accelFlushCmdCount)];
            auto [startVPage, endVPage] = RT.spm_vpage_manager_[accId].getAvailableInterval(spmPages.size());
            assert(endVPage - startVPage + 1 == spmPages.size());
            pipe_buffer->cmdAcc[buffer_idx] = accId;
            for(uint vPage = startVPage;vPage <= endVPage;++ vPage) {
                RT.setPageTable(accId, vPage, spmPages[vPage - startVPage]);
            }
            pipe_buffer->cmdVPageRange[buffer_idx] = {startVPage, endVPage};

            // 发送指令，设置指令计数，修改状态
            sendFlushCmd(accId, tensor_id, stage_idx, dram_base_addr, pipe_buffer->strides, pipe_buffer->shape, startVPage);
            assert(pipe_buffer->cmdRunning[buffer_idx] == false);
            pipe_buffer->cmdRunning[buffer_idx] = true;
            assert(pipe_buffer->fetchFlushSendCmdCounts[buffer_idx] == 0); // export dram/dram_depen类型张量同一时间只能有一条flush指令执行
            ++ pipe_buffer->fetchFlushSendCmdCounts[buffer_idx];

            LOG(PIPELINE_DEBUG_FLAG) << "页表设置.pipe_buffer->cmdAcc[buffer_idx]=" << pipe_buffer->cmdAcc[buffer_idx] << ", vPage=[" << startVPage << "," << endVPage << "], spmPages.front()=" << spmPages.front() << ", spmPages->back()=" << spmPages.back() << std::endl;
            LOG(PIPELINE_DEBUG_FLAG) << "更新后状态：" << "buffer_idx=" << buffer_idx << ", pipe_buffer: \n" << pipe_buffer->toString() << std::endl;
        }
        else {
            // empty，等待数据填充完成
        }
    }
    void Pipeline::processIsolateSpmPipebufferWithoutRingbuffer(TensorId tensor_id, PipelineBuffer* pre_pipe_buffer, uint& pre_buffer_idx, uint nxt_stage_idx, PipelineBuffer* nxt_pipe_buffer, uint& nxt_buffer_idx) {
        auto print_head_to_string = [&]() {
            std::ostringstream ss;
            ss << "[pipeLogIdx-" << pipeLogIdx << "]" << "PipelineState::processIsolateSpmPipebufferWithoutRingbuffer: 检查无ringbuffer的ISOLATE SPM类型pipe buffer状态.";
            return ss.str();
        };
        auto print_base_to_string = [&]() {
            std::ostringstream ss;
            ss << "totalCycles=" << RT.getCycles() 
                << ", tensor_id=" << tensor_id
                << "\n"
                << ", pre_stage_idx=" << action_->segment_mapping_->exportTensorToStageIdx[tensor_id][0]
                << ", pre_buffer_idx=" << pre_buffer_idx
                << ", pre_pipe_buffer->cmdRunning[pre_buffer_idx]=" << pre_pipe_buffer->cmdRunning[pre_buffer_idx] 
                << ", pre_pipe_buffer->fetchFlushSendCmdCounts[pre_buffer_idx]=" << pre_pipe_buffer->fetchFlushSendCmdCounts[pre_buffer_idx]
                << ", pre_pipe_buffer->subBatchOffset=" << pre_pipe_buffer->subBatchOffset
                << "\n"
                << ", nxt_stage_idx=" << nxt_stage_idx
                << ", nxt_buffer_idx=" << nxt_buffer_idx
                << ", nxt_pipe_buffer->cmdRunning[nxt_buffer_idx]=" << nxt_pipe_buffer->cmdRunning[nxt_buffer_idx] 
                << ", nxt_pipe_buffer->fetchFlushSendCmdCounts[nxt_buffer_idx]=" << nxt_pipe_buffer->fetchFlushSendCmdCounts[nxt_buffer_idx]
                << ", nxt_pipe_buffer->subBatchOffset=" << nxt_pipe_buffer->subBatchOffset;
            return ss.str();
        };

        if (nxt_pipe_buffer->cmdRunning[nxt_buffer_idx]) {
            assert(pre_pipe_buffer->cmdRunning[pre_buffer_idx]);
            // 有指令执行，检查指令计数是否为0
            if (nxt_pipe_buffer->fetchFlushSendCmdCounts[nxt_buffer_idx] > 0) return;

            // 执行结束，状态转移
            LOG(PIPELINE_DEBUG_FLAG) << print_head_to_string();
            assert(pre_pipe_buffer->fetchFlushSendCmdCounts[pre_buffer_idx] >= 0);
            nxt_pipe_buffer->cmdRunning[nxt_buffer_idx] = false;
            assert(!nxt_pipe_buffer->bufferHasFullData[nxt_buffer_idx]);
            nxt_pipe_buffer->bufferHasFullData[nxt_buffer_idx] = true;
            
            // 清理src页表，仅当src没有未回收指令时
            if (pre_pipe_buffer->fetchFlushSendCmdCounts[pre_buffer_idx] == 0) {
                for(uint vPage = pre_pipe_buffer->cmdVPageRange[pre_buffer_idx].first;vPage <= pre_pipe_buffer->cmdVPageRange[pre_buffer_idx].second;++ vPage) {
                    RT.invalidPageTable(pre_pipe_buffer->cmdAcc[pre_buffer_idx], vPage);
                }
            }

            // 清理dst页表
            for(uint vPage = nxt_pipe_buffer->cmdVPageRange[nxt_buffer_idx].first;vPage <= nxt_pipe_buffer->cmdVPageRange[nxt_buffer_idx].second;++ vPage) {
                RT.invalidPageTable(nxt_pipe_buffer->cmdAcc[nxt_buffer_idx], vPage);
            }

            // 执行结束，nxtPipeBuffer收到新数据，计数加subbatch
            ++ nxt_pipe_buffer->subBatchOffset;
            LOG(PIPELINE_DEBUG_FLAG) << "Isolate SPM类型有send指令执行结束。" << print_base_to_string()
                    << "\npreVPage=[" << pre_pipe_buffer->cmdVPageRange[pre_buffer_idx].first 
                    << "," << pre_pipe_buffer->cmdVPageRange[pre_buffer_idx].second << "]" 
                    << ", nxtVPage=[" << nxt_pipe_buffer->cmdVPageRange[nxt_buffer_idx].first 
                    << "," << nxt_pipe_buffer->cmdVPageRange[nxt_buffer_idx].second << "]";
            LOG(PIPELINE_DEBUG_FLAG) << "更新后pre_buffer状态：" 
                    << "pre_buffer_idx=" << pre_buffer_idx  << ", pre_pipe_buffer: \n" << pre_pipe_buffer->toString();
            LOG(PIPELINE_DEBUG_FLAG) << "更新后nxt_buffer状态：" 
                    << "nxt_buffer_idx=" << nxt_buffer_idx  << ", nxt_pipe_buffer: \n" << nxt_pipe_buffer->toString() << std::endl;
        } else if(pre_pipe_buffer->bufferHasFullData[pre_buffer_idx] && !nxt_pipe_buffer->bufferHasFullData[nxt_buffer_idx]) {
            // 有完整数据可发送，且接收方buffer空闲
            // 偏移量相同时才能发。不相同，在single buffer时不应该出现；在double buffer时，意味着要发送的buffer idx还在被使用中，没有释放，应该继续等待其释放
            if (pre_pipe_buffer->subBatchOffset != nxt_pipe_buffer->subBatchOffset) {
                assert(pre_pipe_buffer->subBatchOffset + 1 == nxt_pipe_buffer->subBatchOffset);
                return;
            }
            LOG(PIPELINE_DEBUG_FLAG) << print_head_to_string();
            LOG(PIPELINE_DEBUG_FLAG) << "ISOLATE SPM类型pipeBuffer有完整数据可以发送." << print_base_to_string();

            const auto& srcSpmPages = pre_pipe_buffer->pSpmPages[pre_buffer_idx];
            const auto& dstSpmPages = nxt_pipe_buffer->pSpmPages[nxt_buffer_idx]; 
            uint spmPageNum = std::min(srcSpmPages->size(), dstSpmPages->size());
            // assert(spmPageNum < dstSpmPages.size());
            uint accId = action_->acc_source_.all_acc_ids_[RT.GetMinimumAccIdx(action_->acc_source_.all_acc_ids_, RT.accelSendCmdCount)];
            auto [startVPage, endVPage] = RT.spm_vpage_manager_[accId].getAvailableInterval(spmPageNum);
            assert(endVPage - startVPage + 1 == spmPageNum);
            pre_pipe_buffer->cmdAcc[pre_buffer_idx] = accId;
            for(uint vPage = startVPage;vPage <= endVPage;++ vPage) {
                RT.setPageTable(accId, vPage, srcSpmPages->at(vPage - startVPage));
            }
            pre_pipe_buffer->cmdVPageRange[pre_buffer_idx] = {startVPage, endVPage};
            uint srcSpmPageBase = startVPage;
            LOG(PIPELINE_DEBUG_FLAG) << "pre_pipe_buffer页表设置.pre_pipe_buffer->cmdAcc[pre_buffer_idx]=" << pre_pipe_buffer->cmdAcc[pre_buffer_idx] << ", vPage=[" << startVPage << "," << endVPage << "], srcSpmPages.front()=" << srcSpmPages->front() << ", srcSpmPages.back()=" << srcSpmPages->back() << std::endl;
            
            std::tie(startVPage, endVPage) = RT.spm_vpage_manager_[accId].getAvailableInterval(spmPageNum);
            assert(endVPage - startVPage + 1 == spmPageNum);
            nxt_pipe_buffer->cmdAcc[nxt_buffer_idx] = accId;
            for(uint vPage = startVPage;vPage <= endVPage;++ vPage) {
                RT.setPageTable(accId, vPage, dstSpmPages->at(vPage - startVPage));
            }
            nxt_pipe_buffer->cmdVPageRange[nxt_buffer_idx] = {startVPage, endVPage};
            uint dstSpmPageBase = startVPage;
            LOG(PIPELINE_DEBUG_FLAG) << "nxt_pipe_buffer页表设置.prePipeBuffer->cmdAcc[nxtBufferIdx]=" << nxt_pipe_buffer->cmdAcc[nxt_buffer_idx] << ", vPage=[" << startVPage << "," << endVPage << "], dstSpmPages.front()=" << dstSpmPages->front() << ", dstSpmPages.back()=" << dstSpmPages->back() << std::endl;

            // 发送指令，设置指令计数，修改状态。选择而这种相对较小的一个
            // 特判：input/output大小不同，则按照小的发送。对于被跳过的pooling层，无论是变小还是变大，对访存量的估计都是正确的；对padding，访存量估计也是正确的
            if (pre_pipe_buffer->pSpmPages[0]->size() <= nxt_pipe_buffer->pSpmPages[0]->size()) {
                sendSendCmd(pre_pipe_buffer->cmdAcc[pre_buffer_idx], tensor_id, nxt_stage_idx, pre_pipe_buffer->strides, pre_pipe_buffer->shape, srcSpmPageBase, dstSpmPageBase);
            } else {
                sendSendCmd(nxt_pipe_buffer->cmdAcc[nxt_buffer_idx], tensor_id, nxt_stage_idx, nxt_pipe_buffer->strides, nxt_pipe_buffer->shape, srcSpmPageBase, dstSpmPageBase);
            }
            pre_pipe_buffer->cmdRunning[pre_buffer_idx] = true;
            ++ pre_pipe_buffer->fetchFlushSendCmdCounts[pre_buffer_idx];

            assert(nxt_pipe_buffer->cmdRunning[nxt_buffer_idx] == false); // 目的pipebuffer同一时间只能接受一个pipebuffer的send指令
            assert(nxt_pipe_buffer->fetchFlushSendCmdCounts[nxt_buffer_idx] == 0);
            nxt_pipe_buffer->cmdRunning[nxt_buffer_idx] = true;
            ++ (nxt_pipe_buffer->fetchFlushSendCmdCounts[nxt_buffer_idx]);

            LOG(PIPELINE_DEBUG_FLAG) << "更新后pre buffer状态:" << "pre_buffer_idx=" << pre_buffer_idx << ", pre_pipe_buffer: \n" << pre_pipe_buffer->toString();
            LOG(PIPELINE_DEBUG_FLAG) << "更新后nxt buffer状态:" << "nxt_buffer_idx=" << nxt_buffer_idx << ", nxt_pipe_buffer: \n" << nxt_pipe_buffer->toString() << std::endl;
        }
    }
    void Pipeline::processIsolateSpmPipebufferWithoutRingbufferSyn(TensorId tensor_id, std::unordered_map<uint, std::tuple<PipelineBuffer*, PipelineBuffer*, uint, uint>>& p) {
        // 随便拿第一个pipeBuffer
        assert(p.size() >= 1);
        auto& [prePipeBuffer, _, preBufferIdx, _] = p.begin()->second;
        if (prePipeBuffer->cmdRunning[preBufferIdx] && prePipeBuffer->fetchFlushSendCmdCounts[preBufferIdx] == 0) {
            // 检索每一个后继stage，确保pipe buffer已经发送。如果不检查，可能出现后续pipebuffer没执行完前一个batch，当前batch的send指令还没发送到该pipe buffer，但在此处指令计数为0，误以为命令发送成功的情况
            bool tag = true;
            for (auto& [nxtStageIdx, t]: p) {
                auto& [prePipeBufferNew, nxtPipeBuffer, _, nxtBufferIdx] = t;
                if (prePipeBuffer->subBatchOffset == nxtPipeBuffer->subBatchOffset) {
                    tag = false;
                    break;
                }
                assert(nxtPipeBuffer->subBatchOffset - prePipeBuffer->subBatchOffset == 1);
            }
            if (tag) {
                // 确保所有prePipeBuffer都是同一个，且buffer idx相同
                LOG(PIPELINE_DEBUG_FLAG) << "[pipeLogIdx-" << pipeLogIdx << "]" << "出口buffer的所有对应入口均已接受数据。开始状态切换";
                for (auto& [nxtStageIdx, t]: p) {
                    auto& [prePipeBufferNew, nxtPipeBuffer, _, nxtBufferIdx] = t;
                    assert(prePipeBuffer == prePipeBufferNew);
                    assert(!nxtPipeBuffer->cmdRunning[nxtBufferIdx]);
                    if(nxtPipeBuffer->withDoubleBuffer) {
                        nxtBufferIdx = (nxtBufferIdx + 1) & 1; // 等价于(bufferIdx + 1) % 2
                    }
                    LOG(PIPELINE_DEBUG_FLAG) << "入口buffer缓冲区切换: tensor_id=" << tensor_id << ", nxtStageIdx=" << nxtStageIdx << ", 切换为nxtBufferIdx=" << nxtBufferIdx;
                }
                prePipeBuffer->cmdRunning[preBufferIdx] = false;
                assert(prePipeBuffer->bufferHasFullData[preBufferIdx]);
                prePipeBuffer->bufferHasFullData[preBufferIdx] = false;
                ++ prePipeBuffer->subBatchOffset;
                // 缓冲区切换
                if(prePipeBuffer->withDoubleBuffer) {
                    // 需要更新每一个监控对的prePipeBufferIdx
                    for (auto& [nxtStageIdx, t]: p) {
                        auto& [_, _, preBufferIdxNew, _] = t;
                        preBufferIdxNew = (preBufferIdxNew + 1) & 1; // 等价于(preBufferIdxNew + 1) % 2
                    }
                }
                LOG(PIPELINE_DEBUG_FLAG) << "出口buffer缓冲区切换: tensor_id=" << tensor_id << ", 切换为preBufferIdx=" << preBufferIdx << ", prePipeBuffer->subBatchOffset=" << prePipeBuffer->subBatchOffset << std::endl;
            }
        }
    }
    void Pipeline::processSharedSpmPipebufferWithoutRingbuffer(TensorId tensor_id, PipelineBuffer* pre_pipe_buffer, uint buffer_idx, bool pre_tag) {
        assert(tensorId2SharedPipeBuffer[tensor_id].size() >= 1);
        assert(pre_pipe_buffer->withDoubleBuffer);
        for (auto& [_, t]: tensorId2SharedPipeBuffer[tensor_id][buffer_idx]) {
            auto& [pre_pipe_buffer_new, nxt_pipe_buffer, nxt_tag] = t;
            assert(pre_pipe_buffer == pre_pipe_buffer_new);
            assert(pre_tag == nxt_tag);
            assert(nxt_pipe_buffer->withDoubleBuffer);
            if(!pre_pipe_buffer->bufferHasFullData[buffer_idx] || nxt_pipe_buffer->bufferHasFullData[buffer_idx]) {
                // 必须是full empty的情况，其它情况等待数据填充（从empty empty转移）或数据使用（从full full转移）
                return;
            }
        }

        LOG(PIPELINE_DEBUG_FLAG) << "[pipeLogIdx-" << pipeLogIdx << "]" << "PipelineState::processSharedSpmPipebufferWithoutRingbuffer: 检查Shared SPM类型pipe buffer状态.cycles=" << RT.getCycles();
        LOG(PIPELINE_DEBUG_FLAG) << "入口张量tensor_id=" << tensor_id << "与所有出口张量进入full,empty状态，开始状态转移.tag=" << pre_tag;

        // 转移：empty,empty ->(stage执行完) full, empty(tag=false) ->(pipeline通知) full, full ->(stage执行完) full, empty(tag=true)->(pipeline通知)empty, empty...
        // 关键：前一个状态是empty, empty还是full, full
        for (auto& [_, t]: tensorId2SharedPipeBuffer[tensor_id][buffer_idx]) {
            auto& [pre_pipe_buffer_new, nxt_pipe_buffer, nxt_tag] = t;
            assert(pre_pipe_buffer->bufferHasFullData[buffer_idx]);
            assert(!nxt_pipe_buffer->bufferHasFullData[buffer_idx]);

            if (!nxt_tag) {// from empty, empty
                nxt_pipe_buffer->bufferHasFullData[buffer_idx] = true;
                LOG(PIPELINE_DEBUG_FLAG) << "tag=" << nxt_tag << ", stage_idx=" << nxt_pipe_buffer->stage_idx_ << ", tensor_id=" << tensor_id << ", buffer_idx=" << buffer_idx << ", 设置bufferHasFullData为true.";
            }
            nxt_tag = (!nxt_tag);
        }
        if (pre_tag) { // from full, full 
            assert(pre_pipe_buffer->bufferHasFullData[buffer_idx] == true);
            pre_pipe_buffer->bufferHasFullData[buffer_idx] = false;
            LOG(PIPELINE_DEBUG_FLAG) << "tag=" << pre_tag << ", stage_idx=" << pre_pipe_buffer->stage_idx_ << ", tensor_id=" << tensor_id << ", buffer_idx=" << buffer_idx << ", 设置bufferHasFullData为false.";
        }
        // pre_tag不需要转移，因为已经在上面的nxt_tag转移过了
        LOG(PIPELINE_DEBUG_FLAG) << "processSharedSpmPipebufferWithoutRingbuffer处理结束" << std::endl;
    }
    void Pipeline::processEntryIsolateSpmPipebufferWithRingbuffer(TensorId tensor_id, uint stage_idx, PipelineBuffer* pipe_buffer, uint& buffer_idx){ 
        auto print_head_to_string = [&]() {
            std::ostringstream ss;
            ss << "[pipeLogIdx-" << pipeLogIdx << "]" << "PipelineState::processEntryIsolateSpmPipebufferWithRingbuffer: 检查有ring buffer的入口ISOLATE pipe buffer状态.";
            return ss.str();
        };
        auto print_base_to_string = [&]() {
            std::ostringstream ss;
            ss << "totalCycles=" << RT.getCycles() 
                << ", stage_idx=" << stage_idx
                << ", tensor_id=" << tensor_id 
                << ", buffer_idx=" << buffer_idx 
                << ", subBatchOffset=" << pipe_buffer->subBatchOffset
                << ", subBatchSize=" << getSubBatchSize()
                << ", pipe_buffer->cmdRunning[buffer_idx]=" << pipe_buffer->cmdRunning[buffer_idx] 
                << ", pipe_buffer->fetchFlushSendCmdCounts[buffer_idx]=" << pipe_buffer->fetchFlushSendCmdCounts[buffer_idx]
                << ", pipe_buffer->ringBuffer=" << pipe_buffer->ring_buffer_
                << ", pipe_buffer->ringBuffer->toString()=" << pipe_buffer->ring_buffer_->toString();
            return ss.str();
        };
        if(pipe_buffer->cmdRunning[buffer_idx]) {
            // 有指令执行，检查指令计数是否为0。不为0，则继续等待指令执行完成；为0，更新pipeBuffer状态
            if(pipe_buffer->fetchFlushSendCmdCounts[buffer_idx] > 0) return;
            assert(pipe_buffer->fetchFlushSendCmdCounts[buffer_idx] == 0);
            // 执行结束，状态转移
            LOG(PIPELINE_DEBUG_FLAG) << print_head_to_string();
            pipe_buffer->cmdRunning[buffer_idx] = false;
            assert(!pipe_buffer->bufferHasFullData[buffer_idx]);
            pipe_buffer->bufferHasFullData[buffer_idx] = true;

            // 标记ring buffer使用
            pipe_buffer->ring_buffer_->use(pipe_buffer->subBatchOffset);

            // 增加offset
            ++ pipe_buffer->subBatchOffset;

            // 为了避免页表膨胀，手动清空页表
            for(uint vPage = pipe_buffer->cmdVPageRange[buffer_idx].first;vPage <= pipe_buffer->cmdVPageRange[buffer_idx].second;++ vPage) {
                RT.invalidPageTable(pipe_buffer->cmdAcc[buffer_idx], vPage);
            }
            for(uint vPage = pipe_buffer->ringBufferCmdVPageRange[buffer_idx].first;vPage <= pipe_buffer->ringBufferCmdVPageRange[buffer_idx].second;++ vPage) {
                RT.invalidPageTable(pipe_buffer->cmdAcc[buffer_idx], vPage);
            }
            LOG(PIPELINE_DEBUG_FLAG) << "带ring buffer的入口Isolate Spm类型pipeBuffer有Send指令全部执行结束" 
                    << print_base_to_string()
                    << ", vPage=[" << pipe_buffer->cmdVPageRange[buffer_idx].first 
                    << "," << pipe_buffer->cmdVPageRange[buffer_idx].second << "]" 
                    << ", ringBufferVPage=[" << pipe_buffer->ringBufferCmdVPageRange[buffer_idx].first 
                    << "," << pipe_buffer->ringBufferCmdVPageRange[buffer_idx].second << "]" 
                    << ", cmdAcc=" << pipe_buffer->cmdAcc[buffer_idx];
            LOG(PIPELINE_DEBUG_FLAG) << "更新后pipebuffer状态：" << "bufferIdx=" << buffer_idx << ", pipeBuffer: \n" << pipe_buffer->toString();
            // 缓冲区切换
            if(pipe_buffer->withDoubleBuffer) {
                buffer_idx = (buffer_idx + 1) & 1; // 等价于(buffer_idx + 1) % 2
            }
            LOG(PIPELINE_DEBUG_FLAG) << "缓冲区切换至buffer_idx=" << buffer_idx << std::endl;
        } else if (pipe_buffer->bufferHasFullData[buffer_idx] == false) {
            // 如果没有数据，等待ring buffer有数据可用
            auto ring_buffer = pipe_buffer->ring_buffer_;
            if (!ring_buffer->hasReadyBuffer(pipe_buffer->subBatchOffset)) {
                return;
            }
            if (startTime.size() == pipe_buffer->subBatchOffset) {
                startTime.push_back(RT.getCycles());
            }

            LOG(PIPELINE_DEBUG_FLAG) << print_head_to_string();
            LOG(PIPELINE_DEBUG_FLAG) << "带ring buffer的入口Isolate Spm类型pipeBuffer有足够batch执行，准备发送指令.\n"
                        << print_base_to_string();

            // 获取指令所在的加速器
            uint accId = action_->acc_source_.all_acc_ids_[RT.GetMinimumAccIdx(action_->acc_source_.all_acc_ids_, RT.accelSendCmdCount)];
            pipe_buffer->cmdAcc[buffer_idx] = accId;

            uint page_num = ring_buffer->getPSpmPages(pipe_buffer->subBatchOffset)->size();
            // 设置接收方页表映射
            {
                auto& spmPages = *(pipe_buffer->pSpmPages[buffer_idx]);
                assert(page_num <= spmPages.size());
                auto [startVPage, endVPage] = RT.spm_vpage_manager_[accId].getAvailableInterval(page_num);
                assert(endVPage - startVPage + 1 == page_num);
                for(uint vPage = startVPage;vPage <= endVPage;++ vPage) {
                    RT.setPageTable(accId, vPage, spmPages[vPage - startVPage]);
                }
                pipe_buffer->cmdVPageRange[buffer_idx] = {startVPage, endVPage};

                LOG(PIPELINE_DEBUG_FLAG) << "本地buffer页表设置.pipe_buffer->cmdAcc[buffer_idx]=" << pipe_buffer->cmdAcc[buffer_idx] 
                                        << ", cmdVPageRange=[" << startVPage << "," << endVPage  << "]"
                                        << ", spmPages.front()=" << spmPages.front() << ", spmPages.back()=" << spmPages.back();
            }

            // 设置发送方页表映射（ring buffer）
            {
                auto spmPages = *(ring_buffer->getPSpmPages(pipe_buffer->subBatchOffset));
                auto [startVPage, endVPage] = RT.spm_vpage_manager_[accId].getAvailableInterval(page_num);
                assert(page_num == spmPages.size());
                assert(endVPage - startVPage + 1 == page_num);
                for(uint vPage = startVPage;vPage <= endVPage;++ vPage) {
                    RT.setPageTable(accId, vPage, spmPages[vPage - startVPage]);
                }
                pipe_buffer->ringBufferCmdVPageRange[buffer_idx] = {startVPage, endVPage};

                LOG(PIPELINE_DEBUG_FLAG) << "ring buffer页表设置.pipe_buffer->cmdAcc[buffer_idx]=" << pipe_buffer->cmdAcc[buffer_idx] 
                                        << ", ringBufferCmdVPageRange=[" << startVPage << "," << endVPage  << "]"
                                        << ", spmPages.front()=" << spmPages.front() << ", spmPages.back()=" << spmPages.back();
            }

            // 发送指令，设置指令计数，修改状态
            sendSendCmd(accId, tensor_id, stage_idx, ring_buffer->getStrides(), ring_buffer->getShape(), pipe_buffer->ringBufferCmdVPageRange[buffer_idx].first, pipe_buffer->cmdVPageRange[buffer_idx].first);

            pipe_buffer->cmdRunning[buffer_idx] = true;
            assert(pipe_buffer->fetchFlushSendCmdCounts[buffer_idx] == 0); // 同时只能存在一个Send指令
            pipe_buffer->fetchFlushSendCmdCounts[buffer_idx] += 1;

            LOG(PIPELINE_DEBUG_FLAG) << "更新后状态：" << "bufferIdx=" << buffer_idx << ", pipeBuffer: \n" << pipe_buffer->toString() << std::endl;
        } else {
            // 已经有数据，等待数据被消费
            return;
        }
    }
    void Pipeline::processExportIsolateSpmPipebufferWithRingbuffer(TensorId tensor_id, uint stage_idx, PipelineBuffer* pipe_buffer, uint& buffer_idx) {
        auto print_head_to_string = [&]() {
            std::ostringstream ss;
            ss << "[pipeLogIdx-" << pipeLogIdx << "]" << "PipelineState::processExportIsolateSpmPipebufferWithRingbuffer: 检查有ring buffer的出口ISOLATE pipe buffer状态.";
            return ss.str();
        };
        auto print_base_to_string = [&]() {
            std::ostringstream ss;
            ss << "totalCycles=" << RT.getCycles() 
                << ", stage_idx=" << stage_idx
                << ", tensor_id=" << tensor_id 
                << ", buffer_idx=" << buffer_idx 
                << ", subBatchOffset=" << pipe_buffer->subBatchOffset
                << ", subBatchSize=" << getSubBatchSize()
                << ", pipe_buffer->cmdRunning[buffer_idx]=" << pipe_buffer->cmdRunning[buffer_idx] 
                << ", pipe_buffer->fetchFlushSendCmdCounts[buffer_idx]=" << pipe_buffer->fetchFlushSendCmdCounts[buffer_idx]
                << ", pipe_buffer->ringBuffer=" << pipe_buffer->ring_buffer_
                << ", pipe_buffer->ringBuffer->toString()=" << pipe_buffer->ring_buffer_->toString();
            return ss.str();
        };
        if(pipe_buffer->cmdRunning[buffer_idx]) {
            // 有指令执行，检查指令计数是否为0。不为0，则继续等待指令执行完成；为0，更新pipeBuffer状态
            if(pipe_buffer->fetchFlushSendCmdCounts[buffer_idx] > 0) return;
            assert(pipe_buffer->fetchFlushSendCmdCounts[buffer_idx] == 0);
            // 执行结束，状态转移
            LOG(PIPELINE_DEBUG_FLAG) << print_head_to_string();
            pipe_buffer->cmdRunning[buffer_idx] = false;
            assert(pipe_buffer->bufferHasFullData[buffer_idx]);
            pipe_buffer->bufferHasFullData[buffer_idx] = false;

            // 标记ring buffer填充
            pipe_buffer->ring_buffer_->fill(pipe_buffer->subBatchOffset);

            // 增加offset
            ++ pipe_buffer->subBatchOffset;

            // 为了避免页表膨胀，手动清空页表
            for(uint vPage = pipe_buffer->cmdVPageRange[buffer_idx].first;vPage <= pipe_buffer->cmdVPageRange[buffer_idx].second;++ vPage) {
                RT.invalidPageTable(pipe_buffer->cmdAcc[buffer_idx], vPage);
            }
            for(uint vPage = pipe_buffer->ringBufferCmdVPageRange[buffer_idx].first;vPage <= pipe_buffer->ringBufferCmdVPageRange[buffer_idx].second;++ vPage) {
                RT.invalidPageTable(pipe_buffer->cmdAcc[buffer_idx], vPage);
            }
            LOG(PIPELINE_DEBUG_FLAG) << "带ring buffer的入口ISOLATE SPM类型pipeBuffer有send指令执行结束。\n" 
                    << print_base_to_string()
                    << ", vPage=[" << pipe_buffer->cmdVPageRange[buffer_idx].first 
                    << "," << pipe_buffer->cmdVPageRange[buffer_idx].second << "]" 
                    << ", ringBufferVPage=[" << pipe_buffer->ringBufferCmdVPageRange[buffer_idx].first 
                    << "," << pipe_buffer->ringBufferCmdVPageRange[buffer_idx].second << "]" 
                    << ", cmdAcc=" << pipe_buffer->cmdAcc[buffer_idx];
            LOG(PIPELINE_DEBUG_FLAG) << "更新后pipebuffer状态：" << "buffer_idx=" << buffer_idx << ", pipeBuffer: \n" << pipe_buffer->toString();
            // 缓冲区切换
            if(pipe_buffer->withDoubleBuffer) {
                buffer_idx = (buffer_idx + 1) & 1; // 等价于(bufferIdx + 1) % 2
            }
            LOG(PIPELINE_DEBUG_FLAG) << "缓冲区切换至bufferIdx=" << buffer_idx << std::endl;
        } else if (pipe_buffer->bufferHasFullData[buffer_idx]) {
            // 如果有数据，等待ring buffer有空闲空间可用
            auto ring_buffer = pipe_buffer->ring_buffer_;
            if (!ring_buffer->hasIdleBuffer()) {
                return;
            }

            LOG(PIPELINE_DEBUG_FLAG) << print_head_to_string();
            
            // 获取指令所在的加速器
            uint accId = action_->acc_source_.all_acc_ids_[RT.GetMinimumAccIdx(action_->acc_source_.all_acc_ids_, RT.accelSendCmdCount)];
            pipe_buffer->cmdAcc[buffer_idx] = accId;

            uint page_num = ring_buffer->getPSpmPages(pipe_buffer->subBatchOffset)->size();
            // 设置接收方页表映射（ring buffer）
            {
                auto spmPages = *(ring_buffer->getPSpmPages(pipe_buffer->subBatchOffset));
                auto [startVPage, endVPage] = RT.spm_vpage_manager_[accId].getAvailableInterval(page_num);
                assert(spmPages.size() == page_num);
                assert(endVPage - startVPage + 1 == page_num);
                for(uint vPage = startVPage;vPage <= endVPage;++ vPage) {
                    RT.setPageTable(accId, vPage, spmPages[vPage - startVPage]);
                }
                pipe_buffer->ringBufferCmdVPageRange[buffer_idx] = {startVPage, endVPage};

                LOG(PIPELINE_DEBUG_FLAG) << "ring buffer页表设置.pipe_buffer->cmdAcc[buffer_idx]=" << pipe_buffer->cmdAcc[buffer_idx] 
                                        << ", cmdVPageRange=[" << startVPage << "," << endVPage  << "]"
                                        << ", spmPages.front()=" << spmPages.front() << ", spmPages.back()=" << spmPages.back();
            }

            // 设置发送方页表映射
            {
                auto& spmPages = *(pipe_buffer->pSpmPages[buffer_idx]);
                auto [startVPage, endVPage] = RT.spm_vpage_manager_[accId].getAvailableInterval(page_num);
                assert(page_num <= spmPages.size());
                assert(endVPage - startVPage + 1 == page_num);
                for(uint vPage = startVPage;vPage <= endVPage;++ vPage) {
                    RT.setPageTable(accId, vPage, spmPages[vPage - startVPage]);
                }
                pipe_buffer->cmdVPageRange[buffer_idx] = {startVPage, endVPage};

                LOG(PIPELINE_DEBUG_FLAG) << "本地buffer页表设置.pipeBuffer->cmdAcc[bufferIdx]=" << pipe_buffer->cmdAcc[buffer_idx] 
                                        << ", pipe_buffer->cmdVPageRange[buffer_idx]=[" << startVPage << "," << endVPage  << "]"
                                        << ", spmPages.front()=" << spmPages.front() << ", spmPages.back()=" << spmPages.back();
            }

            // 发送指令，设置指令计数，修改状态
            sendSendCmd(accId, tensor_id, stage_idx, ring_buffer->getStrides(), ring_buffer->getShape(), pipe_buffer->cmdVPageRange[buffer_idx].first, pipe_buffer->ringBufferCmdVPageRange[buffer_idx].first);

            pipe_buffer->cmdRunning[buffer_idx] = true;
            assert(pipe_buffer->fetchFlushSendCmdCounts[buffer_idx] == 0);
            pipe_buffer->fetchFlushSendCmdCounts[buffer_idx] += 1;

            LOG(PIPELINE_DEBUG_FLAG) << "更新后状态：" << "buffer_idx=" << buffer_idx << ", pipe_buffer: " << pipe_buffer->toString() << std::endl;
        } else {
            // 已经有数据，等待数据被消费
            return;
        }
    }
    void Pipeline::processEntryAllRingbuffer(TensorId tensor_id, uint stage_idx, PipelineBuffer* pipe_buffer, bool& tag) {
        // tag=false, empty->(有ready的ring buffer，将页表设置为一块ring buffer，完成设置为true, full)true, full->(stage执行完成)true, empty->(调用ring buffer的use方法标记已使用)false, empty
         auto print_head_to_string = [&]() {
            std::ostringstream ss;
            ss << "[pipeLogIdx-" << pipeLogIdx << "]" << "PipelineState::processEntryAllRingbuffer: 检查入口all ring buffer类型pipebuffer状态.tensor_id=" << tensor_id << ", stage_idx=" << stage_idx;
            return ss.str();
        };
        if (pipe_buffer->bufferHasFullData[0]) {
            // buffer还在被stage使用中
            return;
        }
        if (!tag) {
            // 还没有设置page set
            if (pipe_buffer->ring_buffer_->hasReadyBuffer(pipe_buffer->subBatchOffset)) {
                LOG(PIPELINE_DEBUG_FLAG) << print_head_to_string();
                // 有ring buffer可用
                pipe_buffer->pSpmPages[0] = pipe_buffer->ring_buffer_->getPSpmPages(pipe_buffer->subBatchOffset);
                pipe_buffer->subBatchOffset ++;
                pipe_buffer->bufferHasFullData[0] = true;
                tag = true;
                LOG(PIPELINE_DEBUG_FLAG) << "设置pipe buffer页表。页表对应offset=" << pipe_buffer->subBatchOffset - 1 << ", 状态转移至tag=" << tag << ", pipe_buffer:\n" << pipe_buffer->toString() << std::endl;
            }
        } else if (tag) {
            LOG(PIPELINE_DEBUG_FLAG) << print_head_to_string();
            pipe_buffer->ring_buffer_->use(pipe_buffer->subBatchOffset - 1);
            tag = false;
            LOG(PIPELINE_DEBUG_FLAG) << "标记ring buffer已经被使用。对应offset=" << pipe_buffer->subBatchOffset - 1 << ", 状态转移至tag=" << tag << ", pipe_buffer:\n" << pipe_buffer->toString() << std::endl;
        }
    }
    void Pipeline::processExportAllRingbuffer(TensorId tensor_id, uint stage_idx, PipelineBuffer* pipe_buffer, bool& tag) {
        // tag=false, empty->(初始化物理页)true, empty->(stage执行完成)true, full->(调用ring buffer的fill方法标记已填充）false, full -> (等待下一块buffer空闲，并修改物理页到下一块buffer)true, empty
         auto print_head_to_string = [&]() {
            std::ostringstream ss;
            ss << "[pipeLogIdx-" << pipeLogIdx << "]" << "PipelineState::processExportAllRingbuffer: 检查出口all ring buffer类型pipebuffer状态.tensor_id=" << tensor_id << ", stage_idx=" << stage_idx;
            return ss.str();
        };
        if (!tag && !pipe_buffer->bufferHasFullData[0]) { // false, empty->true, empty
            // 初次使用，设置页表
            LOG(PIPELINE_DEBUG_FLAG) << print_head_to_string();
            assert(pipe_buffer->ring_buffer_->hasIdleBuffer());
            pipe_buffer->pSpmPages[0] = pipe_buffer->ring_buffer_->getPSpmPages(pipe_buffer->subBatchOffset);
            tag = true;
            LOG(PIPELINE_DEBUG_FLAG) << "初次使用，设置页表为ring buffer第一个空闲位置.更新后，pie_buffer=" << pipe_buffer->toString() << std::endl;
            return;
        }

        if (tag && pipe_buffer->bufferHasFullData[0]) { // true, full -> false, full
            // 层执行结束，标记填充
            LOG(PIPELINE_DEBUG_FLAG) << print_head_to_string();
            pipe_buffer->ring_buffer_->fill(pipe_buffer->subBatchOffset);
            ++ pipe_buffer->subBatchOffset;
            tag = false;
            LOG(PIPELINE_DEBUG_FLAG) << "标记ring buffer填充完成。对应offset=" << pipe_buffer->subBatchOffset - 1 << ", 状态转移至tag=" << tag << ", pipe_buffer:\n" << pipe_buffer->toString() << std::endl;
            // 转移到false full，继续检查是否有idle buffer，执行后续逻辑，无需返回
        } 

        if (!tag && pipe_buffer->bufferHasFullData[0]) { // false, full -> true, empty
            if (!pipe_buffer->ring_buffer_->hasIdleBuffer()) {
                return;
            }
            LOG(PIPELINE_DEBUG_FLAG) << print_head_to_string();
            tag = true;
            pipe_buffer->bufferHasFullData[0] = false;
            pipe_buffer->pSpmPages[0] = pipe_buffer->ring_buffer_->getPSpmPages(pipe_buffer->subBatchOffset);
            LOG(PIPELINE_DEBUG_FLAG) << "占用ring buffer一块存储区域。对应offset=" << pipe_buffer->subBatchOffset << ", 状态转移至tag=" << tag << ", pipe_buffer:\n" << pipe_buffer->toString() << std::endl;
        }
    }

    void Pipeline::stateChange(PipelineState state_) {
        LOG(PIPELINE_DEBUG_FLAG) << "[pipeLogIdx-" << pipeLogIdx << "]" << "Pipeline::stateChage: Pipeline state change from " << state << " to " << state_ << std::endl;
        state = state_;
    }

    void Pipeline::sendFetchCmd(uint accId, uint tensorId, uint stageIdx, uint64 dramBaseAddr, const std::vector<uint64>& strides, const std::vector<uint64>& shape, uint vPageBase) {
        //fetchCmdCount
        AccelSPMFetch *fetchCMD = new AccelSPMFetch();

        Tensor tensorFetch(dramBaseAddr, strides);
        Tensor spmFetchLayout(vPageBase * RT.hw.pageBytes, strides);

        SPMTensor spmFetchTensor;

        spmFetchTensor.set(spmFetchLayout, false, true); // in fect, bypass and reuse are not used
        fetchCMD->shape = shape;
        fetchCMD->inputTensors[0] = tensorFetch;
        fetchCMD->inputSPMTensors[0] = spmFetchTensor;
        fetchCMD->tensorId = tensorId;
        fetchCMD->stageIdx = stageIdx;
        fetchCMD->is_pipeline_cmd = true;
        
        RT.system->addCmd(accId, fetchCMD);

        watingCmd.emplace_back(accId, fetchCMD->globalCmdId);
        RT.accelFetchCmdCount[accId] ++;
        LOG(PIPELINE_DEBUG_FLAG) << "[pipeLogIdx-" << pipeLogIdx << "]" << "Pipeline::sendFetchCmd: totalCycles=" << RT.getCycles() << ",cmd=" << fetchCMD->toString() << std::endl;
    }

    void Pipeline::sendFlushCmd(uint accId, uint tensorId, uint stageIdx, uint64 dramBaseAddr, const std::vector<uint64>& strides, const std::vector<uint64>& shape, uint vPageBase) {
        AccelSPMFlush* flushCMD = new AccelSPMFlush();

        Tensor tensorFlush(dramBaseAddr, strides);
        Tensor spmFlushLayout(vPageBase * RT.hw.pageBytes, strides);

        SPMTensor spmFlushTensor;

        spmFlushTensor.set(spmFlushLayout, false, true); // in fect, bypass and reuse are not used
        flushCMD->shape = shape;
        flushCMD->outputTensors[0] = tensorFlush;
        flushCMD->outputSPMTensors[0] = spmFlushTensor;
        flushCMD->tensorId = tensorId;
        flushCMD->stageIdx = stageIdx;
        flushCMD->is_pipeline_cmd = true;

        RT.system->addCmd(accId, flushCMD);

        watingCmd.emplace_back(accId, flushCMD->globalCmdId);
        RT.accelFlushCmdCount[accId] ++;
        LOG(PIPELINE_DEBUG_FLAG) << "[pipeLogIdx-" << pipeLogIdx << "]" << "Pipeline::sendFlushCmd: totalCycles=" << RT.getCycles() << ",cmd=" << flushCMD->toString() << std::endl;
    }

    void Pipeline::sendSendCmd(uint accId, uint tensorId, uint stageIdx, const std::vector<uint64>& strides, const std::vector<uint64>& shape, uint srcVPageBase, uint dstVPageBase) {
        AccelSPMSend* sendCMD = new AccelSPMSend();

        Tensor spmSrcLayout((uint64)srcVPageBase * RT.hw.pageBytes, strides);
        Tensor spmDstFlushLayout((uint64)dstVPageBase * RT.hw.pageBytes, strides);

        SPMTensor spmSendSrcTensor;
        SPMTensor spmSendDstTensor;

        spmSendSrcTensor.set(spmSrcLayout, false, false);
        spmSendDstTensor.set(spmDstFlushLayout, false, false);
        sendCMD->shape = shape;
        sendCMD->inputSPMTensors[0] = spmSendSrcTensor;
        sendCMD->outputSPMTensors[0] = spmSendDstTensor;
        sendCMD->tensorId = tensorId;
        sendCMD->stageIdx = stageIdx;
        sendCMD->is_pipeline_cmd = true;

        RT.system->addCmd(accId, sendCMD);

        watingCmd.emplace_back(accId, sendCMD->globalCmdId);
        ++ RT.accelSendCmdCount[accId];
        LOG(PIPELINE_DEBUG_FLAG) << "[pipeLogIdx-" << pipeLogIdx << "]" << "Pipeline::sendSendCmd: totalCycles=" << RT.getCycles() << ",cmd=" << sendCMD->toString() << std::endl;
    }

    void Pipeline::waitCmd() {
        // 指令收集，查找对应tensor，更新指令计数
        watingCmd.remove_if([this](const auto& item) {
            auto cmd = RT.getFinishedCmd(item.first, item.second, true);
            if(cmd == NULL) {
                return false;
            }
            auto tensorId = cmd->tensorId;
            uint stageIdx = cmd->stageIdx;

            LOG(PIPELINE_DEBUG_FLAG) << "[pipeLogIdx-" << pipeLogIdx << "]" << "PipelineState::STATE_STAGE_EXECUTE: 收到指令." 
                        << "globalCmdId=" << item.second
                        << ", accId=" << item.first
                        << ", stageIdx=" << stageIdx
                        << ", tensorId=" << tensorId;

            if(this->tensorId2EntryDramOrDramDepenPipeBuffer.find(tensorId) != this->tensorId2EntryDramOrDramDepenPipeBuffer.end() &&
                this->tensorId2EntryDramOrDramDepenPipeBuffer[tensorId].find(stageIdx) != this->tensorId2EntryDramOrDramDepenPipeBuffer[tensorId].end()) {
                auto& [pipeBuffer, bufferIdx] = this->tensorId2EntryDramOrDramDepenPipeBuffer[tensorId][stageIdx];
                assert(pipeBuffer->cmdRunning[bufferIdx]);
                assert(pipeBuffer->fetchFlushSendCmdCounts[bufferIdx] > 0);
                -- pipeBuffer->fetchFlushSendCmdCounts[bufferIdx];
                LOG(PIPELINE_DEBUG_FLAG) << "[pipeLogIdx-" << pipeLogIdx << "]" << "指令来自tensorId2EntryDramOrDramDepenPipeBuffer。更新后, bufferIdx=" << bufferIdx << ", pipeBuffer->fetchFlushSendCmdCounts[bufferIdx]=" << pipeBuffer->fetchFlushSendCmdCounts[bufferIdx] << "\n"; 
            } else if(this->tensorId2ExportDramOrDramDepenPipeBuffer.find(tensorId) != this->tensorId2ExportDramOrDramDepenPipeBuffer.end()
                    && this->tensorId2ExportDramOrDramDepenPipeBuffer[tensorId].find(stageIdx) != this->tensorId2ExportDramOrDramDepenPipeBuffer[tensorId].end()) {
                auto& [pipeBuffer, bufferIdx] = this->tensorId2ExportDramOrDramDepenPipeBuffer[tensorId][stageIdx];
                assert(pipeBuffer->cmdRunning[bufferIdx]);
                assert(pipeBuffer->fetchFlushSendCmdCounts[bufferIdx] > 0);
                -- pipeBuffer->fetchFlushSendCmdCounts[bufferIdx];
                LOG(PIPELINE_DEBUG_FLAG) << "[pipeLogIdx-" << pipeLogIdx << "]" << "指令来自tensorId2ExportDramOrDramDepenPipeBuffer。更新后, bufferIdx=" << bufferIdx << ", pipeBuffer->fetchFlushSendCmdCounts[bufferIdx]=" << pipeBuffer->fetchFlushSendCmdCounts[bufferIdx] << "\n"; 
            } else if(this->tensorId2EntryIsolatePipeBufferWithRingbuffer.find(tensorId) != this->tensorId2EntryIsolatePipeBufferWithRingbuffer.end() &&
                this->tensorId2EntryIsolatePipeBufferWithRingbuffer[tensorId].find(stageIdx) != this->tensorId2EntryIsolatePipeBufferWithRingbuffer[tensorId].end()) {
                auto& [pipeBuffer, bufferIdx] = this->tensorId2EntryIsolatePipeBufferWithRingbuffer[tensorId][stageIdx];
                assert(pipeBuffer->cmdRunning[bufferIdx]);
                assert(pipeBuffer->fetchFlushSendCmdCounts[bufferIdx] > 0);
                -- pipeBuffer->fetchFlushSendCmdCounts[bufferIdx];
                LOG(PIPELINE_DEBUG_FLAG) << "[pipeLogIdx-" << pipeLogIdx << "]" << "指令来自tensorId2EntryIsloatePipeBufferWithDoublebuffer。更新后, bufferIdx=" << bufferIdx << ", pipeBuffer->fetchFlushSendCmdCounts[bufferIdx]=" << pipeBuffer->fetchFlushSendCmdCounts[bufferIdx] << "\n"; 
            } else if(this->tensorId2ExportIsolatePipeBufferWithRingbuffer.find(tensorId) != this->tensorId2ExportIsolatePipeBufferWithRingbuffer.end() &&
                this->tensorId2ExportIsolatePipeBufferWithRingbuffer[tensorId].find(stageIdx) != this->tensorId2ExportIsolatePipeBufferWithRingbuffer[tensorId].end()) {
                auto& [pipeBuffer, bufferIdx] = this->tensorId2ExportIsolatePipeBufferWithRingbuffer[tensorId][stageIdx];
                assert(pipeBuffer->cmdRunning[bufferIdx]);
                assert(pipeBuffer->fetchFlushSendCmdCounts[bufferIdx] > 0);
                -- pipeBuffer->fetchFlushSendCmdCounts[bufferIdx];
                LOG(PIPELINE_DEBUG_FLAG) << "[pipeLogIdx-" << pipeLogIdx << "]" << "指令来自tensorId2ExportIsolatePipeBufferWithDoublebuffer。更新后, bufferIdx=" << bufferIdx << ", pipeBuffer->fetchFlushSendCmdCounts[bufferIdx]=" << pipeBuffer->fetchFlushSendCmdCounts[bufferIdx] << "\n"; 
            } else if(this->tensorId2IsolatePipeBufferNoRingbuffer.find(tensorId) != this->tensorId2IsolatePipeBufferNoRingbuffer.end()
                    && this->tensorId2IsolatePipeBufferNoRingbuffer[tensorId].find(stageIdx) != this->tensorId2IsolatePipeBufferNoRingbuffer[tensorId].end()) {
                auto& [prePipeBuffer, nxtPipeBuffer, preBufferIdx, nxtBufferIdx] = this->tensorId2IsolatePipeBufferNoRingbuffer[tensorId][stageIdx];
                assert(prePipeBuffer->cmdRunning[preBufferIdx]);
                assert(prePipeBuffer->fetchFlushSendCmdCounts[preBufferIdx] > 0);
                -- prePipeBuffer->fetchFlushSendCmdCounts[preBufferIdx];

                assert(nxtPipeBuffer->cmdRunning[nxtBufferIdx]);
                assert(nxtPipeBuffer->fetchFlushSendCmdCounts[nxtBufferIdx] > 0);
                -- nxtPipeBuffer->fetchFlushSendCmdCounts[nxtBufferIdx];
                LOG(PIPELINE_DEBUG_FLAG) << "[pipeLogIdx-" << pipeLogIdx << "]" << "指令来自tensorId2IsolatePipeBuffer。更新后, preBufferIdx=" << preBufferIdx 
                                    << ", prePipeBuffer->fetchFlushSendCmdCounts[preBufferIdx]=" << prePipeBuffer->fetchFlushSendCmdCounts[preBufferIdx] 
                                    << ", nxtBufferIdx=" << nxtBufferIdx
                                    << ", nxtPipeBuffer->fetchFlushSendCmdCounts[nxtBufferIdx]=" << nxtPipeBuffer->fetchFlushSendCmdCounts[nxtBufferIdx] 
                                    << "\n"; 

            } else if(this->tensorId2SharedPipeBuffer.find(tensorId) != this->tensorId2SharedPipeBuffer.end()
                    && this->tensorId2SharedPipeBuffer[tensorId].find(stageIdx) != this->tensorId2SharedPipeBuffer[tensorId].end()) {
                for(uint bufferIdx = 0;bufferIdx < 2;++ bufferIdx) {
                    // 共享pipebuffer不应该有指令，且必须是double-buffer
                    auto& [prePipeBuffer, nxtPipeBuffer, tag] = this->tensorId2SharedPipeBuffer[tensorId][bufferIdx][stageIdx];
                    assert(prePipeBuffer->withDoubleBuffer);
                    assert(nxtPipeBuffer->withDoubleBuffer);

                    assert(!prePipeBuffer->cmdRunning[bufferIdx]);
                    assert(nxtPipeBuffer->fetchFlushSendCmdCounts[bufferIdx] == 0);

                    assert(!prePipeBuffer->cmdRunning[bufferIdx]);
                    assert(nxtPipeBuffer->fetchFlushSendCmdCounts[bufferIdx] == 0);
                }
                assert(false);
            } 
            else {
                assert(false);
            }

            delete cmd;
            return true;
        });
    }
   
    nlohmann::json Pipeline::pipebufferToJson() const {
        nlohmann::json js;
#ifdef JSON_DBG
        js["func"] = "pipebufferToJson";
        for (uint stage_idx = 0;stage_idx < pipebuffer_set.size();++ stage_idx) {
            nlohmann::json js_stage_pipebuffer;
            for (auto& [tensor_id, pipe_buffer]: pipebuffer_set[stage_idx]) {
                js_stage_pipebuffer["tensor-" + std::to_string(tensor_id)] = pipe_buffer.toJson();
            }
            js["stage-" + std::to_string(stage_idx)] = js_stage_pipebuffer;
        }
#endif
        return js;
    }

    std::string Pipeline::pipebufferToString() const {
        return pipebufferToJson().dump(JSON_DUMP_INDENTATION);
    }

    nlohmann::json Pipeline::pipeBufferIndexToJson(bool with_pipebuffer) const {
        nlohmann::json js;
#ifdef JSON_DBG
        js["func"] = "pipeBufferIndexToJson";
        js["with_pipebuffer"] = with_pipebuffer;

        {
            nlohmann::json js_entry_dram; 
            for (const auto& [tensor_id, mp]: tensorId2EntryDramOrDramDepenPipeBuffer) {
                nlohmann::json tensor_entry;
                for (const auto& [stage_idx, p]: mp) {
                    const auto& [pipe_buffer, now_idx] = p;
                    nlohmann::json stage_entry;
                    stage_entry["tensor_id"] = tensor_id;
                    stage_entry["stage_idx"] = stage_idx;
                    stage_entry["now_idx"] = now_idx;
                    if (with_pipebuffer) {
                        stage_entry["pipe_buffer"] = pipe_buffer->toJson();
                    } else {
                        std::ostringstream ss;
                        ss << (&pipe_buffer);
                        stage_entry["pipe_buffer_ptr"] = ss.str();
                    }
                    tensor_entry[std::to_string(stage_idx)] = stage_entry;
                }
                js_entry_dram[std::to_string(tensor_id)] = tensor_entry;
            }

            js["entry_dram_or_dram_depen"] = js_entry_dram;
        }

        {
            nlohmann::json js_export_dram; 
            for (const auto& [tensor_id, mp]: tensorId2ExportDramOrDramDepenPipeBuffer) {
                nlohmann::json tensor_entry;
                for (const auto& [stage_idx, p]: mp) {
                    const auto& [pipe_buffer, now_idx] = p;
                    nlohmann::json stage_entry;
                    stage_entry["tensor_id"] = tensor_id;
                    stage_entry["stage_idx"] = stage_idx;
                    stage_entry["now_idx"] = now_idx;
                    if (with_pipebuffer) {
                        stage_entry["pipe_buffer"] = pipe_buffer->toJson();
                    } else {
                        std::ostringstream ss;
                        ss << (&pipe_buffer);
                        stage_entry["pipe_buffer_ptr"] = ss.str();
                    }
                    tensor_entry[std::to_string(stage_idx)] = stage_entry;
                }
                js_export_dram[std::to_string(tensor_id)] = tensor_entry;
            }

            js["export_dram_or_dram_depen"] = js_export_dram;
        }

        {
            nlohmann::json js_entry_isolate_ringbuffer;
            for (const auto& [tensor_id, mp]: tensorId2EntryIsolatePipeBufferWithRingbuffer) {
                nlohmann::json tensor_entry;
                for (const auto& [stage_idx, p]: mp) {
                    const auto& [pipe_buffer, now_idx] = p;
                    nlohmann::json stage_entry;
                    stage_entry["tensor_id"] = tensor_id;
                    stage_entry["stage_idx"] = stage_idx;
                    stage_entry["now_idx"] = now_idx;
                    if (with_pipebuffer) {
                        stage_entry["pipe_buffer"] = pipe_buffer->toJson();
                    } else {
                        std::ostringstream ss;
                        ss << (&pipe_buffer);
                        stage_entry["pipe_buffer_ptr"] = ss.str();
                    }
                    tensor_entry[std::to_string(stage_idx)] = stage_entry;
                }
                js_entry_isolate_ringbuffer[std::to_string(tensor_id)] = tensor_entry;
            }
            js["entry_isolate_spm_ringbuffer_with_ringbuffer"] = js_entry_isolate_ringbuffer;
        }

        {
            nlohmann::json js_export_isolate_ringbuffer;
            for (const auto& [tensor_id, mp]: tensorId2ExportIsolatePipeBufferWithRingbuffer) {
                nlohmann::json tensor_entry;
                for (const auto& [stage_idx, p]: mp) {
                    const auto& [pipe_buffer, now_idx] = p;
                    nlohmann::json stage_entry;
                    stage_entry["tensor_id"] = tensor_id;
                    stage_entry["stage_idx"] = stage_idx;
                    stage_entry["now_idx"] = now_idx;
                    if (with_pipebuffer) {
                        stage_entry["pipe_buffer"] = pipe_buffer->toJson();
                    } else {
                        std::ostringstream ss;
                        ss << (&pipe_buffer);
                        stage_entry["pipe_buffer_ptr"] = ss.str();
                    }
                    tensor_entry[std::to_string(stage_idx)] = stage_entry;
                }
                js_export_isolate_ringbuffer[std::to_string(tensor_id)] = tensor_entry;
            }
            js["export_isolate_spm_ringbuffer_with_ringbuffer"] = js_export_isolate_ringbuffer;
        }

        {
            nlohmann::json js_isolate_spm; 
            for (const auto& [tensor_id, mp]: tensorId2IsolatePipeBufferNoRingbuffer) {
                nlohmann::json tensor_entry;
                for (const auto& [stage_idx, t]: mp) {
                    const auto& [pre_pipe_buffer, nxt_pipe_buffer, pre_idx, nxt_idx] = t;
                    nlohmann::json stage_entry;
                    stage_entry["tensor_id"] = tensor_id;
                    stage_entry["stage_idx"] = stage_idx;
                    stage_entry["pre_idx"] = pre_idx;
                    stage_entry["nxt_idx"] = nxt_idx;
                    if (with_pipebuffer) {
                        stage_entry["pre_pipe_buffer"] = pre_pipe_buffer->toJson();
                        stage_entry["nxt_pipe_buffer"] = nxt_pipe_buffer->toJson();
                    } else {
                        std::ostringstream pre_ss, nxt_ss;
                        pre_ss << (&pre_pipe_buffer);
                        nxt_ss << (&nxt_pipe_buffer);
                        stage_entry["pre_pipe_buffer_ptr"] = pre_ss.str();
                        stage_entry["nxt_pipe_buffer_ptr"] = nxt_ss.str();
                    }
                    tensor_entry[std::to_string(stage_idx)] = stage_entry;
                }
                js_isolate_spm[std::to_string(tensor_id)] = tensor_entry;
            }
            js["isolate_spm"] = js_isolate_spm;
        }

        {
            nlohmann::json js_shared_spm; 
            for (const auto& [tensor_id, mp]: tensorId2SharedPipeBuffer) {
                nlohmann::json tensor_entry;
                for (const auto& [buffer_idx, mp2]: mp) {
                    nlohmann::json buffer_entry;
                    for (const auto& [stage_idx, t]: mp2) {                        
                        const auto& [pre_pipe_buffer, nxt_pipe_buffer, tag] = t;
                        nlohmann::json stage_entry;
                        stage_entry["tensor_id"] = tensor_id;
                        stage_entry["stage_idx"] = stage_idx;
                        stage_entry["tag"] = tag;
                        if (with_pipebuffer) {
                            stage_entry["pre_pipe_buffer"] = pre_pipe_buffer->toJson();
                            stage_entry["nxt_pipe_buffer"] = nxt_pipe_buffer->toJson();
                        } else {
                            std::ostringstream pre_ss, nxt_ss;
                            pre_ss << (&pre_pipe_buffer);
                            nxt_ss << (&nxt_pipe_buffer);
                            stage_entry["pre_pipe_buffer_ptr"] = pre_ss.str();
                            stage_entry["nxt_pipe_buffer_ptr"] = nxt_ss.str();
                        }
                        buffer_entry[std::to_string(stage_idx)] = stage_entry;
                    }
                    tensor_entry[std::to_string(buffer_idx)] = buffer_entry;
                }
                js_shared_spm[std::to_string(tensor_id)] = tensor_entry;
            }
            js["shared_spm"] = js_shared_spm;
        }

        {
            nlohmann::json js_entry_all_ringbuffer;
            for (const auto& [tensor_id, mp]: tensorId2EntryAllRingbufferPipebuffer) {
                nlohmann::json tensor_entry;
                for (const auto& [stage_idx, p]: mp) {
                    const auto& [pipe_buffer, now_tag] = p;
                    nlohmann::json stage_entry;
                    stage_entry["tensor_id"] = tensor_id;
                    stage_entry["stage_idx"] = stage_idx;
                    stage_entry["now_tag"] = now_tag;
                    if (with_pipebuffer) {
                        stage_entry["pipe_buffer"] = pipe_buffer->toJson();
                    } else {
                        std::ostringstream ss;
                        ss << (&pipe_buffer);
                        stage_entry["pipe_buffer_ptr"] = ss.str();
                    }
                    tensor_entry[std::to_string(stage_idx)] = stage_entry;
                }
                js_entry_all_ringbuffer[std::to_string(tensor_id)] = tensor_entry;
            }
            js["entry_all_ringbuffer"] = js_entry_all_ringbuffer;
        }

        {
            nlohmann::json js_export_all_ringbuffer;
            for (const auto& [tensor_id, mp]: tensorId2ExportAllRingbufferPipebuffer) {
                nlohmann::json tensor_entry;
                for (const auto& [stage_idx, p]: mp) {
                    const auto& [pipe_buffer, now_tag] = p;
                    nlohmann::json stage_entry;
                    stage_entry["tensor_id"] = tensor_id;
                    stage_entry["stage_idx"] = stage_idx;
                    stage_entry["now_tag"] = now_tag;
                    if (with_pipebuffer) {
                        stage_entry["pipe_buffer"] = pipe_buffer->toJson();
                    } else {
                        std::ostringstream ss;
                        ss << (&pipe_buffer);
                        stage_entry["pipe_buffer_ptr"] = ss.str();
                    }
                    tensor_entry[std::to_string(stage_idx)] = stage_entry;
                }
                js_export_all_ringbuffer[std::to_string(tensor_id)] = tensor_entry;
            }
            js["export_all_ringbuffer"] = js_export_all_ringbuffer;
        }
#endif
        return js;
    }

    std::string Pipeline::pipebufferIndexToString(bool with_pipebuffer) const {
        return pipeBufferIndexToJson(with_pipebuffer).dump(JSON_DUMP_INDENTATION);
    }

    nlohmann::json Pipeline::ringBufferToJson() const {
        nlohmann::json js;
#ifdef JSON_DBG
        js["func"] = "ringBufferToJson";
        for(const auto& [tensor_id, ring_buffer]: ring_buffer_) {
            js["tensor-" + std::to_string(tensor_id)] = ring_buffer.toJson();
        }
#endif
        return js;
    }

    std::string Pipeline::ringBufferToString() const {
        return ringBufferToJson().dump(JSON_DUMP_INDENTATION);
    }
}