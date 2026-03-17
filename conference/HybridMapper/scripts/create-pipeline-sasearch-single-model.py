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

# Global candidate lists (edit as needed)
ACC_CANDIDATES = [8,16,24,32]
# ACC_CANDIDATES = [16]
# SPM_PER_ACC_CANDIDATES_KB = [1024]  # per-accelerator SPM in KB
SPM_PER_ACC_CANDIDATES_KB = [256,512,1024,2048]  # per-accelerator SPM in KB

DEFAULT_BATCH_CANDIDATES = [16,32]
# DEFAULT_BATCH_CANDIDATES = [4]

NOC_BW_CANDIDATES = [64]
# NOC_BW_CANDIDATES = [32]

# METHOD = ["ours", "gemini", "tangram"]
METHOD = ["ours2", "gemini2", "tangram2"]
# METHOD = ["tangram", "tangram2"]


# DRAM_BW_CANDIDATES = [10,19]
DRAM_BW_CANDIDATES = [19]

# SA iterations
MAX_ITERATIONS = 1000

OUTPUT_DIR = "output/pipeline/"


def run_for_model(model_name: str, max_iterations: int = MAX_ITERATIONS):
    base_dir = os.path.join(OUTPUT_DIR, model_name)
    model_dump_path = os.path.join(base_dir, "layers.dump")
    layer_mapping_dir_path = os.path.join(base_dir, "mapping")
    graph_partition_dump_path = os.path.join(base_dir, "model_partition.dump")

    if not os.path.exists(model_dump_path):
        print(f"Model dump not found for {model_name}: {model_dump_path}")
        return
    if not os.path.exists(layer_mapping_dir_path):
        print(f"Layer mapping dir not found for {model_name}: {layer_mapping_dir_path}")
        return
    if not os.path.exists(graph_partition_dump_path):
        print(f"Graph partition dump not found for {model_name}: {graph_partition_dump_path}")
        return

    # Build tasks (one task per combination)
    tasks = []
    for acc, spm_per_acc_kb, batch, dram_bw, noc_bw, method in product(ACC_CANDIDATES, SPM_PER_ACC_CANDIDATES_KB, DEFAULT_BATCH_CANDIDATES, DRAM_BW_CANDIDATES, NOC_BW_CANDIDATES, METHOD):
        tasks.append({
            'model_name': model_name,
            'base_dir': base_dir,
            'model_dump_path': model_dump_path,
            'layer_mapping_dir_path': layer_mapping_dir_path,
            'graph_partition_dump_path': graph_partition_dump_path,
            'acc': acc,
            'spm_per_acc_kb': spm_per_acc_kb,
            'batch': batch,
            'dram_bw': dram_bw,
            'noc_bw': noc_bw,
            'max_iterations': max_iterations,
            'method': method
        })

    # Return task list to be processed by caller (main) which may run them in parallel
    return tasks


