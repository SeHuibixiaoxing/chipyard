import os
import argparse
import pickle
import traceback
from itertools import product
from multiprocessing import Pool
from typing import Any, Dict, Tuple
from HybridMapper.GraphPartition import GraphPartitionRecord, GraphPartitionCandidate
from HybridMapper.SASearch import GraphPartitionSimulatedAnnealing, PipelineTarget, SolutionToRecordFactory, PipelineSolution
from HybridMapper.Model import load_model, Model
from HybridMapper.Target import Target
from HybridMapper.CustomYamlDumper import *
from HybridMapper.SchedulePointRecord import SchedulePointRecord, SchedulePointRecodList


# Global candidate lists (edit as needed)
ACC_CANDIDATES = [8] # 暂时只支持一个，不能有多个
SPM_PER_ACC_CANDIDATES_KB = [1024 * 1024]  # per-accelerator SPM in KB
DEFAULT_BATCH_CANDIDATES = [16]
# DRAM_CHANNEL = [1]
# DRAM_BW_PER_CHANNEL = [19]
NOC_BW_CANDIDATES = [64]
DRAM_BW_CANDIDATES = [19]

SCHEDULE_POINT_NUM = [3, 4, 5, 6]

OUTPUT_DIR = "output/pipeline/"

def run_for_model(model_name: str):
    base_dir = os.path.join(OUTPUT_DIR, model_name)
    model_dump_path = os.path.join(base_dir, "layers.dump")
    layer_mapping_dir_path = os.path.join(base_dir, "mapping")

    if not os.path.exists(model_dump_path):
        print(f"Model dump not found for {model_name}: {model_dump_path}")
        return
    if not os.path.exists(layer_mapping_dir_path):
        print(f"Layer mapping dir not found for {model_name}: {layer_mapping_dir_path}")
        return

    # Build tasks (one task per combination)
    tasks = []
    for acc, spm_per_acc_kb, batch, dram_bw, noc_bw, partition_num in product(ACC_CANDIDATES, SPM_PER_ACC_CANDIDATES_KB, DEFAULT_BATCH_CANDIDATES, DRAM_BW_CANDIDATES, NOC_BW_CANDIDATES, SCHEDULE_POINT_NUM):
        tasks.append({
            'model_name': model_name,
            'base_dir': base_dir,
            'model_dump_path': model_dump_path,
            'layer_mapping_dir_path': layer_mapping_dir_path,
            'acc': acc,
            'spm_per_acc_kb': spm_per_acc_kb,
            'batch': batch,
            'dram_bw': dram_bw,
            'noc_bw': noc_bw,
            'partition_num': partition_num,
        })

    # Return task list to be processed by caller (main) which may run them in parallel
    return tasks


def process_task(task: Dict[str, Any]) -> SchedulePointRecord:
    model_name = task['model_name']
    model_dump_path = task['model_dump_path']
    layer_mapping_dir_path = task['layer_mapping_dir_path']
    acc = task['acc']
    spm_per_acc_kb = task['spm_per_acc_kb']
    batch = task['batch']
    dram_bw = task['dram_bw']
    noc_bw = task['noc_bw']
    partition_num = task['partition_num']

    # Load model and layer mapping per process to avoid pickling complex objects
    model: Model = load_model(model_dump_path)
    model.load_layer_mapping(layer_mapping_dir_path)

    spmKB_total = spm_per_acc_kb * acc

    print(f"\n[PID {os.getpid()}] Running Schedule Point for model={model_name} acc={acc} spm_per_acc_kb={spm_per_acc_kb} batch={batch} dram_bw={dram_bw} noc_bw={noc_bw} partition_num={partition_num}")

    hw_target = Target(
        numArrays=512,
        dram_bw_per_cycle=dram_bw,
        spmKB=spmKB_total,
        noc_bw_per_cycle=noc_bw,
        enablePipeline=True
    )

    pipeline_target = PipelineTarget(
        model=model,
        layer_mapping_dir_path=layer_mapping_dir_path,
        start_layer_idx=0,
        end_layer_idx=model.getNumLayers() - 1,
        default_batch=batch,
        target=hw_target,
        remote_layer_compute_ratio=1.1,
        resource_util_ratio=0,
        enable_decoupling=True
    )
    
    partition_plan = [(i, i) for i in range(model.getNumLayers())]
    acc_plan = [[acc] for _ in range(model.getNumLayers())]
    
    sa = GraphPartitionSimulatedAnnealing(
        pipeline_target=pipeline_target,
        partition_plan=partition_plan,
        acc_plan=acc_plan,
        layer_mapping=model._layer_mapping)
    
    # 将segments划分为partition_num个部分，要求各部分的exec_cost之和尽可能相等
    solution = sa.init_solution
    segments = solution._segment_solution_list
    total_cost = sum(seg.get_exec_cost() for seg in segments)
    target_cost_per_partition = total_cost / partition_num
    partitions: list[PipelineSolution.SegmentSolution] = []
    partitions_cost: list[float] = []
    current_partition = []
    current_cost = 0.0
    for seg in segments:
        seg_cost = seg.get_exec_cost()
        if current_cost + seg_cost > target_cost_per_partition and current_partition and len(partitions) < partition_num - 1:
            partitions.append(current_partition)
            partitions_cost.append(current_cost)
            current_partition = [seg]
            current_cost = seg_cost
        else:
            current_partition.append(seg)
            current_cost += seg_cost
        
    if current_partition:
        partitions.append(current_partition)
        partitions_cost.append(current_cost)
    
    
    record = SchedulePointRecord(model_name, [part[-1]._end_layer_idx for part in partitions], acc, partitions_cost)
    
    return record



def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--models', type=lambda s: s.split(','), default="", help='Comma-separated model names')
    args = parser.parse_args()

    record_list = SchedulePointRecodList()

    for model_name in args.models:
        tasks = run_for_model(model_name)
        if not tasks:
            continue

        for t in tasks:
            record_list.add_record(process_task(t))
    
    record_list.write(os.path.join(OUTPUT_DIR, "pipeline_schedule_points.yaml"))
    pickle.dump(record_list, open(os.path.join(OUTPUT_DIR, "pipeline_schedule_points.dump"), 'wb'))
    

if __name__ == '__main__':
    main()
