import os
import pickle
from enum import Enum,unique
from HybridMapper.CustomYamlDumper import *
from collections import defaultdict
from HybridMapper.Model import Layer, Model
@unique
class TensorMetaType(Enum):
    TYPE_IO = 1,
    TYPE_NORMAL_DRAM = 2,
    TYPE_NORMAL_SPM = 3,
    TYPE_SHARED = 4,
    TYPE_PURE_DECOUPLING = 5,
    
    UNKNOWN=1000

@unique
class TensorType(Enum):
    # IO type
    IO_SINGLE = 1
    IO_DOUBLE = 2
    IO_NOBUFFER = 3
    
    # Normal DRAM
    INTER_DRAM_SINGLE = 4
    INTER_DRAM_DOUBLE = 5
    
    # Normal SPM
    INTER_SINGLE = 6
    INTER_DOUBLE = 7
    
    # Shared
    INTER_SHARED_WRITE = 8
    INTER_SHARED_READ = 9
    
    # Pure Decoupling
    INTER_PURE_DECOUPLING = 10
    
    UNKNOWN=1000
        
    @classmethod
    def get_io_type(cls):
        return [
            TensorType.IO_SINGLE,
            TensorType.IO_DOUBLE,
            TensorType.IO_NOBUFFER
        ]
    
    @classmethod
    def get_normal_dram_type(cls):
        return [
            TensorType.INTER_DRAM_SINGLE,
            TensorType.INTER_DRAM_DOUBLE,
        ]
    
    @classmethod
    def get_normal_spm_type(cls):
        return [
            TensorType.INTER_SINGLE,
            TensorType.INTER_DOUBLE,
        ]
    
    @classmethod
    def get_shared_type(cls):
        return [
            TensorType.INTER_SHARED_WRITE,
            TensorType.INTER_SHARED_READ,
        ]
    
    @classmethod
    def get_pure_decoupling_type(cls):
        return [
            TensorType.INTER_PURE_DECOUPLING
        ]
    
    @classmethod
    def get_need_noc_add(cls):
        return [
            TensorType.INTER_SINGLE
        ]
    
    @classmethod
    def get_need_noc_max(cls):
        return [
            TensorType.INTER_DOUBLE
        ]
    
    @classmethod
    def get_need_dram_add(cls):
        return [
            TensorType.IO_SINGLE,
            TensorType.IO_NOBUFFER,
            TensorType.INTER_DRAM_SINGLE,
        ]

    @classmethod
    def get_need_dram_max(cls):
        return [
            TensorType.IO_DOUBLE,
            TensorType.INTER_DRAM_DOUBLE
        ]
    
    
    @classmethod
    def get_need_acces_dram_first(cls):
        return [
            TensorType.IO_SINGLE, 
            TensorType.IO_DOUBLE, 
            TensorType.IO_NOBUFFER, 
            TensorType.INTER_DRAM_SINGLE, 
            TensorType.INTER_DRAM_DOUBLE
        ]
    
    @classmethod
    def get_not_runtime_buffer(cls):
        return [
            TensorType.IO_NOBUFFER,   
        ]
    @classmethod
    def get_runtime_dram_list(cls):
        return [
            TensorType.IO_SINGLE,
            TensorType.IO_DOUBLE,
        ]
    @classmethod
    def get_runtime_isolate_spm_list(cls):
        return [
            TensorType.INTER_SINGLE,
            TensorType.INTER_DOUBLE
        ]
    @classmethod
    def get_runtime_shared_spm_list(cls):
        return [
            TensorType.INTER_SHARED_WRITE,
            TensorType.INTER_SHARED_READ,
        ]
    
    @classmethod
    def get_runtime_dram_depen_list(cls):
        return [
            TensorType.INTER_DRAM_SINGLE,
            TensorType.INTER_DRAM_DOUBLE,
        ]
    
    @classmethod
    def get_runtime_all_ringbuffer_list(cls):
        return [
            TensorType.INTER_PURE_DECOUPLING
        ]
    
    @classmethod
    def get_runtime_with_doublebuffer(cls):
        return [
            TensorType.IO_DOUBLE,
            TensorType.INTER_DRAM_DOUBLE,
            TensorType.INTER_DOUBLE,
            TensorType.INTER_SHARED_WRITE,
            TensorType.INTER_SHARED_READ,
            
        ]
    @classmethod
    def get_runtime_without_doublebuffer(cls):
        return [
            TensorType.IO_SINGLE,
            TensorType.INTER_DRAM_SINGLE,
            TensorType.INTER_PURE_DECOUPLING,
        ]
    
    def is_spm_bypass(self):
        return self in [TensorType.IO_NOBUFFER]
    
    def is_dram_bypass(self):
        return (not self.is_spm_bypass()) and (not self.unknown())
    
    def unknown(self):
        return self == TensorType.UNKNOWN
    
    def with_doublebuffer(self):
        return self in TensorType.get_runtime_with_doublebuffer()

    def without_doublebuffer(self):
        return self in TensorType.get_runtime_without_doublebuffer()

