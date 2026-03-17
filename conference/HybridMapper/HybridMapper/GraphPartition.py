import numpy as np
import sys
import os
from itertools import accumulate
from HybridMapper.Model import Model, Layer
from HybridMapper.Target import Target
from HybridMapper.CustomYamlDumper import *

class GraphPartitionCandidate:
    class KEY:
        target = "target"
        acc_num = "acc_num"
        default_batch = "default_batch"
        dram_bw_per_cycle = "dram_bw_per_cycle"
        noc_bw_per_cycle = "noc_bw_per_cycle"
        start_layer_idx = "start_layer_idx"
        end_layer_idx = "end_layer_idx"
        total_macs_num = "total_macs_num"
        
        partition_plan = "partition_plan"
        acc_plan = "acc_plan"
        layer_is_compute_bound = "layer_is_compute_bound"
        
        total_evaluate_time = "total_evaluate_time"
        layer_weight_load_time = "layer_weight_load_time"
        layer_evaluate_time = "layer_evaluate_time"
        segment_actual_acc_num = "segment_actual_acc_num"
        total_actual_acc_num = "total_actual_acc_num"
    
    def __init__(self, 
                 target: Target, 
                 default_batch: int, 
                 partition_plan: list[tuple[int, int]], 
                 acc_plan: list[list[int]], 
                 layer_is_compute_bound: list[list[int]],
                 start_layer_idx: int,
                 end_layer_idx: int,
                 layer_evaluate_time: list[list[int]], 
                 total_evaluate_time: int,
                 layer_weight_load_time: list[list[int]],
                 segment_actual_acc_num: list[int],
                 total_actual_acc_num: int):
        
        self.target: Target = target
        self.default_batch: int = default_batch
        self.partition_plan: list[tuple[int, int]] = partition_plan
        self.acc_plan: list[list[int]] = acc_plan
        self.layer_is_compute_bound: list[list[int]] = layer_is_compute_bound
        self.start_layer_idx: int = start_layer_idx
        self.end_layer_idx: int = end_layer_idx
        self.layer_evaluate_time: list[list[int]] = layer_evaluate_time
        self.total_evaluate_time: int = total_evaluate_time
        self.layer_weight_load_time: list[list[int]] = layer_weight_load_time
        self.segment_actual_acc_num: list[int] = segment_actual_acc_num
        self.total_actual_acc_num: int = total_actual_acc_num
        
        self.target_node = dict({
            self.KEY.acc_num: target.numArrays, 
            self.KEY.total_macs_num: target.num_macs_per_array * target.numArrays, 
            self.KEY.default_batch: default_batch,
            self.KEY.dram_bw_per_cycle: target.dram_bw_per_cycle,
            self.KEY.noc_bw_per_cycle: target.noc_bw_per_cycle,
            self.KEY.start_layer_idx: start_layer_idx,
            self.KEY.end_layer_idx: end_layer_idx,
        })
        
        self.node = dict({
            self.KEY.target: self.target_node,
            self.KEY.partition_plan: partition_plan,
            self.KEY.layer_is_compute_bound: layer_is_compute_bound,
            self.KEY.acc_plan: acc_plan,
            self.KEY.total_evaluate_time: total_evaluate_time,
            self.KEY.layer_evaluate_time: layer_evaluate_time,
            self.KEY.layer_weight_load_time: layer_weight_load_time,
            self.KEY.segment_actual_acc_num: self.segment_actual_acc_num,
            self.KEY.total_actual_acc_num: self.total_actual_acc_num
        })

class GraphPartitionRecord:
    class KEY:
        candidates = "candidates"
    
    def __init__(self, 
                 candidate_list: list[GraphPartitionCandidate] = []):
        
        self.candidate_list = candidate_list
        self.node = {}
        self._cast_candidate_to_yaml_node()
    
    def _cast_candidate_to_yaml_node(self):
        self.node[self.KEY.candidates] = []
        for can in self.candidate_list:
            self.node[self.KEY.candidates].append(can.node)
    
    # Add this method to the GraphPartitionRecord class
    def merge(self, other: 'GraphPartitionRecord', inplace: bool = False) -> 'GraphPartitionRecord':
        """
        Merge two GraphPartitionRecord objects by combining their candidates.
        
        Args:
            other: Another GraphPartitionRecord to merge with this one
            inplace: If True, merge into this object. If False, create a new object.
            
        Returns:
            The merged GraphPartitionRecord (this object if inplace=True, new object otherwise)
        """
        # Get candidates from both records
        
        self_candidates = self.candidate_list
        other_candidates = []
        if other is not None:
            other_candidates = other.candidate_list
        
        # Combine candidates
        merged_candidates = self_candidates + other_candidates
        
        if inplace:
            # Modify this object in place
            self.candidate_list = merged_candidates
            self._cast_candidate_to_yaml_node()
            
            return self
        else:
            # Create and return a new object
            return GraphPartitionRecord(merged_candidates)
    
    def write(self, path: str):
        assert os.path.exists(os.path.dirname(os.path.normpath(path)))
        with open(path, "w") as file:
            yaml.dump(self.node, file, Dumper=CustomYamlDumper)

