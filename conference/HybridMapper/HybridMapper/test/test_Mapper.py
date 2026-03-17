from HybridMapper.Mapper import *


if __name__ == "__main__":
    # prob = ProblemConv()
    prob = ProblemResadd()

    # ResNet

    # prob.setDimList([1, 3, 64, 112, 112, 7, 7, 1])
    # prob.setStride(2, 2)

    # prob.setDimList([1, 128, 128, 56, 56, 3, 3, 1])
    # prob.setStride(2, 2)

    # prob.setDimList([1, 1024, 1024, 7, 7, 3, 3, 1])
    # prob.setStride(2, 2)

    # prob.setDimList([1, 64, 128, 112, 112, 1, 1, 1])
    # prob.setStride(1, 1)

    # prob.setDimList([1, 512, 1024, 14, 14, 1, 1, 1])
    # prob.setStride(1, 1)

    # prob.setDimList([1, 4096, 1000, 1, 1, 1, 1, 1])
    # prob.setStride(1, 1)

    # prob.setDimList([1, 1024, 14, 14, 1])

    prob.setDimList([1, 128, 56, 56, 1])

    # DW-conv

    # prob.setDimList([1, 1, 1, 112, 112, 7, 7, 64])
    # prob.setStride(1, 1)

    # prob.setDimList([1, 1, 1, 28, 28, 3, 3, 256])
    # prob.setStride(1, 1)

    # prob.setDimList([1, 1, 1, 7, 7, 3, 3, 1024])
    # prob.setStride(1, 1)

    # ViT

    # prob.setDimList([1, 3, 64, 14, 14, 16, 16, 1])
    # prob.setStride(16, 16)

    # prob.setDimList([50, 768, 2304, 1, 1, 1, 1, 1])
    # prob.setStride(1, 1)

    # prob.setDimList([50, 3072, 768, 1, 1, 1, 1, 1])
    # prob.setStride(1, 1)

    # prob.setDimList([50, 64, 50, 1, 1, 1, 1, 12])  # multi-head
    # prob.setStride(1, 1)

    # target = Target(1, 0, isCache=True)
    # target = Target(1, 0)
    # target = Target(1, 1024)
    # target = Target(1, 2048)
    # target = Target(1, 4096)
    # target = Target(2, 1024)
    target = Target(4, 1024)

    dramKeep = [1, 1, 1, 1]
    # dramKeep = [1, 1, 1, 0]
    # dramKeep = [1, 1, 0, 0]
    # dramKeep = [1, 1, 0, 1]

    mapper = getMapper(prob, target, dramKeep, showGurobiOutput=False)
    # mapper.tileAndDistribute()
    mapper.run()

    if isinstance(mapper, MapperConv):
        print(mapper.tileShape, mapper.spatialFactors)
        print(mapper.optIndices)
        print(mapper.opt.resultsToString())
        print()
        for subOpt, indices in mapper.subOptList:
            if subOpt.valid():
                mark = "*" if subOpt == mapper.opt else ""
                print(f"{indices}: objVal={subOpt.objVal} {mark}")

    elif isinstance(mapper, MapperResadd):
        print(mapper.tileShape)
        print(mapper.spmFactors, mapper.spatialFactors)
        print()