def get_meta_type(tensor_type: TensorType) -> TensorMetaType:
    if tensor_type in TensorType.get_io_type():
        return TensorMetaType.TYPE_IO
    elif tensor_type in TensorType.get_normal_dram_type():
        return TensorMetaType.TYPE_NORMAL_DRAM
    elif tensor_type in TensorType.get_normal_spm_type():
        return TensorMetaType.TYPE_NORMAL_SPM
    elif tensor_type in TensorType.get_shared_type():
        return TensorMetaType.TYPE_SHARED
    elif tensor_type in TensorType.get_pure_decoupling_type():
        return TensorMetaType.TYPE_PURE_DECOUPLING
    else:
        return TensorMetaType.UNKNOWN
@unique
class TensorTypeRuntime(Enum):
    DRAM = 1
    ISOLATE_SPM = 2
    SHARED_SPM = 3
    DRAM_DEPEN = 4
    ALL_RINGBUFFER = 5
    
    UNKNOWN=1000
    @classmethod
    def generate_from_mapping_type(cls, mapping_type: TensorType):
        if mapping_type in TensorType.get_runtime_dram_list():
            return TensorTypeRuntime.DRAM
        elif mapping_type in TensorType.get_runtime_dram_depen_list():
            return TensorTypeRuntime.DRAM_DEPEN
        elif mapping_type in TensorType.get_runtime_isolate_spm_list():
            return TensorTypeRuntime.ISOLATE_SPM
        elif mapping_type in TensorType.get_runtime_shared_spm_list():
            return TensorTypeRuntime.SHARED_SPM
        elif mapping_type in TensorType.get_runtime_all_ringbuffer_list():
            return TensorTypeRuntime.ALL_RINGBUFFER
        else:
            assert(False)
        
