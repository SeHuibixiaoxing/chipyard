#include "pipeline_stage.h"

namespace mudnac {
    Stage::Stage(uint stageLogIdx_, const StageConfig& stage_config): stageLogIdx(stageLogIdx_) {
        resetALLStage();
        initStage(stage_config.model_, stage_config.stage_mapping_, stage_config.subbatch_size_, stage_config.acc_, stage_config.tensor_spm_ppages_);
        for (const auto pipe_buffer: stage_config.pipeline_buffers_) {
            configPipelineBuffer(pipe_buffer);
        }
    }

    void Stage::resetALLStage() {
        stageMapping = NULL;
        layers.clear();
        accPool.clear();

        stage_initialized = false;

        entryTensorPipelineBuffer.clear();
        exportTensorPipelineBuffer.clear();
        tensor_spm_ppages_.clear();

        layerDone.clear();
        layerRunning.clear();

        layerRunCount.clear();

        accIdxInUse.clear();

        state = StageState::STATE_BEGINING;
        continueRun = false;
        innerTensorUsage.clear();

        fetchCmdWaiting.clear();
        vPageRangeFetchCmd.clear();

        startTime.clear();
        endTime.clear();
        preWait.clear();
        sucWait.clear();
        launchTime = 0;
        finishConfigTime = 0;
        finishFixTensorFetchTime = 0;
        hasGeneratePerf = false;

        batchProcessed = batchProcessing = 0;
    }

    void Stage::initStage(Model *model, const StageMapping *stage_mapping, uint subbatch_size, const std::vector<uint> &acc, const std::unordered_map<TensorId, std::vector<PageSetShared>> &tensor_spm_ppages) {
        stageMapping = stage_mapping;
        subBatchSize = subbatch_size;
        setLayers(model);
        setPipeAcc(acc);
        tensor_spm_ppages_ = tensor_spm_ppages;

        stage_initialized = true;
    }

    void Stage::setLayers(const Model* model) {
        assert(stageMapping != NULL);
        for(uint layerIdx = 0;layerIdx < stageMapping->layerIdList.size(); layerIdx++) {
            auto layerId = stageMapping->layerIdList[layerIdx];
            LOG(STAGE_DEBUG_FLAG) << "[stageLogIdx-" << stageLogIdx << "]" << "Stage::setLayers: add layerIdx=" << layerIdx << ",layerId=" << layerId << " in stage" << std::endl;
            auto layer = model->layers[layerId];
            layers.push_back(layer);
            layer->setPipelineMode();
            layer->resetAllState();
            layer->setPipeLayerMapping(stageMapping->layerMapping[layerIdx]);
            layer->setPipeLayerMappingForLazyFetch(stageMapping->layerMappingLazyFetch[layerIdx]);
        }
        layerDone.resize(stageMapping->layerIdList.size(), 0);
        layerRunning.resize(stageMapping->layerIdList.size(), 0);
        layerRunCount.resize(stageMapping->layerIdList.size(), 0);
    }

    void Stage::setPipeAcc(const std::vector<uint>& acc) {
        accPool = acc;
        accIdxInUse.resize(acc.size(), false);
        assert(accPool.size() == stageMapping->accUtil);

        // 如果指定了加速器，检查被分配的是否是指定的加速器
        if (!stageMapping->pAccelIdx.empty()) {
            assert(stageMapping->pAccelIdx.size() == 1);
            for (uint i = 0;i < stageMapping->pAccelIdx[0].size();i++) {
                assert(accPool[i] == stageMapping->pAccelIdx[0][i]);
            }
        }
    }

    void Stage::configPipelineBuffer(PipelineBuffer* pipeline_buffer) {
        if (stageMapping->isEntryTensor(pipeline_buffer->tensorId)) {
            entryTensorPipelineBuffer.emplace(pipeline_buffer->tensorId, pipeline_buffer);
        } else if(stageMapping->isExportTensor(pipeline_buffer->tensorId)) {
            exportTensorPipelineBuffer.emplace(pipeline_buffer->tensorId, pipeline_buffer);
        } else {
            assert(false);
        }
    }

