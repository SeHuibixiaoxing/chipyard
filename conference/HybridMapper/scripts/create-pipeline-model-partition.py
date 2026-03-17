import os
import argparse
import pickle
from concurrent.futures import ProcessPoolExecutor, as_completed
from HybridMapper.GraphPartition import GraphPartitioner
from HybridMapper.Target import Target
from HybridMapper.Model import Model

target_acc_list = [8, 16, 24, 32]
dram_bw_list = [19]
noc_bw_per_cycle_list = [32, 64]
default_batch_list = [1, 2, 4, 8, 16, 32]

def get_graph_target():
    re = []
    for acc in target_acc_list:
        for dram_bw_per_cycle in dram_bw_list:
            for noc_bw_per_cycle in noc_bw_per_cycle_list:
                re.append(Target(acc, acc * 8 * 1024, enablePipeline=True, dram_bw_per_cycle=dram_bw_per_cycle, noc_bw_per_cycle=noc_bw_per_cycle))
    return re


OUTPUT_DIR = "output/pipeline/"

def partition_single(model_bytes: bytes, target: Target, default_batch: int, start_layer_idx: int):
    """Run a single partition job in a separate process."""
    model: Model = pickle.loads(model_bytes)
    partitioner = GraphPartitioner(
        model=model,
        target=target,
        default_batch=default_batch,
    )
    new_record, _ = partitioner.partition(start_layer_idx=start_layer_idx)
    return new_record

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

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", type=str, required=False, default="")
    parser.add_argument("--workers", type=int, default=max(os.cpu_count()//2, 1),
                        help="Number of parallel worker processes")
    
    args = parser.parse_args()
    
    model_name = args.model
    base_dir = os.path.join(OUTPUT_DIR, model_name)
    model_dump_path = os.path.join(base_dir, "layers.dump")
    layer_mapping_dir_path = os.path.join(base_dir, "mapping")
    
    print(f"Loading model: {model_name} from path: {model_dump_path}")
    model: Model = load_model(model_dump_path)
    model.load_layer_mapping(layer_mapping_dir_path)
    model_bytes = pickle.dumps(model)
    
    print(f"Generating partition plan for {model_name} with {args.workers} workers...")
    
    start_layer_idx = 0
    record = None
    target_list = get_graph_target()
    batch_candidates = list(default_batch_list)
    jobs = [(target, default_batch) for target in target_list for default_batch in batch_candidates]
    print(f"Submitting {len(jobs)} jobs: {len(target_list)} targets x {len(batch_candidates)} batches")
    
    with ProcessPoolExecutor(max_workers=args.workers) as executor:
        future_to_job = {
            executor.submit(partition_single, model_bytes, target, default_batch, start_layer_idx): (target, default_batch)
            for target, default_batch in jobs
        }
        for future in as_completed(future_to_job):
            target, default_batch = future_to_job[future]
            try:
                new_record = future.result()
            except Exception as exc:
                print(f"Partition failed for acc={target.numArrays}, batch={default_batch}: {exc}")
                continue
            if record is None:
                record = new_record
            else:
                record.merge(new_record, inplace=True)
            print(f"Completed acc={target.numArrays}, batch={default_batch}; total candidates={len(record.candidate_list)}")
    
    if record is None:
        raise RuntimeError("No partition records generated.")
    
    model_partition_path = os.path.join(base_dir, "model_partition.yaml")
    model_partition_dump_path = os.path.join(base_dir, "model_partition.dump")

    record.write(model_partition_path)
    with open(model_partition_dump_path, "wb") as f:
        pickle.dump(record, f)  # 将对象转换为字节流写入文件
    
if __name__ == "__main__":
    main()