import os
from multiprocessing import Pool
import argparse

# modelList = [
#     "resnet50",
#     "mobilenetv2",
#     "effinetb0",
#     "vitb16",
#     "pointpillars",
#     "w2v2base",
#     "bertbase",
#     "gnmt",
# ]


modelList = [
    "mobilenetv2",
]

# cacheSizeList = [1, 2, 4, 8, 16]
# cacheSizeList = [32, 64, 128, 256]
# cacheSizeList = [1, 2, 4, 8, 16, 32, 64, 128, 256]
# cacheSizeList = [1, 4, 16, 64, 256]
# cacheSizeList = [1, 4]
# cacheSizeList = [4, 16, 32]
# cacheSizeList = [1, 2, 4, 8]
# cacheSizeList = [16]
cacheSizeList = [4]

# accAllocIdxList = [0, 1, 2]
accAllocIdxList = [0]

BINARY_PATH = "./build/expr_single_model"
OUTPUT_ROOT = "expr/output/expr_single_model/script_output/"


def run(system, modelName, spmAllocMode, cacheSize, accAllocIdx):
    outputName = f"{modelName}_{system}{cacheSize}_acc{accAllocIdx}"
    outputRoot = f"{OUTPUT_ROOT}"
    cmd = (f"{BINARY_PATH} "
           f"--system {system} "
           f"--cache {cacheSize} "
           f"--model {modelName} "
           f"--output {outputName} "
           f"--output-root {outputRoot} "
           f"--spm-alloc-mode {spmAllocMode} "
           f"--acc-alloc-idx {accAllocIdx} "
           f"--spm-addr-type block"
           )
    print(cmd)
    os.system(cmd)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--system", type=str, default="cache")
    parser.add_argument("--spm-alloc-mode", type=str, default="static")
    parser.add_argument("--num-proc", type=int, default=60)
    args = parser.parse_args()

    system = args.system
    spmAllocMode = args.spm_alloc_mode
    numProcesses = args.num_proc
    assert system in {"cache", "spm"}
    assert spmAllocMode in {"static", "dynamic", "fix"}

    argList = []
    for accAllocIdx in accAllocIdxList:
        for cacheSize in cacheSizeList:
            for modelName in modelList:
                argList.append((system, modelName, spmAllocMode, cacheSize, accAllocIdx))

    with Pool(processes=numProcesses) as pool:
        pool.starmap(run, argList)


if __name__ == "__main__":
    main()
