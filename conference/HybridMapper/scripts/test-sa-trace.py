#!/usr/bin/env python3
"""
Test script to demonstrate SA tracing functionality.
Runs a small SA search with trace logging enabled.
"""

import os
import pickle
from HybridMapper.SASearch import GraphPartitionSimulatedAnnealing, PipelineTarget
from HybridMapper.Model import Model
from HybridMapper.Target import Target
from HybridMapper.GraphPartition import GraphPartitionRecord

# Configuration
MODEL_NAME = "resnet50"
OUTPUT_DIR = "output/pipeline/"
TRACE_OUTPUT_DIR = "output/pipeline/traces/"
MAX_ITERATIONS = 1000  # Small number for testing

def main():
    # Setup paths
    base_dir = os.path.join(OUTPUT_DIR, MODEL_NAME)
    model_dump_path = os.path.join(base_dir, "layers.dump")
    layer_mapping_dir_path = os.path.join(base_dir, "mapping")
    graph_partition_dump_path = os.path.join(base_dir, "model_partition.dump")
    
    # Create trace output directory
    trace_dir = os.path.join(TRACE_OUTPUT_DIR, MODEL_NAME)
    os.makedirs(trace_dir, exist_ok=True)
    
    print(f"Loading model: {MODEL_NAME}")
    with open(model_dump_path, 'rb') as f:
        model: Model = pickle.load(f)
    model.load_layer_mapping(layer_mapping_dir_path)
    
    print(f"Loading graph partition...")
    with open(graph_partition_dump_path, "rb") as f:
        graph_partition_record: GraphPartitionRecord = pickle.load(f)
    
    # Use first candidate for testing
    if not graph_partition_record.candidate_list:
        print("No graph partition candidates found!")
        return
    
    candidate = graph_partition_record.candidate_list[0]
    from HybridMapper.GraphPartition import GraphPartitionCandidate
    
    start_layer_idx = candidate.target_node[GraphPartitionCandidate.KEY.start_layer_idx]
    end_layer_idx = candidate.target_node[GraphPartitionCandidate.KEY.end_layer_idx]
    acc_num = candidate.target_node[GraphPartitionCandidate.KEY.acc_num]
    total_macs_num = candidate.target_node[GraphPartitionCandidate.KEY.total_macs_num]
    dram_bw_per_cycle = candidate.target_node[GraphPartitionCandidate.KEY.dram_bw_per_cycle]
    noc_bw_per_cycle = candidate.target_node[GraphPartitionCandidate.KEY.noc_bw_per_cycle]
    spm_kb_per_acc = 1024
    spm_kb = spm_kb_per_acc*acc_num  # Example value
    
    print(f"\nConfiguration:")
    print(f"  Layers: {start_layer_idx} to {end_layer_idx}")
    print(f"  Accelerators: {acc_num}")
    print(f"  SPM: {spm_kb} KB")
    print(f"  Max iterations: {MAX_ITERATIONS}")
    
    # Create hardware target
    hw_target = Target(
        numArrays=acc_num,
        num_macs_per_array=total_macs_num // acc_num,
        dram_bw_per_cycle=dram_bw_per_cycle,
        spmKB=spm_kb,
        noc_bw_per_cycle=noc_bw_per_cycle
    )
    
    # Create pipeline target
    pipeline_target = PipelineTarget(
        model=model,
        layer_mapping_dir_path=layer_mapping_dir_path,
        start_layer_idx=start_layer_idx,
        end_layer_idx=end_layer_idx,
        default_batch=candidate.target_node[GraphPartitionCandidate.KEY.default_batch],
        target=hw_target,
        remote_layer_compute_ratio=1.1,
        resource_util_ratio=0,
        enable_decoupling=True
    )
    
    # Create SA instance
    print(f"\nInitializing simulated annealing...")
    sa = GraphPartitionSimulatedAnnealing(
        pipeline_target=pipeline_target,
        partition_plan=candidate.node[GraphPartitionCandidate.KEY.partition_plan],
        acc_plan=candidate.node[GraphPartitionCandidate.KEY.acc_plan],
        layer_mapping=model._layer_mapping,
        max_iterations=MAX_ITERATIONS,
        update_stride=1
    )
    
    # Run with trace
    trace_path = os.path.join(trace_dir, "test_trace.yaml")
    print(f"\nRunning SA with trace logging...")
    print(f"Trace will be saved to: {trace_path}")
    
    best_state, best_energy = sa.run_with_trace(trace_path)
    
    print(f"\n✓ SA completed!")
    print(f"  Best energy: {best_energy}")
    print(f"  Trace saved: {trace_path}")
    print(f"\nTrace file contains:")
    print(f"  - Model parameters (inputs/outputs/weights with page-aligned sizes)")
    print(f"  - Model topology (layer connections)")
    print(f"  - Per-step SA details (temperature, acceptance, operations, states)")
    print(f"  - Final best solution")

if __name__ == "__main__":
    main()
