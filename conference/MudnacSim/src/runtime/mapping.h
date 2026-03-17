#ifndef MUDNACSIM_MAPPING_H
#define MUDNACSIM_MAPPING_H


#include "tools.h"
#include "yaml-cpp/yaml.h"
#include "loopnest.h"


namespace mudnac {
    class PipelineMapping;
    class LayerMapping;
    class StageMapping;

    typedef std::shared_ptr<PipelineMapping> PipelineMappingShared;
    typedef std::shared_ptr<LayerMapping> LayerMappingShared;
    typedef std::shared_ptr<StageMapping> StageMappingShared;

    // 层mapping。由HybridMapper产生
    class LayerMapping {
    public:
        bool mappingForPipeline; // 【new】是否是pipeline使用的映射。如果为true，则spm地址页面对齐

        LoopNest ln;

        uint64 targetAccUtil;
        uint64 targetSpmUtil;
        uint64 accUtil;
        uint64 spmUtil;
        uint64 spmPageUtil; // 【new】spm页数
        uint64 lgSpmUtil;
        uint64 lgAccUtil;

        uint64 dramAccess;
        uint64 psumAccumCount;
        uint64 totalTiles;

        std::vector<uint64> spmBypass;  // Format: [tensor0 bypass, tensor1 bypass, ...]. Value range: {0, 1}.
        std::vector<uint64> dramBypass;  // Format: [tensor0 bypass, tensor1 bypass, ...]. Value range: {0, 1}.

        std::vector<uint64> spmDimensions;  // Format: [dim0, dim1, dim2, ...].
        std::vector<uint64> accDimensions;  // Format: [dim0, dim1, dim2, ...].

        std::vector<uint64> spmTensorAddr;  // Format: [tensor0 addr, tensor1 addr, ...]. 新增限制：要求对齐页面大小
        std::vector<uint> spmTensorPageCount; // // 【new】Format: [tensor0 spm page counts, tensor1 spm spage counts, ...].
        std::vector<uint> firstTensorPageNum; // // 【new】Format: [tensor0 first vspm page number, tensor1 first vspm spage number, ...].

        std::vector<uint64> tensorMulticastCount;  // Format: [tensor0 multicast, tensor1 multicast, ...].
        std::vector<uint64> spmTensorTileCount;  // Format: [tensor0 tiles, tensor1 tiles, ...].

        uint64 subBatch; // batch size for this mapping. only use for pipeline. 此值目前始终为1

        bool hasInSPM(uint tensorIdxInMapping) const;
        bool alwaysFromDram(uint tensorIdxInMapping) const;
        bool fromSpmAndDram(uint tensorIdxInMapping) const;

        nlohmann::json toJson() const;
        std::string toString() const;
    };

    
    // 流水线阶段Mapping信息
    class StageMapping {
    public:
        enum class TensorStayType {
            DRAM, // entry tensor: load from dram; export tensor: flush to dram。该类型pipe tensor是整个pipeline的入口和出口，当有数据能输出时则写回dram，有batch供给时就开始读入
            DRAM_DEPEN, // 该类型pipe tensor是流水线内部的tensor，必须在出口pipebuffer写回后，对应入口pipebuffer才能读入。通过比较它们的batchOffset实现
            ISOLATE_SPM, // entry tensor: send from last stage spm; export tensor: send to the next stage spm
            SHARED_SPM, // entry tensor: reuse spm from last stage; export tensor: reuse spm for the next stage
            ALL_RINGBUFFER, // all ring buffer：全部使用ring buffer，不额外预留存储空间
            UNKNOWN
        };

        static std::string tensorStayTypeToString(TensorStayType type);
        static TensorStayType stringToTensorStayType(const std::string& type);

        // 基本属性，从yaml文件中读取
        std::vector<uint64> layerIdList; // layer Id list in stage. they are ordered by execution sequence.
        std::vector<TensorId> tensorIdList;
        std::vector<TensorId> entryTensorId;
        std::vector<TensorId> exportTensorId;
        std::vector<std::vector<uint64>> dramBypassList;
        std::vector<std::vector<uint64>> spmBypassList;
        std::vector<TensorStayType> entryTensorType; 
        std::vector<uint> entryTensorDoubleBuffer; // {0, 1} 0: single buffer, 1: double buffer
        std::vector<TensorStayType> exportTensorType;
        std::vector<uint> exportTensorDoubleBuffer; // {0, 1} 0: single buffer, 1: double buffer

        std::vector<TensorId> fixTensorDramBypassId; // 固定张量
        std::vector<TensorId> innerIsolateTensorId; // 内部独立张量
        std::vector<TensorId> innerSharedTensorId; // 内部共享张量

        std::vector<std::vector<uint>> tensorUsageCountList;
        std::vector<std::vector<uint64>> tensorUseLazyFetch; // 张量是否使用lazy fetch。对固定张量来说，在第一次使用时fetch load，后续仅load；【TODO】其它类型张量实现

        uint accUtil;