    void Stage::disableRun()
    {
        continueRun = false;
    }

    // 辅助函数
    bool Stage::isEntryTensorInStage(TensorId tensorId) {
        return stageMapping->isEntryTensor(tensorId);
    }
    bool Stage::isExportTensorInStage(TensorId tensorId){
        return stageMapping->isExportTensor(tensorId);
    }
    bool Stage::isFixedTensorInStage(TensorId tensorId) {
        return stageMapping->isFixedTensor(tensorId);
    }
    bool Stage::isInnerIsolateTensorInStage(TensorId tensorId) {
        return stageMapping->isInnerIsolateTensor(tensorId);
    }
    bool Stage::isInnerSharedTensorInStage(TensorId tensorId) {
        return stageMapping->isInnerSharedTensor(tensorId);
    }

    uint Stage::getBatchProcessing() {
        return batchProcessing;
    }
    uint Stage::getBatchProcessed() {
        return batchProcessed;
    }

    bool Stage::getContinueRun()
    {
        return continueRun;
    }

    void Stage::generatePerformance()
    {
        if (hasGeneratePerf) {
            return;
        }
        assert(startTime.size() == endTime.size());
        preWait.resize(startTime.size(), 0);
        sucWait.resize(startTime.size(), 0);
        duration.resize(startTime.size(), 0);
        auto preTime = finishFixTensorFetchTime;

        for (uint i = 0;i < startTime.size();++ i) {
            preWait[i] = startTime[i] - preTime;
            duration[i] = endTime[i] - startTime[i];
            if (i < startTime.size() - 1) {
                sucWait[i] = startTime[i + 1] - endTime[i];
            } else {
                sucWait[i] = 0;
            }
            preTime = endTime[i];
        }

        hasGeneratePerf = true;
    }

    std::string Stage::getPerformance() {
        if (!hasGeneratePerf) {
            generatePerformance();
        }
        std::stringstream ss;
        // 输出摘要信息
        ss << "stageLogIdx=" << stageLogIdx << ", performance: \n";
        ss << "launchTime=" << launchTime 
           << ", finishConfigTime=" << finishConfigTime
           << ", finishFixTensorFetchTime=" << finishFixTensorFetchTime
           << ", totalSubBatch=" << startTime.size()
           << "\n";
        if (startTime.empty()) {
            return ss.str();
        }

        // 输出表头
        static const std::vector<std::string> headers = {"start", "end", "preWait", "duration", "sucWait"};
        for (const auto& header : headers) {
            ss << std::left << std::setw(12) << header;
        }
        ss << "\n";

        // 输出每一行的性能数据
        for (uint i = 0; i < startTime.size(); ++i) {
            ss << std::left << std::setw(12) << startTime[i];
            ss << std::left << std::setw(12) << endTime[i];
            ss << std::left << std::setw(12) << preWait[i];
            ss << std::left << std::setw(12) << duration[i];
            ss << std::left << std::setw(12) << sucWait[i];
            ss << "\n";
        }

        return ss.str();
    }

