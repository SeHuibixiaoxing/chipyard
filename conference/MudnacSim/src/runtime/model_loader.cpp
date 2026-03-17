#include "model_loader.h"

namespace mudnac {
    bool LayersParser::Layer::typeIsConv() { 
        return type == "conv"; 
    }
    bool LayersParser::Layer::typeIsResadd() {
        return type == "resadd"; 
    }
    void LayersParser::Layer::load(YAML::Node &layer, uint64 addrOffset) {
        type = layer["type"].as<std::string>();
        layerGroupType = layer["layerGroupType"].as<std::string>();
        param = layer["param"].as<std::vector<uint>>();
        tensorIds = layer["tensorIds"].as<std::vector<TensorId>>();
        address = layer["address"].as<std::vector<uint64>>();
        address2 = layer["address2"].as<std::vector<uint64>>();
        for (auto &addr: address) { addr += addrOffset; }
        for (auto &addr: address2) { addr += addrOffset; }
    }

    void LayersParser::load(const std::string &path, uint64 addrOffset) {
        YAML::Node node = YAML::LoadFile(path + "/layers.yaml");
        maxAddr = addrOffset + node["address"].as<std::vector<uint64>>()[1];
        layers.resize(node["layers"].size());
        for (uint layerId = 0; layerId < layers.size(); layerId++) {
            YAML::Node layer = node["layers"][layerId];
            layers[layerId].load(layer, addrOffset);
        }
    }
    void MappingParser::Candidate::load(YAML::Node &node) {
        std::string key = "mappingForPipeline";
        mappingForPipeline = node[key].as<uint>();
        key = "target";
        targetAccel = node[key]["accel"].as<uint64>();
        targetSpm = node[key]["spm"].as<uint64>();
        key = "mapping";
        tile = node[key]["tile"].as<std::vector<uint64>>();
        factors = node[key]["factors"].as<std::vector<std::vector<uint64>>>();
        permutation = node[key]["permutation"].as<std::vector<std::vector<uint64>>>();
        spmBypass = node[key]["spmBypass"].as<std::vector<uint64>>();
        dramBypass = node[key]["dramBypass"].as<std::vector<uint64>>();
        key = "performance";
        accelUtil = node[key]["accelUtil"].as<uint64>();
        spmUtil = node[key]["spmUtil"].as<uint64>();
        spmPageUtil = node[key]["spmPageUtil"].as<uint64>();
        dramAccess = node[key]["dramAccess"].as<uint64>();
        key = "layerGroup";
        lgSpmUtil = node[key]["lgSpmUtil"].as<uint64>();
        lgAccelUtil = node[key]["lgAccelUtil"].as<uint64>();
        key = "others";
        psumAccumCount = node[key]["psumAccumCount"].as<uint64>();
        totalTiles = node[key]["totalTiles"].as<uint64>();
        spmDimensions = node[key]["spmDimensions"].as<std::vector<uint64>>();
        spmTensorAddr = node[key]["spmTensorAddr"].as<std::vector<uint64>>();
        spmTensorPageCount = node[key]["spmTensorPageCount"].as<std::vector<uint>>();
        firstTensorPageNum = node[key]["firstTensorPageNum"].as<std::vector<uint>>();
        spmTensorTileCount = node[key]["spmTensorTileCount"].as<std::vector<uint64>>();
        spmTensorUtil = node[key]["spmTensorUtil"].as<std::vector<uint64>>();
        tensorMulticastCount = node[key]["tensorMulticastCount"].as<std::vector<uint64>>();
    }
    void MappingParser::load(const std::string &path) {
        auto root = YAML::LoadFile(path);
        YAML::Node node = root["candidates"];
        isPipelineMappingTable = root["pipeline"].as<bool>();
        candidates.resize(node.size());
        for (uint i = 0; i < candidates.size(); i++) {
            YAML::Node cNode = node[i];
            candidates[i].load(cNode);
        }
    }
    