        // 导出属性，结合模型信息，从基本属性中导出。在load过程中生成。
        std::unordered_map<TensorId, std::array<uint64, 2>> tensorId2DramBaseAddr; // buffer[0,1] 对于入口/出口到dram的，应该都是double-buffer（TODO: 暂时是single-buffer，以后改double-buffer）
        std::unordered_map<TensorId, std::vector<uint64>> tensorId2Shape;
        std::unordered_map<TensorId, std::vector<uint64>> tensorId2Strides;
        std::unordered_map<TensorId, uint64> tensorId2UseLazyFetch; // 是否使用lazy fetch
        std::unordered_map<TensorId, uint64> tensorId2UsageCount; // 每个张量被Stage内多少个层使用。当其减少为0时，会被释放。仅用于内部共享张量的计算。
        std::vector<std::vector<uint>> vAccelIdx; // virtual accel ids for each layer
        std::vector<std::vector<uint>> pAccelIdx; // 非空时指定stage分配的物理加速器
        std::unordered_map<TensorId, uint64> tensorId2SpmPageNums; // 注意，该mp仅存储单个tensor所需页数，且仅存1个bufffer的大小，即对于使用singble-buffer和double-buffer的张量，页面数相同，但double-buffer需要分配两倍大小
        std::vector<LayerMapping*> layerMapping; // layerMapping[i] is the layer mapping for layerList[i]
        std::vector<LayerMapping*> layerMappingLazyFetch; // layerMapping[i] is the layer mapping with lazyFetch for layerList[i]

        bool isEntryTensor(uint tensorId) const;
        bool isExportTensor(uint tensorId) const;
        bool isFixedTensor(uint tensorId) const;
        bool isInnerIsolateTensor(uint tensorId) const;
        bool isInnerSharedTensor(uint tensorId) const;

        TensorStayType getTensorStayType(TensorId tensorId) const;

        uint getDoubleBuffer(TensorId tensorId) const;
        bool isDRAMPipeBuffer(TensorId tensorId) const;
        bool isDRAMDepenPipeBuffer(TensorId tensorId) const;
        bool isIsolatePipeBuffer(TensorId tensorId) const;
        bool isSharedPipeBuffer(TensorId tensorId) const;
        bool isAllRingbufferPipeBuffer(TensorId tensorId) const;
        bool isDoubleBuffer(TensorId tensorId) const;

        nlohmann::json toJson() const;
        std::string toString() const;
    };

    // Segment候选方案
    class SegmentMapping {
    public:
        int subBatchSize;

        // map index
        std::vector<std::vector<TensorId>> stageEntryTensorIds; // stageIdx->vec[tensorId]
        std::vector<std::vector<TensorId>> stageExportTensorIds; // stageIdx->vec[tensorId]
        std::unordered_map<TensorId, std::vector<uint>> entryTensorToStageIdx; // entry tensorId->vec<stageIdx>
        std::unordered_map<TensorId, std::vector<uint>> exportTensorToStageIdx; // export tensorId->vec<stageIdx>

        std::vector<std::set<TensorId>> entryTensorNoIndegree; // 没有入度的入口张量。[stageIdx] -> vec<TensorId>
        std::vector<std::set<TensorId>> exportTensorNoOutdegree; // 没有出度的出口张量。[stageIdx] -> vec<TensorId>

        std::vector<std::pair<uint, TensorId>> input_tensor_id_stage; // stage_idx, tensor_id。流水线输入张量及其所在stage
        std::vector<std::pair<uint, TensorId>> output_tensor_id_stage; // stage_idx, tensor_id。流水线输出张量及其所在stage

        // ring buffer config
        struct RingBufferConfig {
            // use_: 是否使用ring buffer
            // size_: ring buffer大小（多少块）
            // out_degree_: tensor出度
            uint use_, size_, out_degree_, spm_util_per_;

            nlohmann::json toJson() const;
        };
        std::unordered_map<TensorId, RingBufferConfig> ring_buffer_config_; // tensorId的ring buffer配置

        std::unordered_map<TensorId, std::vector<uint64>> tensor_id_to_min_shape, tensor_id_to_max_shape;
        std::unordered_map<TensorId, std::vector<uint64>> tensor_id_to_min_strides, tensor_id_to_max_strides;
        std::unordered_map<TensorId, uint64> tensor_id_to_min_spm_page, tensor_id_to_max_spm_page;


        uint segmentIdx;
        uint startLayerIdx;
        uint endLayerIdx;
        
        std::vector<std::unordered_map<TensorId, uint64>> tensorSpmUtilInStage;
        std::unordered_map<TensorId, uint64> tensorSpmUtilShared;
        std::unordered_map<TensorId, uint64> tensorSpmUtilInRingbuffer;
        std::unordered_map<TensorId, uint64> tensorSpmUtilWeight;
        std::unordered_map<TensorId, bool> sharedTensorIsReadFirst; // true: 读优先；false: 写优先
        
        uint64 totalSpmUtil;
        uint accUtil;
        uint64 cost;
        
        std::vector<StageMapping> stage_mapping_vec;
        
        // 添加createMap方法
        void createMap();
        
        nlohmann::json toJson() const;
        std::string toString() const;
    };

    class PipelineMapping {
    public:
        PipelineMapping() {
        }

        // Pipeline候选方案
        std::vector<SegmentMapping> segmentCandidates;
        uint64 pipelineCost;
        
        // Target相关字段
        uint targetNumArrays;
        uint targetSpmKB;
        uint targetSpmKBPerArray;
        uint targetNumMacsPerArray;
        double targetDramBwPerCycle;
        uint targetNocBwPerCycle;
        uint targetDefaultBatch;

        uint64 maxSpmPageSegmentUse;
        uint maxNumArraysSegmentUse;

        nlohmann::json toJson() const;
        std::string toString() const;
    };

} // mudnac


#endif //MUDNACSIM_MAPPING_H