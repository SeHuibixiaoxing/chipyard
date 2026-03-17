import os
from multiprocessing import Pool

modelList = [
    "resnet50",
    "mobilenetv2",
    "effinetb0",
    "vitb16",
    "pointpillars",
    "w2v2base",
    "bertbase",
    "gnmt",
]

system = "cache"
modelRoot = "models/"


def run(modelName):
    cmd = (f"./build/expr_profiling "
           f"--system {system} "
           f"--model {modelName} "
           f"--output {modelName} ")
    print(cmd)
    os.system(cmd)


def main():
    argList = []
    for modelName in modelList:
        argList.append((modelName,))
    with Pool(processes=40) as pool:
        pool.starmap(run, argList)


if __name__ == "__main__":
    main()