def process_task(task: Dict[str, Any]) -> Tuple[bool, str]:
    """Worker function to process a single combination task.
    Returns (success, message).
    """
    try:
        model_name = task['model_name']
        model_dump_path = task['model_dump_path']
        layer_mapping_dir_path = task['layer_mapping_dir_path']
        graph_partition_dump_path = task['graph_partition_dump_path']
        acc = task['acc']
        spm_per_acc_kb = task['spm_per_acc_kb']
        batch = task['batch']
        dram_bw = task['dram_bw']
        noc_bw = task['noc_bw']
        max_iterations = task.get('max_iterations', MAX_ITERATIONS)
        method = task.get('method', 'ours')

        # Load model and layer mapping per process to avoid pickling complex objects
        model: Model = load_model(model_dump_path)
        model.load_layer_mapping(layer_mapping_dir_path)

        with open(graph_partition_dump_path, 'rb') as f:
            graph_partition_record: GraphPartitionRecord = pickle.load(f)

        # find a candidate that covers the entire model (start_layer==0 and end==N-1)
        full_candidates = [c for c in graph_partition_record.candidate_list if c.target_node[GraphPartitionCandidate.KEY.start_layer_idx] == 0 and c.target_node[GraphPartitionCandidate.KEY.end_layer_idx] == model.getNumLayers() - 1]
        if not full_candidates:
            return False, f"No full-model graph partition candidate found for {model_name}"

        # Select a base candidate that matches the requested hardware target
        matching_candidates = []
        for c in full_candidates:
            tn = c.target_node
            if tn[GraphPartitionCandidate.KEY.acc_num] != acc:
                continue
            if tn[GraphPartitionCandidate.KEY.default_batch] != batch:
                continue
            if tn[GraphPartitionCandidate.KEY.dram_bw_per_cycle] != dram_bw:
                continue
            if tn[GraphPartitionCandidate.KEY.noc_bw_per_cycle] != noc_bw:
                continue
            matching_candidates.append(c)

        if not matching_candidates:
            return False, f"No matching full-model graph partition candidate for {model_name} acc={acc} batch={batch} dram_bw={dram_bw} noc_bw={noc_bw}"

        base_candidate = matching_candidates[0]
        spmKB_total = spm_per_acc_kb * acc

        print(f"\n[PID {os.getpid()}] Running SA for model={model_name} acc={acc} spm_per_acc_kb={spm_per_acc_kb} batch={batch} dram_bw={dram_bw} noc_bw={noc_bw}")

        hw_target = Target(
            numArrays=acc,
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
            enable_decoupling=False if method in ['tangram2', 'gemini2'] else True
        )

        final_solution = None
        if method in ['ours', 'ours2']:
            sa = GraphPartitionSimulatedAnnealing(
                pipeline_target=pipeline_target,
                partition_plan=base_candidate.node[GraphPartitionCandidate.KEY.partition_plan],
                acc_plan=base_candidate.node[GraphPartitionCandidate.KEY.acc_plan],
                layer_mapping=model._layer_mapping,
                max_iterations=max_iterations,
                enable_partition_search=True if method == 'ours2' else False
            )

            sa.anneal()

            best_state, best_energy = sa.get_best_state()
            final_solution = sa._state_to_solution(best_state)
        elif method in ['gemini', 'gemini2']:
            sa = GraphPartitionSimulatedAnnealing(
                pipeline_target=pipeline_target,
                partition_plan=base_candidate.node[GraphPartitionCandidate.KEY.partition_plan],
                acc_plan=base_candidate.node[GraphPartitionCandidate.KEY.acc_plan],
                layer_mapping=model._layer_mapping,
                max_iterations=max_iterations,
                gemini_like=True,
                enable_partition_search=True if method == 'gemini2' else False
            )

            sa.anneal()

            best_state, best_energy = sa.get_best_state()
            final_solution = sa._state_to_solution(best_state)
        elif method in ['tangram', 'tangram2']:
            sa = GraphPartitionSimulatedAnnealing(
                pipeline_target=pipeline_target,
                partition_plan=base_candidate.node[GraphPartitionCandidate.KEY.partition_plan],
                acc_plan=base_candidate.node[GraphPartitionCandidate.KEY.acc_plan],
                layer_mapping=model._layer_mapping,
                max_iterations=max_iterations,
                tangram_like=True,
                enable_partition_search=True if method == 'tangram2' else False
            )
            
            if method == 'tangram2':
                sa.anneal()
                best_state, best_energy = sa.get_best_state()
                final_solution = sa._state_to_solution(best_state)
            else:
                best_energy = sa.init_energy
                final_solution = sa.init_solution

        pipeline_record = SolutionToRecordFactory.create_pipeline_record_from_pipeline_solution(final_solution, model)

        out_dir = os.path.join(OUTPUT_DIR, model_name, 'entire_model')
        os.makedirs(out_dir, exist_ok=True)
        filename = f"{acc}_{spm_per_acc_kb}_{batch}_{dram_bw}_{noc_bw}_{method}.yaml"
        out_path = os.path.join(out_dir, filename)
        pipeline_record.write(out_path)
        msg = f"Saved partition YAML to {out_path} (best energy={best_energy},init energy={sa.init_energy})"
        if (best_energy >= PipelineSolution.SegmentSolution.SPM_EXCEED_PANITY):
            msg += f" [SPM EXCEEDED] filename={filename}"            
            print(msg)
            return False, msg

        print(msg)
        return True, msg

    except Exception as e:
        traceback_str = traceback.format_exc()
        err_msg = f"\n Error processing task for model={task.get('model_name')} acc={task.get('acc')}: {e}\n{traceback_str}"
        print(err_msg)
        return False, err_msg


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--models', type=lambda s: s.split(','), default="", help='Comma-separated model names')
    parser.add_argument('--max-iter', type=int, default=MAX_ITERATIONS)
    parser.add_argument('--jobs', type=int, default=max(os.cpu_count() // 2, 1), help='Number of parallel worker processes (default: number of CPUs)')
    args = parser.parse_args()

    jobs = args.jobs

    for model_name in args.models:
        tasks = run_for_model(model_name, args.max_iter)
        if not tasks:
            continue

        print(f"Processing {len(tasks)} tasks for model {model_name} using {jobs} job(s)")

        results = []
        if jobs == 1:
            for t in tasks:
                results.append(process_task(t))
        else:
            with Pool(processes=jobs) as pool:
                results = pool.map(process_task, tasks)

        # Summarize results
        success_count = sum(1 for ok, _ in results if ok)
        fail_count = len(results) - success_count
        print(f"Model {model_name}: {success_count} succeeded, {fail_count} failed")


if __name__ == '__main__':
    main()
