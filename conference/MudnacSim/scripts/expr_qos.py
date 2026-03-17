import os
from multiprocessing import Pool
import argparse
import time


methodNameList = ["plain", "moca", "aurora", "scsmtn"]
# methodNameList = ["moca", "aurora", "scsmtn"]
# methodNameList = ["scsmtn"]
targetScaleList = [1.4, 1.2, 1, 0.8, 0.6]
# targetScaleList = [1]
# targetScaleList = [1.2]

BINARY_PATH = "./build/expr_qos"
OUTPUT_ROOT = f"./expr/output/expr_qos/script_output"


def run(tasks, targetScale, methodName):
    system = "cache"
    memoryMode = "unlimited"
    accAllocMode = "static"
    spmAllocMode = "static"
    spmAllocIdx = 1
    if methodName == "moca":
        memoryMode = "limited"
    elif methodName == "aurora":
        memoryMode = "limited"
        accAllocMode = "dynamic"
    elif methodName == "scsmtn":
        system = "spm"
        memoryMode = "limited"
        accAllocMode = "dynamic"
        spmAllocMode = "dynamic"
    else:
        assert methodName == "plain"

    outputName = f"tasks{tasks}_scale{targetScale}_{methodName}"
    outputRoot = OUTPUT_ROOT
    cmd = (f"{BINARY_PATH} "
           f"--system {system} "
           f"--tasks {tasks} "
           f"--target-scale {targetScale} "
           f"--memory-mode {memoryMode} "
           f"--acc-alloc-mode {accAllocMode} "
           f"--spm-alloc-mode {spmAllocMode} "
           f"--spm-alloc-idx {spmAllocIdx} "
           f"--output {outputName} "
           f"--output-root {outputRoot} "
           )
    r = os.system(cmd)
    return r


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--tasks", type=int, default=50)
    parser.add_argument("--num-proc", type=int, default=60)
    args = parser.parse_args()

    tasks = args.tasks
    numProcesses = args.num_proc

    argList = []
    for methodName in methodNameList:
        for targetScale in targetScaleList:
            argList.append((tasks, targetScale, methodName))

    timeStart = time.time()
    with Pool(processes=numProcesses) as pool:
        results = pool.starmap(run, argList)
    print(results)
    timeEnd = time.time()
    print(f"elapse: {int((timeEnd - timeStart) / 60)}min")


if __name__ == "__main__":
    main()
