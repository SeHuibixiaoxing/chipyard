import os
import argparse
import pickle
import csv
import matplotlib.pyplot as plt
from HybridMapper.GraphPartition import GraphPartitionRecord, GraphPartitionCandidate
from HybridMapper.SASearch import GraphPartitionSimulatedAnnealing, PipelineTarget, SolutionToRecordFactory
from HybridMapper.Model import Model
from HybridMapper.Target import Target
from HybridMapper.PipelineTarget import get_graph_target, get_default_batch_candidate, get_spm_kb_per_core_candidate
from HybridMapper.StageRecord import TensorType,TensorMetaType,TensorTypeRuntime

OUTPUT_DIR = "output/pipeline/"
MAX_ITERATIONS = 100

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

def get_spm_util(solution):
    """Calculate total SPM utilization across all segments"""
    return sum(seg._total_spm_util for seg in solution._segment_solution_list)

def get_meta_type_counts(solution):
    """Count occurrences of each tensor meta type"""
    counts = {}
    for seg in solution._segment_solution_list:
        for meta_type in seg._tensor_meta_type.values():
            name = meta_type.name
            counts[name] = counts.get(name, 0) + 1
    return counts

def save_best_solution_to_yaml(model_name, decouple, start_layer_idx, final_solution, model, solution_index):
    """
    Save the best solution to YAML files using SolutionToRecordFactory.
    
    Directory structure: model_name/decoupling_or_coupling/start_layer_idx/0.yaml, 1.yaml, ...
    """
    # Determine directory name based on decoupling setting
    decouple_dir = "decoupling" if decouple else "coupling"
    
    # Create output directory
    output_dir = os.path.join(OUTPUT_DIR, model_name, decouple_dir, str(start_layer_idx))
    os.makedirs(output_dir, exist_ok=True)
    
    # Convert pipeline solution to pipeline record
    pipeline_record = SolutionToRecordFactory.create_pipeline_record_from_pipeline_solution(
        final_solution, model)
    
    # Save to YAML file with incrementing index
    yaml_file_path = os.path.join(output_dir, f"{solution_index}.yaml")
    pipeline_record.write(yaml_file_path)
    
    print(f"Best solution saved to {yaml_file_path}")
    return yaml_file_path

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", type=str, required=False, default="")
    
    args = parser.parse_args()
    
    model_name = args.model
    base_dir = os.path.join(OUTPUT_DIR, model_name)
    model_dump_path = os.path.join(base_dir, "layers.dump")
    layer_mapping_dir_path = os.path.join(base_dir, "mapping")
    graph_partition_dump_path = os.path.join(base_dir, "model_partition.dump")
    
    print(f"Loading model: {model_name} from path: {model_dump_path}")
    model: Model = load_model(model_dump_path)
    model.load_layer_mapping(layer_mapping_dir_path)
    
    print(f"Loading graph partition: from path: {graph_partition_dump_path}")
    with open(graph_partition_dump_path, "rb") as f:
        graph_partition_record: GraphPartitionRecord = pickle.load(f)
    
    results = []
    meta_types = [
        TensorMetaType.TYPE_IO.name,
        TensorMetaType.TYPE_NORMAL_DRAM.name,
        TensorMetaType.TYPE_NORMAL_SPM.name,
        TensorMetaType.TYPE_SHARED.name,
        TensorMetaType.TYPE_PURE_DECOUPLING.name
    ]
    
    # Keep track of solution indices for each directory
    solution_indices = {}
    
    for graph_partition_candidate in graph_partition_record.candidate_list:
        # Extract hardware parameters
        start_layer_idx = graph_partition_candidate.target_node[GraphPartitionCandidate.KEY.start_layer_idx]
        end_layer_idx = graph_partition_candidate.target_node[GraphPartitionCandidate.KEY.end_layer_idx]
        
        # Skip if not full model
        if start_layer_idx != 0 or end_layer_idx != model.getNumLayers() - 1:
            continue
            
        acc_num = graph_partition_candidate.target_node[GraphPartitionCandidate.KEY.acc_num]
        total_macs_num = graph_partition_candidate.target_node[GraphPartitionCandidate.KEY.total_macs_num]
        dram_bw_per_cycle = graph_partition_candidate.target_node[GraphPartitionCandidate.KEY.dram_bw_per_cycle]
        noc_bw_per_cycle = graph_partition_candidate.target_node[GraphPartitionCandidate.KEY.noc_bw_per_cycle]
        for spm_per_core in get_spm_kb_per_core_candidate():
            spmKB = spm_per_core * acc_num
            
            for decouple in [True, False]:
                hw_target: Target = Target(
                    numArrays=acc_num,
                    num_macs_per_array=total_macs_num // acc_num,
                    dram_bw_per_cycle=dram_bw_per_cycle,
                    spmKB=spmKB,
                    noc_bw_per_cycle=noc_bw_per_cycle
                )
                pipeline_target = PipelineTarget(
                    model=model,
                    layer_mapping_dir_path=layer_mapping_dir_path,
                    start_layer_idx=start_layer_idx,
                    end_layer_idx=end_layer_idx,
                    default_batch=graph_partition_candidate.target_node[GraphPartitionCandidate.KEY.default_batch],
                    target=hw_target,
                    remote_layer_compute_ratio=1.1,
                    resource_util_ratio=0,
                    enable_decoupling=decouple
                )
                
                print(f"Creating simulated annealing search for decouple={decouple}...")
                sa = GraphPartitionSimulatedAnnealing(
                    pipeline_target=pipeline_target,
                    partition_plan=graph_partition_candidate.node[GraphPartitionCandidate.KEY.partition_plan],
                    acc_plan=graph_partition_candidate.node[GraphPartitionCandidate.KEY.acc_plan],
                    layer_mapping=model._layer_mapping,
                    max_iterations=MAX_ITERATIONS,
                )
                # Record initial state
                initial_energy = sa.energy()
                initial_solution = sa._state_to_solution(sa.state)
                initial_spm_util = get_spm_util(initial_solution)
                initial_meta_counts = get_meta_type_counts(initial_solution)
                
                print(f"Init solution energy: {initial_energy}")
                print(f"Init SPM utilization: {initial_spm_util}")
                
                print(f"Running simulated annealing search...")
                sa.anneal()
                
                # Get best solution
                best_state, best_energy = sa.get_best_state()
                final_solution = sa._state_to_solution(best_state)
                final_spm_util = get_spm_util(final_solution)
                final_meta_counts = get_meta_type_counts(final_solution)
                
                # Get or create solution index for this directory
                decouple_dir = "decoupling" if decouple else "coupling"
                dir_key = (model_name, decouple_dir, str(start_layer_idx))
                if dir_key not in solution_indices:
                    solution_indices[dir_key] = 0
                else:
                    solution_indices[dir_key] += 1
                    
                # Save best solution to YAML
                yaml_file_path = save_best_solution_to_yaml(
                    model_name, decouple, start_layer_idx, final_solution, model, solution_indices[dir_key])
                
                # Collect results
                result = {
                    'acc_num': acc_num,
                    'spmKB': spmKB,
                    'dram_bw': dram_bw_per_cycle,
                    'noc_bw': noc_bw_per_cycle,
                    'default_batch': graph_partition_candidate.target_node[GraphPartitionCandidate.KEY.default_batch],
                    'decouple': decouple,
                    'start_layer_idx': start_layer_idx,
                    'end_layer_idx': end_layer_idx,
                    'initial_energy': initial_energy,
                    'final_energy': best_energy,
                    'initial_spm_util': initial_spm_util,
                    'final_spm_util': final_spm_util,
                    'yaml_file': yaml_file_path
                }
                
                # Add meta type counts
                for mt in meta_types:
                    result[f'initial_meta_{mt}'] = initial_meta_counts.get(mt, 0)
                    result[f'final_meta_{mt}'] = final_meta_counts.get(mt, 0)
                
                results.append(result)
                print(f"Completed run for decouple={decouple}")
                print(f"Best solution energy: {best_energy}")
                print(f"Best solution exec cost: {final_solution.get_exec_cost()}")
                print(f"Best solution acc use: {final_solution.get_segment_acc_use()}")
                print(f"SPM utilization: {final_spm_util}/{hw_target.spmBytes}")
    
    # Save results to CSV
    results_file = os.path.join(base_dir, 'sasearch_results.csv')
    with open(results_file, 'w', newline='') as f:
        fieldnames = list(results[0].keys())
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for r in results:
            writer.writerow(r)
    print(f"Results saved to {results_file}")
    
    # Generate visualization
    plt.figure(figsize=(14, 10))
    
    # Energy comparison
    plt.subplot(2, 1, 1)
    x = range(len(results))
    width = 0.35
    initial_energies = [r['initial_energy'] for r in results]
    final_energies = [r['final_energy'] for r in results]
    decouple_labels = [f"{r['acc_num']}({'D' if r['decouple'] else 'ND'})" for r in results]
    
    plt.bar([i - width/2 for i in x], initial_energies, width, label='Initial Energy')
    plt.bar([i + width/2 for i in x], final_energies, width, label='Final Energy')
    plt.xlabel('Configuration (AccNum/Decouple)')
    plt.ylabel('Energy')
    plt.title('Energy Comparison Across Configurations')
    plt.xticks(x, decouple_labels, rotation=45)
    plt.legend()
    
    # SPM utilization comparison
    plt.subplot(2, 1, 2)
    initial_spm = [r['initial_spm_util'] for r in results]
    final_spm = [r['final_spm_util'] for r in results]
    spm_capacity = [r['spmKB'] * 1024 for r in results]  # Convert KB to bytes
    
    plt.bar([i - width for i in x], initial_spm, width, label='Initial SPM Util')
    plt.bar(x, final_spm, width, label='Final SPM Util')
    plt.bar([i + width for i in x], spm_capacity, width, alpha=0.3, label='SPM Capacity')
    plt.xlabel('Configuration (AccNum/Decouple)')
    plt.ylabel('SPM Utilization (bytes)')
    plt.title('SPM Utilization Comparison')
    plt.xticks(x, decouple_labels, rotation=45)
    plt.legend()
    
    plt.tight_layout()
    plot_file = os.path.join(base_dir, 'sasearch_comparison.png')
    plt.savefig(plot_file)
    print(f"Visualization saved to {plot_file}")
            

if __name__ == "__main__":
    main()