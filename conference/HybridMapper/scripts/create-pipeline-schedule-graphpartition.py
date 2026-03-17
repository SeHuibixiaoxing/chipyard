
import os
import argparse
import pickle
from HybridMapper.GraphPartition import GraphPartitioner
from HybridMapper.Target import Target
from HybridMapper.Model import Model
from HybridMapper.SchedulePointRecord import SchedulePointRecord, SchedulePointRecodList

target_acc_list = [8,16,24,32]
dram_bw_list = [19]
noc_bw_per_cycle_list = [64]
default_batch_list = [16]
partition_num_list = [3,4,5,6] 

def get_graph_target():
    re = []
    for acc in target_acc_list:
        for dram_bw_per_cycle in dram_bw_list:
            for noc_bw_per_cycle in noc_bw_per_cycle_list:
                re.append(Target(acc, acc * 8 * 1024, enablePipeline=True, dram_bw_per_cycle=dram_bw_per_cycle, noc_bw_per_cycle=noc_bw_per_cycle))
    return re


OUTPUT_DIR = "output/pipeline/"

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


import multiprocessing

def partition_segment_task(args):
    model_name, partition_num, candidate_idx, rec, base_dir, model_dump_path, layer_mapping_dir_path, target_list, batch_candidates = args
    assert(partition_num == rec.target[rec.KEY.num])
    end_layers = rec.node[SchedulePointRecord.KEY.point]
    start_layer = 0
    results = []
    # Load model once per process
    model: Model = load_model(model_dump_path)
    model.load_layer_mapping(layer_mapping_dir_path)
    partition_dir = os.path.join(base_dir, "schedule_point", "partition", str(partition_num))
    os.makedirs(partition_dir, exist_ok=True)
    for seg_idx, end_layer in enumerate(end_layers):
        seg_start = start_layer
        seg_end = end_layer
        start_layer = end_layer + 1
        if seg_start > seg_end:
            raise RuntimeError(f"invalid segment {seg_start}-{seg_end} for model {model_name}")
        record = None
        for target in target_list:
            for default_batch in batch_candidates:
                partitioner = GraphPartitioner(
                    model=model,
                    target=target,
                    default_batch=default_batch,
                )
                _, new_record = partitioner.partition(start_layer_idx=seg_start, end_layer_idx=seg_end)
                if record is None:
                    record = new_record
                else:
                    record.merge(new_record, inplace=True)
        if record is None:
            raise RuntimeError(f"No partition record generated for model={model_name}, partition_num={partition_num}, segment={seg_start}-{seg_end}")
        rec_dir = os.path.join(partition_dir)
        os.makedirs(rec_dir, exist_ok=True)
        yaml_path = os.path.join(rec_dir, f"model_partition_{seg_idx}.yaml")
        dump_path = os.path.join(rec_dir, f"model_partition_{seg_idx}.dump")
        record.write(yaml_path)
        with open(dump_path, "wb") as f:
            pickle.dump(record, f)
        results.append((model_name, partition_num, seg_idx, yaml_path))
    return results

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", type=str, required=False, default="", help="Target model name; empty means all models in schedule dump")
    parser.add_argument("--jobs", type=int, default=max(multiprocessing.cpu_count() // 2, 1), help="Number of parallel processes; -1 means auto")
    args = parser.parse_args()

    schedule_dump_path = os.path.join(OUTPUT_DIR, "pipeline_schedule_points.dump")
    if not os.path.exists(schedule_dump_path):
        raise FileNotFoundError(f"Schedule points dump not found: {schedule_dump_path}")

    schedule_list: SchedulePointRecodList = pickle.load(open(schedule_dump_path, 'rb'))
    schedule_records = schedule_list.record_list if hasattr(schedule_list, 'record_list') else []

    if not schedule_records:
        raise RuntimeError("No schedule point candidates found in dump.")

    target_models = {rec.target[SchedulePointRecord.KEY.model] for rec in schedule_records}
    if args.model:
        target_models = {m for m in target_models if m == args.model}
    if not target_models:
        raise RuntimeError("No matching models found in schedule point dump.")

    target_list = get_graph_target()
    batch_candidates = list(default_batch_list)

    tasks = []
    for partition_num in partition_num_list:
        for model_name in target_models:
            base_dir = os.path.join(OUTPUT_DIR, model_name)
            model_dump_path = os.path.join(base_dir, "layers.dump")
            layer_mapping_dir_path = os.path.join(base_dir, "mapping")
            if not os.path.exists(model_dump_path):
                raise RuntimeError(f"{model_name}: missing model dump {model_dump_path}")
            if not os.path.exists(layer_mapping_dir_path):
                raise RuntimeError(f"{model_name}: missing layer mapping dir {layer_mapping_dir_path}")
            model_candidates = [rec for rec in schedule_records if rec.target[SchedulePointRecord.KEY.model] == model_name and rec.target[SchedulePointRecord.KEY.num] == partition_num]
            if not model_candidates:
                continue
            for candidate_idx, rec in enumerate(model_candidates):
                tasks.append((model_name, partition_num, candidate_idx, rec, base_dir, model_dump_path, layer_mapping_dir_path, target_list, batch_candidates))

    print(f"Launching {len(tasks)} partitioning tasks in parallel...")
    with multiprocessing.Pool(processes=args.jobs) as pool:
        results = pool.map(partition_segment_task, tasks)
    print(f"Partitioning complete. {sum(len(r) for r in results)} segments processed.")

if __name__ == "__main__":
    main()