import os
import math
import pickle
import sys

from abc import ABC, abstractmethod

from HybridMapper.CustomYamlDumper import *
from HybridMapper.Problem import Problem, ProblemConv, ProblemResadd, ProblemEmpty
from HybridMapper.MTMRecord import MTMRecord, LayerMappingCandidate
def load_model(model_path):
    """
    Load a model from a pickle dump file.
    
    Args:
        model_path (str): Path to the model dump file
        
    Returns:
        Model: Loaded model object
    """
    with open(model_path, 'rb') as f:
        model = pickle.load(f)
    return model

class Layer(ABC):
    """
    Attributes:
        type: Layer type. (conv/resadd)
        param: Layer parameters. [p0, p1, p2, ...]
        data: Data ID for each tensor of the layer. [id0, id1, id2, ...]
        size: Size(bytes) for each tensor of the layer. [size0, size1, ...]
        dramKeep: If a tensor is stored in DRAM during layer group mapping.
                  Otherwise, a tensor will be fully and only stored in SPM.
                  However, a memory space (addr, size) will be remained for every tensor.
        spmKeep: If a tensor is stored in SPM partly or fully.
        layerGroupType: Layer-group-related type. (none/head/inter/tail)
        compute: Compute(mult) operation count.
        isInterDataList: If a tensor is an intermediate tensor between layers.
    """

    # note: word size are fixed to 1, 4
    WORD_BYTES = 1
    BIAS_WORD_BYTES = 4

    TYPE_CONV = "conv"
    TYPE_RESADD = "resadd"
    TYPE_POOL = "pool"

    LG_TYPE_NONE = "none"
    LG_TYPE_HEAD = "head"
    LG_TYPE_INTER = "inter"
    LG_TYPE_TAIL = "tail"
    
    BATCH_SIZE = 1

    def __init__(self, type_: str, param: list[int], data: list[int], size: list[int], compute: int,
                 dramKeep: list[int], layerGroupType: str, isInterDataList: list[bool], spmKeep: list[int]):
        # basic attributes
        self.type: str = type_
        self.param: list[int] = param
        self.data: list[int] = data
        self.size: list[int] = size
        # layer-block mapping-related
        self.dramKeep: list[int] = dramKeep
        self.spmKeep: list[int] = spmKeep
        self.layerGroupType: str = layerGroupType
        # compute and memory statistics
        self.compute: int = compute
        self.isInterDataList: list[bool] = isInterDataList
        
        if Layer.BATCH_SIZE != 1:
            param[0] = Layer.BATCH_SIZE

    def isConv(self):
        return self.type == self.TYPE_CONV

    def isResadd(self):
        return self.type == self.TYPE_RESADD

    def isPool(self):
        return self.type == self.TYPE_POOL

    def getTotalDataSize(self):
        return sum(self.size)

    def getInterDataSize(self):
        return sum([(self.size[i] if self.isInterDataList[i] else 0) for i in range(len(self.size))])

    def getNonInterDataSize(self):
        return self.getTotalDataSize() - self.getInterDataSize()

    def getComputeIntensity(self):
        return self.compute / self.getTotalDataSize()
    
    def setSpmKeep(self, spmKeep: list[int]):
        self.spmKeep = spmKeep
    
    def setDramKeep(self, dramKeep: list[int]):
        self.dramKeep = dramKeep

    def getDataSize(self, id: int):
        for idx, v in enumerate(self.data):
            if id == v:
                return self.size[idx]
        raise Exception("id not found")
    def getDataSizeDict(self):
        re = {}
        for idx, v in enumerate(self.data):
            re[v] = self.size[idx]
        return re
    @abstractmethod
    def getInputTensorId(self):
        pass

    @abstractmethod
    def getOutputTensorId(self):
        pass
    
    def getWeightTensorId(self):
        return [id for id in self.data if ((id not in self.getInputTensorId()) and (id not in self.getOutputTensorId()))]
    
    def getTotalSize(self, id_list: list[int]):
        return sum([self.getDataSize(id) for id in id_list])