    void ModelLoader::load(Model &m, const std::string &path, uint64 addrOffset, uint tid, uint numAccelAllocs) {
        if(numAccelAllocs == 0) numAccelAllocs = RT.gv.NUM_ACCEL_ALLOCS;
        LayersParser layersParser;
        layersParser.load(path, addrOffset);
        m.numLayers = layersParser.getNumLayers();
        m.addrLo = addrOffset;
        m.addrHi = layersParser.maxAddr;
        m.path = path;

        std::vector<MappingParser> mapParserList(m.numLayers);
        for (uint i = 0; i < m.numLayers; i++) {
            std::string mapPath = path + "/mapping/" + std::to_string(i) + ".yaml";
            mapParserList[i].load(mapPath);
        }

        // Generate layers
        m.layers.resize(m.numLayers, NULL);
        for (uint i = 0; i < m.numLayers; i++) {
            LayersParser::Layer &def = layersParser.layers[i];
            MappingParser &mapping = mapParserList[i];
            if (def.typeIsConv()) {
                LayerConv *layer = new LayerConv;
                m.layers[i] = layer;
                initLayerConv(*layer, def);
            } else if (def.typeIsResadd()) {
                LayerResadd *layer = new LayerResadd;
                m.layers[i] = layer;
                initLayerResadd(*layer, def);
            } else {
                assert(false);
            }
            if(mapping.isPipelineMappingTable) {
                initLayerMappingTablePipeline(*m.layers[i], mapping);
            } else {
                initLayerMappingTable(*m.layers[i], mapping, numAccelAllocs);
            }
            m.layers[i]->resetAllState();
            m.layers[i]->setTid(tid);
            m.layers[i]->lgType = def.layerGroupType;
            m.layers[i]->tensorIds = def.tensorIds;
        }
    }

    void ModelLoader::initLayerConv(LayerConv &layer, LayersParser::Layer &def) {
        layer.setParam(def.param);
        layer.bias.set(def.address[0], {layer.OC * 4,
                                        4});
        layer.weight.set(def.address[1], {layer.KW * layer.G * layer.IC * layer.OC,
                                            layer.G * layer.IC * layer.OC,
                                            layer.IC * layer.OC,
                                            layer.OC,
                                            1});
        layer.input.set(def.address[2], {layer.IW() * layer.N * layer.G * layer.IC,
                                            layer.N * layer.G * layer.IC,
                                            layer.G * layer.IC,
                                            layer.IC,
                                            1});
        layer.output.set(def.address[3], {layer.OW * layer.N * layer.G * layer.OC,
                                            layer.N * layer.G * layer.OC,
                                            layer.G * layer.OC,
                                            layer.OC,
                                            1});
        
        layer.bias2.set(def.address2[0], {layer.OC * 4,
                                        4});
        layer.weight2.set(def.address2[1], {layer.KW * layer.G * layer.IC * layer.OC,
                                            layer.G * layer.IC * layer.OC,
                                            layer.IC * layer.OC,
                                            layer.OC,
                                            1});
        layer.input2.set(def.address2[2], {layer.IW() * layer.N * layer.G * layer.IC,
                                            layer.N * layer.G * layer.IC,
                                            layer.G * layer.IC,
                                            layer.IC,
                                            1});
        layer.output2.set(def.address2[3], {layer.OW * layer.N * layer.G * layer.OC,
                                            layer.N * layer.G * layer.OC,
                                            layer.G * layer.OC,
                                            layer.OC,
                                            1});
    }
    void ModelLoader::initLayerResadd(LayerResadd &layer, LayersParser::Layer &def) {
        layer.setParam(def.param);
        layer.input0.set(def.address[0], {layer.W * layer.N * layer.G * layer.C,
                                            layer.N * layer.G * layer.C,
                                            layer.G * layer.C,
                                            layer.C,
                                            1});
        layer.input1.set(def.address[1], {layer.W * layer.N * layer.G * layer.C,
                                            layer.N * layer.G * layer.C,
                                            layer.G * layer.C,
                                            layer.C,
                                            1});
        layer.output.set(def.address[2], {layer.W * layer.N * layer.G * layer.C,
                                            layer.N * layer.G * layer.C,
                                            layer.G * layer.C,
                                            layer.C,
                                            1});
        layer.input0_2.set(def.address2[0], {layer.W * layer.N * layer.G * layer.C,
                                            layer.N * layer.G * layer.C,
                                            layer.G * layer.C,
                                            layer.C,
                                            1});
        layer.input1_2.set(def.address2[1], {layer.W * layer.N * layer.G * layer.C,
                                            layer.N * layer.G * layer.C,
                                            layer.G * layer.C,
                                            layer.C,
                                            1});
        layer.output_2.set(def.address2[2], {layer.W * layer.N * layer.G * layer.C,
                                            layer.N * layer.G * layer.C,
                                            layer.G * layer.C,
                                            layer.C,
                                            1});
    }
    void ModelLoader::initLayerMappingTable(Layer &layer, MappingParser &map, uint numAccelAlloc) {
        layer.mappingTable.resize(numAccelAlloc); // RT.gv.NUM_ACCEL_ALLOCS
        uint numSpmAllocs = map.candidates.size() / numAccelAlloc;  // RT.gv.NUM_ACCEL_ALLOCS
        for (auto &line: layer.mappingTable) {
            line.resize(numSpmAllocs);
        }
        for (uint accelAllocIdx = 0; accelAllocIdx < numAccelAlloc; accelAllocIdx++) {
            for (uint spmAllocIdx = 0; spmAllocIdx < numSpmAllocs; spmAllocIdx++) {
                uint i = accelAllocIdx * numSpmAllocs + spmAllocIdx;
                createLayerMapping(layer.mappingTable[accelAllocIdx][spmAllocIdx], map.candidates[i]);
            }
        }
    }

