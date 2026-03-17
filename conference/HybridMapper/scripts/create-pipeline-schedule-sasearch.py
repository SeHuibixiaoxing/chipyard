import os
import argparse
import pickle
import traceback
from multiprocessing import Pool
from typing import Any, Dict, List, Optional, Tuple
from HybridMapper.GraphPartition import GraphPartitionRecord, GraphPartitionCandidate
from HybridMapper.SASearch import GraphPartitionSimulatedAnnealing, PipelineTarget, SolutionToRecordFactory
import yaml
from HybridMapper.CustomYamlDumper import CustomYamlDumper
from HybridMapper.Model import load_model, Model
from HybridMapper.Target import Target
from HybridMapper.SchedulePointRecord import SchedulePointRecord, SchedulePointRecodList

# Global candidate lists (edit as needed)
ACC_CANDIDATES = [8,16,24,32]
# ACC_CANDIDATES = [32]
SPM_PER_ACC_CANDIDATES_KB = [1024]  # per-accelerator SPM in KB
DEFAULT_BATCH_CANDIDATES = [16]
NOC_BW_CANDIDATES = [64]
DRAM_BW_CANDIDATES = [19]
PARTITION_NUM = [3, 4, 5, 6]
# PARTITION_NUM = [4]
METHOD = ["ours", "gemini", "tangram"]
# METHOD = ["ours"]

# SA iterations
MAX_ITERATIONS = 1000

OUTPUT_DIR = "output/pipeline/"
SCHEDULE_DUMP_PATH = os.path.join(OUTPUT_DIR, "pipeline_schedule_points.dump")


def _parse_models_arg(raw: str) -> Optional[set[str]]:
    if not raw:
        return None
    models = {m.strip() for m in raw.split(',') if m.strip()}
    return models

def _candidate_in_search_space(target_node: Dict[str, Any]) -> bool:
    return (
        target_node.get(GraphPartitionCandidate.KEY.acc_num) in ACC_CANDIDATES
        and target_node.get(GraphPartitionCandidate.KEY.default_batch) in DEFAULT_BATCH_CANDIDATES
        and target_node.get(GraphPartitionCandidate.KEY.dram_bw_per_cycle) in DRAM_BW_CANDIDATES
        and target_node.get(GraphPartitionCandidate.KEY.noc_bw_per_cycle) in NOC_BW_CANDIDATES
    )


def _load_schedule_records(model_filter: set[str] = None) -> Dict[str, List[SchedulePointRecord]]:
    if not os.path.exists(SCHEDULE_DUMP_PATH):
        raise FileNotFoundError(f"Schedule point dump not found: {SCHEDULE_DUMP_PATH}")

    schedule_list: SchedulePointRecodList = pickle.load(open(SCHEDULE_DUMP_PATH, 'rb'))
    schedule_records = schedule_list.record_list

    records_by_model: Dict[str, List[SchedulePointRecord]] = {}
    for rec in schedule_records:
        model_name = rec.target[SchedulePointRecord.KEY.model]
        # if model_filter is None, accept all models
        if model_filter is not None and model_name not in model_filter:
            continue
        records_by_model.setdefault(model_name, []).append(rec)
    return records_by_model