class LayerConv(Layer):
    def __init__(self, param: list[int], data: list[int], dramKeep: list[int] = None,
                 layerGroupType: str = Layer.LG_TYPE_NONE, isInterDataList: list[bool] = None, spmKeep: list[int] = None):
        """
        Arguments:
            param: [N, IC, OC, OH, OW, KH, KW, G, strideH, strideW]
            data: [Bias ID, Weight ID, Input ID, Output ID]
        """
        
        if Layer.BATCH_SIZE != 1:
            param[0] = Layer.BATCH_SIZE

        assert len(param) == 10 and len(data) == 4
        assert dramKeep is None or len(dramKeep) == 4
        assert spmKeep is None or len(spmKeep) == 4
        assert isInterDataList is None or len(isInterDataList) == 4

        if dramKeep is None:
            dramKeep = [1, 1, 1, 1]
        if isInterDataList is None:
            isInterDataList = [False, False, True, True]

        N, IC, OC, OH, OW, KH, KW, G, sH, sW = param
        size = [  # Bias, Weight, Input, Output
            G * OC * self.BIAS_WORD_BYTES,
            KH * KW * G * IC * OC * self.WORD_BYTES,
            ((OH - 1) * sH + KH) * ((OW - 1) * sW + KW) * N * G * IC * self.WORD_BYTES,
            OH * OW * N * G * OC * self.WORD_BYTES
        ]
        compute = N * IC * OC * OH * OW * KH * KW * G # TODO: 改为更精确的模型

        super().__init__(Layer.TYPE_CONV, param, data, size, compute,
                         dramKeep, layerGroupType, isInterDataList, spmKeep)

    def IW(self):
        N, IC, OC, OH, OW, KH, KW, G, sH, sW = self.param
        return (OW - 1) * sW + KW

    def IH(self):
        N, IC, OC, OH, OW, KH, KW, G, sH, sW = self.param
        return (OH - 1) * sH + KH


    def getInputTensorId(self):
        return [self.data[2]]
    
    def getOutputTensorId(self):
        return [self.data[3]]
    
class LayerResadd(Layer):
    def __init__(self, param: list[int], data: list[int], dramKeep: list[int] = None,
                 layerGroupType: str = Layer.LG_TYPE_NONE, isInterDataList: list[bool] = None, spmKeep: list[int] = None):
        """
        Arguments:
            param: [N, C, H, W, G]
            data: [Input0, Input1, Output]
        """

        assert len(param) == 5 and len(data) == 3
        assert dramKeep is None or len(dramKeep) == 3
        assert spmKeep is None or len(spmKeep) == 3
        assert isInterDataList is None or len(isInterDataList) == 3
        
        
        if Layer.BATCH_SIZE != 1:
            param[0] = Layer.BATCH_SIZE

        if dramKeep is None:
            dramKeep = [1, 1, 1]
        if isInterDataList is None:
            isInterDataList = [True, True, True]

        N, C, H, W, G = param
        size = [  # Input0, Input1, Output
            N * C * H * W * G * self.WORD_BYTES,
            N * C * H * W * G * self.WORD_BYTES,
            N * C * H * W * G * self.WORD_BYTES,
        ]
        compute = 0

        super().__init__(Layer.TYPE_RESADD, param, data, size, compute,
                         dramKeep, layerGroupType, isInterDataList, spmKeep)

    def getInputTensorId(self):
        return [self.data[0], self.data[1]]
    
    def getOutputTensorId(self):
        return [self.data[2]]
    