    bool Stage::tick(){
        switch(state) {
            case StageState::STATE_BEGINING: {
                LOG(STAGE_DEBUG_FLAG) << "[stageLogIdx-" << stageLogIdx << "]" << "StageState::STATE_BEGINING: stage begin!" << std::endl;
                continueRun = true;
                launchTime = RT.getCycles();
                startTime.clear();
                endTime.clear();

                stateChange(StageState::STATE_CONFIG);
                break;
            }
            case StageState::STATE_CONFIG: {
                // 初始化
                if(!stage_initialized) {
                    return false;
                }
                LOG(STAGE_DEBUG_FLAG) << "[stageLogIdx-" << stageLogIdx << "]" << "StageState::STATE_CONFIG: stage initialization finish!" << std::endl;
                // 检查pipeline buffer是否全部配置完成
                for (auto tensor_id: stageMapping->entryTensorId) {
                    if (entryTensorPipelineBuffer.find(tensor_id) == entryTensorPipelineBuffer.end()) {
                        return false;
                    }
                }
                for (auto tensor_id: stageMapping->exportTensorId) {
                    if (exportTensorPipelineBuffer.find(tensor_id) == exportTensorPipelineBuffer.end()) {
                        return false;
                    }
                }
                LOG(STAGE_DEBUG_FLAG) << "[stageLogIdx-" << stageLogIdx << "]" << "StageState::STATE_CONFIG: stage pipeline buffer config finish!" << std::endl;
                stateChange(StageState::STATE_FETHCH_FIX_TENSOR);
                break;
            }
            case StageState::STATE_FETHCH_FIX_TENSOR: {
                LOG(STAGE_DEBUG_FLAG) << "[stageLogIdx-" << stageLogIdx << "]" << "StageState::STATE_FETHCH_FIX_TENSOR: 读取固定张量.";
                // 设置固定张量的临时spm页表，用于fetch指令
                for(uint i = 0;i < stageMapping->fixTensorDramBypassId.size();i++) {
                    TensorId tensorId = stageMapping->fixTensorDramBypassId[i];
                    // 如果采用lazyfetch，此处不读取weight
                    auto it = stageMapping->tensorId2UseLazyFetch.find(tensorId);
                    if (it == stageMapping->tensorId2UseLazyFetch.end() || !it->second) {
                        assert(tensor_spm_ppages_[tensorId].size() == 1);
                        auto pageSet = tensor_spm_ppages_[tensorId][0];
                        uint acc = accPool[RT.GetMinimumAccIdx(accPool, RT.accelFetchCmdCount)];
                        auto [vPageStart, vPageEnd] = RT.spm_vpage_manager_[acc].getAvailableInterval(pageSet->size());
                        assert(vPageEnd - vPageStart + 1 == pageSet->size());
                        for(uint j = 0;j < pageSet->size();++ j) {
                            setAccSpmPage(acc, vPageStart + j, pageSet->at(j));
                        }
                        sendFetchCmd(acc, tensorId, vPageStart);
                        vPageRangeFetchCmd.emplace(tensorId, std::make_pair(vPageStart, vPageEnd));
                        LOG(STAGE_DEBUG_FLAG) << "发送固定张量指令.tensoId=" << tensorId
                                    << ", acc=" << acc
                                    << ", vPageStart=" << vPageStart 
                                    << ", vPageEnd=" << vPageEnd;
                    } else {
                        LOG(STAGE_DEBUG_FLAG) << "tensorId=" << tensorId
                                << ", (it == stageMapping->tensorId2UseLazyFetch.end())=" << (it == stageMapping->tensorId2UseLazyFetch.end())
                                << ", stageMapping->tensorId2UseLazyFetch[tensorId]=" << it->second
                                << ", 采用lazyFetch,跳过Fetch阶段";
                    }
                }
                LOG(STAGE_DEBUG_FLAG) << "固定张量指令发送完成." << std::endl;
                stateChange(StageState::STATE_WAIT_FETCH);
                break;
            }
            case StageState::STATE_WAIT_FETCH: {
                waitFetchCmd();
                if(fetchCmdWaiting.empty()) {
                    LOG(STAGE_DEBUG_FLAG) << "[stageLogIdx-" << stageLogIdx << "]" << "StageState::STATE_WAIT_FETCH: fetchCmdWaiting.empty()==true, 所有不使用lazyFetch的固定张量读取完成" << std::endl;
                    assert(vPageRangeFetchCmd.empty());
                    stateChange(StageState::STATE_PIPELINE_WAITING);
                    finishFixTensorFetchTime = RT.getCycles();
                }
                break;
            }
            case StageState::STATE_PIPELINE_WAITING: {
                // 如果continueRun=false，不再执行stage
                if(!continueRun) {
                    LOG(STAGE_DEBUG_FLAG) << "[stageLogIdx-" << stageLogIdx << "]" << "StageState::STATE_PIPELINE_WAITING: continueRun=" << continueRun << ". stage结束。" << std::endl;
                    stateChange(STATE_ENDING);
                    return false;
                }

                // 等待入口buffer中数据准备.所有使用中的入口buffer数据准备完毕，才能开始执行
                for(auto& [tensorId, pipeBuffer]: entryTensorPipelineBuffer) {
                    if(!pipeBuffer->inUseBufferFull()) {
                        return false;
                    }
                    LOG(STAGE_DEBUG_DETAIL_FLAG) << "[stageLogIdx-" << stageLogIdx << "]" << "StageState::STATE_PIPELINE_WAITING: 入口张量tensorId=" << tensorId << "，inUseIdx=" << pipeBuffer->inUseIdx << "准备完成";
                }
                // 等待出口buffer空闲.所有使用中的出口buffer数据已空（全部发送出去），才能开始执行
                for(auto& [tensorId, pipeBuffer] : exportTensorPipelineBuffer) {
                    if(pipeBuffer->inUseBufferFull()) {
                        return false;
                    }
                    LOG(STAGE_DEBUG_DETAIL_FLAG) << "[stageLogIdx-" << stageLogIdx << "]" << "StageState::STATE_PIPELINE_WAITING: 出口张量tensorId=" << tensorId << "，inUseIdx=" << pipeBuffer->inUseIdx << "为空";
                }
                LOG(STAGE_DEBUG_FLAG) << "[stageLogIdx-" << stageLogIdx << "]" << "StageState::STATE_PIPELINE_WAITING: 入口张量准备完成，出口张量为空，开始执行" << std::endl;
                startTime.push_back(RT.getCycles());
                batchProcessing += subBatchSize;
                stateChange(StageState::STATE_LAYER_EXECUTE);
                break;
            }
            case StageState::STATE_LAYER_EXECUTE: {
                for(uint layerIdx = 0;layerIdx < layers.size();++ layerIdx) {
                    // 获取可执行层
                    auto layer = layers[layerIdx];

                    // 如果已经执行过，或正在执行中，跳过
                    if(layerDone[layerIdx] || layerRunning[layerIdx]) continue;

                    // 如果加速器被占用，跳过
                    bool accOk = true;
                    for(auto accIdx: stageMapping->vAccelIdx[layerIdx]) {
                        if(accIdxInUse[accIdx]) {
                            accOk = false;
                            break;
                        }
                    }
                    if(!accOk) {
                        continue;
                    }

                    // 依赖的input/output tensor是否已经准备完成
                    auto layerInputTensorIds = stageMapping->entryTensorId;
                    auto layerOutputTensorIds = stageMapping->exportTensorId;
                    bool inputTensorReady = true;
                    for(auto tensorId: layerInputTensorIds) {
                        //不会是出口张量
                        assert(!isExportTensorInStage(tensorId));
                        //入口张量已经在前面检查过（一定完成）
                        if(isEntryTensorInStage(tensorId)) {
                            assert(entryTensorPipelineBuffer.at(tensorId)->inUseBufferFull());
                        }
                        //inner isolate、shared是否已经算完
                        if(isInnerIsolateTensorInStage(tensorId) || isInnerSharedTensorInStage(tensorId)) {
                            if(innerTensorUsage.find(tensorId) == innerTensorUsage.end()) {
                                inputTensorReady = false;
                                break;
                            }
                        }
                    }
                    if(!inputTensorReady) {
                        continue;
                    }

                    LOG(STAGE_DEBUG_FLAG) << "[stageLogIdx-" << stageLogIdx << "]" << "StageStage::STATE_LAYER_EXECUTE: 层开始执行, layerIdxInStage=" << layerIdx;
                    // 配置层加速器
                    std::vector<uint> layerAccSets;
                    layerAccSets.reserve(stageMapping->vAccelIdx[layerIdx].size());
                    std::transform(
                        stageMapping->vAccelIdx[layerIdx].begin(), 
                        stageMapping->vAccelIdx[layerIdx].end(), 
                        std::back_inserter(layerAccSets),
                        [&](int i){return accPool[i];});
                    std::for_each(stageMapping->vAccelIdx[layerIdx].begin(), stageMapping->vAccelIdx[layerIdx].end(), [this](auto accIdx){this->accIdxInUse[accIdx] = true;});
                    layer->setPipeAcc(layerAccSets);

                    LOG(STAGE_DEBUG_FLAG) << "[stageLogIdx-" << stageLogIdx << "]" << "StageStage::STATE_LAYER_EXECUTE: 加速器分配结果layerAccSets=" << mudnac::toString(layerAccSets) 
                            << ", accPool=" << mudnac::toString(accPool)
                            << ", stageMapping->vAccelIdx=" << mudnac::toString(stageMapping->vAccelIdx[layerIdx])
                            << ", accIdxInUse(分配结束后)=" << mudnac::toString(accIdxInUse);

                    // 为输出inner shared类型分配物理页，并创建使用记录
                    // for(auto tensorId: layer->getOutputTensorIds()) {
                    //     if(isInnerSharedTensorInStage(tensorId)) {
                    //         // 创建使用记录
                    //         assert(innerTensorUsage.find(tensorId) == innerTensorUsage.end());
                    //         innerTensorUsage.insert({tensorId, stageMapping->tensorId2UsageCount.at(tensorId)});

                    //         LOG(STAGE_DEBUG_FLAG) << "[stageLogIdx-" << stageLogIdx << "]" << "StageStage::STATE_LAYER_EXECUTE: inner shared类型输出张量，创建待使用次数记录,tensorId=" << tensorId
                    //             << ", innerTensorUsage=" << innerTensorUsage.at(tensorId);
                    //     }
                    // }
                    
                    // 为所有layer tensor设置页表
                    for(uint tensorIdx = 0;tensorIdx < layer->tensorIds.size();++ tensorIdx) {
                        TensorId tensorId = layer->tensorIds[tensorIdx];
                        PageSetShared spmPPageSet = NULL;
                        if(entryTensorPipelineBuffer.find(tensorId) != entryTensorPipelineBuffer.end()) {
                            spmPPageSet = entryTensorPipelineBuffer.at(tensorId)->getPSpmPagesInUse();
                        } else if(exportTensorPipelineBuffer.find(tensorId) != exportTensorPipelineBuffer.end()) {
                            spmPPageSet = exportTensorPipelineBuffer.at(tensorId)->getPSpmPagesInUse();
                        } else if(tensor_spm_ppages_.find(tensorId) != tensor_spm_ppages_.end()) {
                            assert(tensor_spm_ppages_.at(tensorId).size() == 1);
                            spmPPageSet = tensor_spm_ppages_.at(tensorId)[0];
                        } else {
                            assert(false);
                        }
                        layer->setPipeSpmForAcc(tensorIdx, *spmPPageSet);
                        LOG(STAGE_DEBUG_FLAG) << "[stageLogIdx-" << stageLogIdx << "]" << "StageStage::STATE_LAYER_EXECUTE: 设置页表,tensorId=" << tensorId
                                << ", spmPPageSet.size=" << spmPPageSet->size()
                                << ", spmPPageSet.front()=" << spmPPageSet->front()
                                << ", spmPPageSet.back()=" << spmPPageSet->back();
                    }

                    // 改变layer mapping方法
                    if(layerRunCount[layerIdx] == 0 || layerRunCount[layerIdx] == 1) {
                        layer->swapLayerMappingForLazyFetch();
                        LOG(STAGE_DEBUG_FLAG) << "[stageLogIdx-" << stageLogIdx << "]" << "StageStage::STATE_LAYER_EXECUTE: 交换layer mapping.layerIdx=" << layerIdx 
                                << ", layerRunCount[layerIdx]=" << layerRunCount[layerIdx]
                                << ", layer->mapping=" << layer->mapping->toString();
                    }
                    
                    // 更新层状态
                    layerRunning[layerIdx] = 1;
                    layerDone[layerIdx] = 0;
                    ++ layerRunCount[layerIdx];

                    LOG(STAGE_DEBUG_FLAG) << "[stageLogIdx-" << stageLogIdx << "]" << "StageStage::STATE_LAYER_EXECUTE: 层状态更新,layerIdx=" << layerIdx 
                            << ", layerRunning[layerIdx]=" << layerRunning[layerIdx]
                            << ", layerDone[layerIdx]=" << layerDone[layerIdx]
                            << ", layerRunCount[layerIdx]=" << layerRunCount[layerIdx]
                            << std::endl;
                }

                
                // tick所有执行中的层，并更新状态
                for(uint layerIdx = 0;layerIdx < layers.size();++ layerIdx) {
                    if(layerRunning[layerIdx]) {
                        assert(!layerDone[layerIdx]);
                        auto layerOk = layers[layerIdx]->tick();
                        // tick()返回true, 说明执行结束
                        if(layerOk) {
                            LOG(STAGE_DEBUG_FLAG) << "[stageLogIdx-" << stageLogIdx << "]" << "StageStage::STATE_LAYER_EXECUTE: 层执行完成。layerIdx=" << layerIdx 
                                    << ", layerOk=" << layerOk;
                            // 设置层状态记录
                            layerRunning[layerIdx] = false;
                            layerDone[layerIdx] = true;

                            LOG(STAGE_DEBUG_FLAG) << "[stageLogIdx-" << stageLogIdx << "]" << "layerRunning=" << layerRunning[layerIdx]
                                    << ", layerDone=" << layerDone[layerIdx];
                            // 重置层状态机
                            layers[layerIdx]->resetStateForPipelineReuse();

                            // 删除页表
                            auto layer = layers[layerIdx];
                            for(uint tensorIdx = 0;tensorIdx < layer->tensorIds.size();++ tensorIdx) {
                                layer->invalidSpmVPageForAcc(tensorIdx);
                                LOG(STAGE_DEBUG_FLAG) << "[stageLogIdx-" << stageLogIdx << "]" << "删除页表，tensorId=" << layer->tensorIds[tensorIdx]
                                        << ", tensorIdx=" << tensorIdx;
                            }

                            // 最后释放加速器
                            layer->releasePipeAcc();

                            std::vector<uint> layerAccSets;
                            layerAccSets.reserve(stageMapping->vAccelIdx[layerIdx].size());
                            std::transform(
                                stageMapping->vAccelIdx[layerIdx].begin(), 
                                stageMapping->vAccelIdx[layerIdx].end(), 
                                std::back_inserter(layerAccSets),
                                [&](int i){return accPool[i];});
                            std::for_each(stageMapping->vAccelIdx[layerIdx].begin(), stageMapping->vAccelIdx[layerIdx].end(), [this](auto accIdx){
                                assert(this->accIdxInUse[accIdx]);
                                this->accIdxInUse[accIdx] = false;
                            });

                            LOG(STAGE_DEBUG_FLAG) << "[stageLogIdx-" << stageLogIdx << "]" << "删除加速器在Stage中的占用记录，更新后accIdxInUse=" << mudnac::toString(accIdxInUse) 
                                    << ", accPool=" << mudnac::toString(accPool)
                                    << ", stageMapping->vAccelIdx=" << mudnac::toString(stageMapping->vAccelIdx[layerIdx])
                                    << ", accIdxInUse(分配结束后)=" << mudnac::toString(accIdxInUse);
                            
                            // 输入inner shared类型tensor，查询剩余次数记录，等于0时释放并删除记录
                            // for(auto tensorId: layer->getInputTensorIds()) {
                            //     if(isInnerSharedTensorInStage(tensorId)) {
                            //         assert(innerTensorUsage.find(tensorId) != innerTensorUsage.end());
                            //         auto& value = innerTensorUsage.at(tensorId);
                            //         assert(value > 0);
                            //         -- value;
                            //         LOG(STAGE_DEBUG_FLAG) << "[stageLogIdx-" << stageLogIdx << "]" << "inner shared类型输入，更新待使用次数记录,tensorId=" << tensorId
                            //                 << ", innerTensorUsage(更新后)=" << innerTensorUsage.at(tensorId);
                            //         if(value == 0) {
                            //             innerTensorUsage.erase(tensorId);
                            //             LOG(STAGE_DEBUG_FLAG) << "[stageLogIdx-" << stageLogIdx << "]" << "inner shared类型输入，使用次数为0，删除,tensorId=" << tensorId;
                            //         }
                            //     }
                            // }
                        }
                    }
                }

                bool allLayerDone = true;
                for (uint layerIdx = 0; layerIdx < layers.size();++ layerIdx) {
                    if(!layerDone[layerIdx]) {
                        allLayerDone = false;
                        break;
                    }
                }
                if (allLayerDone) {
                    // 所有层执行结束，设置输出tensor full和输入tensor empty
                    LOG(STAGE_DEBUG_FLAG) << "[stageLogIdx-" << stageLogIdx << "]" << "所有层执行结束，设置输出tensor full和输入tensor empty.";
                    for(auto& [tensorId, pipeBuffer]: exportTensorPipelineBuffer) {
                        pipeBuffer->bufferHasFullData[pipeBuffer->inUseIdx] = true;
                        LOG(STAGE_DEBUG_FLAG) << "[stageLogIdx-" << stageLogIdx << "]" << "pipeBuffer状态:tensorId=" << tensorId
                                << ", pipeBuffer:" << pipeBuffer->toString();
                    }
                    for(auto& [tensorId, pipeBuffer]: entryTensorPipelineBuffer) {
                        pipeBuffer->bufferHasFullData[pipeBuffer->inUseIdx] = false;
                        LOG(STAGE_DEBUG_FLAG) << "[stageLogIdx-" << stageLogIdx << "]" << "pipeBuffer状态:tensorId=" << tensorId
                                << ", pipeBuffer:" << pipeBuffer->toString();
                    }
                    // 重置Layer状态
                    for(uint layerIdx = 0; layerIdx < layers.size();++ layerIdx) {
                        assert(layerDone[layerIdx]);
                        assert(!layerRunning[layerIdx]);
                        layerDone[layerIdx] = false;
                        layerRunning[layerIdx] = false;
                        LOG(STAGE_DEBUG_FLAG) << "[stageLogIdx-" << stageLogIdx << "]" << "layerRunning=" << layerRunning[layerIdx]
                                << ", layerDone=" << layerDone[layerIdx];
                    }
                    stateChange(StageState::STATE_PIPELINE_LOOP);
                }
                break;
            }
            case StageState::STATE_PIPELINE_LOOP: {
                LOG(STAGE_DEBUG_FLAG)<< "[stageLogIdx-" << stageLogIdx << "]" << "StageStage::STATE_PIPELINE_LOOP: 交换pipe buffer。" << std::endl;

                // 交换buffer
                for(auto& [tensorId, pipeBuffer]: exportTensorPipelineBuffer) {
                    std::swap(pipeBuffer->inUseIdx, pipeBuffer->noUseIdx);
                    LOG(STAGE_DEBUG_FLAG) << "[stageLogIdx-" << stageLogIdx << "]" << "出口tensorId=" << tensorId << ",pipeBuffer=" << pipeBuffer->toString() << std::endl;
                }
                for(auto& [tensorId, pipeBuffer]: entryTensorPipelineBuffer) {
                    std::swap(pipeBuffer->inUseIdx, pipeBuffer->noUseIdx);
                    LOG(STAGE_DEBUG_FLAG) << "[stageLogIdx-" << stageLogIdx << "]" << "入口tensorId=" << tensorId << ",pipeBuffer=" << pipeBuffer->toString() << std::endl;
                }
                
                endTime.push_back(RT.getCycles());
                batchProcessing -= subBatchSize;
                batchProcessed += subBatchSize;
                stateChange(StageState::STATE_PIPELINE_WAITING);
                break;
            }
            case StageState::STATE_ENDING: {
                break;
            }
        }
        return state == StageState::STATE_ENDING;
    }

