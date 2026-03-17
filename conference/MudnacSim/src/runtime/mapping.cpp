#include "mapping.h"

namespace mudnac {
    bool LayerMapping::hasInSPM(uint tensorIdxInMapping) const {
        assert(spmBypass.size() > tensorIdxInMapping);
        assert(dramBypass.size() > tensorIdxInMapping);
        return !spmBypass[tensorIdxInMapping] && dramBypass[tensorIdxInMapping];
    }
    bool LayerMapping::alwaysFromDram(uint tensorIdxInMapping) const {
        assert(spmBypass.size() > tensorIdxInMapping);
        assert(dramBypass.size() > tensorIdxInMapping);
        return spmBypass[tensorIdxInMapping] && !dramBypass[tensorIdxInMapping];
    }
    bool LayerMapping::fromSpmAndDram(uint tensorIdxInMapping) const {
        assert(spmBypass.size() > tensorIdxInMapping);
        assert(dramBypass.size() > tensorIdxInMapping);
        return !spmBypass[tensorIdxInMapping] && !dramBypass[tensorIdxInMapping];
    }

    nlohmann::json LayerMapping::toJson() const {
        nlohmann::json js;
#ifdef JSON_DBG
        js["factors"] = ln.factors;
        js["permutation"] = ln.loops;
        js["targetAccUtil"] = targetAccUtil;

        js["targetSpmUtil"] = targetSpmUtil;
        js["accUtil"] = accUtil;
        js["spmUtil"] = spmUtil;
        js["lgSpmUtil"] = lgSpmUtil;
        js["lgAccUtil"] = lgAccUtil;
        js["dramAccess"] = dramAccess;
        js["psumAccumCount"] = psumAccumCount;
        js["totalTiles"] = totalTiles;
        js["spmBypass"] = spmBypass;
        js["dramBypass"] = dramBypass;
    
        auto packFuncToVec = [this](auto func, uint len) {
            std::vector<bool> re;
            for (uint i = 0;i < len;++ i) {
                re.push_back((this->*func)(i));
            }
            return re;
        };
        js["alwaysFromDram"] = packFuncToVec(&LayerMapping::alwaysFromDram, dramBypass.size());
        js["hasInSPM"] = packFuncToVec(&LayerMapping::hasInSPM, dramBypass.size());
        js["fromSpmAndDram"] = packFuncToVec(&LayerMapping::fromSpmAndDram, dramBypass.size());


        js["spmDimensions"] = spmDimensions;
        js["accDimensions"] = accDimensions;
        js["spmTensorAddr"] = spmTensorAddr;
        

        js["tensorMulticastCount"] = tensorMulticastCount;
        js["spmTensorTileCount"] = spmTensorTileCount;
#endif
        return js;
    }

    std::string LayerMapping::toString() const {
        return toJson().dump(JSON_DUMP_INDENTATION);
    }
    
