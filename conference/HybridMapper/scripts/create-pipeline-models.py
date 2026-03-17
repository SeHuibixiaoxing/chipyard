import os
import argparse

modelList = [
        "resnet50", # ok
        "mobilenetv2", # ok
        "effinetb0", # ok
        "vitb16", # ok
        "pointpillars", # ok
        "w2v2base", # ok
        "bertbase", # ok
        "gnmt", # done
        
        "berttiny",
        "bertmini",
        "bertsmall",
        "bertmedium",
        "vitsmall16",
        
        "resnet18", # ok

        # "ptv3",
        # "onet",
        # "testmodel",
        # "unet",
        
        # "bert_mha",
        # "bert_ffn"
    ]

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--models', type=lambda s: s.split(','),  default=[], help='逗号分隔的模型名称')
    parser.add_argument('--stages', type=lambda s: s.split(','),  default=[], help='逗号分隔的执行阶段')
    parser.add_argument('--max-iter', type=int, default=100, help='Maximum iterations for SA search')
    parser.add_argument('--num-proc', type=int, default=-1, help='Number of processes for SA search')
    args = parser.parse_args()
    
    model_names = args.models if args.models else modelList
    stage_vec = args.stages
    
    model_create_enable: bool = (not stage_vec) or "MC" in stage_vec
    layer_mapping_enable: bool = (not stage_vec) or "LM" in stage_vec
    
    graph_partition_enable: bool = (not stage_vec) or "GP" in stage_vec
    
    sa_search_enable: bool = (not stage_vec) or "SA" in stage_vec
    
    entire_mpdel_sa_search_enable: bool = (not stage_vec) or "EMSA" in stage_vec
    
    schedule_point_enable: bool = (not stage_vec) or "SP" in stage_vec
    schedule_point_graphpartition_enable: bool = (not stage_vec) or "SPGP" in stage_vec
    schedule_point_sasearch_enable: bool = (not stage_vec) or "SPSA" in stage_vec
    schedule_point_profiling_enable: bool = (not stage_vec) or "SPPF" in stage_vec
    
    if model_create_enable:
        for modelName in model_names:
            assert(modelName in modelList)
            print(f"\nCreating {modelName} model...")
            os.system(f"python scripts/create-pipeline-model-mapping.py --model {modelName} --no-mapping ")
    
    if layer_mapping_enable:
        for modelName in model_names:
            assert(modelName in modelList)
            print(f"\nCreating {modelName} layer mapping...")
            os.system(f"python scripts/create-pipeline-model-mapping.py --model {modelName} ")

    if graph_partition_enable:
        for modelName in model_names:
            assert(modelName in modelList)
            print(f"\nCreating {modelName} model partition...")
            os.system(f"python scripts/create-pipeline-model-partition.py --model {modelName}")
    
    if sa_search_enable:
        for modelName in model_names:
            assert(modelName in modelList)
            print(f"\nCreating {modelName} model partition...")
            os.system(f"python scripts/create-pipeline-sasearch.py --model {modelName}")
    
    if entire_mpdel_sa_search_enable:
        for modelName in model_names:
            assert(modelName in modelList)
            print(f"\nCreating {modelName} entire model partition...")
            os.system(f"python scripts/create-pipeline-sasearch-single-model.py --models {modelName} --max-iter {args.max_iter}" + f" --jobs {args.num_proc}" if args.num_proc > 0 else "")
            print(f"\nEmitting {modelName} Gemmini pipeline-runtime artifacts...")
            os.system(f"python scripts/create-gemmini-pipeline-runtime-artifacts.py --model {modelName}")

    if schedule_point_enable:
        print("\nCreating schedule points...")
        os.system(f"python scripts/create-pipeline-schedule-point.py --models {','.join(model_names)}")
        
    if schedule_point_graphpartition_enable:
        print("\nCreating schedule point graph partitions...")
        for modelName in model_names:
            assert(modelName in modelList)
            print(f"\nCreating {modelName} schedule point graph partitions...")
            os.system(f"python scripts/create-pipeline-schedule-graphpartition.py --model {modelName}" + f" --jobs {args.num_proc}" if args.num_proc > 0 else "")
    
    if schedule_point_sasearch_enable:
        print("\nCreating schedule point SA searches...")
        os.system(f"python scripts/create-pipeline-schedule-sasearch.py --models {','.join(model_names)} --max-iter {args.max_iter}" + f" --jobs {args.num_proc}" if args.num_proc > 0 else "")

    if schedule_point_profiling_enable:
        print("\nCreating schedule point profiling files...")
        os.system(f"python scripts/create-schedule-point-profiling-file.py --models {','.join(model_names)}")
    
if __name__ == "__main__":
    main()