    void Stage::stateChange(StageState state_) {
        LOG(STAGE_DEBUG_FLAG) << "[stageLogIdx-" << stageLogIdx << "]" << "Stage::stateChange: stage state change from " << state << " to " << state_ << std::endl;
        state = state_;
    }
    
    void Stage::setAccSpmPage(uint acc_id, uint vPage, uint pPage) {
        dynamic_cast<BaseSPMSystem *>(RT.system)->accels[acc_id].setPageTable(vPage, pPage, true);
    }

    void Stage::setAllAccAllocatedSpmPage(uint vPage, uint pPage) {
        for(auto acc_id: accPool) {
            setAccSpmPage(acc_id, vPage, pPage);
        }
    }

    void Stage::invalidAccSpmPage(uint accId, uint vPage) {
        dynamic_cast<BaseSPMSystem *>(RT.system)->accels[accId].invalidPageTable(vPage);
    }
    
    void Stage::invalidAllAccAllocatedSpmPage(uint vPage) {
        for(auto acc_id: accPool) {
            invalidAccSpmPage(acc_id, vPage);
        }
    }

    void Stage::sendFetchCmd(uint accId, TensorId tensorId, uint vPageBase) {
        AccelSPMFetch *fetchCMD = new AccelSPMFetch();

        Tensor tensorFetch(stageMapping->tensorId2DramBaseAddr.at(tensorId)[0], stageMapping->tensorId2Strides.at(tensorId));
        Tensor spmFetchLayout(vPageBase * RT.hw.pageBytes, stageMapping->tensorId2Strides.at(tensorId));

        SPMTensor spmFetchTensor;

        spmFetchTensor.set(spmFetchLayout, false, true); // in fect, bypass and reuse are not used
        fetchCMD->shape = stageMapping->tensorId2Shape.at(tensorId);
        fetchCMD->inputTensors[0] = tensorFetch;
        fetchCMD->inputSPMTensors[0] = spmFetchTensor;
        fetchCMD->is_pipeline_cmd = true;
        RT.system->addCmd(accId, fetchCMD);

        fetchCmdWaiting.emplace_back(tensorId, accId, fetchCMD->globalCmdId);
        ++ RT.accelFetchCmdCount[accId];
        LOG(STAGE_DEBUG_FLAG) << "Stage::sendFetchCmd: " << fetchCMD->toString() << std::endl;
    }