class LayerPool(Layer):
    def __init__(self, param: list[int], data: list[int]):
        """
        Arguments:
            param: [N, IC, IH, IW, KH, KW, stridesH, stridesW]
            data: [Input ID, Output ID]
        """
        assert len(param) == 8 and len(data) == 2
        N, IC, IH, IW, KH, KW, sH, sW = param
        size = [
            N * IC * IH * IW * self.WORD_BYTES,
            N * IC * ((IH - KH) // sH + 1) * ((IW - KW) // sW + 1) * self.WORD_BYTES
        ]
        compute = 0
        
        super().__init__(Layer.TYPE_POOL, param, data, size, compute,
                         dramKeep = [0,0], layerGroupType=Layer.LG_TYPE_NONE, isInterDataList=None, spmKeep = [0,0])
        
    def getInputTensorId(self):
        return [self.data[0]]
    
    def getOutputTensorId(self):
        return [self.data[1]]
    
class Model:
    """
    Attributes:
        layerList: Layers of the model in execution order.
        baseAddr: base memory address
        ceilAddr: ceil memory address
        nextDataId: next availble data ID for automatic data ID assignment
        dataAddrMap: data ID --> memory address
        dataSizeMap: data ID --> memory size
    """

    AUTO_DATA_ID_BASE = 1000000

    def __init__(self, addrOffset=0):
        self.layerList: list[Layer] = []

        self.baseAddr = self.alignUp(addrOffset)
        self.ceilAddr = self.baseAddr

        self.nextDataId = self.AUTO_DATA_ID_BASE
        self.dataAddrMap: dict[int, int] = {}
        self.dataAddrMap2: dict[int, int] = {} # for pipeline double-buffer
        self.dataSizeMap: dict[int, int] = {}
        
        self.pageSize = 1 # KB
        
        
        self._graph_exdata_has_created: bool = False
        self._segment_in_degree: list[list[dict[int, int]]] = None
        self._segment_out_degree: list[list[dict[int, int]]] = None
        self._segment_max_size: list[list[dict[int, int]]] = None
        self._segment_min_size: list[list[dict[int, int]]] = None
        
        self._layer_mapping_has_read: bool = False
        self._layer_mapping_list: list[MTMRecord] = None
    
    def get_model_in_out_ids(self):
        out_id_set = set()
        for layer in self.layerList:
            for out_id in layer.getOutputTensorId():
                out_id_set.add(out_id)
        
        in_id_set = set()
        for layer in self.layerList:
            for in_id in layer.getInputTensorId() + layer.getWeightTensorId():
                in_id_set.add(in_id)
                
        model_input_ids = set()
        model_output_ids = set()
        for layer in self.layerList:
            for in_id in layer.getInputTensorId():
                if in_id not in out_id_set:
                    model_input_ids.add(in_id)
            for out_id in layer.getOutputTensorId():
                if out_id not in in_id_set:
                    model_output_ids.add(out_id)
        
        return list(model_input_ids), list(model_output_ids)

    def reserveDataId(self, num=1):
        nextDataId = self.nextDataId
        self.nextDataId += num
        return nextDataId

    def appendLayer(self, layer: Layer):
        self.layerList.append(layer)

        # Set ID for each tensor.
        for i in range(len(layer.data)):
            dataId = layer.data[i]
            if dataId < 0:
                layer.data[i] = self.nextDataId
                self.nextDataId += 1
            else:
                assert dataId < self.nextDataId

        # Set/update size for each tensor.
        # Intermediate tensors are outputs of the former layers and inputs of the later layers.
        # For convolution with input padding, input tensors need more memory space for padded words.
        # Therefore, sizes of these tensors need to be updated.
        for i, dataId in enumerate(layer.data):
            size = self.alignUp(layer.size[i])
            if dataId not in self.dataSizeMap:
                self.dataSizeMap[dataId] = size
            else:
                if size > self.dataSizeMap[dataId]:
                    print(f"Model.appendLayer: resize memory size for tensor {dataId} "
                          f"({self.dataSizeMap[dataId]} -> {layer.size[i]})")
                    self.dataSizeMap[dataId] = size

    def checkLayerGroup(self) -> bool:
        valid = True
        for i in range(self.getNumLayers()):
            if self.layerList[i].layerGroupType in {Layer.LG_TYPE_HEAD, Layer.LG_TYPE_NONE}:
                valid &= (i == 0) or (self.layerList[i - 1].layerGroupType in {Layer.LG_TYPE_NONE, Layer.LG_TYPE_TAIL})
            elif self.layerList[i].layerGroupType in {Layer.LG_TYPE_INTER, Layer.LG_TYPE_TAIL}:
                valid &= (i > 0) and (self.layerList[i - 1].layerGroupType in {Layer.LG_TYPE_INTER, Layer.LG_TYPE_HEAD})
            else:
                assert False
        return valid

    def generateMemoryMapping(self):
        assert len(self.dataAddrMap) == 0
        assert len(self.dataAddrMap2) == 0
        
        nextAddr = self.baseAddr
        for dataId, size in self.dataSizeMap.items():
            self.dataAddrMap[dataId] = nextAddr
            # self.dataAddrMap2[dataId] = nextAddr + size
            # nextAddr += size * 2
            nextAddr += size
        
        for dataId, size in self.dataSizeMap.items():
            self.dataAddrMap2[dataId] = nextAddr
            nextAddr += size
        
        self.ceilAddr = nextAddr

    def getProblemList(self) -> list[Problem]:
        probList = []
        for layer in self.layerList:
            if layer.isConv():
                prob = ProblemConv(layer.param[: 8], layer.param[8: 10], layer.dramKeep, layer.spmKeep)
            elif layer.isResadd():
                prob = ProblemResadd(layer.param, layer.dramKeep, layer.spmKeep)
            elif layer.isPool():
                prob = ProblemEmpty()
            else:
                assert False, "other layer types are not supported for now"
            probList.append(prob)
        return probList

    @staticmethod
    def alignUp(a, align=64):
        c = a % align
        return a if c == 0 else a + (align - c)

    def write(self, path: str):
        assert os.path.exists(os.path.dirname(os.path.normpath(path)))
        node = dict({
            "address": [self.baseAddr, self.ceilAddr],
            "layers": []
        })
        for i, layer in enumerate(self.layerList):
            layerNode = dict()
            layerNode["index"] = int(i)
            layerNode["type"] = layer.type
            layerNode["param"] = layer.param
            layerNode["tensorIds"] = layer.data
            layerNode["address"] = [self.dataAddrMap[data] for data in layer.data]
            layerNode["address2"] = [self.dataAddrMap2[data] for data in layer.data]
            layerNode["tensorSize"] = layer.size
            layerNode["tensorPageSize"] = [int(math.ceil(size / (self.pageSize * 1024.0))) * self.pageSize * 1024 for size in layer.size]
            layerNode["layerGroupType"] = layer.layerGroupType
            layerNode["compute"] = layer.compute
            layerNode["input_ids"] = layer.getInputTensorId()
            layerNode["output_ids"] = layer.getOutputTensorId()
            layerNode["data_size"] = layer.getDataSizeDict()
            
            node["layers"].append(layerNode)
        with open(path, "w") as file:
            yaml.dump(node, file, Dumper=CustomYamlDumper)

    def dump(self, path: str):
        with open(path, "wb") as f:
            pickle.dump(self, f)
        
    def getInfoString(self):
        s = (f"Model\n"
             f"--- Num of Layers: {self.getNumLayers()}\n"
             f"--- Num of Tensors: {len(self.dataAddrMap)}\n"
             f"--- Address Range: [{self.baseAddr}, {self.ceilAddr})\n"
             f"--- Address Size: {self.ceilAddr - self.baseAddr}\n"
             f"--- Total Memory Access: {self.getTotalMemoryAccess()}\n"
             f"--- Repeated Memory Access: {self.getRepeatedMemoryAccess()}"
             f"({self.getRepeatedMemoryAccess() / (self.ceilAddr - self.baseAddr) :.2f})\n"
             f"--- Total Weight Memory Access: {self.getTotalWeightMemoryAccess()}"
             f"({self.getWeightMemoryAccessRate() :.2f})\n"
             f"--- Compute: total={self.getTotalCompute()}, avg={self.getAvgCompute() :.2f}\n"
             f"--- Compute Intensity: {self.getComputeIntensity() :.2f}\n")
        return s

    def getLayersString(self):
        s = ""
        for i, layer in enumerate(self.layerList):
            s += (f"{i} "
                  f"{layer.type} "
                  f"param={layer.param} "
                  f"data={layer.data} "
                  f"size(KB)={[int(s / 1024) for s in layer.size]} "
                  f"dramKeep={layer.dramKeep} "
                  f"spmKeep={layer.spmKeep} "
                  f"lgType={layer.layerGroupType} "
                  f"\n")
        return s

    def getNumLayers(self) -> int:
        return len(self.layerList)

    def getLayerMemoryAccessList(self) -> list[int]:
        return [layer.getTotalDataSize() for layer in self.layerList]

    def getTotalMemoryAccess(self) -> int:
        return sum(self.getLayerMemoryAccessList())

    def getAvgLayerMemoryAccess(self) -> float:
        return self.getTotalMemoryAccess() / self.getNumLayers()

    def getRepeatedMemoryAccess(self) -> int:
        return self.getTotalMemoryAccess() - (self.ceilAddr - self.baseAddr)

    def getLayerWeightMemoryAccessList(self) -> list[int]:
        return [layer.getNonInterDataSize() for layer in self.layerList]

    def getTotalWeightMemoryAccess(self) -> int:
        return sum(self.getLayerWeightMemoryAccessList())

    def getAvgWeightMemoryAccess(self) -> float:
        return self.getTotalWeightMemoryAccess() / self.getNumLayers()

    def getWeightMemoryAccessRate(self) -> float:
        return self.getTotalWeightMemoryAccess() / self.getTotalMemoryAccess()

    def getLayerComputeList(self) -> list[int]:
        return [layer.compute for layer in self.layerList]

    def getTotalCompute(self) -> int:
        return sum(self.getLayerComputeList())

    def getAvgCompute(self) -> float:
        return self.getTotalCompute() / self.getNumLayers()

    def getComputeIntensity(self) -> int:
        return self.getTotalCompute() / self.getTotalMemoryAccess()
    
    def load_layer_mapping(self, mapping_dir):
        self._layer_mapping: list[MTMRecord] = []
        for i in range(len(self.layerList)):
            dump_path = os.path.join(mapping_dir, f"{i}.dump")
            assert(dump_path)
            record = None
            with open(dump_path, "rb") as f:
                record = pickle.load(f)
            
            assert(record != None)
            self._layer_mapping.append(record)
        
        self._layer_mapping_has_read = True
    
    def get_layer_mapping_all(self, layer_idx: int):
        if not self._layer_mapping_has_read:
            assert(False)
        
        return self._layer_mapping[layer_idx]

    def get_layer_mapping(self, layer_idx: int, dram_bypass: list[int], spm_bypass: list[int], acc_num: int) -> LayerMappingCandidate | None:
        if not self._layer_mapping_has_read:
            assert(False)
            
        return self._layer_mapping[layer_idx].getMap(MTMRecord.MapKey(dram_bypass=dram_bypass, spm_bypass=spm_bypass, acc_num=acc_num))
    
    def get_layer_tensor_spm_util(self, layer_idx: int, tensor_id: int):
        layer: Layer = self.layerList[layer_idx]
        layer_mapping: LayerMappingCandidate = self.get_layer_mapping(layer_idx, [1 for id in layer.data], [0 for id in layer.data], acc_num=1)
        for idx in range(len(layer.data)):
            if tensor_id == layer.data[idx]:
                return layer_mapping.othersNode[layer_mapping.KEY.spmTensorUtil][idx]
                
        assert(False)
            
    def _generate_graph_info(self):
        # 记录每个段中，每个tensor的入度、出度、最小大小、最大大小
        self._segment_in_degree = [[{} for _ in range(len(self.layerList))] for _ in range(len(self.layerList))]
        self._segment_out_degree = [[{} for _ in range(len(self.layerList))] for _ in range(len(self.layerList))]
        self._segment_max_size = [[{} for _ in range(len(self.layerList))] for _ in range(len(self.layerList))]
        self._segment_min_size = [[{} for _ in range(len(self.layerList))] for _ in range(len(self.layerList))]
        # 记录每个段中，tensor id到它作为输入/输出张量的stage的stage idx
        self._tensor_id_to_as_input_stage_idx_vec = [[{} for _ in range(len(self.layerList))] for _ in range(len(self.layerList))]
        self._tensor_id_to_as_output_stage_idx_vec = [[{} for _ in range(len(self.layerList))] for _ in range(len(self.layerList))]
        # 各个段中tensor_id集合
        self._tensor_ids = [[[] for _ in range(len(self.layerList))] for _ in range(len(self.layerList))]
        # 记录每个段中，各层深度
        self._segment_layer_depth = [[[] for _ in range(len(self.layerList))] for _ in range(len(self.layerList))]
        # 统计层真实的输入id，包括weight
        self._segment_input_ids = [[[] for _ in range(len(self.layerList))] for _ in range(len(self.layerList))]
        # 统计层真实的输出id
        self._segment_output_ids = [[[] for _ in range(len(self.layerList))] for _ in range(len(self.layerList))]
        # 统计层真实的权重id，不包括作为输入的权重
        self._segment_weight_ids = [[[] for _ in range(len(self.layerList))] for _ in range(len(self.layerList))]
        
        
        # 统计每个层真实的输入、输出、权重id
        tensor_id_as_output_in_total_graph = set() # 整个计算图中，作为输出的tensor id
        for layer in self.layerList:
            for out_id in layer.getOutputTensorId():
                tensor_id_as_output_in_total_graph.add(out_id)
        
        
        for start_layer_idx in range(len(self.layerList)):
            for end_layer_idx in range(start_layer_idx, len(self.layerList)):
                # 获取当前段中的所有层
                segment:list[Layer] = self.layerList[start_layer_idx:end_layer_idx + 1]
                
                for layer in segment:
                    # 输出id，就是每个层的输出id
                    self._segment_output_ids[start_layer_idx][end_layer_idx].append(layer.getOutputTensorId())
                    # 输入id，就是每个层的输入id+作为某个网络层（可以不是段内网络层）输出的权重id
                    input_ids = [id for id in layer.getInputTensorId()] + [id for id in layer.getWeightTensorId() if id in tensor_id_as_output_in_total_graph]
                    self._segment_input_ids[start_layer_idx][end_layer_idx].append(input_ids)
                    # 权重id，就是每个层的权重id+作为某个网络层（可以不是段内网络层）输出的权重id
                    weight_ids = []
                    for weight_id in layer.getWeightTensorId():
                        if weight_id not in tensor_id_as_output_in_total_graph:
                            weight_ids.append(weight_id)
                    self._segment_weight_ids[start_layer_idx][end_layer_idx].append(weight_ids)
        
        for start_layer_idx in range(len(self.layerList)):
            for end_layer_idx in range(start_layer_idx, len(self.layerList)):
                # 获取当前段中的所有层
                segment:list[Layer] = self.layerList[start_layer_idx:end_layer_idx + 1]
                
                # 统计入度、出度、最小大小、最大大小
                in_degree = {}
                out_degree = {}
                min_size = {}
                max_size = {}
                for layer_idx, layer in enumerate(segment):                    
                    for input_id in self._segment_input_ids[start_layer_idx][end_layer_idx][layer_idx]:
                        in_degree[input_id] = in_degree.get(input_id, 0) + 1
                        data_size = self.get_layer_tensor_spm_util(layer_idx + start_layer_idx, input_id)
                        min_size[input_id] = min(min_size.get(input_id, sys.maxsize), data_size)
                        max_size[input_id] = max(max_size.get(input_id, 0), data_size)
                        
                    for output_id in self._segment_output_ids[start_layer_idx][end_layer_idx][layer_idx]:
                        out_degree[output_id] = out_degree.get(output_id, 0) + 1
                        data_size = self.get_layer_tensor_spm_util(layer_idx + start_layer_idx, output_id)
                        min_size[output_id] = min(min_size.get(output_id, sys.maxsize), layer.getDataSize(output_id))
                        max_size[output_id] = max(max_size.get(output_id, 0), layer.getDataSize(output_id))
                
                self._segment_in_degree[start_layer_idx][end_layer_idx] = in_degree
                self._segment_out_degree[start_layer_idx][end_layer_idx] = out_degree
                self._segment_max_size[start_layer_idx][end_layer_idx] = max_size
                self._segment_min_size[start_layer_idx][end_layer_idx] = min_size
                
                # 统计每个层的深度
                layer_depths = []
                dependencies: dict[int, list[int]] = {} # dependencies[i]：层i依赖的层列表，idx为stage_idx
                for i, layer in enumerate(segment):
                    dependencies[i] = []
                    # 检查该层的输入是否依赖于段内的其他层输出
                    for input_id in self._segment_input_ids[start_layer_idx][end_layer_idx][i]:
                        for j, prev_layer in enumerate(segment[:i]): # 要求模型构建满足拓扑序
                            if input_id in self._segment_output_ids[start_layer_idx][end_layer_idx][j]:
                                dependencies[i].append(j)
                
                for i in range(len(segment)):
                    if not dependencies[i]:  # 没有依赖
                        layer_depths.append(0)
                    else:
                        max_depth = 0
                        for dep in dependencies[i]:
                            max_depth = max(max_depth, layer_depths[dep])
                        layer_depths.append(max_depth + 1)

                self._segment_layer_depth[start_layer_idx][end_layer_idx] = layer_depths
                
                # 统计每个tensor作为输入/输出tensor时对应的stage idx
                tensor_id_to_as_input_stage_idx_vec = {}
                tensor_id_to_as_output_stage_idx_vec = {}
                for stage_idx, layer in enumerate(segment):
                    for tensor_id in self._segment_input_ids[start_layer_idx][end_layer_idx][stage_idx]:
                        if tensor_id_to_as_input_stage_idx_vec.get(tensor_id, None) is None:
                            tensor_id_to_as_input_stage_idx_vec[tensor_id] = [stage_idx]
                        else:
                            tensor_id_to_as_input_stage_idx_vec[tensor_id].append(stage_idx)
                    
                    for tensor_id in self._segment_output_ids[start_layer_idx][end_layer_idx][stage_idx]:
                        if tensor_id_to_as_output_stage_idx_vec.get(tensor_id, None) is None:
                            tensor_id_to_as_output_stage_idx_vec[tensor_id] = [stage_idx]
                        else:
                            tensor_id_to_as_output_stage_idx_vec[tensor_id].append(stage_idx)
                
                self._tensor_id_to_as_input_stage_idx_vec[start_layer_idx][end_layer_idx] = tensor_id_to_as_input_stage_idx_vec
                self._tensor_id_to_as_output_stage_idx_vec[start_layer_idx][end_layer_idx] = tensor_id_to_as_output_stage_idx_vec

                self._tensor_ids[start_layer_idx][end_layer_idx] = list(set(list(tensor_id_to_as_input_stage_idx_vec.keys()) + list(tensor_id_to_as_output_stage_idx_vec.keys())))
        
        self._graph_exdata_has_created = True
    
    def get_in_degree(self, start_layer_idx: int, end_layer_idx: int) -> dict:
        if not self._graph_exdata_has_created:
            self._generate_graph_info()
        
        return self._segment_in_degree[start_layer_idx][end_layer_idx]

    def get_out_degree(self, start_layer_idx: int, end_layer_idx: int) -> dict:
        if not self._graph_exdata_has_created:
            self._generate_graph_info()
        
        return self._segment_out_degree[start_layer_idx][end_layer_idx]
    
    def get_max_size(self, start_layer_idx: int, end_layer_idx: int) -> dict:
        if not self._graph_exdata_has_created:
            self._generate_graph_info()
        
        return self._segment_max_size[start_layer_idx][end_layer_idx]
    
    def get_min_size(self, start_layer_idx: int, end_layer_idx: int) -> dict:
        if not self._graph_exdata_has_created:
            self._generate_graph_info()
        
        return self._segment_min_size[start_layer_idx][end_layer_idx]

    def get_segment_layer_input_ids(self, start_layer_idx: int, end_layer_idx: int) -> list[list[int]]:
        if not self._graph_exdata_has_created:
            self._generate_graph_info()
        
        return self._segment_input_ids[start_layer_idx][end_layer_idx]

    def get_segment_layer_output_ids(self, start_layer_idx: int, end_layer_idx: int) -> list[list[int]]:
        if not self._graph_exdata_has_created:
            self._generate_graph_info()
        
        return self._segment_output_ids[start_layer_idx][end_layer_idx]
    
    def get_segment_layer_weight_ids(self, start_layer_idx: int, end_layer_idx: int) -> list[list[int]]:
        if not self._graph_exdata_has_created:
            self._generate_graph_info()
        
        return self._segment_weight_ids[start_layer_idx][end_layer_idx]

    def get_depth(self, start_layer_idx: int, end_layer_idx: int) -> list[int]:
        if not self._graph_exdata_has_created:
            self._generate_graph_info()
        
        return self._segment_layer_depth[start_layer_idx][end_layer_idx]

    def get_tensor_as_input_stage_dict(self, start_layer_idx: int, end_layer_idx: int) -> dict[int, list[int]]:
        if not self._graph_exdata_has_created:
            self._generate_graph_info()
        
        return self._tensor_id_to_as_input_stage_idx_vec[start_layer_idx][end_layer_idx]
    
    def get_tensor_as_output_stage_dict(self, start_layer_idx: int, end_layer_idx: int) -> dict[int, list[int]]:
        if not self._graph_exdata_has_created:
            self._generate_graph_info()
        
        return self._tensor_id_to_as_output_stage_idx_vec[start_layer_idx][end_layer_idx]
    
    def get_tensor_ids(self, start_layer_idx: int, end_layer_idx: int) -> dict[int, list[int]]:
        if not self._graph_exdata_has_created:
            self._generate_graph_info()
        
        return self._tensor_ids[start_layer_idx][end_layer_idx]

    def setSpmKeepAllConv(self, spmKeep: list[int]):
        assert len(spmKeep) == 4
        for layer in self.layerList:
            if layer.isConv():
                layer.setSpmKeep(spmKeep)
    
    def setSpmKeepAllResadd(self, spmKeep: list[int]):
        assert len(spmKeep) == 3
        for layer in self.layerList:
            if layer.isResadd():
                layer.setSpmKeep(spmKeep)
                
    def setDramKeepAllConv(self, dramKeep: list[int]):
        assert len(dramKeep) == 4
        for layer in self.layerList:
            if layer.isConv():
                layer.setDramKeep(dramKeep)
    
    def setDramKeepAllResadd(self, dramKeep: list[int]):
        assert len(dramKeep) == 3
        for layer in self.layerList:
            if layer.isResadd():
                layer.setDramKeep(dramKeep)