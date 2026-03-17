
import random
import math
import copy
import sys
import os
from HybridMapper.CustomYamlDumper import *
from abc import ABC, abstractmethod
from itertools import accumulate
from simanneal import Annealer
from HybridMapper.GraphPartition import GraphPartitionRecord, GraphPartitionCandidate
from HybridMapper.StageRecord import StageRecord, StageCandidate, StageCandidateFactory, TensorType, TensorTypeRuntime, get_meta_type, TensorMetaType
from HybridMapper.Model import Model,Layer
from HybridMapper.Target import Target
from HybridMapper.MTMRecord import MTMRecord, LayerMappingCandidate
import yaml
import pickle
import time

class PipelineTarget:
    def __init__(self,
                 model: Model,
                 layer_mapping_dir_path: str,
                 start_layer_idx: int,
                 end_layer_idx: int,
                 default_batch: int,
                 target: Target,
                 remote_layer_compute_ratio: float = 1.1,
                 resource_util_ratio: float = 0,
                 enable_decoupling: bool = True):
        """
        初始化搜索目标（约束条件）
        
        Args:
            model: 模型object
            layer_mapping_dir_path: layer mapping结果目录路径
            start_layer_idx: 起始层
            end_layer_idx: 结束层
            default_batch: batch数
            target: 硬件目标
            remote_layer_compute_ratio: 每层因访问远程张量导致的延迟因子
            resource_util_ratio: 资源占用在代价函数中的占比
        """
        self.model: Model = model
        self.start_layer_idx: int = start_layer_idx
        self.end_layer_idx: int = end_layer_idx
        self.default_batch: int = default_batch
        self.target: Target = target
        self.layer_num: int = end_layer_idx - start_layer_idx + 1
        self.remote_layer_compute_ratio: float = remote_layer_compute_ratio
        self.resource_util_ratio: float = resource_util_ratio
        self.enable_decoupling: bool = enable_decoupling
        
        self.model.load_layer_mapping(layer_mapping_dir_path)

