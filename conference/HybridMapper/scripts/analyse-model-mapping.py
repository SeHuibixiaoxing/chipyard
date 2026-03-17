from HybridMapper.models import models
from HybridMapper.HybridMapper import *
import argparse
import os


def main():
    outputRootPath = "output"
    defaultModelName = "testmodel"

    parser = argparse.ArgumentParser()
    parser.add_argument("--model", type=str, required=False, default=defaultModelName)
    args = parser.parse_args()
    modelName = args.model

    model = models.create(modelName)
    model.generateMemoryMapping()
    print()
    print(model.getInfoString())

    print(f"\n{model.getLayersString()}\n")

    modelMappingPath = os.path.join(outputRootPath, modelName)
    assert os.path.exists(modelMappingPath), modelMappingPath
    mappingList: list[MTMRecord] = []
    for i in range(model.getNumLayers()):
        mappingPath = os.path.join(modelMappingPath, "mapping", f"{i}.yaml")
        assert os.path.exists(mappingPath), mappingPath
        record = MTMRecord()
        record.read(mappingPath)
        mappingList.append(record)

    avgM = model.getAvgLayerMemoryAccess()
    avgO = sum([layer.size[-1] for layer in model.layerList]) / model.getNumLayers()
    intermediateSum = 0

    maxDramBytes = 0
    minDramBytes = 0

    for i in range(model.getNumLayers()):
        layer = model.layerList[i]
        record = mappingList[i]
        numSpmAllocs = int(record.getNumCandidates() / 3)

        maxDramBytes += record.getDramAccess(0)
        minDramBytes += record.getDramAccess(numSpmAllocs - 1)

        markMA = ""
        if layer.getTotalDataSize() > avgM:
            markMA += "*"
        if layer.getTotalDataSize() > avgM * 1.5:
            markMA += "*"
        if layer.getTotalDataSize() > avgM * 2:
            markMA += "*"

        markO = ""
        if layer.size[-1] > avgO:
            markO += "*"
        if layer.size[-1] > avgO * 1.5:
            markO += "*"
        elif layer.size[-1] > avgO * 2:
            markO += "*"

        if layer.size[-1] < 8 * 1024 * 1024:
            intermediateSum += layer.size[-1]

        print(f"{i}: "
              f"C={layer.compute}, "
              f"M={layer.getTotalDataSize()}{markMA}, "
              f"CI={layer.getComputeIntensity():.2f}, "
              f"W={(layer.getNonInterDataSize() / layer.getTotalDataSize()):.2f}, "
              f"DRAM={[record.getDramAccess(i) for i in range(numSpmAllocs)]}, "
              f"O={layer.size[-1]}{markO}")

    print()
    print(f"inter-result size rate: {intermediateSum * 2 / model.getTotalMemoryAccess()}")
    print(f"DRAM Bytes: max={maxDramBytes}, min={minDramBytes}, save={(maxDramBytes - minDramBytes) / maxDramBytes}")


if __name__ == "__main__":
    main()
