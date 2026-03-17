import os
import itertools
from multiprocessing import Pool, cpu_count
from pathlib import Path

# Hardcoded candidate lists
modelList = [
    "resnet18",
    "resnet50",
    "mobilenetv2",
    "effinetb0",
    "vitb16",
    "pointpillars",
    "w2v2base",
    "bertbase",
    "gnmt",
    
    # "vitsmall16",
    # "berttiny",
    # "bertmini",
    # "bertsmall",
    # "bertmedium",
]

channels_list = [1]
accels_list = [64]
spm_sizes_kb = [1024]
noc_flits_list = [64]
batch_sizes = [16,]
fix_acc_idx_list = [4]

modelRoot = "models/"

outputRoot = Path("expr/output/expr_profiling2/script_output/")
outputRoot.mkdir(parents=True, exist_ok=True)


def run(modelName, channel, accels, spm_size_per_bank, noc_flits_size, batch_size, fix_acc_idx):
    output_name = f"{modelName}_channel{channel}_acc{accels}_spm{spm_size_per_bank}_noc{noc_flits_size}_batch{batch_size}_accidx{fix_acc_idx}.profile"
    cmd = (f"./build/expr_profiling2 "
           f"--model {modelName} "
           f"--output {modelName} "
           f"--channels {channel} "
           f"--accels {accels} "
           f"--spm-size-per-bank {spm_size_per_bank} "
           f"--noc-flit-size {noc_flits_size} "
           f"--batch-size {batch_size} "
           f"--output-root {str(outputRoot)} "
           f"--profiling-fix-acc-alloc-idx {fix_acc_idx} "
           f"--profiling-file-path profiling/{output_name}")
    print(cmd)
    return os.system(cmd)


def main():
    # build cartesian product of candidates
    combos = list(itertools.product(modelList, channels_list, accels_list, spm_sizes_kb, noc_flits_list, batch_sizes, fix_acc_idx_list))

    # prepare arg list for starmap
    argList = [(m, c, a, s, n, b, ai) for (m, c, a, s, n, b, ai) in combos]

    # choose worker count
    workers = min(40, max(1, cpu_count()))
    print(f"Starting {len(argList)} runs with {workers} workers")

    with Pool(processes=workers) as pool:
        pool.starmap(run, argList)


if __name__ == "__main__":
    main()