    std::string StageMapping::tensorStayTypeToString(TensorStayType type) {
        switch (type) {
            case TensorStayType::DRAM:
                return "DRAM";
            case TensorStayType::DRAM_DEPEN:
                return "DRAM_DEPEN";
            case TensorStayType::ISOLATE_SPM:
                return "ISOLATE_SPM";
            case TensorStayType::SHARED_SPM:
                return "SHARED_SPM";
            case TensorStayType::ALL_RINGBUFFER:
                return "ALL_RINGBUFFER";
            case TensorStayType::UNKNOWN:
            default:
                return "UNKNOWN";
        }
    }
    StageMapping::TensorStayType StageMapping::stringToTensorStayType(const std::string& type) {
        if(type == "DRAM") {
            return StageMapping::TensorStayType::DRAM;
        } else if(type == "DRAM_DEPEN") {
            return StageMapping::TensorStayType::DRAM_DEPEN;
        } else if(type == "ISOLATE_SPM") {
            return StageMapping::TensorStayType::ISOLATE_SPM;
        } else if(type == "SHARED_SPM") {
            return StageMapping::TensorStayType::SHARED_SPM;
        } else if(type == "ALL_RINGBUFFER") {
            return StageMapping::TensorStayType::ALL_RINGBUFFER;
        }
        assert(false);
        return TensorStayType::UNKNOWN;
    }
    bool StageMapping::isEntryTensor(uint tensorId) const {
        return std::find(entryTensorId.begin(), entryTensorId.end(), tensorId) != entryTensorId.end();
    }
    bool StageMapping::isExportTensor(uint tensorId) const {
        return std::find(exportTensorId.begin(), exportTensorId.end(), tensorId) != exportTensorId.end();
    }
    bool StageMapping::isFixedTensor(uint tensorId) const {
        return std::find(fixTensorDramBypassId.begin(), fixTensorDramBypassId.end(), tensorId) != fixTensorDramBypassId.end();            
    }
    bool StageMapping::isInnerIsolateTensor(uint tensorId) const {
        return std::find(innerIsolateTensorId.begin(), innerIsolateTensorId.end(), tensorId) != innerIsolateTensorId.end();    
    }
    bool StageMapping::isInnerSharedTensor(uint tensorId) const {
        return std::find(innerSharedTensorId.begin(), innerSharedTensorId.end(), tensorId) != innerSharedTensorId.end();    
    }

    StageMapping::TensorStayType StageMapping::getTensorStayType(TensorId tensorId) const {
        for(uint i = 0;i < entryTensorId.size();++ i){
            if(entryTensorId[i] == tensorId) {
                return entryTensorType[i];
            }
        }
        for(uint i = 0;i < exportTensorId.size();++ i){
            if(exportTensorId[i] == tensorId) {
                return exportTensorType[i];
            }
        }
        assert(false);
        return TensorStayType::UNKNOWN;
    }

    uint StageMapping::getDoubleBuffer(TensorId tensorId) const {
        for(uint i = 0;i < entryTensorId.size();++ i){
            if(entryTensorId[i] == tensorId) {
                return entryTensorDoubleBuffer[i];
            }
        }
        for(uint i = 0;i < exportTensorId.size();++ i){
            if(exportTensorId[i] == tensorId) {
                return exportTensorDoubleBuffer[i];
            }
        }
        assert(false);
        return 0;
    }
    bool StageMapping::isDRAMPipeBuffer(TensorId tensorId) const {
        return getTensorStayType(tensorId) == TensorStayType::DRAM;
    }
    bool StageMapping::isDRAMDepenPipeBuffer(TensorId tensorId) const {
        return getTensorStayType(tensorId) == TensorStayType::DRAM_DEPEN;
    }
    bool StageMapping::isIsolatePipeBuffer(TensorId tensorId) const {
        return getTensorStayType(tensorId) == TensorStayType::ISOLATE_SPM;
    }
    bool StageMapping::isSharedPipeBuffer(TensorId tensorId) const {
        return getTensorStayType(tensorId) == TensorStayType::SHARED_SPM;

    }

    bool StageMapping::isAllRingbufferPipeBuffer(TensorId tensorId) const {
        return getTensorStayType(tensorId) == TensorStayType::ALL_RINGBUFFER;
    }

    bool StageMapping::isDoubleBuffer(TensorId tensorId) const {
        return getDoubleBuffer(tensorId) == 1;
    }