class StageCandidate:
    class KEY:
        # 模型stage候选唯一id
        globalStageId = "globalStageId"
        
        accUtil = "accUtil"
        
        # 下列元素构成key，用于唯一地定位一个模型内的stage mapping方法
        layerIdList = "layerIdList" # 模型内层编号。目前，始终只有一层
        
        # layer相关信息
        tensorIdList = "tensorIdList" # 后续所有tensor相关mapping选项，顺序与该list相同。即tensor_attribute[idx]表示的是tensorIdList[idx]这个tensor的mapping选项
        entryTensorIdList = "entryTensorIdList" # 所有入口tensor相关mapping选项
        exportTensorIdList = "exportTensorIdList" # 所有出口tensor相关mapping选项
        
        # 细化mapping策略，用于驱动流水线执行
        dramBypassList = "dramBypassList"
        spmBypassList = "spmBypassList"
        
        entryTensorTypeList = "entryTensorTypeList" # tensor的运行时类型(str)。值={"DRAM", "ISOLATE_SPM", "SHARED_SPM", "DRAM_DEPEN"， "ALL_RINGBUFFER"}
        entryTensorDoubleBufferList = "entryTensorDoubleBufferList"
        
        exportTensorTypeList = "exportTensorTypeList" # tensor的运行时类型(str)。值={"DRAM", "ISOLATE_SPM", "SHARED_SPM", "DRAM_DEPEN"， "ALL_RINGBUFFER"}
        exportTensorDoubleBufferList = "exportTensorDoubleBufferList"
        
        fixTensorDramBypassIdList = "fixTensorDramBypassIdList"
        innerIsolateTensorId = "innerIsolateTensorId" # 目前为空值
        innerSharedTensorId = "innerSharedTensorId" # 目前为空值
        
        tensorUsageCountList = "tensorUsageCountList" # 顺序与tensorIdList一致. 目前全部为1
        tensorUseLazyFetch = "tensorUseLazyFetch" # 目前为空值
        
        vAccIdxList = "vAccIdxList"
        pAccIdxList = "pAccIdxList"
        
    def __init__(self, 
                globalStageId: int,
                
                accUtil: int,
                                
                layerIdList: list[int],
                
                tensorIdList: list[int],
                entryTensorIdList: list[int],
                exportTensorIdList: list[int],
                
                dramBypassList: list[list[int]],
                spmBypassList: list[list[int]],
                
                entryTensorTypeList: list[int],
                entryTensorDoubleBufferList: list[int],
                
                exportTensorTypeList: list[int],
                exportTensorDoubleBufferList: list[int],
                 
                fixTensorDramBypassIdList: list[int],
                innerIsolateTensorId: list[int],
                innerSharedTensorId: list[int],
                
                tensorUsageCountList: list[list[int]],
                tensorUseLazyFetch: list[list[int]],
                
                vAccIdxList: list[list[int]],
                pAccIdxList: list[list[int]]
            ):
        self.globalStageId = globalStageId
        
        self.accUtil = accUtil
        
        self.layerIdList = layerIdList
        
        self.tensorIdList = tensorIdList
        self.entryTensorIdList = entryTensorIdList
        self.exportTensorIdList = exportTensorIdList
        
        self.dramBypassList = dramBypassList
        self.spmBypassList = spmBypassList
        
        self.entryTensorTypeList = entryTensorTypeList
        self.entryTensorDoubleBufferList = [int(x) for x in entryTensorDoubleBufferList]
        
        self.exportTensorTypeList = exportTensorTypeList
        self.exportTensorDoubleBufferList = [int(x) for x in exportTensorDoubleBufferList]
        
        self.fixTensorDramBypassIdList = fixTensorDramBypassIdList
        self.innerIsolateTensorId = innerIsolateTensorId
        self.innerSharedTensorId = innerSharedTensorId
        
        self.tensorUsageCountList = tensorUsageCountList
        self.tensorUseLazyFetch = tensorUseLazyFetch
        
        self.vAccIdxList = vAccIdxList
        self.pAccIdxList = pAccIdxList
        
        self.node = dict({
            self.KEY.globalStageId: self.globalStageId,
            
            self.KEY.accUtil: self.accUtil,
            
            self.KEY.layerIdList: self.layerIdList,
            
            self.KEY.tensorIdList: self.tensorIdList,
            self.KEY.entryTensorIdList: self.entryTensorIdList,
            self.KEY.exportTensorIdList: self.exportTensorIdList,
            
            self.KEY.dramBypassList: self.dramBypassList,
            self.KEY.spmBypassList: self.spmBypassList,
            
            self.KEY.entryTensorTypeList: self.entryTensorTypeList,
            self.KEY.entryTensorDoubleBufferList: self.entryTensorDoubleBufferList,
            
            self.KEY.exportTensorTypeList: self.exportTensorTypeList,
            self.KEY.exportTensorDoubleBufferList: self.exportTensorDoubleBufferList,
            
            self.KEY.fixTensorDramBypassIdList: self.fixTensorDramBypassIdList,
            self.KEY.innerIsolateTensorId: self.innerIsolateTensorId,
            self.KEY.innerSharedTensorId: self.innerSharedTensorId,
            
            self.KEY.tensorUsageCountList: self.tensorUsageCountList,
            self.KEY.tensorUseLazyFetch: self.tensorUseLazyFetch,
            
            self.KEY.vAccIdxList: self.vAccIdxList,
            self.KEY.pAccIdxList: self.pAccIdxList
        })

