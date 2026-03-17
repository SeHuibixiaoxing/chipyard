import os
import math
from multiprocessing import Pool
import argparse
import time

modelList = [
    "resnet50",
    "mobilenetv2",
    # "effinetb0",
    "vitb16",
    # "pointpillars",
    # "w2v2base",
    # "bertbase",
    # "gnmt",
]

cacheSizeList = [16]
# cacheSizeList = [64]
# cacheSizeList = [4, 16, 64]
# cacheSizeList = [1, 2, 4, 8, 16, 32, 64]

threadsList = [16]
# threadsList = [8, 4, 1]
# threadsList = [16, 8, 4, 2, 1]

accAllocIdxList = [0]
# accAllocIdxList = [2, 1, 0]

BINARY_PATH = "./build/expr_multi_models"

WAYS = 12
# WAYS = 16

# channels = 4
channels = 8

OUTPUT_ROOT = "./expr/output/expr_multi_models/script_output/"
# OUTPUT_ROOT = "./expr/output/expr_multi_models/script_output_backup_flit32/"


def run(noc, system, cacheSize, threads, modelName, spmAllocMode, accAllocIdx):
    outputName = f"NoC{noc}_{system}{cacheSize}_thr{threads}_acc{accAllocIdx}_{spmAllocMode}"
    outputRoot = os.path.join(OUTPUT_ROOT, modelName)
    if accAllocIdx == 0:
        accels = threads
    elif accAllocIdx == 1:
        accels = threads * 2
    elif accAllocIdx == 2:
        accels = threads * 4
    else:
        assert False
    cmd = (f"{BINARY_PATH} "
           f"--noc {noc} "
           f"--system {system} "
           f"--channels {channels} "
           f"--cache {cacheSize} "
           f"--ways {WAYS} "
           f"--accels {accels} "
           f"--threads {threads} "
           f"--model {modelName} "
           f"--output {outputName} "
           f"--output-root {outputRoot} "
           f"--spm-alloc-mode {spmAllocMode} "
           f"--acc-alloc-idx {accAllocIdx} "
           )
    print(cmd)
    os.system(cmd)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--noc", type=str, default="none")
    parser.add_argument("--system", type=str, default="cache")
    parser.add_argument("--spm-alloc-mode", type=str, default="static")
    parser.add_argument("--num-proc", type=int, default=60)
    args = parser.parse_args()

    noc = args.noc
    system = args.system
    spmAllocMode = args.spm_alloc_mode
    numProcesses = args.num_proc
    assert noc in {"none", "HomoTileMesh"}
    assert system in {"cache", "spm"}
    assert spmAllocMode in {"static", "dynamic", "fix"}

    argList = []
    for threads in threadsList:
        for accAllocIdx in accAllocIdxList:
            for cache in cacheSizeList:
                for modelName in modelList:
                    argList.append((noc, system, cache, threads, modelName, spmAllocMode, accAllocIdx))

    timeStart = time.time()
    with Pool(processes=numProcesses) as pool:
        pool.starmap(run, argList)
    timeEnd = time.time()
    print(f"elapse: {int((timeEnd - timeStart) / 60)}min")


if __name__ == "__main__":
    main()
