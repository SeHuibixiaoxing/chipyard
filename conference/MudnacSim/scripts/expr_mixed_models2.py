import os
from multiprocessing import Pool
import argparse
import time

# cacheList = [16]
# cacheList = [4, 8, 16, 32, 64]
# cacheList = [1, 4, 16, 64]
cacheList = [4, 16, 64]
# cacheList = [1024]

# threadsList = [16]
threadsList = [16, 8, 4, 2, 1]
# threadsList = [32, 16, 8, 4, 2, 1]
# threadsList = [32, 16, 4, 1]

BINARY_PATH = "./build/expr_mixed_models2"
OUTPUT_ROOT = f"./expr/output/expr_mixed_models2/script_output"
CHANNELS = 4
WAYS = 12


def getSpmAllocIdx(cacheSizeMB, numAccels):
    cachePerAccel = int(cacheSizeMB * 1024 / numAccels)
    print(f"getSpmAllocIdx: cachePerAccel={cachePerAccel}")
    if cachePerAccel < 256:
        return 1
    elif cachePerAccel < 512:
        return 2
    elif cachePerAccel < 1024:
        return 3
    elif cachePerAccel < 2048:
        return 4
    elif cachePerAccel < 4096:
        return 5
    else:
        return 6


def run(system, cache, threads, benchmark, spmAllocMode):
    outputName = f"ca{cache}_th{threads}"
    outputRoot = os.path.join(OUTPUT_ROOT, f"bm{benchmark}_{system}_{spmAllocMode}")
    spmAllocIdx = 0
    # spmAllocIdx = getSpmAllocIdx(cache, threads)
    cmd = (f"{BINARY_PATH} "
           f"--system {system} "
           f"--channels {CHANNELS} "
           f"--cache {cache} "
           f"--ways {WAYS} "
           f"--accels {threads} "
           f"--threads {threads} "
           f"--benchmark {benchmark} "
           f"--output {outputName} "
           f"--output-root {outputRoot} "
           f"--spm-alloc-mode {spmAllocMode} "
           f"--spm-alloc-idx {spmAllocIdx} "
           )
    print(cmd)
    os.system(cmd)


def main():
    global WAYS
    parser = argparse.ArgumentParser()
    parser.add_argument("--system", type=str, default="cache")
    parser.add_argument("--spm-alloc-mode", type=str, default="static")
    parser.add_argument("--benchmark", type=int, default=0)
    parser.add_argument("--num-proc", type=int, default=60)
    args = parser.parse_args()

    system = args.system
    spmAllocMode = args.spm_alloc_mode
    benchmark = args.benchmark
    numProcesses = args.num_proc

    assert system in {"cache", "spm"}
    assert spmAllocMode in {"static", "dynamic", "fix"}

    if system == "cache":
        WAYS = 16
    else:
        WAYS = 12

    argList = []
    for threads in threadsList:
        for cache in cacheList:
            argList.append((system, cache, threads, benchmark, spmAllocMode))

    timeStart = time.time()
    with Pool(processes=numProcesses) as pool:
        pool.starmap(run, argList)
    timeEnd = time.time()
    print(f"elapse: {int((timeEnd - timeStart) / 60)}min")


if __name__ == "__main__":
    main()
