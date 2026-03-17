from HybridMapper.Optimizer import *


def test1():
    dramRepeat = [["G", "OC"],
                  ["G", "OC", "N"],
                  ["G", "OC", "N", "OH", "OW"],
                  ["G", "OC", "N", "OH", "OW"]]
    spmRepeat = [["G", "OC"],
                 ["G", "OC", "N", "OH", "OW", "IC", "KH", "KW"],
                 ["G", "OC", "N", "OH", "OW", "IC"],
                 ["G", "OC", "N", "OH", "OW"]]

    dramKeep = [1, 1, 1, 1]

    # spmKeep = [1, 1, 1, 1]
    # spmKeep = [1, 1, 1, 0]
    spmKeep = [0, 1, 1, 0]
    # spmKeep = [0, 1, 0, 0]
    # spmKeep = [0, 0, 1, 0]

    # prob = ProblemConv([1024, 1024, 1024, 1, 1, 1, 1, 1], [1, 1])
    # tileShape = [256, 256, 256, 1, 1, 1, 1, 1]
    # spatialFactors = [1, 1, 1, 1, 1, 1, 1, 1]

    prob = ProblemConv([1, 256, 256, 56, 56, 3, 3, 1], [2, 2])
    tileShape = [1, 64, 64, 7, 7, 3, 3, 1]
    spatialFactors = [1, 1, 1, 1, 1, 1, 1, 1]

    target = Target()
    opt = Optimizer(prob, target, tileShape, spatialFactors, dramRepeat, spmRepeat, dramKeep, spmKeep,
                    showGurobiOutput=True, gurobiThreads=1)
    opt.setVarEqualToValue(0, "IC", 1)
    opt.setVarEqualToValue(0, "KH", 1)
    opt.setVarEqualToValue(0, "KW", 1)
    opt.run()
    assert opt.valid()
    opt.turnVarToValue()
    print(opt.resultsToString())


if __name__ == "__main__":
    test1()