def _build_tasks_for_model(model_name: str, schedule_records: List[SchedulePointRecord], max_iterations: int) -> List[Dict[str, Any]]:
    base_dir = os.path.join(OUTPUT_DIR, model_name)
    model_dump_path = os.path.join(base_dir, "layers.dump")
    layer_mapping_dir_path = os.path.join(base_dir, "mapping")

    if not os.path.exists(model_dump_path):
        print(f"Model dump not found for {model_name}: {model_dump_path}")
        return []
    if not os.path.exists(layer_mapping_dir_path):
        print(f"Layer mapping dir not found for {model_name}: {layer_mapping_dir_path}")
        return []

    tasks: List[Dict[str, Any]] = []

    for rec_idx, rec in enumerate(schedule_records):
        end_layers: List[int] = rec.node.get(SchedulePointRecord.KEY.point, [])
        if not end_layers:
            raise RuntimeError(f"No schedule points in record {rec_idx} for model {model_name}")

        # partition_num comes from schedule point target
        partition_num = rec.target.get(SchedulePointRecord.KEY.num, len(end_layers))
        partition_dir = os.path.join(base_dir, "schedule_point", "partition", str(partition_num))
        if not os.path.exists(partition_dir):
            raise RuntimeError(f"Partition directory missing for model={model_name} partition_num={partition_num}: {partition_dir}")

        start_layer = 0
        for seg_idx, end_layer in enumerate(end_layers):
            seg_start = start_layer
            seg_end = end_layer
            start_layer = end_layer + 1

            partition_dump_path = os.path.join(partition_dir, f"model_partition_{seg_idx}.dump")
            if not os.path.exists(partition_dump_path):
                raise RuntimeError(f"Partition dump missing for model={model_name} partition_num={partition_num} segment={seg_idx}: {partition_dump_path}")

            with open(partition_dump_path, 'rb') as f:
                partition_record: GraphPartitionRecord = pickle.load(f)

            if not partition_record.candidate_list:
                raise RuntimeError(f"No graph partition candidates for model={model_name} partition_num={partition_num} segment={seg_idx}")

            for cand_idx, cand in enumerate(partition_record.candidate_list):
                tn = cand.target_node
                if tn.get(GraphPartitionCandidate.KEY.start_layer_idx) != seg_start or tn.get(GraphPartitionCandidate.KEY.end_layer_idx) != seg_end:
                    raise RuntimeError(f"Candidate {cand_idx} does not match segment layers {seg_start}-{seg_end} for model {model_name}")

                # 只保留需要的acc、batch、drambw、nocbw组合
                if not _candidate_in_search_space(tn):
                    continue
                
                partition_plan = cand.node[GraphPartitionCandidate.KEY.partition_plan]
                acc_plan = cand.node[GraphPartitionCandidate.KEY.acc_plan]

                for spm_per_acc_kb in SPM_PER_ACC_CANDIDATES_KB:
                    for method in METHOD:
                        tasks.append({
                            'model_name': model_name,
                            'model_dump_path': model_dump_path,
                            'layer_mapping_dir_path': layer_mapping_dir_path,
                            'acc': int(tn[GraphPartitionCandidate.KEY.acc_num]),
                            'spm_per_acc_kb': int(spm_per_acc_kb),
                            'batch': int(tn[GraphPartitionCandidate.KEY.default_batch]),
                            'dram_bw': int(tn[GraphPartitionCandidate.KEY.dram_bw_per_cycle]),
                            'noc_bw': int(tn[GraphPartitionCandidate.KEY.noc_bw_per_cycle]),
                            'partition_plan': partition_plan,
                            'acc_plan': acc_plan,
                            'segment_idx': seg_idx,
                            'seg_start': seg_start,
                            'seg_end': seg_end,
                            'max_iterations': max_iterations,
                            'method': method,
                            'candidate_idx': cand_idx,
                            'schedule_record_idx': rec_idx,
                            'partition_num': partition_num,
                        })
    return tasks

