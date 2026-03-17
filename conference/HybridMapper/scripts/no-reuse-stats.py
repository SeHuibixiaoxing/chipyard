import math

from HybridMapper.models import models
from HybridMapper.HybridMapper import *
from HybridMapper.Model import *
import argparse
import os


modelNameList = [
    "resnet50",
    "mobilenetv2",
    "effinetb0",
    "vitb16",
    "pointpillars",
    "w2v2base",
    "bertbase",
    "gnmt",
]

memoryFootprintGap = 256 * 1024


def noReuseStats(modelName: str):
    outputRootPath = "output"

    model = models.create(modelName)
    model.generateMemoryMapping()
    print()
    print(model.getInfoString())
    print()

    modelMappingPath = os.path.join(outputRootPath, modelName)
    assert os.path.exists(modelMappingPath), modelMappingPath
    mappingList: list[MTMRecord] = []
    for i in range(model.getNumLayers()):
        mappingPath = os.path.join(modelMappingPath, "mapping", f"{i}.yaml")
        assert os.path.exists(mappingPath), mappingPath
        record = MTMRecord()
        record.read(mappingPath)
        mappingList.append(record)

    memoryFootprint = 0
    dataToReuseCountDict = dict()
    dataToReusePointDict = dict()

    def mul(s_: list):
        a_ = 1
        for i_ in s_:
            a_ *= i_
        return a_

    def addTo_dataToReuseCountDict(dataIdx_, count_):
        if dataIdx_ in dataToReuseCountDict:
            dataToReuseCountDict[dataIdx_] += count_
        else:
            dataToReuseCountDict[dataIdx_] = count_

    def addTo_dataToReusePointDict(dataIdx_, point_):
        if dataIdx_ in dataToReusePointDict:
            dataToReusePointDict[dataIdx_].append(point_)
        else:
            dataToReusePointDict[dataIdx_] = [point_]

    for i, layer in enumerate(model.layerList):
        record = mappingList[i]
        if isinstance(layer, LayerConv):
            # print("conv")
            factors = record.getFactors(0)[0]
            # print(factors)

            # bias
            dataIdx = layer.data[0]
            reuseCount = 1
            addTo_dataToReuseCountDict(dataIdx, reuseCount)
            # print(f"bias: {reuseCount}")

            # weight
            dataIdx = layer.data[1]
            if mul([factors[1], factors[5], factors[6]]) > 1:
                reuseCount = mul([factors[0], factors[3], factors[4]])
                addTo_dataToReuseCountDict(dataIdx, reuseCount)
            else:
                reuseCount = 1
                addTo_dataToReuseCountDict(dataIdx, reuseCount)
            # print(f"weight: {reuseCount}")

            # input
            dataIdx = layer.data[2]
            if mul([factors[3], factors[4], factors[0], factors[1]]) > 1:
                reuseCount = mul([factors[2]])
                addTo_dataToReuseCountDict(dataIdx, reuseCount)
            else:
                reuseCount = 1
                addTo_dataToReuseCountDict(dataIdx, reuseCount)
            # print(f"input: {reuseCount}")

            # output
            dataIdx = layer.data[3]
            reuseCount = 1
            addTo_dataToReuseCountDict(dataIdx, reuseCount)
            # print(f"output: {reuseCount}")

            for t, dataIdx in enumerate(layer.data):
                addTo_dataToReusePointDict(dataIdx, memoryFootprint)
            for t, dataIdx in enumerate(layer.data):
                memoryFootprint += model.dataSizeMap[dataIdx]

        elif isinstance(layer, LayerResadd):
            # print("resadd")
            for t, dataIdx in enumerate(layer.data):
                addTo_dataToReuseCountDict(dataIdx, 1)
            for t, dataIdx in enumerate(layer.data):
                addTo_dataToReusePointDict(dataIdx, memoryFootprint)
            for t, dataIdx in enumerate(layer.data):
                memoryFootprint += model.dataSizeMap[dataIdx]

        else:
            assert False

    print(dataToReuseCountDict)
    maxReusceCount = max(dataToReuseCountDict.values())
    print(maxReusceCount)

    histNum = [0 for _ in range(0, maxReusceCount)]
    histBytes = [0 for _ in range(0, maxReusceCount)]
    for key, value in dataToReuseCountDict.items():
        assert 1 <= value <= maxReusceCount
        histNum[value - 1] += 1
        assert key in model.dataSizeMap
        histBytes[value - 1] += model.dataSizeMap[key]
    print(histNum)
    print(histNum[0] / sum(histNum))
    print(histBytes)
    print(histBytes[0] / sum(histBytes))

    maxReuseDistance = 0
    dataToReuseDistanceDict = dict()
    for key, value in dataToReusePointDict.items():
        if len(value) > 1:
            distList = []
            for i in range(1, len(value)):
                d = value[i] - value[i - 1]
                assert d > 0
                distList.append(d)
                if d > maxReuseDistance:
                    maxReuseDistance = d
            dataToReuseDistanceDict[key] = distList
    print(memoryFootprint)
    print(dataToReuseDistanceDict)

    maxBars = math.ceil(maxReuseDistance / memoryFootprintGap)
    histReuseDistNum = [0 for _ in range(0, maxBars)]
    histReuseDistBytes = [0 for _ in range(0, maxBars)]
    for key, value in dataToReuseDistanceDict.items():
        for dist in value:
            barIdx = math.floor(dist / memoryFootprintGap)
            assert 0 <= barIdx < maxBars
            histReuseDistNum[barIdx] += 1
            histReuseDistBytes[barIdx] += model.dataSizeMap[key]

    print(histReuseDistNum)
    print(histReuseDistBytes)

    with open(f"output/no-reuse-stats/{modelName}", 'w') as file:
        file.writelines([f"{b}\n" for b in histBytes])
    with open(f"output/reuse-dist-stats/{modelName}", 'w') as file:
        file.writelines([f"{b}\n" for b in histReuseDistBytes])

    return histNum, histBytes, histReuseDistNum, histReuseDistBytes


