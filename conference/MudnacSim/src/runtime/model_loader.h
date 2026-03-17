#ifndef MUDNACSIM_MODEL_LOADER_H
#define MUDNACSIM_MODEL_LOADER_H


#include "yaml-cpp/yaml.h"
#include "layer_conv.h"
#include "layer_resadd.h"


namespace mudnac {
    class LayersParser {
    public:
        class Layer {
        public:
            std::string type;
            std::string layerGroupType;
            std::vector<uint> param;
            std::vector<TensorId> tensorIds;
            std::vector<uint64> address;
            std::vector<uint64> address2; // for pipeline double-buffer

            bool typeIsConv();

            bool typeIsResadd();

            void load(YAML::Node &layer, uint64 addrOffset = 0);
        };

        uint64 maxAddr = 0;
        std::vector<Layer> layers;

        void load(const std::string &path, uint64 addrOffset = 0);

        inline uint getNumLayers() { return layers.size(); }
    };

    class MappingParser {
    public:
        class Candidate {
        public:
            bool mappingForPipeline; // 是否是pipeline使用的映射。如果为true，则spm地址页面对齐

            // target
            uint64 targetAccel, targetSpm;
            // mapping
            std::vector<uint64> tile;
            std::vector<std::vector<uint64>> factors;
            std::vector<std::vector<uint64>> permutation;
            std::vector<uint64> spmBypass;
            std::vector<uint64> dramBypass;
            // performance
            uint64 accelUtil;
            uint64 spmUtil;
            uint64 spmPageUtil; // 【new】spm页数
            uint64 dramAccess;
            // layerGroup
            uint64 lgSpmUtil;
            uint64 lgAccelUtil;
            // others
            uint64 psumAccumCount;
            uint64 totalTiles;
            std::vector<uint64> spmDimensions;
            std::vector<uint64> spmTensorAddr;
            std::vector<uint> spmTensorPageCount; // // 【new】Format: [tensor0 spm page counts, tensor1 spm spage counts, ...].
            std::vector<uint> firstTensorPageNum; // // 【new】Format: [tensor0 first vspm page number, tensor1 first vspm spage number, ...].
            std::vector<uint64> spmTensorTileCount;
            std::vector<uint64> spmTensorUtil;
            std::vector<uint64> tensorMulticastCount;

            void load(YAML::Node &node);
        };

        std::vector<Candidate> candidates;

        void load(const std::string &path);

        bool isPipelineMappingTable;
    };

    class ModelLoader {
    public:
        static void load(Model &m, const std::string &path, uint64 addrOffset = 0, uint tid = 0, uint numAccelAllocs = 0);

        static void initLayerConv(LayerConv &layer, LayersParser::Layer &def);

        static void initLayerResadd(LayerResadd &layer, LayersParser::Layer &def);

        static void initLayerMappingTable(Layer &layer, MappingParser &map, uint numAccelAlloc);
        
        static void initLayerMappingTablePipeline(Layer &layer, MappingParser &map);
        
        // 初始化Pipeline Mapping Table
        static void initPipelineMappingTable(Model *model, PipelineMappingParser &pipelineMap);

    private:
        static void createLayerMapping(LayerMapping& mapping, MappingParser::Candidate &can);
    };
}


#endif //MUDNACSIM_MODEL_LOADER_H