def process_task(task: Dict[str, Any]) -> Tuple[bool, Dict[str, Any]]:
    """Worker function to process a single combination task.
    Returns (success, payload/message).
    """
    try:
        model_name = task['model_name']
        model_dump_path = task['model_dump_path']
        layer_mapping_dir_path = task['layer_mapping_dir_path']
        acc = int(task['acc'])
        spm_per_acc_kb = int(task['spm_per_acc_kb'])
        batch = int(task['batch'])
        dram_bw = int(task['dram_bw'])
        noc_bw = int(task['noc_bw'])
        seg_idx = int(task['segment_idx'])
        seg_start = int(task['seg_start'])
        seg_end = int(task['seg_end'])
        max_iterations = task.get('max_iterations', MAX_ITERATIONS)
        method = task.get('method', 'ours')

        partition_plan = [tuple(p) for p in task['partition_plan']]
        acc_plan = [list(map(int, plan)) for plan in task['acc_plan']]

        # Load model and layer mapping per process to avoid pickling complex objects
        model: Model = load_model(model_dump_path)
        model.load_layer_mapping(layer_mapping_dir_path)

        spmKB_total = spm_per_acc_kb * acc

        print(
            f"\n[PID {os.getpid()}] Segment={seg_idx} model={model_name} acc={acc} spm_per_acc_kb={spm_per_acc_kb} "
            f"batch={batch} dram_bw={dram_bw} noc_bw={noc_bw} method={method}"
        )

        hw_target = Target(
            numArrays=acc,
            dram_bw_per_cycle=dram_bw,
            spmKB=spmKB_total,
            noc_bw_per_cycle=noc_bw,
            enablePipeline=True,
        )

        pipeline_target = PipelineTarget(
            model=model,
            layer_mapping_dir_path=layer_mapping_dir_path,
            start_layer_idx=seg_start,
            end_layer_idx=seg_end,
            default_batch=batch,
            target=hw_target,
            remote_layer_compute_ratio=1.1,
            resource_util_ratio=0,
            enable_decoupling=False if method in ['tangram', 'gemini'] else True
        )

        final_solution = None
        best_energy = None
        sa = None
        if method == 'ours':
            sa = GraphPartitionSimulatedAnnealing(
                pipeline_target=pipeline_target,
                partition_plan=partition_plan,
                acc_plan=acc_plan,
                layer_mapping=model._layer_mapping,
                max_iterations=max_iterations,
                enable_partition_search=True
            )

            sa.anneal()
            best_state, best_energy = sa.get_best_state()
            final_solution = sa._state_to_solution(best_state)
        elif method == 'gemini':
            sa = GraphPartitionSimulatedAnnealing(
                pipeline_target=pipeline_target,
                partition_plan=partition_plan,
                acc_plan=acc_plan,
                layer_mapping=model._layer_mapping,
                max_iterations=max_iterations,
                gemini_like=True,
                enable_partition_search=True
            )

            sa.anneal()
            best_state, best_energy = sa.get_best_state()
            final_solution = sa._state_to_solution(best_state)
        elif method == 'tangram':
            sa = GraphPartitionSimulatedAnnealing(
                pipeline_target=pipeline_target,
                partition_plan=partition_plan,
                acc_plan=acc_plan,
                layer_mapping=model._layer_mapping,
                max_iterations=max_iterations,
                tangram_like=True,
                enable_partition_search=True
            )
            
            sa.anneal()
            best_state, best_energy = sa.get_best_state()
            final_solution = sa._state_to_solution(best_state)
        else:
            return False, f"Unknown method {method}"

        pipeline_record = SolutionToRecordFactory.create_pipeline_record_from_pipeline_solution(final_solution, model)

        result_entry = {
            'model_name': model_name,
            'partition_num': task.get('partition_num'),
            'seg_idx': seg_idx,
            'candidate_idx': task.get('candidate_idx'),
            'spm_per_acc_kb': spm_per_acc_kb,
            'method': method,
            'target': {
                'acc': acc,
                'batch': batch,
                'dram_bw': dram_bw,
                'noc_bw': noc_bw,
            },
            'energy': best_energy,
            'init_energy': sa.init_energy if sa else None,
            'pipeline_record': pipeline_record.node,
        }

        msg = (
            f"Processed pipeline record model={model_name} partition_num={task.get('partition_num')} seg={seg_idx} "
            f"acc={acc} batch={batch} dram_bw={dram_bw} noc_bw={noc_bw} method={method} "
            f"(best energy={best_energy}, init energy={sa.init_energy if sa else 'n/a'})"
        )
        print(msg)
        return True, result_entry

    except Exception as e:
        traceback_str = traceback.format_exc()
        err_msg = (
            f"Error processing task for model={task.get('model_name')} acc={task.get('acc')} "
            f" batch={task.get('batch')} dram_bw={task.get('dram_bw')} noc_bw={task.get('noc_bw')} method={task.get('method')} "
            f" partition_num={task.get('partition_num')} segment={task.get('segment_idx')}: {e}\n{traceback_str}"
        )
        print(err_msg)
        return False, {'error': err_msg, 'model_name': task.get('model_name')}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--models', type=str, default="", help='Comma-separated model names; empty means all in schedule dump')
    parser.add_argument('--max-iter', type=int, default=MAX_ITERATIONS)
    parser.add_argument('--jobs', type=int, default=max(os.cpu_count() // 2, 1), help='Number of parallel worker processes (default: number of CPUs)')
    args = parser.parse_args()

    model_filter = _parse_models_arg(args.models)

    records_by_model = _load_schedule_records(model_filter)
    if not records_by_model:
        raise RuntimeError("No schedule point records matched the requested models.")

    all_tasks: List[Dict[str, Any]] = []
    for model_name, recs in records_by_model.items():
        model_tasks = _build_tasks_for_model(model_name, recs, args.max_iter)
        print(f"Collected {len(model_tasks)} tasks for model {model_name}")
        all_tasks.extend(model_tasks)

    if not all_tasks:
        print("No tasks to process.")
        return

    jobs = args.jobs
    print(f"Processing {len(all_tasks)} tasks using {jobs} job(s)")

    results = []
    if jobs == 1:
        for t in all_tasks:
            results.append(process_task(t))
    else:
        with Pool(processes=jobs) as pool:
            results = pool.map(process_task, all_tasks)

    success_count = 0
    fail_count = 0
    for ok, payload in results:
        if not ok:
            fail_count += 1
            continue
        success_count += 1

        model_name = payload['model_name']
        partition_num = int(payload['partition_num'])
        seg_idx = int(payload['seg_idx'])
        target = payload['target']
        method = payload['method']

        out_dir = os.path.join(
            OUTPUT_DIR,
            model_name,
            'schedule_point',
            'pipeline_mapping',
            str(partition_num),
            str(seg_idx),
        )
        os.makedirs(out_dir, exist_ok=True)

        filename = f"{target['acc']}_{payload['spm_per_acc_kb']}_{target['batch']}_{target['dram_bw']}_{target['noc_bw']}_{method}.yaml"
        out_path = os.path.join(out_dir, filename)

        with open(out_path, 'w') as f:
            yaml.dump(payload['pipeline_record'], f, Dumper=CustomYamlDumper)

        print(f"Saved pipeline record to {out_path}")

    print(f"All models: {success_count} succeeded, {fail_count} failed")


if __name__ == '__main__':
    main()
