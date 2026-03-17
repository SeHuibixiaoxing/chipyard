from HybridMapper.MTMRecord import MTMRecord
from HybridMapper.Model import Model
from HybridMapper.Target import Target
import os


class _Tensor:
    def __init__(self, dataId, spmUtil, firstLayerIdx, lastLayerIdx):
        self.dataId = dataId
        self.spmUtil = spmUtil
        self.firstLayerIdx = firstLayerIdx
        self.lastLayerIdx = lastLayerIdx

    def getLifetime(self):
        return self.lastLayerIdx - self.firstLayerIdx + 1


class _SPMSpace:
    class _Segment:
        def __init__(self, dataId, addr, size):
            self.dataId = dataId
            self.addr = addr
            self.size = size

    def __init__(self):
        self.segments: list[_SPMSpace._Segment] = []

    def __str__(self):
        return "[" + " ".join([f"({seg.addr},{seg.size},{seg.dataId})" for seg in self.segments]) + "]"

    def getDataId(self, segId: int):
        return self.segments[segId].dataId

    def getAddr(self, segId: int):
        return self.segments[segId].addr

    def getSize(self, segId: int):
        return self.segments[segId].size

    def getSizeTotal(self):
        if len(self.segments) == 0:
            return 0
        return self.getAddr(len(self.segments) - 1) + self.getSize(len(self.segments) - 1)

    def hasDataId(self, dataId: int) -> bool:
        return any([s.dataId == dataId for s in self.segments])

    def getSegIdByDataId(self, dataId: int) -> int:
        for segId, seg in enumerate(self.segments):
            if seg.dataId == dataId:
                return segId
        assert False

    def getAvailableAddr(self, size: int) -> int:
        floorAddr = 0
        for segId in range(len(self.segments)):
            ceilAddr = self.getAddr(segId)
            assert ceilAddr >= floorAddr
            if ceilAddr - floorAddr >= size:
                return floorAddr
            floorAddr = self.getAddr(segId) + self.getSize(segId)
        return floorAddr

    def addDataIdToAddr(self, dataId: int, size: int, addr: int):
        floorAddr = 0
        for segId in range(len(self.segments)):
            ceilAddr = self.getAddr(segId)
            assert ceilAddr >= floorAddr
            if ceilAddr >= addr + size:
                assert floorAddr <= addr
                self.segments.insert(segId, self._Segment(dataId, addr, size))
                return
            floorAddr = self.getAddr(segId) + self.getSize(segId)
        assert floorAddr <= addr
        self.segments.append(self._Segment(dataId, addr, size))