    nlohmann::json StageMapping::toJson() const {
        nlohmann::json js;    
#ifdef JSON_DBG
        js["v_accel_idx"] = vAccelIdx;
        js["p_accel_idx"] = pAccelIdx;
        js["acc_util"] = accUtil;

        {
            nlohmann::json entry_js;
            for(int i = 0;i < entryTensorId.size();++ i) {
                entry_js["tensor-" + std::to_string(entryTensorId[i])] = {
                    {"tensor_type", tensorStayTypeToString(entryTensorType[i])},
                    {"enable_double_buffer", entryTensorDoubleBuffer[i]},
                };
            }
            js["entry_tensor"] = entry_js;
        }

        {
            nlohmann::json export_js;
            for(int i = 0;i < exportTensorId.size();++ i) {
                export_js["tensor-" + std::to_string(exportTensorId[i])] = {
                    {"tensor_type", tensorStayTypeToString(exportTensorType[i])},
                    {"enable_double_buffer", exportTensorDoubleBuffer[i]},
                };
            }
            js["export_tensor"] = export_js;
        }
        
        js["fix_tensor_dram_bypass_id"] = fixTensorDramBypassId;
        js["inner_isolate_tensor_id"] = innerIsolateTensorId;
        js["inner_shared_tensor_id"] = innerSharedTensorId;

        js["tensor_usage_count_list"] = tensorUsageCountList;
        js["tensor_use_lazy_fetch"] = tensorUseLazyFetch;

        js["tensor_id_to_dram_base_addr"] = tensorId2DramBaseAddr;
        js["tensor_id_to_shape"] = tensorId2Shape;
        js["tensor_id_to_strides"] = tensorId2Strides;
        js["tensor_id_to_usage_count"] = tensorId2UsageCount;
        js["tensor_id_to_use_lazyfetch"] = tensorId2UseLazyFetch;

        js["layer_id_list"] = layerIdList;
        js["tensor_id_list"] = tensorIdList;
        js["dram_bypass_list"] = dramBypassList;
        js["spm_bypass_list"] = spmBypassList;

        {
            nlohmann::json layer_mapping_js;
            for(int i = 0;i < layerIdList.size();++ i) {
                if (layerMapping.size() > i && layerMapping[i] != nullptr) {
                    layer_mapping_js.push_back({
                        {"idx", i},
                        {"id", layerIdList[i]},
                        {"mapping", layerMapping[i]->toJson()}
                    });
                }
            }
            js["layer_mapping"] = layer_mapping_js;
        }
        
        {
            nlohmann::json layer_mapping_lazy_fetch_js;
            for(int i = 0;i < layerIdList.size();++ i) {
                if (layerMappingLazyFetch.size() > i && layerMappingLazyFetch[i] != nullptr) {
                    layer_mapping_lazy_fetch_js.push_back({
                        {"idx", i},
                        {"id", layerIdList[i]},
                        {"mapping", layerMappingLazyFetch[i]->toJson()}
                    });
                }
            }
            js["layer_mapping_lazy_fetch"] = layer_mapping_lazy_fetch_js;
        }
#endif
        return js;
    }

    std::string StageMapping::toString() const {
        return toJson().dump(JSON_DUMP_INDENTATION);
    }

