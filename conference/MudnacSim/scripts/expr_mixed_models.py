import os
from multiprocessing import Pool
import time

N_PROCESSES = 80
# N_PROCESSES = 16

BINARY_PATH = "./build/expr_mixed_models"

# OUTPUT_ROOT = "expr/output/expr_mixed_models/default/expr"
OUTPUT_ROOT = "expr/output/expr_mixed_models/default/test"

SPM_MULTICAST = 0
# SPM_MULTICAST = 1

# ACCELS = 8
ACCELS = 16
# ACCELS = 32
# ACCELS = 64

# ACC_ALLOC_IDX = 0
# ACC_ALLOC_IDX = 1
ACC_ALLOC_IDX = 2

# THREADS = 1
# THREADS = 4
# THREADS = 8
# THREADS = 16
# THREADS = 32
# THREADS = 64
if ACC_ALLOC_IDX == 0:
    THREADS = ACCELS
elif ACC_ALLOC_IDX == 1:
    THREADS = int(ACCELS / 2)
elif ACC_ALLOC_IDX == 2:
    THREADS = int(ACCELS / 4)
else:
    assert False

# BENCHMARK = 0
# BENCHMARK = 1
# BENCHMARK = 2
BENCHMARK = 3

# CHANNELS = 4
CHANNELS = 8
# CHANNELS = 16

TIME = 0.05
# TIME = 0.5

# [system, spm-alloc-mode, spm-page-strategy, spm-addr-type]
systemAndSpmSettingList = [
    ["cache", "static", "naive", "block"],
    # ["spm", "fix", "naive", "block"],
    # ["spm", "fix", "naive", "conti"],
    # ["spm", "fix", "ordered", "conti"],
]

# systemAndSpmSettingList = [
#     ["spm", "fix", "naive", "block"],
#     ["spm", "fix", "naive", "conti"],
# ]

# [system, spm-alloc-mode, spm-page-strategy, spm-addr-type]
# systemAndSpmSettingList = [
#     ["cache", "static", "naive", "block"],
#     ["spm", "dynamic", "naive", "block"],
#     ["spm", "dynamic", "naive", "conti"],
#     ["spm", "dynamic", "ordered", "conti"],
# ]

# [noc, noc0-flit-size]
nocAndFlitSettingList = [
    # ["none", 64],
    ["HomoTileMesh", 64],
    # ["HomoTileMesh", 32],
    # ["HomoTileMesh", 16],
]

# cacheList = [16]
# cacheList = [64]
# cacheList = [128]
# cacheList = [16, 64]
cacheList = [64]
# cacheList = [16, 32, 64]
# cacheList = [32, 64, 128]
# cacheList = [64, 128, 256]


def run(system, spmAllocMode, spmPageStrategy, spmAddrType, noc, noc0FlitSize, cache):
    outputRoot = os.path.join(OUTPUT_ROOT, f"bm-{BENCHMARK}_cache-{cache}_accels-{ACCELS}_accAllocIdx-{ACC_ALLOC_IDX}")
    outputName = (f"{system}"
                  f"_saddr-{spmAddrType}"
                  f"_noc-{noc}"
                  f"_flit-{noc0FlitSize}"
                  f"_page-{spmPageStrategy}")
    cmd = (f"{BINARY_PATH} "
           f"--system {system} "
           f"--channels {CHANNELS} "
           f"--cache {cache} "
           f"--accels {ACCELS} "
           f"--threads {THREADS} "
           f"--benchmark {BENCHMARK} "
           f"--output-root {outputRoot} "
           f"--output {outputName} "
           f"--spm-alloc-mode {spmAllocMode} "
           f"--spm-page-strategy {spmPageStrategy} "
           f"--noc {noc} "
           f"--noc0-flit-size {noc0FlitSize} "
           f"--time {TIME} "
           f"--acc-alloc-idx {ACC_ALLOC_IDX} "
           f"--spm-addr-type {spmAddrType} "
           f"--spm-multicast {SPM_MULTICAST} ")
    print(cmd)
    os.system(cmd)


def main():
    argList = []
    for systemAndSpm in systemAndSpmSettingList:
        for nocAndFlit in nocAndFlitSettingList:
            for cache in cacheList:
                argList.append(systemAndSpm + nocAndFlit + [cache])

    timeStart = time.time()
    with Pool(processes=N_PROCESSES) as pool:
        pool.starmap(run, argList)
    timeEnd = time.time()
    print(f"elapse: {int((timeEnd - timeStart) / 60)}min")


if __name__ == "__main__":
    main()