class PostProcess:
    def __init__(self, model: Model, targetList: list[Target]):
        self.model = model
        self.targetList = targetList
        self.mappingList: list[MTMRecord] = []

    def read(self, outputFolder: str):
        for layerIdx in range(self.model.getNumLayers()):
            mappingPath = os.path.join(outputFolder, "mapping", f"{layerIdx}.yaml")
            assert os.path.exists(mappingPath), mappingPath
            record = MTMRecord()
            record.read(mappingPath)
            self.mappingList.append(record)

    def write(self, outputFolder: str):
        for layerIdx in range(self.model.getNumLayers()):
            mappingPath = os.path.join(outputFolder, "mapping", f"{layerIdx}.yaml")
            self.mappingList[layerIdx].write(mappingPath)

    def resetLayerGroupAccelUtil(self):
        """
        The method resets the "lgAccelUtil" values to the maximum value in each layer group.
        In a layer group/block, different layers may utilize different numbers of accelerators.
        Reset only happens for layer-block mapping candidates.
        """
        for targetIdx, target in enumerate(self.targetList):
            # Only layer-block mapping candidates need the following process
            if not target.enableILR:
                continue
            print(f"PostProcess: targetIdx={targetIdx}")
            # Variables
            lgHeadLayerIdx = 0
            lgAccelUtil = 0
            for layerIdx, layer in enumerate(self.model.layerList):
                # Ignore single layers
                if layer.layerGroupType == layer.LG_TYPE_NONE:
                    continue
                # If it's a head layer, reset all variables
                if layer.layerGroupType == layer.LG_TYPE_HEAD:
                    lgHeadLayerIdx = layerIdx
                    lgAccelUtil = 0
                # Update lgAccelUtil
                lgAccelUtil = max(lgAccelUtil, self.mappingList[layerIdx].getAccelUtil(targetIdx))
                # If it's a tail layer, set lgAccelUtil for all layers in the block
                if layer.layerGroupType == layer.LG_TYPE_TAIL:
                    print(f"PostProcess: Set lgAccelUtil to {lgAccelUtil} "
                          f"for layer {lgHeadLayerIdx}-{layerIdx}")
                    for j in range(lgHeadLayerIdx, layerIdx + 1):
                        self.mappingList[j].setLgAccelUtil(targetIdx, lgAccelUtil)

    def resetLayerGroupSPMTensorAddr(self):
        """
        The method do tensor mapping in SPM and resets "spmTensorAddr", "spmTensorUtil", "spmUtil" and "lgSpmUtil".
        In a layer group/block, some intermediate tensors have lifetimes longer than one layer for interlayer reuse.
        Reset only happens for layer-block mapping candidates.
        """
        for targetIdx, target in enumerate(self.targetList):
            # Only layer-block mapping candidates need the following process
            if not target.enableILR:
                continue
            print(f"PostProcess: targetIdx={targetIdx}")
            # Varibales
            lgHeadLayerIdx = 0
            lgTailLayerIdx = 0
            spmTensorMap: map[int: _Tensor] = dict()  # DataId -> _Tensor
            spmSpaceTable: list[_SPMSpace] = [_SPMSpace() for _ in range(self.model.getNumLayers())]

            for layerIdx, layer in enumerate(self.model.layerList):
                # Ignore single layers
                if layer.layerGroupType == layer.LG_TYPE_NONE:
                    continue
                # If it's a head layer, reset all variables
                if layer.layerGroupType == layer.LG_TYPE_HEAD:
                    lgHeadLayerIdx = layerIdx
                    lgTailLayerIdx = layerIdx
                    spmTensorMap.clear()
                assert spmSpaceTable[layerIdx].getSizeTotal() == 0

                # Add/update tensor record in spmTensorMap
                for tensorId, dataId in enumerate(layer.data):
                    if not layer.dramKeep[tensorId]:
                        # It's an SPM tensor
                        if dataId in spmTensorMap:
                            # Update the tensor record in spmTensorMap
                            spmTensorMap[dataId].spmUtil = max(
                                spmTensorMap[dataId].spmUtil,
                                self.mappingList[layerIdx].getSpmTensorUtil(targetIdx)[tensorId])
                            assert layerIdx > spmTensorMap[dataId].lastLayerIdx
                            spmTensorMap[dataId].lastLayerIdx = layerIdx
                        else:
                            # Add a new tensor record in spmTensorMap
                            spmTensorMap[dataId] = _Tensor(
                                dataId, self.mappingList[layerIdx].getSpmTensorUtil(targetIdx)[tensorId],
                                layerIdx, layerIdx)

                # If it's the last layer, generate SPM mapping and update values for this layer block
                if layer.layerGroupType == layer.LG_TYPE_TAIL:
                    lgTailLayerIdx = layerIdx
                    # Map each tensor with a specific SPM address.
                    for curLayerIdx in range(lgHeadLayerIdx, lgTailLayerIdx + 1):
                        curLayer = self.model.layerList[curLayerIdx]
                        curSpmSpace = spmSpaceTable[curLayerIdx]
                        # Sort layer-crossing tensors of this layer by lifetime
                        tensorList: list[tuple[int, int]] = []  # (DataId, Size)
                        for tensorId, dataId in enumerate(curLayer.data):
                            if not curLayer.dramKeep[tensorId]:
                                assert dataId in spmTensorMap
                                tensorList.append((dataId, spmTensorMap[dataId].spmUtil))
                        tensorList.sort(key=lambda t: spmTensorMap[t[0]].getLifetime())
                        tensorList.reverse()
                        for tensorId, dataId in enumerate(curLayer.data):
                            if curLayer.dramKeep[tensorId]:
                                assert dataId not in spmTensorMap
                                tensorList.append(
                                    (dataId, self.mappingList[curLayerIdx].getSpmTensorUtil(targetIdx)[tensorId]))

                        # Generate SPM tensor address
                        for dataId, size in tensorList:
                            if not curSpmSpace.hasDataId(dataId):
                                addr = curSpmSpace.getAvailableAddr(size)
                                firstLayerIdx = lastLayerIdx = curLayerIdx
                                if dataId in spmTensorMap:
                                    assert firstLayerIdx == spmTensorMap[dataId].firstLayerIdx
                                    lastLayerIdx = spmTensorMap[dataId].lastLayerIdx
                                # print(f"PostProcess: map tensor(dataId={dataId},util={size},"
                                #       f"layer=({firstLayerIdx}-{lastLayerIdx})) at {addr} "
                                #       f"for layer {curLayerIdx}")
                                for k in range(firstLayerIdx, lastLayerIdx + 1):
                                    spmSpaceTable[k].addDataIdToAddr(dataId, size, addr)
                        print(f"PostProcess: SPM mapping: layer={curLayerIdx}, map={str(curSpmSpace)}")

                    # Update tensor addresses in mapping results
                    for curLayerIdx in range(lgHeadLayerIdx, lgTailLayerIdx + 1):
                        curLayer = self.model.layerList[curLayerIdx]
                        curSpmSpace = spmSpaceTable[curLayerIdx]
                        spmUtil = curSpmSpace.getSizeTotal()
                        addrList = [curSpmSpace.getAddr(curSpmSpace.getSegIdByDataId(dataId))
                                    for dataId in curLayer.data]
                        utilList = [curSpmSpace.getSize(curSpmSpace.getSegIdByDataId(dataId))
                                    for dataId in curLayer.data]
                        assert spmUtil >= sum(utilList)
                        print(f"PostProcess: reset spmUtil from {self.mappingList[curLayerIdx].getSpmUtil(targetIdx)} "
                              f"to {spmUtil} for layer {curLayerIdx}")
                        self.mappingList[curLayerIdx].setSpmUtil(targetIdx, spmUtil)
                        print(f"PostProcess: reset spmTensorAddr from "
                              f"{self.mappingList[curLayerIdx].getSpmTensorAddr(targetIdx)} "
                              f"to {addrList} for layer {curLayerIdx}")
                        self.mappingList[curLayerIdx].setSpmTensorAddr(targetIdx, addrList)
                        print(f"PostProcess: reset spmTensorUtil from "
                              f"{self.mappingList[curLayerIdx].getSpmTensorUtil(targetIdx)} "
                              f"to {utilList} for layer {curLayerIdx}")
                        self.mappingList[curLayerIdx].setSpmTensorUtil(targetIdx, utilList)

                    # Update lgSpmUtil
                    lgSpmUtil = max([self.mappingList[curLayerIdx].getSpmUtil(targetIdx)
                                     for curLayerIdx in range(lgHeadLayerIdx, lgTailLayerIdx + 1)])
                    print(f"PostProcess: set lgSpmUtil to {lgSpmUtil} for layer {lgHeadLayerIdx}-{lgTailLayerIdx}")
                    for curLayerIdx in range(lgHeadLayerIdx, lgTailLayerIdx + 1):
                        self.mappingList[curLayerIdx].setLgSpmUtil(targetIdx, lgSpmUtil)