    nlohmann::json SegmentMapping::toJson() const {
        nlohmann::json js;
#ifdef JSON_DBG
        js["segment_idx"] = segmentIdx;
        js["start_layer_idx"] = startLayerIdx;
        js["end_layer_idx"] = endLayerIdx;
        js["sub_batch_size"] = subBatchSize;
        
        // ring buffer config
        {
            nlohmann::json ring_buffer_config_js;
            for (auto& [tensorId, ring_buffer_config]: ring_buffer_config_) {
                ring_buffer_config_js["tensor-" + std::to_string(tensorId)] = ring_buffer_config.toJson();
            }
            js["ring_buffer_config"] = ring_buffer_config_js;
        }
        
        {
            nlohmann::json tensor_min_size_js;
            // tensor min info
            for (auto& [tensor_id, shape]: tensor_id_to_min_shape) {
                tensor_min_size_js["tensor-" + std::to_string(tensor_id)] = {
                    {"shape", shape},
                    {"strides", tensor_id_to_min_strides.at(tensor_id)},
                    {"spm_page_num", tensor_id_to_min_spm_page.at(tensor_id)},
                };
            }
            js["tensor_to_min_size"] = tensor_min_size_js;
        }

        {
            nlohmann::json tensor_max_size_js;
            // tensor max info
            for (auto& [tensor_id, shape]: tensor_id_to_max_shape) {
                tensor_max_size_js["tensor-" + std::to_string(tensor_id)] = {
                    {"shape", shape},
                    {"strides", tensor_id_to_max_strides.at(tensor_id)},
                    {"spm_page_num", tensor_id_to_max_spm_page.at(tensor_id)},
                };
            }
            js["tensor_to_max_size"] = tensor_max_size_js;
        }
        
        js["tensor_spm_util_in_stage"] = tensorSpmUtilInStage;
        js["tensor_spm_util_shared"] = tensorSpmUtilShared;
        js["tensor_spm_util_in_ringbuffer"] = tensorSpmUtilInRingbuffer;
        js["tensor_spm_util_weight"] = tensorSpmUtilWeight;
        js["total_spm_util"] = totalSpmUtil;
        js["acc_util"] = accUtil;
        js["cost"] = cost;
        
        // map index
        js["stage_entry_tensor_ids"] = stageEntryTensorIds;
        js["stage_export_tensor_ids"] = stageExportTensorIds;
        js["entry_tensor_to_stage_idx"] = entryTensorToStageIdx;
        js["export_tensor_to_stage_idx"] = exportTensorToStageIdx;
        
        // no indegree/outdegree
        {
            nlohmann::json entry_tensor_no_indegree_js;
            for (uint stageIdx = 0; stageIdx < entryTensorNoIndegree.size(); ++stageIdx) {
                entry_tensor_no_indegree_js["stage-" + std::to_string(stageIdx)] = entryTensorNoIndegree[stageIdx];
            }
            js["entry_tensor_no_indegree"] = entry_tensor_no_indegree_js;
        }
        
        {
            nlohmann::json export_tensor_no_outdegree_js;
            for (uint stageIdx = 0; stageIdx < exportTensorNoOutdegree.size(); ++stageIdx) {
                export_tensor_no_outdegree_js["stage-" + std::to_string(stageIdx)] = exportTensorNoOutdegree[stageIdx];
            }
            js["export_tensor_no_outdegree"] = export_tensor_no_outdegree_js;
        }
        
        js["input_tensor_id_stage"] = input_tensor_id_stage;
        js["output_tensor_id_stage"] = output_tensor_id_stage;
        
        nlohmann::json stages_js = nlohmann::json::array();
        for (const auto& stage : stage_mapping_vec) {
            stages_js.push_back(stage.toJson());
        }
        js["stage_mapping_vec"] = stages_js;
#endif
        return js;
    }

    std::string SegmentMapping::toString() const {
        return toJson().dump(JSON_DUMP_INDENTATION);
    }

    nlohmann::json SegmentMapping::RingBufferConfig::toJson() const {
        nlohmann::json js;
#ifdef JSON_DBG
        js = nlohmann::json{
            {"use", use_},
            {"size", size_},
            {"out_degree", out_degree_}
        };
#endif
        return js;
    }

    nlohmann::json PipelineMapping::toJson() const {
        nlohmann::json js;
#ifdef JSON_DBG
        js["segment_candidates"] = segmentCandidates.size();
        js["pipeline_cost"] = pipelineCost;
        
        // Target相关字段
        nlohmann::json target_js;
        target_js["num_arrays"] = targetNumArrays;
        target_js["spm_kb"] = targetSpmKB;
        target_js["spm_kb_per_array"] = targetSpmKBPerArray;
        target_js["num_macs_per_array"] = targetNumMacsPerArray;
        target_js["dram_bw_per_cycle"] = targetDramBwPerCycle;
        target_js["noc_bw_per_cycle"] = targetNocBwPerCycle;
        target_js["default_batch"] = targetDefaultBatch;
        js["target"] = target_js;
        
        nlohmann::json segments_js = nlohmann::json::array();
        for (const auto& segment : segmentCandidates) {
            segments_js.push_back(segment.toJson());
        }
        js["segment_candidates"] = segments_js;
#endif
        return js;
    }

    std::string PipelineMapping::toString() const {
        return toJson().dump(JSON_DUMP_INDENTATION);
    }