    void Stage::waitFetchCmd() {
        fetchCmdWaiting.remove_if([this](const auto& item) {
            auto [tensor_id, acc_id, global_cmd_id] = item;
            auto cmd = RT.getFinishedCmd(acc_id, global_cmd_id, true);
            if(cmd != NULL) {
                LOG(STAGE_DEBUG_FLAG) << "[stageLogIdx-" << stageLogIdx << "]" << "Stage::waitFetchCmd: 读取到一条fetch指令结束.globalCmdId=" << cmd->globalCmdId << ", fetchCmdWaiting.size()=" << fetchCmdWaiting.size() << ", accId=" << acc_id;
                assert(global_cmd_id== cmd->globalCmdId);
                delete cmd;
                auto it = vPageRangeFetchCmd.find(tensor_id);
                assert(it != vPageRangeFetchCmd.end());
                auto [startVPage, endVPage] = it->second;
                // 释放固定张量的临时spm页表
                for(uint vPage = startVPage;vPage <= endVPage;vPage++) {
                    invalidAccSpmPage(acc_id, vPage);
                }
                vPageRangeFetchCmd.erase(it);
                LOG(STAGE_DEBUG_FLAG) << "[stageLogIdx-" << stageLogIdx << "]" << "Stage::waitFetchCmd: 释放页表,tensor_id=" << tensor_id << ", acc=" << acc_id << ", vpage=[" << startVPage << "," << endVPage << "]" << std::endl;
                return true;
            }
            return false;
        });
    }
}