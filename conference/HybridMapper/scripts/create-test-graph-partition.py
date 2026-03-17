import os
import argparse
import pickle
import csv
import matplotlib.pyplot as plt
import inspect
from HybridMapper.GraphPartition import GraphPartitionRecord, GraphPartitionCandidate
from HybridMapper.SASearch import GraphPartitionSimulatedAnnealing, PipelineTarget, SolutionToRecordFactory, PipelineSolution
from HybridMapper.Model import Model,load_model
from HybridMapper.Target import Target
from HybridMapper.PipelineTarget import get_graph_target, get_default_batch_candidate, get_spm_kb_per_core_candidate
from HybridMapper.StageRecord import TensorType,TensorMetaType,TensorTypeRuntime

class UnitTest:
    def __init__(self):
        self.hw_target: Target = Target(
            numArrays=16,
            num_macs_per_array=16*16,
            dram_bw_per_cycle=19.28,
            spmKB=16 * 2,
            noc_bw_per_cycle=64
        )
        self.model_path = "output/pipeline/resnet18/layers.dump"
        self.layer_mapping_dir_path = "output/pipeline/resnet18/mapping"
        self.yaml_dir_path = "output/pipeline/resnet18-test"
        
        self.model = load_model(self.model_path)
        self.pipeline_target = PipelineTarget(
            model=self.model,
            layer_mapping_dir_path=self.layer_mapping_dir_path,
            start_layer_idx=14,
            end_layer_idx=17,
            default_batch=8,
            target=self.hw_target,
            remote_layer_compute_ratio=1.1,
            resource_util_ratio=0,
            enable_decoupling=True
        )
    
    # 测试IO、NORMAL_SPM类型
    def create_test_case1(self):
        pipeline_solution = PipelineSolution(
            pipeline_target=self.pipeline_target,
            partition_plan=[[14,15]],
            acc_plan=[[4,4]],
            tensor_type=[
                [
                    {14: TensorType.IO_SINGLE, 15: TensorType.INTER_SINGLE},
                    {15: TensorType.INTER_DOUBLE, 16: TensorType.IO_DOUBLE},
                ]],
        )
        
        pipeline_record = SolutionToRecordFactory.create_pipeline_record_from_pipeline_solution(
            pipeline_solution, self.model)
        
        self._save_pipeline_record(pipeline_record)

    # 测试NORMAL_DRAM类型
    def create_test_case2(self):
        pipeline_solution = PipelineSolution(
            pipeline_target=self.pipeline_target,
            partition_plan=[[14,15]],
            acc_plan=[[4,4]],
            tensor_type=[
                [
                    {14: TensorType.IO_SINGLE, 15: TensorType.INTER_DRAM_SINGLE},
                    {15: TensorType.INTER_DRAM_DOUBLE, 16: TensorType.IO_DOUBLE},
                ]],
        )
        
        pipeline_record = SolutionToRecordFactory.create_pipeline_record_from_pipeline_solution(
            pipeline_solution, self.model)
        
        self._save_pipeline_record(pipeline_record)
    
    # 测试SHARED_READ类型
    def create_test_case3(self):
        pipeline_solution = PipelineSolution(
            pipeline_target=self.pipeline_target,
            partition_plan=[[14,15]],
            acc_plan=[[4,4]],
            tensor_type=[
                [
                    {14: TensorType.IO_SINGLE, 15: TensorType.INTER_SHARED_READ},
                    {15: TensorType.INTER_SHARED_READ, 16: TensorType.IO_DOUBLE},
                ]],
        )
        
        pipeline_record = SolutionToRecordFactory.create_pipeline_record_from_pipeline_solution(
            pipeline_solution, self.model)
        
        self._save_pipeline_record(pipeline_record)

    # 测试SHARED_WRITE类型
    def create_test_case4(self):
        pipeline_solution = PipelineSolution(
            pipeline_target=self.pipeline_target,
            partition_plan=[[14,15]],
            acc_plan=[[4,4]],
            tensor_type=[
                [
                    {14: TensorType.IO_SINGLE, 15: TensorType.INTER_SHARED_WRITE},
                    {15: TensorType.INTER_SHARED_WRITE, 16: TensorType.IO_DOUBLE},
                ]],
        )
        
        pipeline_record = SolutionToRecordFactory.create_pipeline_record_from_pipeline_solution(
            pipeline_solution, self.model)
        
        self._save_pipeline_record(pipeline_record)
    
    # 测试PURE_DECOUPLING类型
    def create_test_case5(self):
        pipeline_solution = PipelineSolution(
            pipeline_target=self.pipeline_target,
            partition_plan=[[14,15]],
            acc_plan=[[4,4]],
            tensor_type=[
                [
                    {14: TensorType.IO_SINGLE, 15: TensorType.INTER_PURE_DECOUPLING},
                    {15: TensorType.INTER_PURE_DECOUPLING, 16: TensorType.IO_DOUBLE},
                ]],
        )
        
        pipeline_record = SolutionToRecordFactory.create_pipeline_record_from_pipeline_solution(
            pipeline_solution, self.model)
        
        self._save_pipeline_record(pipeline_record)
    
    # 测试多个segment
    def create_test_case6(self):
        pipeline_solution = PipelineSolution(
            pipeline_target=self.pipeline_target,
            partition_plan=[[14,15],[16,17]],
            acc_plan=[[4,4],[4,4]],
            tensor_type=[
                [
                    {14: TensorType.IO_SINGLE, 15: TensorType.INTER_SINGLE},
                    {15: TensorType.INTER_DOUBLE, 16: TensorType.IO_DOUBLE},
                ],
                [
                    {14: TensorType.IO_SINGLE, 17: TensorType.INTER_DOUBLE},
                    {16: TensorType.IO_DOUBLE, 17: TensorType.INTER_SINGLE, 18: TensorType.IO_DOUBLE},
                ]],
        )
        
        pipeline_record = SolutionToRecordFactory.create_pipeline_record_from_pipeline_solution(
            pipeline_solution, self.model)
        
        self._save_pipeline_record(pipeline_record)
    
    # 测试多输出情况的NORMAL_SPM类型
    # 测试带ring buffer的inter double类型
    def create_test_case7(self):
        pipeline_solution = PipelineSolution(
            pipeline_target=self.pipeline_target,
            partition_plan=[[13,17]],
            acc_plan=[[1,4,4,4,1]],
            tensor_type=[
                [
                    {11: TensorType.IO_DOUBLE, 13: TensorType.IO_DOUBLE, 14: TensorType.INTER_DOUBLE},
                    {14: TensorType.INTER_SINGLE, 15: TensorType.INTER_SINGLE},
                    {15: TensorType.INTER_SINGLE, 16: TensorType.INTER_DOUBLE},
                    {14: TensorType.INTER_DOUBLE, 17: TensorType.INTER_DOUBLE},
                    {16: TensorType.INTER_DOUBLE, 17: TensorType.INTER_DOUBLE, 18: TensorType.IO_DOUBLE},
                ]],
        )
        
        pipeline_record = SolutionToRecordFactory.create_pipeline_record_from_pipeline_solution(
            pipeline_solution, self.model)
        
        self._save_pipeline_record(pipeline_record)
    
    # 测试多输出情况下的NORMAL_DRAM类型
    # 测试带ring buffer的inter single类型
    def create_test_case8(self):
        pipeline_solution = PipelineSolution(
            pipeline_target=self.pipeline_target,
            partition_plan=[[13,17]],
            acc_plan=[[1,4,4,4,1]],
            tensor_type=[
                [
                    {11: TensorType.IO_DOUBLE, 13: TensorType.IO_DOUBLE, 14: TensorType.INTER_DRAM_SINGLE},
                    {14: TensorType.INTER_DRAM_DOUBLE, 15: TensorType.INTER_SINGLE},
                    {15: TensorType.INTER_SINGLE, 16: TensorType.INTER_DOUBLE},
                    {14: TensorType.INTER_DRAM_SINGLE, 17: TensorType.INTER_SINGLE},
                    {16: TensorType.INTER_DOUBLE, 17: TensorType.INTER_SINGLE, 18: TensorType.IO_DOUBLE},
                ]],
        )
        
        pipeline_record = SolutionToRecordFactory.create_pipeline_record_from_pipeline_solution(
            pipeline_solution, self.model)
        
        self._save_pipeline_record(pipeline_record)
    
    # 测试多输出情况下的SHARED类型
    def create_test_case9(self):
        pipeline_solution = PipelineSolution(
            pipeline_target=self.pipeline_target,
            partition_plan=[[13,16]],
            acc_plan=[[1,4,4,4]],
            tensor_type=[
                [
                    {11: TensorType.IO_DOUBLE, 13: TensorType.IO_DOUBLE, 14: TensorType.INTER_SHARED_WRITE},
                    {14: TensorType.INTER_SHARED_WRITE, 15: TensorType.INTER_SINGLE},
                    {15: TensorType.INTER_SINGLE, 16: TensorType.IO_SINGLE},
                    {14: TensorType.INTER_SHARED_WRITE, 17: TensorType.IO_DOUBLE}
                ]],
        )
        
        pipeline_record = SolutionToRecordFactory.create_pipeline_record_from_pipeline_solution(
            pipeline_solution, self.model)
        
        self._save_pipeline_record(pipeline_record)
    
    # 测试多输出情况下的ALL_RINGBUFFER类型
    # 测试带ring buffer的dram类型
    def create_test_case10(self):
        pipeline_solution = PipelineSolution(
            pipeline_target=self.pipeline_target,
            partition_plan=[[13,17]],
            acc_plan=[[1,4,4,4,1]],
            tensor_type=[
                [
                    {11: TensorType.IO_DOUBLE, 13: TensorType.IO_DOUBLE, 14: TensorType.INTER_PURE_DECOUPLING},
                    {14: TensorType.INTER_PURE_DECOUPLING, 15: TensorType.INTER_SINGLE},
                    {15: TensorType.INTER_SINGLE, 16: TensorType.INTER_DOUBLE},
                    {14: TensorType.INTER_PURE_DECOUPLING, 17: TensorType.INTER_DRAM_SINGLE},
                    {16: TensorType.INTER_DOUBLE, 17: TensorType.INTER_DRAM_DOUBLE, 18: TensorType.IO_DOUBLE},
                ]],
        )
        
        pipeline_record = SolutionToRecordFactory.create_pipeline_record_from_pipeline_solution(
            pipeline_solution, self.model)
        
        self._save_pipeline_record(pipeline_record)

    # 测试跳跃链接的ALL_RINGBUFFER类型
    def create_test_case11(self):
        pipeline_solution = PipelineSolution(
            pipeline_target=self.pipeline_target,
            partition_plan=[[13,17]],
            acc_plan=[[1,4,4,4,1]],
            tensor_type=[
                [
                    {11: TensorType.IO_DOUBLE, 13: TensorType.IO_DOUBLE, 14: TensorType.INTER_DOUBLE},
                    {14: TensorType.INTER_DOUBLE, 15: TensorType.INTER_SINGLE},
                    {15: TensorType.INTER_SINGLE, 16: TensorType.INTER_DOUBLE},
                    {14: TensorType.INTER_DOUBLE, 17: TensorType.INTER_PURE_DECOUPLING},
                    {16: TensorType.INTER_DOUBLE, 17: TensorType.INTER_PURE_DECOUPLING, 18: TensorType.IO_DOUBLE},
                ]],
        )
        
        pipeline_record = SolutionToRecordFactory.create_pipeline_record_from_pipeline_solution(
            pipeline_solution, self.model)
        
        self._save_pipeline_record(pipeline_record)
    
    # SINGLE BUFFER性能
    def create_test_case12(self):
        pipeline_solution = PipelineSolution(
            pipeline_target=self.pipeline_target,
            partition_plan=[[13,17]],
            acc_plan=[[1,4,4,4,1]],
            tensor_type=[
                [
                    {11: TensorType.IO_SINGLE, 13: TensorType.IO_SINGLE, 14: TensorType.INTER_SINGLE},
                    {14: TensorType.INTER_SINGLE, 15: TensorType.INTER_SINGLE},
                    {15: TensorType.INTER_SINGLE, 16: TensorType.INTER_SINGLE},
                    {14: TensorType.INTER_SINGLE, 17: TensorType.INTER_DRAM_SINGLE},
                    {16: TensorType.INTER_SINGLE, 17: TensorType.INTER_DRAM_SINGLE, 18: TensorType.IO_SINGLE},
                ]],
        )
        
        pipeline_record = SolutionToRecordFactory.create_pipeline_record_from_pipeline_solution(
            pipeline_solution, self.model)
        
        self._save_pipeline_record(pipeline_record)
    
    # 无ring buffer下的性能
    def create_test_case13(self):
        pipeline_solution = PipelineSolution(
            pipeline_target=self.pipeline_target,
            partition_plan=[[13,17]],
            acc_plan=[[1,4,4,4,1]],
            tensor_type=[
                [
                    {11: TensorType.IO_DOUBLE, 13: TensorType.IO_DOUBLE, 14: TensorType.INTER_DOUBLE},
                    {14: TensorType.INTER_DOUBLE, 15: TensorType.INTER_DOUBLE},
                    {15: TensorType.INTER_DOUBLE, 16: TensorType.INTER_DOUBLE},
                    {14: TensorType.INTER_DOUBLE, 17: TensorType.INTER_DRAM_DOUBLE},
                    {16: TensorType.INTER_DOUBLE, 17: TensorType.INTER_DRAM_DOUBLE, 18: TensorType.IO_DOUBLE},
                ]],
        )
        
        pipeline_record = SolutionToRecordFactory.create_pipeline_record_from_pipeline_solution(
            pipeline_solution, self.model)
        
        self._save_pipeline_record(pipeline_record)
    
    # 有ring buffer下的性能
    def create_test_case14(self):
        pipeline_solution = PipelineSolution(
            pipeline_target=self.pipeline_target,
            partition_plan=[[13,17]],
            acc_plan=[[1,4,4,4,1]],
            tensor_type=[
                [
                    {11: TensorType.IO_DOUBLE, 13: TensorType.IO_DOUBLE, 14: TensorType.INTER_DOUBLE},
                    {14: TensorType.INTER_DOUBLE, 15: TensorType.INTER_DOUBLE},
                    {15: TensorType.INTER_DOUBLE, 16: TensorType.INTER_DOUBLE},
                    {14: TensorType.INTER_DOUBLE, 17: TensorType.INTER_DOUBLE},
                    {16: TensorType.INTER_DOUBLE, 17: TensorType.INTER_DOUBLE, 18: TensorType.IO_DOUBLE},
                ]],
        )
        
        pipeline_record = SolutionToRecordFactory.create_pipeline_record_from_pipeline_solution(
            pipeline_solution, self.model)
        
        self._save_pipeline_record(pipeline_record)

    def _save_pipeline_record(self, pipeline_record):
        """
        Save a pipeline record to YAML using filename derived from the caller test method name.
        The filename is the last field of the caller method name after splitting on underscore.
        Example: caller `create_test_case1` -> `case1.yaml`.
        """
        # Ensure output dir exists
        if not os.path.exists(self.yaml_dir_path):
            os.makedirs(self.yaml_dir_path, exist_ok=True)

        # Inspect caller name
        stack = inspect.stack()
        # stack[1] is the immediate caller
        caller = stack[1].function if len(stack) > 1 else "unknown"
        # Derive filename from last field after underscore
        parts = caller.split("_")
        filename_base = parts[-1] if parts else caller
        yaml_file_path = os.path.join(self.yaml_dir_path, f"{filename_base}.yaml")

        pipeline_record.write(yaml_file_path)
        print(f"Wrote pipeline record to {yaml_file_path}")
    

def main():
    tester = UnitTest()
    # Automatically run all create_test_ methods on UnitTest
    methods = [m for m in dir(tester) if m.startswith('create_test_')]
    methods.sort()
    for m in methods:
        func = getattr(tester, m)
        if callable(func):
            print(f"Running {m}...")
            func()
    
if __name__ == "__main__":
    main()
    
    
    