    void SegmentMapping::createMap() {
        assert(!stage_mapping_vec.empty());
        // 处理拓扑关系
        stageExportTensorIds.resize(stage_mapping_vec.size());
        for (uint stageIdx = 0; stageIdx < stage_mapping_vec.size(); ++stageIdx) {
            const auto& stageMapping = stage_mapping_vec[stageIdx];
            for (auto tensorId: stageMapping.exportTensorId) {
                exportTensorToStageIdx[tensorId].push_back(stageIdx);
                stageExportTensorIds[stageIdx].push_back(tensorId);
            }
        }

        stageEntryTensorIds.resize(stage_mapping_vec.size());
        for (uint stageIdx = 0; stageIdx < stage_mapping_vec.size(); ++stageIdx) {
            const auto& stageMapping = stage_mapping_vec[stageIdx];
            for (auto tensorId: stageMapping.entryTensorId) {
                entryTensorToStageIdx[tensorId].push_back(stageIdx);
                stageEntryTensorIds[stageIdx].push_back(tensorId);
            }
        }

        entryTensorNoIndegree.resize(stage_mapping_vec.size());
        exportTensorNoOutdegree.resize(stage_mapping_vec.size());
        for (uint stageIdx = 0; stageIdx < stage_mapping_vec.size(); ++stageIdx) {
            const auto& stageMapping = stage_mapping_vec[stageIdx];
            for (auto tensorId: stageMapping.entryTensorId) {
                // 如果入口tensor不作为任何出口tensor出现，则认为入度0
                if (exportTensorToStageIdx.find(tensorId) == exportTensorToStageIdx.end()) {
                    entryTensorNoIndegree[stageIdx].insert(tensorId);
                    input_tensor_id_stage.emplace_back(stageIdx, tensorId);
                }
            }

            for (auto tensorId: stageMapping.exportTensorId) {
                // 如果出口tensor不作为任何入口tensor出现，则出度为0
                if (entryTensorToStageIdx.find(tensorId) == entryTensorToStageIdx.end()) {
                    exportTensorNoOutdegree[stageIdx].insert(tensorId);
                    output_tensor_id_stage.emplace_back(stageIdx, tensorId);
                }
            }
        }

        // 创建tensor最小/最大spm需求map
        for (uint stage_idx = 0; stage_idx < stage_mapping_vec.size(); ++stage_idx) {
            const auto& stage_mapping = stage_mapping_vec[stage_idx];
            std::vector<TensorId> tensor_ids;
            tensor_ids.insert(tensor_ids.end(), stage_mapping.entryTensorId.begin(), stage_mapping.entryTensorId.end());
            tensor_ids.insert(tensor_ids.end(), stage_mapping.exportTensorId.begin(), stage_mapping.exportTensorId.end());
            for (auto tensor_id: tensor_ids) {
                auto min_it = tensor_id_to_min_spm_page.find(tensor_id);
                if (min_it == tensor_id_to_min_spm_page.end() || min_it->second > stage_mapping.tensorId2SpmPageNums.at(tensor_id)) {
                    tensor_id_to_min_shape[tensor_id] = stage_mapping.tensorId2Shape.at(tensor_id);
                    tensor_id_to_min_strides[tensor_id] = stage_mapping.tensorId2Strides.at(tensor_id);
                    tensor_id_to_min_spm_page[tensor_id] = stage_mapping.tensorId2SpmPageNums.at(tensor_id);
                }

                auto max_it = tensor_id_to_max_spm_page.find(tensor_id);
                if (max_it == tensor_id_to_max_spm_page.end() || max_it->second < stage_mapping.tensorId2SpmPageNums.at(tensor_id)) {
                    tensor_id_to_max_shape[tensor_id] = stage_mapping.tensorId2Shape.at(tensor_id);
                    tensor_id_to_max_strides[tensor_id] = stage_mapping.tensorId2Strides.at(tensor_id);
                    tensor_id_to_max_spm_page[tensor_id] = stage_mapping.tensorId2SpmPageNums.at(tensor_id);
                }
            }
        }
    }

}