class StageCandidateFactory:
    """
    Factory class for creating StageCandidate instances.
    """
    
    def __init__(self, model: Model):
        """
        Initialize the factory with default values or counters.
        """
        self._global_stage_id_counter = 0
        self._model = model
        
    @classmethod
    def get_dram_spm_bypass(cls, layer: Layer, tensor_type_dict: dict[int, TensorType], weight_ids: list[int]):
        dram_bypass = []
        spm_bypass = []
        weight_ids = weight_ids
        for tensor_id in layer.data:
            if tensor_id in weight_ids:
                dram_bypass.append(1)
                spm_bypass.append(0)
            else:
                tensor_type = tensor_type_dict.get(tensor_id, TensorType.UNKNOWN)
                if tensor_type.is_spm_bypass():
                    dram_bypass.append(0)
                    spm_bypass.append(1)
                else:
                    assert(tensor_type.is_dram_bypass())
                    dram_bypass.append(1)
                    spm_bypass.append(0)
        
        return dram_bypass, spm_bypass
    
    def get_stage_id_counter(self):
        return self._global_stage_id_counter
        
    def create_stage_candidate(self,
                               layerIdList: list[int],
                               tensorTypeDict: dict[int, TensorType],
                               accUtil: int,
                               input_ids: list[int],
                               weight_ids: list[int],
                               output_ids: list[int]) -> StageCandidate:
        """
        Create a StageCandidate instance with the specified parameters.
        
        Args:
            layerIdList: List of layer IDs
            tensorTypeDict: Dictory of tensor types
            
        Returns:
            StageCandidate: A new StageCandidate instance
        """
        
        # Generate a unique global stage ID
        global_stage_id = self._global_stage_id_counter
        self._global_stage_id_counter += 1
            
        assert(len(layerIdList)==1) # 暂时只支持1层
        layer: Layer = self._model.layerList[layerIdList[0]]
        
        # Extract tensor information from layers (to be implemented based on actual model)
        tensorIdList = [tensor_id for tensor_id in layer.data]
        entryTensorIdList = [tensor_id for tensor_id in input_ids if tensorTypeDict.get(tensor_id) not in TensorType.get_not_runtime_buffer()]
        exportTensorIdList = [tensor_id for tensor_id in output_ids if tensorTypeDict.get(tensor_id) not in TensorType.get_not_runtime_buffer() ]
        
        # Bypass lists (to be determined based on mapping strategy)
        dramBypassList = []
        spmBypassList = []
            
        dram_bypass, spm_bypass = StageCandidateFactory.get_dram_spm_bypass(layer, tensorTypeDict, weight_ids)
                    
        dramBypassList.append(dram_bypass)
        spmBypassList.append(spm_bypass)
        
        # Runtime tensor type lists (to be determined based on mapping strategy)
        entryTensorTypeList = []
        entryTensorDoubleBufferList = []
        exportTensorTypeList = []
        exportTensorDoubleBufferList = []
        
        id_list = entryTensorIdList + exportTensorIdList
        
        type_list = []
        doublebuffer_list = []
        
        for tensor_id in id_list:
            mapping_type = tensorTypeDict.get(tensor_id, TensorType.UNKNOWN)
            with_doublebuffer = mapping_type.with_doublebuffer()
            runtime_type = TensorTypeRuntime.generate_from_mapping_type(mapping_type).name
        
            type_list.append(runtime_type)
            doublebuffer_list.append(with_doublebuffer)
        
        split_pos = len(entryTensorIdList)
        entryTensorTypeList = type_list[:split_pos]
        entryTensorDoubleBufferList = doublebuffer_list[:split_pos]
        
        exportTensorTypeList = type_list[split_pos:]
        exportTensorDoubleBufferList = doublebuffer_list[split_pos:]
            
        # Fixed tensor lists (to be determined based on mapping strategy)
        fixTensorDramBypassIdList = weight_ids
            
        innerIsolateTensorId = []  # Currently empty
        innerSharedTensorId = []  # Currently empty
        
        # Tensor usage information
        tensorUsageCountList =[[1 for tensor_id in layer.data]]
        tensorUseLazyFetch = [[0 for tensor_id in layer.data]]  # 暂时禁用lazy fetch
        
        # Create and return the StageCandidate
        return StageCandidate(
            globalStageId=global_stage_id,
            
            layerIdList=layerIdList,
            
            tensorIdList=tensorIdList,
            entryTensorIdList=entryTensorIdList,
            exportTensorIdList=exportTensorIdList,
            
            dramBypassList=dramBypassList,
            spmBypassList=spmBypassList,
            
            entryTensorTypeList=entryTensorTypeList,
            entryTensorDoubleBufferList=entryTensorDoubleBufferList,
            
            exportTensorTypeList=exportTensorTypeList,
            exportTensorDoubleBufferList=exportTensorDoubleBufferList,
            
            fixTensorDramBypassIdList=fixTensorDramBypassIdList,
            innerIsolateTensorId=innerIsolateTensorId,
            innerSharedTensorId=innerSharedTensorId,
            
            tensorUsageCountList=tensorUsageCountList,
            tensorUseLazyFetch=tensorUseLazyFetch,
            
            accUtil=accUtil,
            vAccIdxList=[[idx for idx in range(accUtil)]],
            pAccIdxList=[[]],
        )
    
    def reset_id_counter(self):
        """
        Reset the global stage ID counter to 0.
        """
        self._global_stage_id_counter = 0

class StageRecord:
    def __init__(self):
        self.node = []
        self.candidateList: list[StageCandidate] = []

    def add(self, stageCan: StageCandidate):
        self.candidateList.append(stageCan)
        self.node.append(stageCan.node)
        
    def write(self, path: str):
        assert os.path.exists(os.path.dirname(os.path.normpath(path)))
        with open(path, "w") as file:
            yaml.dump(self.node, file, Dumper=CustomYamlDumper)
    def dump(self, path: str):
        with open(path, "wb") as f:
            pickle.dump(self, f)