def main():
    histNum = [0]
    histBytes = [0]
    histReuseDistNum = [0]
    histReuseDistBytes = [0]

    def update(hist_, i_, v_):
        if i_ < len(hist_):
            hist_[i_] += v_
        else:
            hist_.append(v_)

    for modelName in modelNameList:
        print(f"\n{modelName}\n")
        histNum_, histBytes_, histReuseDistNum_, histReuseDistBytes_ = noReuseStats(modelName)
        for i, v in enumerate(histNum_):
            update(histNum, i, v)
        for i, v in enumerate(histBytes_):
            update(histBytes, i, v)
        for i, v in enumerate(histReuseDistNum_):
            update(histReuseDistNum, i, v)
        for i, v in enumerate(histReuseDistBytes_):
            update(histReuseDistBytes, i, v)

    print()
    print(histNum)
    print(histNum[0] / sum(histNum))
    print(histBytes)
    print(histBytes[0] / sum(histBytes))

    print(histReuseDistNum)
    print(histReuseDistBytes)
    print(f">1MB: {sum(histReuseDistBytes[4: ]) / sum(histReuseDistBytes)}")
    print(f">4MB: {sum(histReuseDistBytes[16: ]) / sum(histReuseDistBytes)}")
    print(f">16MB: {sum(histReuseDistBytes[64: ]) / sum(histReuseDistBytes)}")

    with open("output/no-reuse-stats.txt", "w") as file:
        file.writelines([f"{b}\n" for b in histBytes])
    with open("output/reuse-dist-stats.txt", 'w') as file:
        file.writelines([f"{b}\n" for b in histReuseDistBytes])

    return 0


if __name__ == "__main__":
    main()