    void ModelLoader::initLayerMappingTablePipeline(Layer &layer, MappingParser &map) {
        uint mappingSize = map.candidates.size();
        layer.mappingTable.resize(map.candidates.size());
        for(uint i = 0; i < mappingSize;i++) {
            auto& can = map.candidates[i];
            Layer::PipeLayerMapperKey key;
            key.accNum = can.targetAccel;
            key.dramBypass = can.dramBypass;
            key.spmBypass = can.spmBypass;
            layer.pipeLayerMappingTable[key].emplace_back();
            createLayerMapping(layer.pipeLayerMappingTable.at(key).back(), map.candidates[i]);
        }
    }

    void ModelLoader::initPipelineMappingTable(Model *model, PipelineMappingParser &pipelineMap) {
        // 实现Pipeline Mapping Table初始化
        // TODO

    }
    void ModelLoader::createLayerMapping(LayerMapping &mapping, MappingParser::Candidate &can) {
        mapping.mappingForPipeline = can.mappingForPipeline;

        mapping.ln.init(can.factors, can.permutation, can.tile);
        mapping.ln.reset();

        mapping.targetAccUtil = can.targetAccel;
        mapping.targetSpmUtil = can.targetSpm;
        mapping.accUtil = can.accelUtil;
        mapping.spmUtil = can.spmUtil;
        mapping.spmPageUtil = can.spmPageUtil;
        mapping.lgSpmUtil = can.lgSpmUtil;
        mapping.lgAccUtil = can.lgAccelUtil;

        mapping.dramAccess = can.dramAccess;
        mapping.psumAccumCount = can.psumAccumCount;
        mapping.totalTiles = can.totalTiles;

        mapping.spmBypass = can.spmBypass;
        mapping.dramBypass = can.dramBypass;
        mapping.spmDimensions = can.spmDimensions;
        mapping.accDimensions = can.tile;
        mapping.spmTensorAddr = can.spmTensorAddr;
        mapping.spmTensorPageCount = can.spmTensorPageCount;
        mapping.firstTensorPageNum = can.firstTensorPageNum;
        mapping.tensorMulticastCount = can.tensorMulticastCount;
        mapping.spmTensorTileCount = can.spmTensorTileCount;
    }
    
}