class GraphPartitioner:
    def __init__(self, model: Model, target: Target, default_batch: int):
        """
        初始化图划分类
        
        参数:
        model: 神经网络模型
        target: 面向硬件目标
        default_batch: 评估时使用的batch数量
        """
        self.model = model
        self.layers = model.layerList
        self.num_layers = len(self.layers)  # 层数
        self.num_acc = target.numArrays  # 加速器总数
        self.target = target
        self.default_batch = default_batch
        self.allowed_acc_nums = self.target.allowed_acc_nums

        
        # DP状态数组: dp[i] = (min_time, best_plan, best_acc_plan, best_segment_layer_time, layer_is_compute_bound, layer_weight_time, segment_actual_acc_num)
        # 其中:
        #   i: 起始层索引 (0 <= i < n)
        #   min_time: 从第i层到最后一层使用self.num_acc个加速器的最小执行时间
        #   best_plan: 分段方案(list[tuple[int, int]], (起始层索引， 结束层索引））
        #   best_acc_plan: 加速器划分方案
        #   segment_layer_time: 每个分段中各个层的执行时间
        #   layer_is_compute_bound: 各层是否是计算密集(1)，否则时访存密集(0)
        #   layer_weight_time: 各层权重加载时间
        #   segment_actual_acc_num: 实际使用的加速器个数
        self.dp = [(float('inf'), [], [], [], [], [], []) for _ in range(self.num_layers + 1)]
        
        # 初始化DP表
        self._initialize_dp_table()
        
        # 初始化结果
        self.candidates = []
        self.record = None
    
    def _initialize_dp_table(self):
        """初始化动态规划表"""
        # 当没有层需要处理时，执行时间为0
        self.dp[self.num_layers] = (0.0, [], [], [], [], [], [])

    def search_segment(self, segment: list[Layer], num_acc: int, start_layer_idx: int, end_layer_idx: int) -> float:
        """
        segment内加速器划分方案搜索。
        
        参数:
        segment: 层段列表
        num_acc: 分配给该段的加速器数量
        start_layer_idx: segment起始层下标
        end_layer_idx: segment结束层下标
        
        返回:
        max_cycles: 各阶段最大耗时（周期）
        acc_partition: 各层分配加速器数量
        cycles: 该段在给定加速器数量下的各层执行周期数
        is_compute_bound: 层是否是计算秘籍(1), 否则是访存密集(0)
        actual_acc_num: 实际使用的加速器数量
        """
        
        # 张量分类。统计每个tensor的入度和出度
        in_degree = self.model.get_in_degree(start_layer_idx, end_layer_idx)
        out_degree = self.model.get_out_degree(start_layer_idx, end_layer_idx)
        min_size = self.model.get_min_size(start_layer_idx, end_layer_idx)
        max_size = self.model.get_max_size(start_layer_idx, end_layer_idx)
        input_ids = self.model.get_segment_layer_input_ids(start_layer_idx, end_layer_idx)
        weight_ids = self.model.get_segment_layer_weight_ids(start_layer_idx, end_layer_idx)
        output_ids = self.model.get_segment_layer_output_ids(start_layer_idx, end_layer_idx)
        
        layer_ops: list[int] = []
        dram_bytes: list[int] = []
        dram_cycles: list[int] = []
        noc_cycles: list[int] = []
        
        for idx, layer in enumerate(segment):
            # 计算每一层的计算次数
            layer_ops.append(layer.compute)
            
            # 计算每一层的noc访存量
            layer_dram_bytes = 0
            input_noc_bytes = 0
            output_noc_bytes = 0
            
            for id in input_ids[idx]:
                if out_degree.get(id, 0) == 0:
                    layer_dram_bytes += self.target.ceilPage(max_size[id])
                else:
                    input_noc_bytes += self.target.ceilPage(min_size[id])
            
            for id in output_ids[idx]:
                if in_degree.get(id, 0) == 0:
                    layer_dram_bytes += self.target.ceilPage(max_size[id])
                else:
                    output_noc_bytes += in_degree[id] * self.target.ceilPage(min_size[id])
            
            dram_bytes.append(layer_dram_bytes)
            noc_cycles.append(max(input_noc_bytes, output_noc_bytes) // self.target.noc_bw_per_cycle)
        
        dram_cycles= [(0 if x == 0 else int(x /(float(x) / sum(dram_bytes) * self.target.dram_bw_per_cycle))) for x in dram_bytes]
        
        # print(f"layer_ops={layer_ops}\ndram_cycles={dram_cycles}\nnoc_cycles={noc_cycles}") 
         
        # 动态规划求解加速器划分方案
        # dp[i][j]表示前i层，用j个加速器的（最小层最大执行时间，各层加速器数量， 各层执行周期数, 各层计算密集1还是访存密集0）
        # dp[i][j] = min(dp[i - 1][j - k] + layer(i, k))
        segment_size = len(segment)
        dp = [[(float('inf'), [], [], []) for _ in range(num_acc + 1)] for _ in range(segment_size)]
        
        for j in [acc for acc in self.allowed_acc_nums if acc <= num_acc and acc >= segment_size]:
            compute_cycles = layer_ops[0] // (self.target.num_macs_per_array * j)
            layer_cycles = max(compute_cycles, max(dram_cycles[0], noc_cycles[0]))
            is_compute_bound = (layer_cycles == compute_cycles)
            dp[0][j] = (layer_cycles, [j], [layer_cycles], [is_compute_bound])
            # print(f"dp[{0}][{j}]={dp[0][j]}")
        
        for i in range(1, segment_size):
            layer = segment[i]
            for j in range(i+1, num_acc + 1): # 从i开始，0..i层至少需要i+1个加速器
                if num_acc - j < segment_size - i - 1: # 剩余加速器不足以分配给后续层
                    continue
                for k in [acc for acc in self.allowed_acc_nums if acc <= j - i and acc >= 1]: # 至少1个加速器，最多j - i个加速器，即0...i-1层至少每层一个加速器
                    if dp[i - 1][j - k][0] == float('inf'): # 如果不存在合法分配方案
                        continue
                    
                    compute_cycles = layer_ops[i] // (self.target.num_macs_per_array * k)
                    layer_cycles = max(compute_cycles, max(dram_cycles[i], noc_cycles[i]))
                    is_compute_bound = (layer_cycles == compute_cycles)
                    
                    pre_cycles = dp[i - 1][j - k][0]
                    now_cycles = max(pre_cycles, layer_cycles)
                    if dp[i][j][0] > now_cycles:
                        # print(f"dp[{i}][{j}]从dp[{i - 1}][{j - k}]转移. k={k}, layer_cycles={layer_cycles}, layer_ops[{i}]={layer_ops[i]}, dram_cycles[{i}]={dram_cycles[i]}, noc_cycles[{i}]={noc_cycles[i]}")
                        dp[i][j] = (now_cycles, dp[i - 1][j - k][1] + [k], dp[i - 1][j - k][2] + [layer_cycles], dp[i - 1][j - k][3] + [is_compute_bound])
                        
                # print(f"dp[{i}][{j}]={dp[i][j]}")
        
        global_best = float('inf')
        global_best_num_acc = 0
        for i in range(1, num_acc + 1):
            if dp[segment_size - 1][i][0] < global_best:
                global_best = dp[segment_size - 1][i][0]
                global_best_num_acc = i
                
        return dp[segment_size - 1][global_best_num_acc] + (global_best_num_acc,)
    
    def partition(self, start_layer_idx = 0, end_layer_idx: int | None = None) -> tuple[GraphPartitionRecord, GraphPartitionRecord]:
        """
        执行图划分算法
        
        返回:
        GraphPartitionRecord: 可写到yaml文件的记录
        partition_plan: 整个计算图的划分方案，每个元素为: 最小时间, 分段方案(list[tuple[int, int]], (起始层索引， 结束层索引）），加速器划分方案，每个分段中各个层的执行时间, 每个分段权重加载时间, 每个分段实际使用的加速器数量, 整个划分方案实际使用的加速器数量
        """
        
        if end_layer_idx is None:
            end_layer_idx = self.num_layers - 1

        # 重置区间外的状态，并设置段终点后的基准状态
        for idx in range(end_layer_idx + 1, self.num_layers + 1):
            self.dp[idx] = (float('inf'), [], [], [], [], [], [])
        self.dp[end_layer_idx + 1] = (0.0, [], [], [], [], [], [])

        # 从后向前进行动态规划，只在[start_layer_idx, end_layer_idx]区间内搜索
        for i in range(end_layer_idx, start_layer_idx - 1, -1):
            min_time = float('inf')
            best_plan = []
            best_acc_plan = []
            best_segment_layer_time = []
            best_layer_is_compute_bound = []
            best_layer_weight_time = []
            best_segment_acc_num = []
            
            # 枚举可能的段长度k（从i开始的连续层数）
            for k in range(0, min(end_layer_idx - i + 1, self.num_acc)):
                segment = self.layers[i:i + k + 1]
                
                # 搜索段内加速器分配方案
                max_time, acc_partition, layer_times, layer_is_compute_bound, actual_num_acc = self.search_segment(segment, self.num_acc, i, i + k)
                if max_time == float('inf') or actual_num_acc == 0:
                    continue
                # print(f"search segment {i}-{i+k} with {self.num_acc} accelerators.max_time:{max_time},acc_partition:{acc_partition},layer_times={layer_times},layer_is_compute_bound={layer_is_compute_bound}")
                
                # 计算segment执行时间
                segment_time = 0
                if len(segment) <= self.default_batch:
                    pre_max = list(accumulate(layer_times, max))
                    suc_max = list(accumulate(layer_times[::-1], max))[::-1]
                    fill_time = sum(pre_max)
                    drain_time = sum(suc_max) - max_time
                    segment_time = fill_time + drain_time + (self.default_batch - 1) * max_time
                else:
                    for idx in range(self.default_batch + k - 1):
                        segment_time += max(layer_times[max(0, idx - self.default_batch + 1):min(len(segment) + 1, idx + 1)])
                
                # 剩余部分的执行时间
                rest_time, rest_plan, rest_acc_plan, rest_segment_layer_time, rest_layer_is_compute_bound, rest_layer_weight_time, rest_segment_acc_num = self.dp[i + k + 1]
                
                # 整体执行时间是，当前段权重载入时间+当前段执行时间+剩余部分的和
                layer_weight_load_time = [layer.getTotalSize(self.model.get_segment_layer_weight_ids(i, i + k)[stage_idx]) // self.target.dram_bw_per_cycle for stage_idx, layer in enumerate(segment)]
                weight_time = sum(layer_weight_load_time)
                total_time = weight_time + segment_time + rest_time
                
                # print(f"total_time: {segment_time},fill_time: {fill_time},segment_time: {segment_time},rest_time: {rest_time},pre_max={pre_max},suc_max={suc_max}")
                
                # 更新最优解
                if total_time < min_time:
                    min_time = total_time
                    # 构建划分方案：当前段 + 剩余部分的方案
                    best_plan = [(i, i + k)] + rest_plan
                    best_acc_plan = [acc_partition] + rest_acc_plan
                    best_segment_layer_time = [layer_times] + rest_segment_layer_time
                    best_layer_is_compute_bound = [layer_is_compute_bound] + rest_layer_is_compute_bound
                    best_layer_weight_time = [layer_weight_load_time] + rest_layer_weight_time
                    best_segment_acc_num = [actual_num_acc] + rest_segment_acc_num
                
            # 记录最优解
            self.dp[i] = (min_time, best_plan, best_acc_plan, best_segment_layer_time, best_layer_is_compute_bound, best_layer_weight_time, best_segment_acc_num)
            # print(f"layer {i} to {self.num_layers - 1}: min_time={min_time}, best_plan={best_plan}, best_acc_plan={best_acc_plan}")
            
            if self.dp[i][0] == float('inf'):
                assert(False)
            
        self.candidates = []
        for i in range(start_layer_idx, end_layer_idx + 1):
            self.candidates.append(GraphPartitionCandidate(
                target=self.target,
                default_batch=self.default_batch,
                total_evaluate_time=self.dp[i][0],
                partition_plan=self.dp[i][1],
                acc_plan=self.dp[i][2],
                layer_evaluate_time=self.dp[i][3],
                layer_is_compute_bound=self.dp[i][4],
                layer_weight_load_time=self.dp[i][5],
                start_layer_idx=i,
                end_layer_idx=end_layer_idx,
                total_actual_acc_num=max(self.dp[i][6]),
                segment_actual_acc_num=self.dp[i][6]
            ))
        
        self.record = GraphPartitionRecord(self.candidates)
        record_only_first = GraphPartitionRecord(self.candidates[0:1])
        
        # 返回所有以end_layer_idx为结尾的方案，以及以[start_layer_idx, end_layer_idx]方案
        return self.record, record_only_first
    def write(self, path: str):
        if self.record is None:
            raise ValueError("no record")
        self.record.write(path)
    
