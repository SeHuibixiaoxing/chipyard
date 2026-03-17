from HybridMapper.HybridMapper import *
from HybridMapper.Problem import *


def main():
    probList = [
        ProblemConv([1, 3, 64, 112, 112, 7, 7, 1], [2, 2]),
        ProblemConv([1, 128, 128, 56, 56, 3, 3, 1], [2, 2]),
        ProblemConv([1, 256, 256, 28, 28, 3, 3, 1], [2, 2]),
        ProblemConv([1, 1024, 1024, 7, 7, 3, 3, 1], [2, 2]),
        ProblemConv([1, 64, 128, 112, 112, 1, 1, 1], [1, 1]),
        ProblemConv([1, 512, 1024, 14, 14, 1, 1, 1], [1, 1]),
        ProblemConv([1, 4096, 1000, 1, 1, 1, 1, 1], [1, 1]),

        ProblemConv([1, 3, 64, 14, 14, 16, 16, 1], [16, 16]),
        ProblemConv([50, 768, 2304, 1, 1, 1, 1, 1], [1, 1]),
        ProblemConv([50, 64, 50, 1, 1, 1, 1, 12], [1, 1]),

        ProblemConv([1, 1, 1, 112, 112, 7, 7, 64], [1, 1]),
        ProblemConv([1, 1, 1, 28, 28, 3, 3, 256], [1, 1]),
        ProblemConv([1, 1, 1, 7, 7, 3, 3, 1024], [1, 1]),

        ProblemResadd([1, 256, 56, 56, 1]),
        ProblemResadd([1, 2048, 14, 14, 1]),
    ]

    dramKeepList = []
    for prob in probList:
        if isinstance(prob, ProblemConv):
            # dramKeepList.append([1, 1, 1, 1])
            # dramKeepList.append([1, 1, 1, 0])
            # dramKeepList.append([1, 1, 0, 1])
            dramKeepList.append([1, 1, 0, 0])
        elif isinstance(prob, ProblemResadd):
            # dramKeepList.append([1, 1, 1])
            # dramKeepList.append([1, 1, 0])
            dramKeepList.append([0, 0, 0])

    targetList = [
        Target(1, 0, isCache=True),
        Target(1, 256),
        Target(1, 1024 * 1024, enableILR=True),

        Target(2, 0, isCache=True),
        Target(2, 256),
        Target(2, 1024 * 1024, enableILR=True),

        Target(4, 0, isCache=True),
        Target(4, 256),
        Target(4, 1024 * 1024, enableILR=True),
    ]

    outputFolder = "HybridMapper/test/test_HybridMapper_output"
    name = "mapping0"
    outputPath = os.path.join(outputFolder, name)

    hm = HybridMapper(probList, dramKeepList, targetList, outputPath)
    hm.run()


if __name__ == "__main__":
    main()