class PipelineSolution:
    class LayerSolution:
        def __init__(self,
                     layer: Layer,
                     layer_mapping_record: MTMRecord,
                     num_acc: int,
                     tensor_type_dict: dict[int, TensorType],
                     pipeline_target: PipelineTarget,
                     input_ids: list[int],
                     weight_ids: list[int],
                     output_ids: list[int]):
            self._layer: Layer = layer
            self._layer_mapping_record: MTMRecord = layer_mapping_record
            self._num_acc: int = num_acc
            self._tensor_type_dict: dict[int, TensorType] = tensor_type_dict
            self._pipeline_target: PipelineTarget = pipeline_target
            self.input_ids: list[int] = input_ids
            self.weight_ids: list[int] = weight_ids
            self.output_ids: list[int] = output_ids
            
            self._compute_cycles: int = 0
            self._dram_bytes: int = 0
            
            # 初始化 bypass 列表
            self._dram_bypass = []
            self._spm_bypass = []
            
            # 根据 tensor_type_dict 设置 bypass 列表
            self._initialize_bypass_lists()
            
            # 获取层映射
            self._layer_mapping: LayerMappingCandidate = None
            self._update_layer_mapping()
            
            self.set_num_acc(self._num_acc)
            self._io_tensor_size: dict[int, int] = {tensor_id: self._pipeline_target.target.ceilPage(self._layer.getDataSize(tensor_id)) for tensor_id in self.input_ids + self.output_ids}
            self._io_tensor_need_dram: dict[int, int] = {tensor_id: (1 if self._tensor_type_dict[tensor_id] in TensorType.get_need_acces_dram_first() else 0) for tensor_id in self._io_tensor_size.keys()}
            self._dram_bytes_tensor_dict: dict[int, int] = {tensor_id: tensor_size for tensor_id, tensor_size in self._io_tensor_size.items() if self._io_tensor_need_dram[tensor_id] == 1}
            self._dram_bytes = sum(self._dram_bytes_tensor_dict.values())
        
        def _initialize_bypass_lists(self):
            """
            根据 tensor_type_dict 初始化 dram_bypass 和 spm_bypass 列表
            """
            # 获取权重张量ID
            weight_ids = self.weight_ids
            
            # 为每个张量设置 bypass 值
            for tensor_id in self._layer.data:
                if tensor_id in weight_ids:
                    # 权重张量通常存储在DRAM中，SPM bypass
                    self._dram_bypass.append(1)
                    self._spm_bypass.append(0)
                else:
                    tensor_type = self._tensor_type_dict.get(tensor_id, TensorType.UNKNOWN)
                    if tensor_type.is_spm_bypass():
                        # SPM bypass 张量
                        self._dram_bypass.append(0)
                        self._spm_bypass.append(1)
                    else:
                        # DRAM bypass 张量
                        assert(tensor_type.is_dram_bypass())
                        self._dram_bypass.append(1)
                        self._spm_bypass.append(0)
        
        def _update_layer_mapping(self):
            """
            根据当前的 bypass 设置和加速器数量更新层映射
            """
            self._layer_mapping = self._layer_mapping_record.getMap(
                MTMRecord.MapKey(self._dram_bypass, self._spm_bypass, self._num_acc)
            )
        
        def get_dram_bytes(self):
            return self._dram_bytes
        
        def get_dram_bytes_tensor_dict(self):
            return self._dram_bytes_tensor_dict 
           
        def set_num_acc(self, num_acc):
            self._num_acc = num_acc
            self._compute_cycles = self._layer.compute // (self._num_acc * self._pipeline_target.target.num_macs_per_array)
        
        def set_tensor_type(self, tensor_id: int, tensor_type: TensorType):
            old_tensor_type: TensorType = self._tensor_type_dict[tensor_id]
            old_need_dram: int = self._io_tensor_need_dram[tensor_id]
            new_need_dram: int = 1 if tensor_type in TensorType.get_need_acces_dram_first() else 0
            data_size: int = self._io_tensor_size[tensor_id]
            
            self._dram_bytes = self._dram_bytes + data_size * (new_need_dram - old_need_dram)
            
            self._io_tensor_need_dram[tensor_id] = new_need_dram
            self._tensor_type_dict[tensor_id] = tensor_type
            
            # 更新 bypass 列表以反映新的 tensor_type
            self._update_bypass_for_tensor(tensor_id, tensor_type)
            # 重新获取层映射
            self._update_layer_mapping()
        
        def _update_bypass_for_tensor(self, tensor_id: int, tensor_type: TensorType):
            """
            更新特定张量的 bypass 设置
            """
            # 找到张量在列表中的索引
            tensor_idx = self._layer.data.index(tensor_id)
            
            # 更新 bypass 列表
            if tensor_id in self.weight_ids:
                # 权重张量
                self._dram_bypass[tensor_idx] = 1
                self._spm_bypass[tensor_idx] = 0
            elif tensor_type.is_spm_bypass():
                self._dram_bypass[tensor_idx] = 0
                self._spm_bypass[tensor_idx] = 1
            else:
                assert(tensor_type.is_dram_bypass())
                self._dram_bypass[tensor_idx] = 1
                self._spm_bypass[tensor_idx] = 0
        
        def get_compute_cycles(self):
            return self._compute_cycles
        
    class SegmentSolution:
        
        SPM_EXCEED_PANITY = int(1e13)
        
        def __init__(self,
                     start_layer_idx: int,
                     end_layer_idx: int,
                     model: Model,
                     tensor_type_dict_list: list[dict[int, TensorType]],
                     num_acc_list: list[int],
                     pipeline_target: PipelineTarget):
            self._start_layer_idx = start_layer_idx
            self._end_layer_idx = end_layer_idx
            self._model = model
            self._tensor_type_dict_list = tensor_type_dict_list
            self._num_acc_list = num_acc_list
            self._pipeline_target = pipeline_target
            self._tensor_ids = self._model.get_tensor_ids(start_layer_idx, end_layer_idx)
            self._input_ids = self._model.get_segment_layer_input_ids(start_layer_idx, end_layer_idx)
            self._weight_ids = self._model.get_segment_layer_weight_ids(start_layer_idx, end_layer_idx)
            self._output_ids = self._model.get_segment_layer_output_ids(start_layer_idx, end_layer_idx)
            
            self._layer_solution_list = [PipelineSolution.LayerSolution(
                layer=self._model.layerList[layer_idx],
                layer_mapping_record=self._model.get_layer_mapping_all(layer_idx),
                num_acc=self._num_acc_list[layer_idx_in_segment],
                tensor_type_dict=self._tensor_type_dict_list[layer_idx_in_segment],
                pipeline_target=self._pipeline_target,
                input_ids=self._input_ids[layer_idx_in_segment],
                weight_ids=self._weight_ids[layer_idx_in_segment],
                output_ids=self._output_ids[layer_idx_in_segment]
            ) for layer_idx_in_segment, layer_idx in enumerate(range(start_layer_idx, end_layer_idx + 1))]
        
            self._dram_bytes_list = [x.get_dram_bytes() for x in self._layer_solution_list]
            self._total_dram_bytes = sum(self._dram_bytes_list)
            self._generate_tensor_meta_type()
            self._generate_dram_cycles()
            self._generate_stage_info()
            self._generate_noc_cycles()
            self._generate_stage_cycles_all()
            self._generate_ring_buffer()
            self._generate_spm_need()
            self._generate_acc_use()
        
        def _generate_tensor_meta_type(self):
            self._tensor_meta_type: dict[int, TensorMetaType] = {}
            for tensor_type_dict in self._tensor_type_dict_list:
                for tensor_id, tensor_type in tensor_type_dict.items():
                    if self._tensor_meta_type.get(tensor_id, TensorMetaType.UNKNOWN) == TensorMetaType.UNKNOWN:
                        self._tensor_meta_type[tensor_id] = get_meta_type(tensor_type)
                    else:
                        assert(self._tensor_meta_type[tensor_id] == get_meta_type(tensor_type))
        
        def _generate_dram_cycles(self):
            self._dram_cycles = [(0 if x == 0 else int(self._total_dram_bytes // self._pipeline_target.target.dram_bw_per_cycle)) for x in self._dram_bytes_list]
        
        def _generate_stage_info(self):
            self._layer_list: list[Layer] = self._model.layerList[self._start_layer_idx: self._end_layer_idx + 1]
            
            self._tensor_id_to_as_input_stage_idx_vec = self._model.get_tensor_as_input_stage_dict(self._start_layer_idx, self._end_layer_idx)
            self._tensor_id_to_as_output_stage_idx_vec = self._model.get_tensor_as_output_stage_dict(self._start_layer_idx, self._end_layer_idx)
                        
            self._depth_list = self._model.get_depth(self._start_layer_idx, self._end_layer_idx)
            
            self._tensor_in_degree = self._model.get_in_degree(self._start_layer_idx, self._end_layer_idx)
            self._tensor_out_degree = self._model.get_out_degree(self._start_layer_idx, self._end_layer_idx)
            self._tensor_max_size = self._model.get_max_size(self._start_layer_idx, self._end_layer_idx)
            self._tensor_min_size = self._model.get_min_size(self._start_layer_idx, self._end_layer_idx)
        
        def _generate_noc_cycles(self):
            self._noc_cycles = []
            for stage_idx in range(len(self._layer_list)):
                self._noc_cycles.append(self._generate_noc_cycles_for_layer(stage_idx))
            
        def _generate_noc_cycles_for_layer(self, stage_idx: int):
            layer = self._layer_list[stage_idx]
            noc_cycles = {}
            for id in self._input_ids[stage_idx]:
                if self._tensor_out_degree.get(id, 0) != 0:
                    noc_cycles[id] = self._pipeline_target.target.ceilPage(self._tensor_min_size[id]) // self._pipeline_target.target.noc_bw_per_cycle
            
            for id in self._output_ids[stage_idx]:
                if self._tensor_in_degree.get(id, 0) != 0:
                    noc_cycles[id] = (self._tensor_in_degree[id] * self._pipeline_target.target.ceilPage(self._tensor_min_size[id])) // self._pipeline_target.target.noc_bw_per_cycle
            
            return noc_cycles
        
        def _generate_stage_cycles_per_layer(self, stage_idx: int):
            ids = [(id, 1) for id in self._input_ids[stage_idx]] + [(id, 0) for id in self._output_ids[stage_idx]]
            
            dram_max_cycles = 0
            noc_max_cycles = 0
            
            dram_add_cycles = 0
            noc_add_cycles = 0
            
            compute_cycles = self._layer_solution_list[stage_idx].get_compute_cycles()
            
            remote_num = 0
            
            for tensor_id, is_input in ids:
                type = self._tensor_type_dict_list[stage_idx][tensor_id]
                if type in TensorType.get_need_dram_add():
                    dram_add_cycles = max(dram_add_cycles, self._dram_cycles[stage_idx])
                elif type in TensorType.get_need_dram_max():
                    dram_max_cycles = max(dram_max_cycles, self._dram_cycles[stage_idx])
                
                if type in TensorType.get_need_noc_add():
                    noc_add_cycles += self._noc_cycles[stage_idx][tensor_id]
                elif type in TensorType.get_need_noc_max():
                    noc_max_cycles += self._noc_cycles[stage_idx][tensor_id]

                if (type in [TensorType.INTER_PURE_DECOUPLING]) or \
                    (type in [TensorType.INTER_SHARED_WRITE] and is_input) or \
                        (type in [TensorType.INTER_SHARED_READ] and not is_input):
                    remote_num += 1
            
            compute_cycles = compute_cycles * int(pow(self._pipeline_target.remote_layer_compute_ratio, remote_num))
            
            return max(dram_add_cycles + compute_cycles + noc_add_cycles, dram_max_cycles, noc_max_cycles)
        
        
        def _generate_stage_cycles_all(self):
            self._stage_cycles = []
            for stage_idx in range(len(self._layer_list)):
                self._stage_cycles.append(self._generate_stage_cycles_per_layer(stage_idx))
        
        def _generate_ring_buffer(self):
            self._ring_buffer_count: dict[int, int] = {}
            self._ring_buffer_size_per: dict[int, int] = {}
            self._ring_buffer_use_count: dict[int, int] = {}
            self._ring_buffer_use_spm: dict[int, bool] = {}
            
            for tensor_id, as_output_stage_idx_vec in self._tensor_id_to_as_output_stage_idx_vec.items():
                assert(len(as_output_stage_idx_vec) == 1)
                
                max_nxt_depth = 0
                if self._tensor_id_to_as_input_stage_idx_vec.get(tensor_id, []):
                    for as_input_stage_idx in self._tensor_id_to_as_input_stage_idx_vec[tensor_id]:
                        stage_depth = self._depth_list[as_input_stage_idx]
                        max_nxt_depth = max(max_nxt_depth, stage_depth)
                
                diff = max_nxt_depth - self._depth_list[as_output_stage_idx_vec[0]]
                
                use_ring_buffer = False
                if self._tensor_meta_type[tensor_id] == TensorMetaType.TYPE_PURE_DECOUPLING:
                    use_ring_buffer = True
                elif self._tensor_meta_type[tensor_id] == TensorMetaType.TYPE_NORMAL_SPM:
                    if diff > 1:
                        use_ring_buffer = True
                elif self._tensor_meta_type[tensor_id] == TensorMetaType.TYPE_NORMAL_DRAM:
                    use_ring_buffer = True
                    
                if use_ring_buffer:
                    self._ring_buffer_count[tensor_id] = diff + 1
                    self._ring_buffer_size_per[tensor_id] = self._pipeline_target.target.ceilPage(self._tensor_max_size[tensor_id] if self._tensor_meta_type[tensor_id] in [TensorMetaType.TYPE_SHARED, TensorMetaType.TYPE_PURE_DECOUPLING] else self._tensor_min_size[tensor_id])
                    self._ring_buffer_use_count[tensor_id] = len(self._tensor_id_to_as_input_stage_idx_vec[tensor_id])
                    self._ring_buffer_use_spm[tensor_id] = not (self._tensor_meta_type[tensor_id] == TensorMetaType.TYPE_NORMAL_DRAM)
        
        def _generate_tensor_spm_need(self, tensor_id: int):
            # 统计stage中各个buffer的大小
            if self._tensor_meta_type[tensor_id] == TensorMetaType.TYPE_IO:
                stage_idx_vec = self._tensor_id_to_as_output_stage_idx_vec.get(tensor_id, []) + self._tensor_id_to_as_input_stage_idx_vec.get(tensor_id, [])
                for stage_idx in stage_idx_vec:
                    layer = self._layer_list[stage_idx]
                    data_size = self._pipeline_target.target.ceilPage(layer.getDataSize(tensor_id))
                    if self._tensor_type_dict_list[stage_idx][tensor_id] == TensorType.IO_DOUBLE:
                        self._tensor_spm_util_in_stage[stage_idx][tensor_id] = data_size * 2
                    elif self._tensor_type_dict_list[stage_idx][tensor_id] == TensorType.IO_SINGLE:
                        self._tensor_spm_util_in_stage[stage_idx][tensor_id] = data_size
            elif self._tensor_meta_type[tensor_id] == TensorMetaType.TYPE_NORMAL_DRAM:
                stage_idx_vec = self._tensor_id_to_as_output_stage_idx_vec.get(tensor_id, []) + self._tensor_id_to_as_input_stage_idx_vec.get(tensor_id, [])
                for stage_idx in stage_idx_vec:
                    layer = self._layer_list[stage_idx]
                    data_size = self._pipeline_target.target.ceilPage(layer.getDataSize(tensor_id))
                    if self._tensor_type_dict_list[stage_idx][tensor_id] == TensorType.INTER_DRAM_SINGLE:
                        self._tensor_spm_util_in_stage[stage_idx][tensor_id] = data_size
                    elif self._tensor_type_dict_list[stage_idx][tensor_id] == TensorType.INTER_DRAM_DOUBLE:
                        self._tensor_spm_util_in_stage[stage_idx][tensor_id] = data_size * 2
                    else:
                        assert(False)
            elif self._tensor_meta_type[tensor_id] == TensorMetaType.TYPE_NORMAL_SPM:
                stage_idx_vec = self._tensor_id_to_as_output_stage_idx_vec[tensor_id] + self._tensor_id_to_as_input_stage_idx_vec[tensor_id]
                for stage_idx in stage_idx_vec:
                    layer = self._layer_list[stage_idx]
                    data_size = self._pipeline_target.target.ceilPage(layer.getDataSize(tensor_id))
                    if self._tensor_type_dict_list[stage_idx][tensor_id] == TensorType.INTER_SINGLE:
                        self._tensor_spm_util_in_stage[stage_idx][tensor_id] = data_size
                    elif self._tensor_type_dict_list[stage_idx][tensor_id] == TensorType.INTER_DOUBLE:
                        self._tensor_spm_util_in_stage[stage_idx][tensor_id] = data_size * 2
                    else:
                        assert(False)
            elif self._tensor_meta_type[tensor_id] == TensorMetaType.TYPE_SHARED:
                self._tensor_spm_util_shared[tensor_id] = self._pipeline_target.target.ceilPage(self._tensor_max_size[tensor_id]) * 2
            elif self._tensor_meta_type[tensor_id] == TensorMetaType.TYPE_PURE_DECOUPLING:
                spm_need = 0
                assert(self._ring_buffer_count.get(tensor_id, 0) > 0)
            else:
                assert(False)
            
            # ring buffer
            ring_buffer_need = self._ring_buffer_count.get(tensor_id, 0) * self._ring_buffer_size_per.get(tensor_id, 0)
            if self._ring_buffer_use_spm.get(tensor_id, False) and ring_buffer_need > 0:
                self._tensor_spm_util_in_ringbuffer[tensor_id] = ring_buffer_need
        
        def _generate_spm_need(self):
            self._tensor_spm_util_in_stage = [{} for stage_idx in range(len(self._layer_list))] # stage_idx -> tensor_id -> dram_util ，仅用于io、normal spm和normal dram类型
            self._tensor_spm_util_shared = {} # tensor_id -> dram_util，仅用于shared
            self._tensor_spm_util_in_ringbuffer = {} # tensor_id -> dram_util
            self._weight_spm_util_stage = [sum([self._pipeline_target.target.ceilPage(layer.getDataSize(weight_id)) for weight_id in self._weight_ids[stage_idx]]) for stage_idx, layer in enumerate(self._layer_list)]
            for tensor_id in self._tensor_max_size.keys():
                self._generate_tensor_spm_need(tensor_id)
            
            self._total_weight_spm_util = sum(self._weight_spm_util_stage)
            self._total_io_spm_util = sum(self._tensor_spm_util_in_ringbuffer.values()) + sum(self._tensor_spm_util_shared.values()) + sum([sum(stage_tensor_som_util.values()) for stage_tensor_som_util in self._tensor_spm_util_in_stage])    
            self._total_spm_util = self._total_weight_spm_util + self._total_io_spm_util
        
        def _generate_acc_use(self):
            self.acc_use = sum(self._num_acc_list)
        
        def evaluate(self):
            exec_cost = 0
            
            if len(self._layer_list) > self._pipeline_target.default_batch:
                cost = 0
                for idx in range(len(self._layer_list) + self._pipeline_target.default_batch - 1):
                    cost += max(self._stage_cycles[max(0, idx - self._pipeline_target.default_batch + 1):min(len(self._layer_list) + 1, idx + 1)])
                exec_cost = cost
            else:
                pre_max = list(accumulate(self._stage_cycles, max))
                suc_max = list(accumulate(self._stage_cycles[::-1], max))[::-1]
                max_time = pre_max[-1]
                fill_time = sum(pre_max)
                drain_time = sum(suc_max) - max_time
                exec_cost = fill_time + drain_time + (self._pipeline_target.default_batch - len(self._layer_list)) * max_time

            if self._pipeline_target.enable_decoupling:            
                if self._total_spm_util > self._pipeline_target.target.spmBytes:
                    exec_cost += self.SPM_EXCEED_PANITY
            else:
                for stage_idx, stage_acc_use in enumerate(self._num_acc_list):
                    stage_spm_cap = stage_acc_use * self._pipeline_target.target.spmBytesPerArray
                    stage_spm_util = sum(list(self._tensor_spm_util_in_stage[stage_idx].values())) + self._weight_spm_util_stage[stage_idx]
                    if stage_spm_util > stage_spm_cap:
                        exec_cost += self.SPM_EXCEED_PANITY
                        continue
                    
            spm_cost = self._total_spm_util / self._pipeline_target.target.spmBytesPerArray / self._pipeline_target.target.numArrays
            acc_cost = self.acc_use / self._pipeline_target.target.numArrays
            
            self.exec_cost = exec_cost
            self.cost = exec_cost + (spm_cost + acc_cost) * exec_cost * self._pipeline_target.resource_util_ratio
            
            return self.cost
        
        def get_exec_cost(self):
            return self.exec_cost
    
    def __init__(self,
                 pipeline_target: PipelineTarget,
                 partition_plan: list[tuple[int, int]],
                 acc_plan:  list[list[int]],
                 tensor_type: list[list[dict[int, TensorType]]]
                ):
        """
        初始化解
        
        Args:
            pipeline_target: 搜索目标（约束条件）
            partition_plan: 图划分方案. e.g. [(0, 2), (3, 5)]表示划分为两个segment,第一个segment包含[0,1,2]三个层,第二个segment包含[3,4,5]三个层
            acc_plan:  加速器划分方案。e.g. [[2, 4], [5, 1]]表示第一个段中,第一层获得2个加速器、第二层获得4个加速器;第二个段中,第一层获得5个加速器,第二层获得1个加速器
            tenor_type: buffer方案。tensor_type[i][j][k] 表示第i个segment中第j层tensorid=k的tensor类型
        """
        self.pipeline_target = pipeline_target
        
        self._partition_plan: list[tuple[int, int]] = partition_plan
        self._acc_plan: list[list[int]] = acc_plan
        self._cost: float = float('inf')
        self._tensor_type:list[list[dict[int, TensorType]]] = tensor_type
        
        # 创建segment solution
        self._segment_solution_list: list[PipelineSolution.SegmentSolution] = []
        for segment_idx, (start_layer_idx, end_layer_idx) in enumerate(partition_plan):
            self._segment_solution_list.append(PipelineSolution.SegmentSolution(start_layer_idx=start_layer_idx, end_layer_idx=end_layer_idx, model=self.pipeline_target.model, tensor_type_dict_list=self._tensor_type[segment_idx], num_acc_list=self._acc_plan[segment_idx], pipeline_target=self.pipeline_target))

        self._tensor_meta_type = [x._tensor_meta_type for x in self._segment_solution_list]
        
        self._cost = sum([x.evaluate() for x in self._segment_solution_list])
        
    def get_cost(self) -> float:
        return self._cost
    
    def get_exec_cost(self) -> float:
        return sum([x.get_exec_cost() for x in self._segment_solution_list])
    
    def get_segment_acc_use(self) -> list[int]:
        return [x.acc_use for x in self._segment_solution_list]
    
    def get_tensor_meta_type(self) -> list[dict[int, TensorMetaType]]:
        return self._tensor_meta_type
    
class Evaluator(ABC):
    def __init__(self, pipeline_target: PipelineTarget):
        self.pipeline_target = pipeline_target
        
    @abstractmethod
    def evaluate(self, solution: PipelineSolution) -> float:    
        """
        评估解的评估结果
        
        Args:
            solution: PipelineSolution对象
            
        Returns:
            float: 评估结果
        """
        pass

class AnalysisModelEvaluator(Evaluator):
    def __init__(self, pipeline_target: PipelineTarget):
        super(AnalysisModelEvaluator, self).__init__(pipeline_target)
        
    def evaluate(self, solution: PipelineSolution):
        return solution.get_cost()

class GraphPartitionSimulatedAnnealing(Annealer):
    """
    使用模拟退火算法优化Pipeline划分的搜索框架
    """
    
    class KEY:
        partition_plan = 'partition_plan'
        acc_plan = 'acc_plan'
        tensor_type = 'tensor_type'
        tensor_meta_type = 'tensor_meta_type'
            
    def __init__(self, 
                 pipeline_target: PipelineTarget,
                 partition_plan: list[tuple[int, int]],
                 acc_plan: list[list[int]],
                 layer_mapping: list[MTMRecord],
                 max_iterations: int = 10000000,
                 seed: int = 20001002,
                 update_stride: int = 1,
                 enable_io_no_buffer: bool = False,
                 gemini_like: bool = False,
                 tangram_like: bool = False,
                 enable_partition_search: bool = False):
        """
        初始化模拟退火搜索器
        
        Args:
            pipeline_target: PipelineTarget对象, 包含搜索目标（约束条件）
            initial_solution: 初始解
            max_iterations: 最大迭代次数
        """
        random.seed(seed)
        self.pipeline_target: PipelineTarget = pipeline_target
        self.max_iterations: int = max_iterations
        self.layer_mapping: list[MTMRecord] = layer_mapping
        self.enable_io_no_buffer: bool = enable_io_no_buffer
        self.gemini_like: bool = gemini_like
        self.tangram_like: bool = tangram_like
        self.enable_partition_search: bool = enable_partition_search
        
        # 初始化tensortype和tensor meta type
        tensor_type: list[list[dict[int, TensorType]]] = [] # segment_idx -> stage_idx -> tensor_id -> tensor type
        tensor_meta_type: list[dict[int, TensorMetaType]] = [] # segment_idx -> tensor_id -> tensor meta type
        for segment_idx, (start_layer_idx, end_layer_idx) in enumerate(partition_plan):
            segment_layers: list[Layer] = self.pipeline_target.model.layerList[start_layer_idx: end_layer_idx + 1]
            segment_tensor_type: list[dict[int, TensorType]] = []
            segment_tensor_metatype: dict[int, TensorMetaType] = {}
            in_degree = self.pipeline_target.model.get_in_degree(start_layer_idx, end_layer_idx)
            out_degree = self.pipeline_target.model.get_out_degree(start_layer_idx, end_layer_idx)
            for stage_idx, layer in enumerate(segment_layers):
                stage_tensor_type: dict[int, TensorType] = {}   
                for id in self.pipeline_target.model.get_segment_layer_input_ids(start_layer_idx, end_layer_idx)[stage_idx] + self.pipeline_target.model.get_segment_layer_output_ids(start_layer_idx, end_layer_idx)[stage_idx]:
                    if in_degree.get(id, 0) == 0 or out_degree.get(id, 0) == 0:
                        stage_tensor_type[id] = TensorType.IO_SINGLE
                        segment_tensor_metatype[id] = TensorMetaType.TYPE_IO
                    else:
                        stage_tensor_type[id] = TensorType.INTER_SINGLE
                        segment_tensor_metatype[id] = TensorMetaType.TYPE_NORMAL_SPM
                        
                segment_tensor_type.append(stage_tensor_type)
            
            tensor_type.append(segment_tensor_type)
            tensor_meta_type.append(segment_tensor_metatype)
        
        # 记录所有搜索过的解及其评估结果
        self.search_history: list[tuple[dict, float]] = []
        self.search_history_enery: list[float] = []
        
        # 初始化状态
        super(GraphPartitionSimulatedAnnealing, self).__init__({
            self.KEY.partition_plan: partition_plan,
            self.KEY.acc_plan: acc_plan,
            self.KEY.tensor_type: tensor_type,
            self.KEY.tensor_meta_type: tensor_meta_type
        })
        
        self.init_solution = self._state_to_solution(self.state)
        self.init_energy = self.init_solution.get_cost()
        
        
        # 设置模拟退火参数
        self.steps = max_iterations
        self.Tmax = 5000000.0  # 初始温度 TODO 调大试一试
        self.Tmin = 2.5      # 最终温度
        # self.updates = self.steps // update_stride   # 更新频率
        self.updates = 0
        
        # 设置模拟退火算子
        self._op_list = []
        
        if self.gemini_like:
            self._op_list = [self._op_move_acc,
                            self._op_add_acc,
                            self._op_remove_acc]
        elif self.tangram_like:
            self._op_list = []
        else:
            self._op_list = [self._op_move_acc,
                            self._op_add_acc,
                            self._op_remove_acc,
                            self._op_change_tensor_meta_type,
                            self._op_change_tensor_type]
        
        
        if self.enable_partition_search:
            self._op_list.append(self._op_merge_adjacent_segments)
            self._op_list.append(self._op_split_segment)

        # internal: keep last operation record for logging
        self._last_change_record = None
        # logging store
        self._trace_steps = []
        self._trace_meta = {}
        self._enable_trace = False
            
    def _solution_to_state(self, solution: PipelineSolution) -> dict:
        """
        将PipelineSolution解转换为状态字典
        
        Args:
            solution: PipelineSolution对象
            
        Returns:
            dict: 状态字典
        """
        
        state = {
            self.KEY.partition_plan: solution.partition_plan,
            self.KEY.acc_plan: solution.acc_plan,
            self.KEY.tensor_type: solution.tensor_type,
            self.KEY.tensor_meta_type: solution.tensor_meta_type
        }
        return state

    # -------- helper: initialize tensor type/meta for a segment --------
    def _init_segment_tensor_info(self, start_layer_idx: int, end_layer_idx: int):
        segment_layers: list[Layer] = self.pipeline_target.model.layerList[start_layer_idx: end_layer_idx + 1]
        segment_tensor_type: list[dict[int, TensorType]] = []
        segment_tensor_metatype: dict[int, TensorMetaType] = {}
        in_degree = self.pipeline_target.model.get_in_degree(start_layer_idx, end_layer_idx)
        out_degree = self.pipeline_target.model.get_out_degree(start_layer_idx, end_layer_idx)
        for stage_idx, _ in enumerate(segment_layers):
            stage_tensor_type: dict[int, TensorType] = {}
            inputs = self.pipeline_target.model.get_segment_layer_input_ids(start_layer_idx, end_layer_idx)[stage_idx]
            outputs = self.pipeline_target.model.get_segment_layer_output_ids(start_layer_idx, end_layer_idx)[stage_idx]
            for tid in inputs + outputs:
                if in_degree.get(tid, 0) == 0 or out_degree.get(tid, 0) == 0:
                    stage_tensor_type[tid] = TensorType.IO_SINGLE
                    segment_tensor_metatype[tid] = TensorMetaType.TYPE_IO
                else:
                    stage_tensor_type[tid] = TensorType.INTER_SINGLE
                    segment_tensor_metatype[tid] = TensorMetaType.TYPE_NORMAL_SPM
            segment_tensor_type.append(stage_tensor_type)
        return segment_tensor_type, segment_tensor_metatype
    
    def _state_to_solution(self, state: dict) -> PipelineSolution:
        """
        将状态字典转换为PipelineSolution解
        
        Args:
            state: 状态字典
            
        Returns:
            PipelineSolution: 重构的PipelineSolution对象
        """
        
        solution = PipelineSolution(
            pipeline_target=self.pipeline_target,
            partition_plan=state[self.KEY.partition_plan],
            acc_plan=state[self.KEY.acc_plan],
            tensor_type=state[self.KEY.tensor_type]
        )
        return solution
    
    def _op_update_acc(self, state, idx_diff: int):
        """Add an accelerator to a randomly selected layer if possible"""
        acc_plan = state[self.KEY.acc_plan]
        target = self.pipeline_target.target
        
        # Get all possible (segment_idx, layer_idx) pairs
        all_positions = []
        for segment_idx, segment in enumerate(acc_plan):
            for layer_idx in range(len(segment)):
                all_positions.append((segment_idx, layer_idx))
        
        # Randomly shuffle the positions to try
        random.shuffle(all_positions)
        
        for segment_idx, layer_idx in all_positions:
            current_acc = acc_plan[segment_idx][layer_idx]            
            current_idx = target.allowed_acc_nums.index(current_acc)
            next_idx = current_idx + idx_diff
            
            # If there's a next value
            if next_idx >= 0 and next_idx < len(target.allowed_acc_nums):
                next_acc = target.allowed_acc_nums[next_idx]
                
                # Check if adding would exceed the total numArrays
                segment_total = sum(acc_plan[segment_idx])
                if segment_total - current_acc + next_acc <= target.numArrays:
                    # Update the state
                    # record
                    self._last_change_record = {
                        "type_id": (1 if idx_diff > 0 else 2),
                        "type": ("add_acc" if idx_diff > 0 else "remove_acc"),
                        "segment_idx": segment_idx,
                        "layer_idx": layer_idx,
                        "from": current_acc,
                        "to": next_acc
                    }
                    acc_plan[segment_idx][layer_idx] = next_acc
                    return True
        
        # No valid move found
        return False
    
    def _op_add_acc(self, state):
        return self._op_update_acc(state, 1)
    
    def _op_remove_acc(self, state):
        return self._op_update_acc(state, -1)
    
    def _op_move_acc(self, state):
        """
        在同一个segment内, 将一个网络层的加速器数量减少, 另一个网络层的加速器数量增加
        
        Returns:
            bool: 是否成功执行操作
        """
        acc_plan = state[self.KEY.acc_plan]
        target = self.pipeline_target.target
        
        # 获取所有可能的段索引（至少包含2个层的段）
        segment_indices = [i for i, segment in enumerate(acc_plan) if len(segment) >= 2]
        if not segment_indices:
            return False
        
        random.shuffle(segment_indices)
        
        # 随机选择一个段
        for segment_idx in segment_indices:
            segment = acc_plan[segment_idx]
            
            # 获取该段中所有可能的层索引对
            layer_indices = list(range(len(segment)))
            random.shuffle(layer_indices)
            
            # 尝试所有可能的层对组合
            for i in range(len(layer_indices)):
                for j in range(len(layer_indices)):
                    if i == j:
                        continue
                    
                    from_idx = layer_indices[i]
                    to_idx = layer_indices[j]
                    
                    # 从from_idx层减少加速器，向to_idx层增加加速器
                    current_from_acc = segment[from_idx]
                    current_to_acc = segment[to_idx]
                    
                    current_from_acc_idx = target.allowed_acc_nums.index(current_from_acc)
                    current_to_acc_idx = target.allowed_acc_nums.index(current_to_acc)
                    
                    nxt_from_acc_idx = current_from_acc_idx - 1
                    nxt_to_acc_idx = current_to_acc_idx + 1
                    
                    if nxt_from_acc_idx < 0 or nxt_to_acc_idx >= len(target.allowed_acc_nums):
                        continue
                    
                    nxt_from_acc = target.allowed_acc_nums[nxt_from_acc_idx]
                    nxt_to_acc = target.allowed_acc_nums[nxt_to_acc_idx]
                    
                    # 检查移动后是否仍然满足总加速器数量限制
                    segment_total = sum(segment)
                    new_segment_total = segment_total - current_from_acc - current_to_acc + nxt_from_acc + nxt_to_acc
                    if new_segment_total <= target.numArrays:
                        # 更新状态
                        self._last_change_record = {
                            "type_id": 0,
                            "type": "move_acc",
                            "segment_idx": segment_idx,
                            "from_layer_idx": from_idx,
                            "to_layer_idx": to_idx,
                            "from_acc": {"from": current_from_acc, "to": nxt_from_acc},
                            "to_acc": {"from": current_to_acc, "to": nxt_to_acc}
                        }
                        segment[from_idx] = nxt_from_acc
                        segment[to_idx] = nxt_to_acc
                        return True
        
        # 没有找到有效的移动
        return False

    def _op_shift_boundary_left(self, state):
        """Move last layer of a segment to the next segment (left -> right)."""
        partition_plan = state[self.KEY.partition_plan]
        acc_plan = state[self.KEY.acc_plan]
        tensor_type = state[self.KEY.tensor_type]
        tensor_meta_type = state[self.KEY.tensor_meta_type]
        target = self.pipeline_target.target

        if len(partition_plan) < 2:
            return False

        seg_indices = list(range(len(partition_plan) - 1))
        random.shuffle(seg_indices)

        for seg_idx in seg_indices:
            start1, end1 = partition_plan[seg_idx]
            start2, end2 = partition_plan[seg_idx + 1]

            if end1 + 1 != start2:
                assert(False)
            if end1 == start1:
                continue  # cannot shrink to empty
            
            
            donor_candidates = list(range(len(acc_plan[seg_idx + 1])))
            random.shuffle(donor_candidates)

            donor_idx = None
            donor_new_acc = None
            new_layer_acc = None
            
            sum_acc = sum(acc_plan[seg_idx])
            if sum_acc < target.numArrays:
                for k in range(target.allowed_acc_nums, -1, -1):
                    if k <= target.numArrays - sum_acc:
                        new_layer_acc = k
                        break
            else:
                for cand in donor_candidates:
                    donor_acc = acc_plan[seg_idx + 1][cand]
                    allowed = target.allowed_acc_nums
                    if donor_acc not in allowed:
                        assert(False)
                    pos = allowed.index(donor_acc)
                    if pos == 0:
                        continue  # cannot steal from minimal accel setting
                    donor_new_acc = allowed[pos - 1]
                    new_layer_acc = donor_new_acc  # keep segment total unchanged
                    donor_idx = cand
                    break

            if new_layer_acc is None:
                continue

            # Pop layer info from left segment
            moved_layer_tensor_type = tensor_type[seg_idx].pop()
            acc_plan[seg_idx].pop()

            # Update partition ranges
            partition_plan[seg_idx] = (start1, end1 - 1)
            partition_plan[seg_idx + 1] = (end1, end2)

            # Apply stolen acc and insert moved layer at head of right segment
            if donor_new_acc is not None:
                acc_plan[seg_idx + 1][donor_idx] = donor_new_acc
            
            acc_plan[seg_idx + 1] = [new_layer_acc] + acc_plan[seg_idx + 1]
            tensor_type[seg_idx + 1] = [moved_layer_tensor_type] + tensor_type[seg_idx + 1]

            # Rebuild meta types for both affected segments
            # 仅处理seg_idx的输入和输出张量
            tensor_type[seg_idx + 1].append(tensor_type[seg_idx].pop())
            
            # seg_idx
            # 输入张量：如果不存在作为其它层的输入，则为TYPE_IO，tensor_type改为SINGLE;否则不作改动
            # 输出张量：按照拓扑序，不应该作为其它层的输入，不做处理
            sttage_idx_in_pre = end1 - start1 - 1
            input_ids_in_pre = self.pipeline_target.model.get_segment_layer_input_ids(start1, end1)[sttage_idx_in_pre]
            output_ids_in_pre = self.pipeline_target.model.get_segment_layer_input_ids(start1, end1)[sttage_idx_in_pre]
            for tensor_id in input_ids_in_pre:
                in_degree_in_pre = self.pipeline_target.model.get_in_degree(tensor_id, start1, end1 - 1)
                out_degree_in_pre = self.pipeline_target.model.get_out_degree(tensor_id, start1, end1 - 1)
                if in_degree_in_pre.get(tensor_id, 0) == 0 and out_degree_in_pre.get(tensor_id, 0) > 0:
                    tensor_meta_type[seg_idx][tensor_id] = TensorMetaType.TYPE_IO
                    tmp = self.pipeline_target.model.get_tensor_as_output_stage_dict(start1, end1 - 1).get(tensor_id, [])
                    assert(len(tmp) == 1)
                    for stage_idx in tmp:
                        tensor_type[seg_idx][stage_idx][tensor_id] = TensorType.IO_SINGLE
                
                    del tensor_meta_type[seg_idx][tensor_id]
                    
            for tensor_id in output_ids_in_pre:
                del tensor_meta_type[seg_idx][tensor_id]
            
            # seg_idx + 1
            # 输入张量：TYPE_IO,SINGLE
            # 输出张量：如果作为某层的输入，则改为TYPE_NORMAL_SPM，SINGLE
            input_ids_in_nxt = self.pipeline_target.model.get_segment_layer_input_ids(end1, end2)[0]
            output_ids_in_nxt = self.pipeline_target.model.get_segment_layer_output_ids(end1, end2)[0]
            for tensor_id in input_ids_in_nxt:
                if tensor_meta_type[seg_idx + 1].get(tensor_id, None) is not None:
                    assert(tensor_meta_type[seg_idx + 1][tensor_id] == TensorMetaType.TYPE_IO)
                    
                tensor_meta_type[seg_idx + 1][tensor_id] = TensorMetaType.TYPE_IO
                tensor_type[seg_idx + 1][0][tensor_id] = TensorType.IO_SINGLE
            for tensor_id in output_ids_in_nxt:
                in_degree_in_nxt = self.pipeline_target.model.get_in_degree(tensor_id, end1, end2)
                out_degree_in_nxt = self.pipeline_target.model.get_out_degree(tensor_id, end1, end2)
                assert(out_degree_in_nxt.get(tensor_id, 0) == 0)
                if in_degree_in_nxt.get(tensor_id, 0) > 0:
                    tensor_meta_type[seg_idx + 1][tensor_id] = TensorMetaType.TYPE_NORMAL_SPM
                    tmp = self.pipeline_target.model.get_tensor_as_input_stage_dict(end1, end2).get(tensor_id, [])
                    assert(len(tmp) == 1)
                    for stage_idx in tmp:
                        tensor_type[seg_idx + 1][stage_idx][tensor_id] = TensorType.INTER_SINGLE
            
            self._last_change_record = {
                "type_id": 5,
                "type": "shift_boundary_left",
                "from_segment": seg_idx,
                "to_segment": seg_idx + 1,
                "new_partition": [partition_plan[seg_idx], partition_plan[seg_idx + 1]],
                "donor_layer_idx": donor_idx,
                "donor_acc_from": target.allowed_acc_nums[target.allowed_acc_nums.index(donor_new_acc) + 1],
                "donor_acc_to": donor_new_acc,
                "moved_layer_acc": new_layer_acc
            }
            return True

        return False

    def _op_shift_boundary_right(self, state):
        pass

    def _even_acc_plan(self, start_layer_idx: int, end_layer_idx: int) -> list[int]:
        """Generate an acc plan using allowed_acc_nums with longest-layer-first upgrades.

        Start with the minimal allowed acc for every layer in the segment, then repeatedly
        pick the layer with the largest compute time (compute / (acc * macs_per_array))
        and upgrade it to the next allowed acc if total acc budget allows. Stop when no
        further upgrade is possible.
        """
        if start_layer_idx == end_layer_idx:
            for i in range(len(self.pipeline_target.target.allowed_acc_nums) - 1, -1, -1):
                acc = self.pipeline_target.target.allowed_acc_nums[i]
                if acc <= self.pipeline_target.target.numArrays:
                    return [acc]
                
        target = self.pipeline_target.target
        layers = self.pipeline_target.model.layerList[start_layer_idx:end_layer_idx + 1]
        num_layers = len(layers)
        if num_layers == 0:
            return []

        min_acc = target.allowed_acc_nums[0]
        total_min = min_acc * num_layers
        if total_min > target.numArrays:
            return []

        acc_plan = [min_acc for _ in range(num_layers)]
        remaining = target.numArrays - total_min

        def layer_time(idx: int, acc_val: int) -> float:
            # Use layer.compute divided by allocated macs to approximate time
            denom = acc_val * target.num_macs_per_array
            return float('inf') if denom == 0 else layers[idx].compute / denom

        while remaining > 0:
            order = sorted(range(num_layers), key=lambda i: layer_time(i, acc_plan[i]), reverse=True)
            upgraded = False
            for i in order:
                curr = acc_plan[i]
                if curr not in target.allowed_acc_nums:
                    continue
                idx = target.allowed_acc_nums.index(curr)
                if idx + 1 >= len(target.allowed_acc_nums):
                    continue
                next_acc = target.allowed_acc_nums[idx + 1]
                delta = next_acc - curr
                if delta <= remaining:
                    acc_plan[i] = next_acc
                    remaining -= delta
                    upgraded = True
                    break
            if not upgraded:
                break

        return acc_plan

    def _op_merge_adjacent_segments(self, state):
        partition_plan = state[self.KEY.partition_plan]
        acc_plan = state[self.KEY.acc_plan]
        tensor_type = state[self.KEY.tensor_type]
        tensor_meta_type = state[self.KEY.tensor_meta_type]

        if len(partition_plan) < 2:
            return False

        indices = list(range(len(partition_plan) - 1))
        random.shuffle(indices)

        for idx in indices:
            start1, end1 = partition_plan[idx]
            start2, end2 = partition_plan[idx + 1]
            if end1 + 1 != start2:
                assert(False)

            new_start, new_end = start1, end2
            layer_num = new_end - new_start + 1
            new_acc_plan = self._even_acc_plan(new_start, new_end)
            if not new_acc_plan:
                continue

            new_tensor_type, new_tensor_meta = self._init_segment_tensor_info(new_start, new_end)

            partition_plan[idx] = (new_start, new_end)
            del partition_plan[idx + 1]

            acc_plan[idx] = new_acc_plan
            del acc_plan[idx + 1]

            tensor_type[idx] = new_tensor_type
            del tensor_type[idx + 1]

            tensor_meta_type[idx] = new_tensor_meta
            del tensor_meta_type[idx + 1]

            self._last_change_record = {
                "type_id": 6,
                "type": "merge_segments",
                "from_segments": [idx, idx + 1],
                "new_partition": (new_start, new_end)
            }
            return True

        return False

    def _op_split_segment(self, state):
        partition_plan = state[self.KEY.partition_plan]
        acc_plan = state[self.KEY.acc_plan]
        tensor_type = state[self.KEY.tensor_type]
        tensor_meta_type = state[self.KEY.tensor_meta_type]

        if len(partition_plan) == 0:
            return False

        def _segment_cost(seg_idx: int) -> float:
            start, end = partition_plan[seg_idx]
            seg_solution = PipelineSolution.SegmentSolution(
                start_layer_idx=start,
                end_layer_idx=end,
                model=self.pipeline_target.model,
                tensor_type_dict_list=tensor_type[seg_idx],
                num_acc_list=acc_plan[seg_idx],
                pipeline_target=self.pipeline_target,
            )
            return seg_solution.evaluate()

        # Prioritize segments whose cost exceeds the penalty threshold
        high, low = [], []
        for idx in range(len(partition_plan)):
            cost = _segment_cost(idx)
            if cost >= PipelineSolution.SegmentSolution.SPM_EXCEED_PANITY:
                high.append(idx)
            else:
                low.append(idx)
        random.shuffle(high)
        random.shuffle(low)
        indices = high + low
        high_set = set(high)

        for idx in indices:
            start, end = partition_plan[idx]
            if end <= start:
                continue

            # If this segment is in the high-cost bucket, split every layer into its own segment
            if idx in high_set:
                new_partitions = []
                new_accs = []
                new_tensor_types = []
                new_tensor_metas = []
                failed = False
                for layer_id in range(start, end + 1):
                    accs = self._even_acc_plan(layer_id, layer_id)
                    if not accs:
                        failed = True
                        break
                    tt, tm = self._init_segment_tensor_info(layer_id, layer_id)
                    new_partitions.append((layer_id, layer_id))
                    new_accs.append(accs)
                    new_tensor_types.append(tt)
                    new_tensor_metas.append(tm)
                if failed:
                    continue

                # Replace the selected segment with the fully split segments
                partition_plan[idx:idx + 1] = new_partitions
                acc_plan[idx:idx + 1] = new_accs
                tensor_type[idx:idx + 1] = new_tensor_types
                tensor_meta_type[idx:idx + 1] = new_tensor_metas

                self._last_change_record = {
                    "type_id": 7,
                    "type": "split_segment_full_layers",
                    "segment_idx": idx,
                    "split_mode": "per_layer",
                    "new_partitions": new_partitions
                }
                return True

            split_candidates = list(range(start, end))
            random.shuffle(split_candidates)

            for split_pos in split_candidates:
                left_start, left_end = start, split_pos
                right_start, right_end = split_pos + 1, end

                left_acc = self._even_acc_plan(left_start, left_end)
                right_acc = self._even_acc_plan(right_start, right_end)
                if not left_acc or not right_acc:
                    continue

                left_tensor_type, left_tensor_meta = self._init_segment_tensor_info(left_start, left_end)
                right_tensor_type, right_tensor_meta = self._init_segment_tensor_info(right_start, right_end)

                partition_plan[idx] = (left_start, left_end)
                partition_plan.insert(idx + 1, (right_start, right_end))

                acc_plan[idx] = left_acc
                acc_plan.insert(idx + 1, right_acc)

                tensor_type[idx] = left_tensor_type
                tensor_type.insert(idx + 1, right_tensor_type)

                tensor_meta_type[idx] = left_tensor_meta
                tensor_meta_type.insert(idx + 1, right_tensor_meta)

                self._last_change_record = {
                    "type_id": 7,
                    "type": "split_segment",
                    "segment_idx": idx,
                    "split_pos": split_pos,
                    "new_partitions": [(left_start, left_end), (right_start, right_end)]
                }
                return True

        return False
    
    def _op_change_tensor_meta_type(self, state):
        """
        Randomly select a tensor_id and change its TensorMetaType.
        Updates all stages to use the appropriate TensorType based on the new meta type.
        
        Rules:
        1. TYPE_IO cannot be changed
        2. TYPE_NORMAL_DRAM -> INTER_DRAM_SINGLE for all stages
        3. TYPE_NORMAL_SPM -> INTER_SINGLE for all stages
        4. TYPE_SHARED -> INTER_SHARED_WRITE for all stages
        5. TYPE_PURE_DECOUPLING -> INTER_PURE_DECOUPLING for all stages
        
        Returns:
            bool: Whether the operation was successful
        """
        tensor_type = state[self.KEY.tensor_type]
        
        segment_indices = [i for i, _ in enumerate(tensor_type)]
        if not segment_indices:
            return False
        
        random.shuffle(segment_indices)
        
        for segment_idx in segment_indices:
            tensor_meta_type = state[self.KEY.tensor_meta_type][segment_idx]
            tensor_ids = [tensor_id for tensor_id in tensor_meta_type.keys()]
            
            random.shuffle(tensor_ids)
            
            for tensor_id in tensor_ids:
                current_meta_type = tensor_meta_type[tensor_id]                
                if current_meta_type == TensorMetaType.TYPE_IO:
                    continue
                
                # Determine possible new meta types (excluding current and TYPE_IO)
                possible_meta_types = [
                    TensorMetaType.TYPE_NORMAL_DRAM,
                    TensorMetaType.TYPE_NORMAL_SPM,
                    TensorMetaType.TYPE_SHARED,
                    TensorMetaType.TYPE_PURE_DECOUPLING
                ]
                possible_meta_type_idx_candidate = [x for x, meta_type in enumerate(possible_meta_types) if meta_type != current_meta_type]
                new_meta_type_idx = random.choice(possible_meta_type_idx_candidate)
                new_meta_type = possible_meta_types[new_meta_type_idx]
                
                # Determine the appropriate TensorType based on the new meta type
                if new_meta_type == TensorMetaType.TYPE_NORMAL_DRAM:
                    new_tensor_type = TensorType.INTER_DRAM_SINGLE
                elif new_meta_type == TensorMetaType.TYPE_NORMAL_SPM:
                    new_tensor_type = TensorType.INTER_SINGLE
                elif new_meta_type == TensorMetaType.TYPE_SHARED:
                    new_tensor_type = TensorType.INTER_SHARED_WRITE
                elif new_meta_type == TensorMetaType.TYPE_PURE_DECOUPLING:
                    new_tensor_type = TensorType.INTER_PURE_DECOUPLING
                else:
                    assert(False)
                
                # Get old tensor type from first occurrence
                segment_tensor_type = state[self.KEY.tensor_type][segment_idx]
                old_tensor_type = None
                for layer_idx, layer_tensor_type in enumerate(segment_tensor_type):
                    if tensor_id in layer_tensor_type:
                        old_tensor_type = layer_tensor_type[tensor_id]
                        break
                
                tensor_meta_type[tensor_id] = new_meta_type
                
                # Update all occurrences of this tensor_id across this segments
                updated_layers = []
                for layer_idx, layer_tensor_type in enumerate(segment_tensor_type):
                    if tensor_id in layer_tensor_type:
                        layer_tensor_type[tensor_id] = new_tensor_type
                        updated_layers.append(layer_idx)
                self._last_change_record = {
                    "type_id": 3,
                    "type": "change_tensor_meta_type",
                    "segment_idx": segment_idx,
                    "tensor_id": tensor_id,
                    "from_meta_type": current_meta_type.name if hasattr(current_meta_type, 'name') else str(current_meta_type),
                    "to_meta_type": new_meta_type.name if hasattr(new_meta_type, 'name') else str(new_meta_type),
                    "from_tensor_type": old_tensor_type.name if (old_tensor_type and hasattr(old_tensor_type, 'name')) else str(old_tensor_type),
                    "to_tensor_type": new_tensor_type.name if hasattr(new_tensor_type, 'name') else str(new_tensor_type),
                    "affected_layer_indices": updated_layers
                }
                return True
    
    def _op_change_tensor_type(self, state):
        """
        随机选择一个segment、stage和tensor,尝试在同一个tensor meta type下改变其tensor type。
        规则：
        1. TYPE_IO类型不能改变
        2. 只能在当前meta type对应的tensor type中选择(排除当前类型)
        3. 对于SHARED的meta type,更改其tensor type后,同一个segment的相同tensor都应该同步修改为同一个tensor type
        4. 对于SHARED的meta type, 如果是一对多的情况,则必须用写优先
        5. 成功改变后立即返回True,否则继续尝试
        6. 所有尝试失败后返回False
        """
        tensor_type: list[list[dict[int, TensorType]]] = state[self.KEY.tensor_type]
        tensor_meta_type : list[dict[int, TensorMetaType]]= state[self.KEY.tensor_meta_type]
        
        # 定义meta type到对应tensor type的映射
        META_TYPE_TO_TENSOR_TYPES = {
            TensorMetaType.TYPE_NORMAL_DRAM: [TensorType.INTER_DRAM_SINGLE, TensorType.INTER_DRAM_DOUBLE],
            TensorMetaType.TYPE_NORMAL_SPM: [TensorType.INTER_SINGLE, TensorType.INTER_DOUBLE],
            TensorMetaType.TYPE_SHARED: [TensorType.INTER_SHARED_WRITE, TensorType.INTER_SHARED_READ],
            TensorMetaType.TYPE_PURE_DECOUPLING: [TensorType.INTER_PURE_DECOUPLING],
            TensorMetaType.TYPE_IO: [TensorType.IO_NOBUFFER, TensorType.IO_SINGLE] + ([TensorType.IO_DOUBLE] if self.enable_io_no_buffer else [])
        }
        
        # 随机遍历所有segment
        segment_indices = list(range(len(tensor_type)))
        random.shuffle(segment_indices)
        
        for segment_idx in segment_indices:
            seg_tensor_type = tensor_type[segment_idx]
            seg_meta_type = tensor_meta_type[segment_idx]
            
            # 随机遍历segment内所有stage
            stage_indices = list(range(len(seg_tensor_type)))
            random.shuffle(stage_indices)
            
            for stage_idx in stage_indices:
                stage_tensor_type = seg_tensor_type[stage_idx]
                
                # 随机遍历stage内所有tensor
                tensor_ids = list(stage_tensor_type.keys())
                random.shuffle(tensor_ids)
                
                for tensor_id in tensor_ids:
                    meta_type = seg_meta_type[tensor_id]
                    
                    # 跳过TYPE_IO类型（规则1）
                    if meta_type == TensorMetaType.TYPE_IO:
                        continue
                    # 如果是TYPE_SHARED，且一对多，则只能是写优先（规则4）
                    if meta_type == TensorMetaType.TYPE_SHARED:
                        num_stage: int = 0
                        for s_idx, s_tensor_type in enumerate(seg_tensor_type):
                            if tensor_id in s_tensor_type.keys():
                                num_stage += 1
                        if num_stage > 2:                                
                            continue
                    
                    # 获取当前meta type下所有可能的tensor type
                    possible_types = META_TYPE_TO_TENSOR_TYPES.get(meta_type, [])
                    current_type = stage_tensor_type[tensor_id]
                    
                    # 排除当前类型
                    other_types = [t for t in possible_types if t != current_type]
                    
                    if other_types:
                        # 随机选择一个新类型
                        new_type = random.choice(other_types)
                        old_type_name = stage_tensor_type[tensor_id].name if hasattr(stage_tensor_type[tensor_id], 'name') else str(stage_tensor_type[tensor_id])
                        stage_tensor_type[tensor_id] = new_type
                        
                        # 特殊处理SHARED meta type：同步修改segment内所有该tensor id的tensor
                        propagate_layers = []
                        if meta_type == TensorMetaType.TYPE_SHARED:
                            for stage_idx, stage_tensor_type in enumerate(seg_tensor_type):
                                for tensor_id_new, tensor_type in stage_tensor_type.items():
                                    if tensor_id == tensor_id_new:
                                        stage_tensor_type[tensor_id] = new_type
                                        propagate_layers.append(stage_idx)
                        self._last_change_record = {
                            "type_id": 4,
                            "type": "change_tensor_type",
                            "segment_idx": segment_idx,
                            "stage_idx": stage_idx,
                            "tensor_id": tensor_id,
                            "from": old_type_name,
                            "to": new_type.name if hasattr(new_type, 'name') else str(new_type),
                            "propagate": (meta_type == TensorMetaType.TYPE_SHARED),
                            "propagate_layers": propagate_layers
                        }
                        return True  # 成功修改
        
        return False  # 所有尝试都失败
    
    def move(self):
        """
        生成下一个解
        OP1: _op_move_acc
        OP2: _op_change_tensor_type
        """
        
        # 创建算子列表的副本
        available_ops = self._op_list.copy()
        
        # 当还有可用算子时继续尝试
        while available_ops:
            # 随机选择一个算子
            selected_op = random.choice(available_ops)
            
            # 尝试执行该算子
            state = copy.deepcopy(self.state)
            success = selected_op(state)
            
            # 如果成功，直接返回
            # print(success)
            if success:
                self.state = state
                return
            
            # 如果失败，从可用算子列表中移除该算子
            available_ops.remove(selected_op)
        
        # 如果所有算子都返回False，不作改动
        
                
    def energy(self):
        """
        评估当前解的预估周期数
        
        Returns:
            float: 当前解的预估周期数
        """
        current_solution: PipelineSolution = self._state_to_solution(self.state)
        
        return current_solution.get_cost()
    
    def update(self, step, T, E, acceptance, improvement):
        """
        更新搜索过程中的信息
        """
        # 记录当前解和评估结果
        copy_state = copy.deepcopy(self.state)
        self.search_history.append((copy_state, E))
        self.search_history_enery.append(E)
        
        # If tracing is enabled, log this step
        if self._enable_trace and step > 0:
            # Capture last op record and acceptance info
            # Note: simanneal's update is called AFTER acceptance decision
            # so we reconstruct from current state
            self._trace_steps.append({
                "step": step,
                "temperature": T,
                "energy": E,
                "acceptance_rate": acceptance,
                "improvement_rate": improvement,
                "op": copy.deepcopy(self._last_change_record) if self._last_change_record else None,
                "state": self._state_to_serializable(self.state)
            })
            
        # 调用父类的update方法
        super(GraphPartitionSimulatedAnnealing, self).update(step, T, E, acceptance, improvement)
    
    def get_search_history(self):
        """
        获取搜索历史记录
        
        Returns:
            List[Tuple[dict, float]]: 搜索历史记录
        """
        return self.search_history
    
    def get_search_history_enery(self):
        """
        获取搜索历史记录的能量值
        
        Returns:
            List[float]: 搜索历史记录的能量值
        """
        return self.search_history_enery
    
    def get_best_state(self):
        """
        获取最佳解
        
        Returns:
            Tuple[dict, float]: 最佳解和对应的能量值
        """
        return self.best_state, self.best_energy

    # ---------- Trace helpers ----------
    def _build_model_params_yaml(self):
        model = self.pipeline_target.model
        tgt = self.pipeline_target.target
        layers = []
        for idx, layer in enumerate(model.layerList):
            entry = {
                "layer_idx": idx,
                "type": layer.type,
                "inputs": [],
                "outputs": [],
                "weights": []
            }
            for tid in layer.getInputTensorId():
                entry["inputs"].append({
                    "tensor_id": tid,
                    "size": layer.getDataSize(tid),
                    "size_aligned": tgt.ceilPage(layer.getDataSize(tid))
                })
            for tid in layer.getOutputTensorId():
                entry["outputs"].append({
                    "tensor_id": tid,
                    "size": layer.getDataSize(tid),
                    "size_aligned": tgt.ceilPage(layer.getDataSize(tid))
                })
            for tid in layer.getWeightTensorId():
                entry["weights"].append({
                    "tensor_id": tid,
                    "size": layer.getDataSize(tid),
                    "size_aligned": tgt.ceilPage(layer.getDataSize(tid))
                })
            layers.append(entry)
        return {"layers": layers}

    def _build_model_topology_yaml(self):
        model = self.pipeline_target.model
        # Build producer map for tensor ids
        tensor_producer = {}
        for li, layer in enumerate(model.layerList):
            for tid in layer.getOutputTensorId():
                tensor_producer[tid] = li
        edges = []
        for li, layer in enumerate(model.layerList):
            for tid in layer.getInputTensorId() + layer.getWeightTensorId():
                src = tensor_producer.get(tid, None)
                if src is not None:
                    edges.append({
                        "tensor_id": tid,
                        "from_layer": src,
                        "to_layer": li
                    })
        nodes = [{"layer_idx": i, "type": l.type} for i, l in enumerate(model.layerList)]
        return {"nodes": nodes, "edges": edges}

    def _state_to_serializable(self, state: dict):
        # Deep convert enums to names for YAML safety
        s = copy.deepcopy(state)
        # tensor_type: segment -> stage -> {tensor_id: TensorType}
        typed = []
        for seg in s[self.KEY.tensor_type]:
            seg_conv = []
            for stage_map in seg:
                seg_conv.append({tid: (tt.name if hasattr(tt, 'name') else str(tt)) for tid, tt in stage_map.items()})
            typed.append(seg_conv)
        s[self.KEY.tensor_type] = typed
        # tensor_meta_type: segment -> {tensor_id: TensorMetaType}
        meta = []
        for seg_meta in s[self.KEY.tensor_meta_type]:
            meta.append({tid: (mt.name if hasattr(mt, 'name') else str(mt)) for tid, mt in seg_meta.items()})
        s[self.KEY.tensor_meta_type] = meta
        return s

    def run_with_trace(self, output_path: str):
        """
        Run SA using simanneal's anneal() while recording detailed per-step trace,
        then dump a YAML file.
        Content includes:
        - model params (inputs/outputs/weights with page-aligned sizes)
        - model topology
        - per-step: temperature, op record, acceptance/improvement rates, state, energy
        - final best state and energy

        Args:
            output_path: path to output YAML trace file
        """
        # Initialize trace meta
        self._trace_steps = []
        self._enable_trace = True
        self._trace_meta = {
            "created_at": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime()),
            "Tmax": self.Tmax,
            "Tmin": self.Tmin,
            "steps": self.steps,
            "target": {
                "numArrays": self.pipeline_target.target.numArrays,
                "spmKB": self.pipeline_target.target.spmBytes // 1024,
                "spmKB_per_array": self.pipeline_target.target.spmBytesPerArray // 1024,
                "num_macs_per_array": self.pipeline_target.target.num_macs_per_array,
                "dram_bw_per_cycle": self.pipeline_target.target.dram_bw_per_cycle,
                "noc_bw_per_cycle": self.pipeline_target.target.noc_bw_per_cycle,
                "default_batch": self.pipeline_target.default_batch
            }
        }

        # Capture initial state
        initial_state_serializable = self._state_to_serializable(self.state)

        # Build static model info
        model_params = self._build_model_params_yaml()
        model_topology = self._build_model_topology_yaml()

        # Run simanneal's anneal() - it will call our update() hook
        best_state, best_energy = self.anneal()

        # Disable tracing
        self._enable_trace = False

        # Dump YAML
        trace_node = {
            "meta": self._trace_meta,
            "model_params": model_params,
            "model_topology": model_topology,
            "initial_state": initial_state_serializable,
            "steps": self._trace_steps,
            "best": {
                "energy": best_energy,
                "state": self._state_to_serializable(best_state)
            }
        }
        assert os.path.exists(os.path.dirname(os.path.normpath(output_path)))
        with open(output_path, 'w') as f:
            yaml.dump(trace_node, f, Dumper=CustomYamlDumper)

        return best_state, best_energy

class SegmentRecord:
    class KEY:
        segment_idx = "segment_idx"
        start_layer_idx = "start_layer_idx"
        end_layer_idx = "end_layer_idx"
        ring_buffer_count = "ring_buffer_count"
        ring_buffer_size_per = "ring_buffer_size_per"
        ring_buffer_use_count = "ring_buffer_use_count"
        tensor_spm_util_in_stage = "tensor_spm_util_in_stage"
        tensor_spm_util_shared = "tensor_spm_util_shared"
        tensor_spm_util_in_ringbuffer = "tensor_spm_util_in_ringbuffer"
        tensor_spm_util_weight = "tensor_spm_util_weight"
        total_spm_util = "total_spm_util"
        shared_tensor_is_read_first = "shared_tensor_is_read_first"
        acc_util = "acc_util"
        cost = "cost"
        stages = "stages"
    
    def __init__(self, segment_idx: int, start_layer_idx: int, end_layer_idx: int):
        self.segment_idx: int = segment_idx
        self.start_layer_idx: int = start_layer_idx
        self.end_layer_idx: int = end_layer_idx
        self.ring_buffer_count: dict[int, int] = {}
        self.ring_buffer_size_per: dict[int, int] = {}
        self.ring_buffer_use_count: dict[int, int] = {}
        self.tensor_spm_util_in_stage: list[dict[int, int]] = []  # Fixed type annotation
        self.tensor_spm_util_shared: dict[int, int] = {}  # Fixed type annotation
        self.tensor_spm_util_in_ringbuffer: dict[int, int] = {}
        self.tensor_spm_util_weight: dict[int, int] = {}  # Fixed type annotation
        self.total_spm_util: int = 0
        self.shared_tensor_is_read_first: dict[int, int] = {}
        self.acc_util: int = 0
        self.cost: int = 0
        self.stages: list[StageRecord] = []
        
        self.node = {}
    
    def build_node(self):
        """构建YAML节点"""
        self.node = {
            self.KEY.segment_idx: self.segment_idx,
            self.KEY.start_layer_idx: self.start_layer_idx,
            self.KEY.end_layer_idx: self.end_layer_idx,
            self.KEY.ring_buffer_count: self.ring_buffer_count,
            self.KEY.ring_buffer_size_per: self.ring_buffer_size_per,
            self.KEY.ring_buffer_use_count: self.ring_buffer_use_count,
            self.KEY.tensor_spm_util_in_stage: self.tensor_spm_util_in_stage,
            self.KEY.tensor_spm_util_shared: self.tensor_spm_util_shared,
            self.KEY.tensor_spm_util_in_ringbuffer: self.tensor_spm_util_in_ringbuffer,
            self.KEY.tensor_spm_util_weight: self.tensor_spm_util_weight,
            self.KEY.total_spm_util: self.total_spm_util,
            self.KEY.shared_tensor_is_read_first: self.shared_tensor_is_read_first,
            self.KEY.acc_util: self.acc_util,
            self.KEY.cost: int(self.cost),
            self.KEY.stages: [stage.node for stage in self.stages]
        }
    
    def write(self, path: str):
        """写入YAML文件"""
        assert os.path.exists(os.path.dirname(os.path.normpath(path)))
        with open(path, "w") as file:
            yaml.dump(self.node, file, Dumper=CustomYamlDumper)
    
    def dump(self, path: str):
        """序列化到文件"""
        with open(path, "wb") as f:
            pickle.dump(self, f)

class PipelineRecord:
    class KEY:
        segments = "segments"
        
        cost = "cost"
        
        target = "target"
        # Target相关字段
        target_numArrays = "numArrays"
        target_spmKB = "spmKB"
        target_spmKB_per_array = "spmKB_per_array"
        target_num_macs_per_array = "num_macs_per_array"
        target_dram_bw_per_cycle = "dram_bw_per_cycle"
        target_noc_bw_per_cycle = "noc_bw_per_cycle"
        target_default_batch = "default_batch"
    
    def __init__(self):
        self.segments = []  # 包含多个SegmentRecord
        self.cost = 0  # Renamed from total_cost
        self.num_layers = 0
        self.default_batch = 0
        
        # Target related fields
        self.target = None
        self.target_numArrays = 0
        self.target_spmKB = 0
        self.target_spmKB_per_array = 0
        self.target_num_macs_per_array = 0
        self.target_dram_bw_per_cycle = 0
        self.target_noc_bw_per_cycle = 0
        self.target_default_batch = 0
        
        self.node = {}
    
    def build_node(self):
        """构建YAML节点"""
        target_node = {
            self.KEY.target_numArrays: int(self.target_numArrays),
            self.KEY.target_spmKB: int(self.target_spmKB),
            self.KEY.target_spmKB_per_array: int(self.target_spmKB_per_array),
            self.KEY.target_num_macs_per_array: int(self.target_num_macs_per_array),
            self.KEY.target_dram_bw_per_cycle: self.target_dram_bw_per_cycle,
            self.KEY.target_noc_bw_per_cycle: int(self.target_noc_bw_per_cycle),
            self.KEY.target_default_batch: int(self.target_default_batch)
        }
        
        self.node = {
            self.KEY.target: target_node,
            self.KEY.segments: [segment.node for segment in self.segments],
            self.KEY.cost: int(self.cost),
        }
    
    def write(self, path: str):
        """写入YAML文件"""
        assert os.path.exists(os.path.dirname(os.path.normpath(path)))
        with open(path, "w") as file:
            yaml.dump(self.node, file, Dumper=CustomYamlDumper)
    
    def dump(self, path: str):
        """序列化到文件"""
        with open(path, "wb") as f:
            pickle.dump(self, f)

class SolutionToRecordFactory:
    """
    Factory class for converting solution objects to record objects.
    """
    
    @staticmethod
    def create_stage_record_from_layer_solution(layer_solution: PipelineSolution.LayerSolution, 
                                                layer_idx: int,
                                                model: Model,
                                                input_ids: list[int],
                                                weight_ids: list[int], 
                                                output_ids: list[int])-> StageRecord:
        """
        Convert LayerSolution to StageRecord using StageCandidateFactory.
        
        Args:
            layer_solution: LayerSolution object containing layer mapping information
            model: Model object for creating StageCandidateFactory
            
        Returns:
            StageRecord: Converted StageRecord object
        """
        # Create a stage candidate factory for this model
        factory = StageCandidateFactory(model)
        
        # Create stage candidate from layer solution
        stage_candidate = factory.create_stage_candidate(
            layerIdList=[layer_idx],
            tensorTypeDict=layer_solution._tensor_type_dict,
            accUtil=layer_solution._num_acc,
            input_ids= input_ids,
            weight_ids= weight_ids,
            output_ids= output_ids
        )
        
        # Create stage record and add the candidate
        stage_record = StageRecord()
        stage_record.add(stage_candidate)
        
        return stage_record

    @staticmethod
    def create_segment_record_from_segment_solution(segment_solution: 'PipelineSolution.SegmentSolution',
                                                   model: Model,
                                                   segment_idx: int) -> SegmentRecord:
        """
        Convert SegmentSolution to SegmentRecord.
        
        Args:
            segment_solution: SegmentSolution object
            model: Model object
            segment_idx: Index of the segment
            
        Returns:
            SegmentRecord: Converted SegmentRecord object
        """
        # Create segment record with basic information
        segment_record = SegmentRecord(
            segment_idx=segment_idx,
            start_layer_idx=segment_solution._start_layer_idx,
            end_layer_idx=segment_solution._end_layer_idx
        )
        
        # Copy all the metrics from segment solution
        segment_record.ring_buffer_count = segment_solution._ring_buffer_count
        segment_record.ring_buffer_size_per = {tensor_id: segment_solution._pipeline_target.target.getPageNum(spm_util) for tensor_id, spm_util in segment_solution._ring_buffer_size_per.items()}
        segment_record.ring_buffer_use_count = segment_solution._ring_buffer_use_count
        segment_record.tensor_spm_util_in_stage = [{tensor_id: segment_solution._pipeline_target.target.getPageNum(spm_util) for tensor_id, spm_util in dict_util.items()} for dict_util in segment_solution._tensor_spm_util_in_stage]
        segment_record.tensor_spm_util_shared = {tensor_id: segment_solution._pipeline_target.target.getPageNum(spm_util) for tensor_id, spm_util in segment_solution._tensor_spm_util_shared.items()}
        segment_record.tensor_spm_util_in_ringbuffer = {tensor_id: segment_solution._pipeline_target.target.getPageNum(spm_util) for tensor_id, spm_util in segment_solution._tensor_spm_util_in_ringbuffer.items()}
        segment_record.tensor_spm_util_weight = {
            weight_id: segment_solution._pipeline_target.target.getPageNum(layer.getDataSize(weight_id)) 
            for stage_idx, layer in enumerate(segment_solution._layer_list) for weight_id in segment_solution._weight_ids[stage_idx]}
        segment_record.total_spm_util = segment_solution._pipeline_target.target.getPageNum(segment_solution._total_spm_util)
        segment_record.shared_tensor_is_read_first = {}
        for stage_idx, stage_tensor_type in enumerate(segment_solution._tensor_type_dict_list):
            for tensor_id, tensor_type in stage_tensor_type.items():
                if segment_solution._tensor_meta_type[tensor_id] == TensorMetaType.TYPE_SHARED:
                    segment_record.shared_tensor_is_read_first[tensor_id] = 1 if tensor_type == TensorType.INTER_SHARED_READ else 0
        
        segment_record.acc_util = segment_solution.acc_use
        segment_record.cost = segment_solution.get_exec_cost()
        
        # Convert each layer solution to stage record
        for idx_offset, layer_solution in enumerate(segment_solution._layer_solution_list):
            stage_record = SolutionToRecordFactory.create_stage_record_from_layer_solution(
                layer_solution, segment_solution._start_layer_idx + idx_offset, model, segment_solution._input_ids[idx_offset], segment_solution._weight_ids[idx_offset], segment_solution._output_ids[idx_offset])
            segment_record.stages.append(stage_record)
        
        # Build the YAML node
        segment_record.build_node()
        
        return segment_record

    @staticmethod
    def create_pipeline_record_from_pipeline_solution(pipeline_solution: 'PipelineSolution',
                                                     model: Model) -> PipelineRecord:
        """
        Convert PipelineSolution to PipelineRecord.
        
        Args:
            pipeline_solution: PipelineSolution object
            model: Model object
            
        Returns:
            PipelineRecord: Converted PipelineRecord object
        """
        # Create pipeline record
        pipeline_record = PipelineRecord()
        
        # Set cost
        pipeline_record.cost = pipeline_solution.get_cost()
        
        # Set target information
        pipeline_record.target = pipeline_solution.pipeline_target.target
        pipeline_record.target_numArrays = pipeline_solution.pipeline_target.target.numArrays
        pipeline_record.target_spmKB = pipeline_solution.pipeline_target.target.spmBytes // 1024
        pipeline_record.target_spmKB_per_array = pipeline_solution.pipeline_target.target.spmBytesPerArray // 1024
        pipeline_record.target_num_macs_per_array = pipeline_solution.pipeline_target.target.num_macs_per_array
        pipeline_record.target_dram_bw_per_cycle = pipeline_solution.pipeline_target.target.dram_bw_per_cycle
        pipeline_record.target_noc_bw_per_cycle = pipeline_solution.pipeline_target.target.noc_bw_per_cycle
        pipeline_record.target_default_batch = pipeline_solution.pipeline_target.default_batch
        
        # Convert each segment solution to segment record
        for segment_idx, segment_solution in enumerate(pipeline_solution._segment_solution_list):
            segment_record = SolutionToRecordFactory.create_segment_record_from_segment_solution(
                segment_solution, model, segment_idx)
            pipeline_record.segments.append(segment_record)
        
        # Build the YAML node
        pipeline_record.build_node()
        
        return pipeline_record