import os
import pickle
from HybridMapper.CustomYamlDumper import *
from HybridMapper.Mapper import Mapper


class LayerMappingCandidate:
    class KEY:
        mappingForPipeline = "mappingForPipeline"
        
        index = "index"

        target = "target"
        accel = "accel"
        spm = "spm"

        mapping = "mapping"
        tile = "tile"
        factors = "factors"
        permutation = "permutation"
        dramBypass = "dramBypass"
        spmBypass = "spmBypass"

        performance = "performance"
        dramAccess = "dramAccess"
        accelUtil = "accelUtil"
        spmUtil = "spmUtil"
        spmPageUtil = "spmPageUtil"

        layerGroup = "layerGroup"
        lgSpmUtil = "lgSpmUtil"
        lgAccelUtil = "lgAccelUtil"

        others = "others"
        spmTensorAddr = "spmTensorAddr"
        spmTensorUtil = "spmTensorUtil"
        spmTensorPageCount = "spmTensorPageCount"
        firstTensorPageNum = "firstTensorPageNum"
        spmDimensions = "spmDimensions"
        spmTensorTileCount = "spmTensorTileCount"
        psumAccumCount = "psumAccumCount"
        tensorMulticastCount = "tensorMulticastCount"
        totalTiles = "totalTiles"

    def __init__(self):
        self.node = dict({
            self.KEY.index: 0,
            self.KEY.mappingForPipeline: 0,
            self.KEY.target: dict(),
            self.KEY.mapping: dict(),
            self.KEY.performance: dict(),
            self.KEY.layerGroup: dict(),
            self.KEY.others: dict(),
        })
        
        self.targetNode = self.node[self.KEY.target]
        self.mappingNode = self.node[self.KEY.mapping]
        self.perfNode = self.node[self.KEY.performance]
        self.layerGroupNode = self.node[self.KEY.layerGroup]
        self.othersNode = self.node[self.KEY.others]

    def setNode(self, node: dict):
        assert self.KEY.target in node
        assert self.KEY.mapping in node
        assert self.KEY.performance in node
        assert self.KEY.layerGroup in node
        assert self.KEY.others in node
        self.node = node
        self.targetNode = self.node[self.KEY.target]
        self.mappingNode = self.node[self.KEY.mapping]
        self.perfNode = self.node[self.KEY.performance]
        self.layerGroupNode = self.node[self.KEY.layerGroup]
        self.othersNode = self.node[self.KEY.others]

    def set(self, mapper: Mapper, index=0):
        self.node[self.KEY.index] = index
        self.node[self.KEY.mappingForPipeline] = 1 if mapper.mappingForPipeline else 0
        self.targetNode[self.KEY.accel] = mapper.target.numArrays
        self.targetNode[self.KEY.spm] = int(mapper.target.spmBytes / 1024)
        self.mappingNode[self.KEY.tile] = mapper.tileShape
        self.mappingNode[self.KEY.factors] = [mapper.dramFactors, mapper.spmFactors, mapper.spatialFactors]
        self.mappingNode[self.KEY.permutation] = [mapper.dramPerm, mapper.spmPerm, mapper.spatialPerm]
        self.mappingNode[self.KEY.dramBypass] = [1 - i for i in mapper.dramKeep]
        self.mappingNode[self.KEY.spmBypass] = [1 - i for i in mapper.spmKeep]
        self.perfNode[self.KEY.dramAccess] = mapper.dramAccessBytes
        self.perfNode[self.KEY.spmUtil] = mapper.spmUtilBytes
        self.perfNode[self.KEY.spmPageUtil] = mapper.spmPageUtil
        self.perfNode[self.KEY.accelUtil] = mapper.accelUtil
        self.layerGroupNode[self.KEY.lgSpmUtil] = 0  # init to zero
        self.layerGroupNode[self.KEY.lgAccelUtil] = 0  # init to zero
        self.othersNode[self.KEY.spmTensorAddr] = [sum(mapper.spmTensorUtil[: i])
                                                   for i in range(len(mapper.spmTensorUtil))]
        self.othersNode[self.KEY.spmTensorUtil] = mapper.spmTensorUtil
        self.othersNode[self.KEY.spmTensorPageCount] = [mapper.target.getPageNum(x) for x in mapper.spmTensorUtil]
        self.othersNode[self.KEY.firstTensorPageNum] = [mapper.target.getPageNum(sum(mapper.spmTensorUtil[: i]))
                                                        for i in range(len(mapper.spmTensorUtil))] if mapper.target.enablePipeline else []
        self.othersNode[self.KEY.spmDimensions] = mapper.spmDimensions
        self.othersNode[self.KEY.spmTensorTileCount] = mapper.spmTensorTileCount
        self.othersNode[self.KEY.psumAccumCount] = mapper.psumAccumCount
        self.othersNode[self.KEY.tensorMulticastCount] = mapper.tensorMulticastCount
        self.othersNode[self.KEY.totalTiles] = mapper.totalTiles

    def getDramAccess(self) -> int:
        assert self.KEY.dramAccess in self.perfNode
        return self.perfNode[self.KEY.dramAccess]

    def getSpmUtil(self) -> int:
        assert self.KEY.spmUtil in self.perfNode
        return self.perfNode[self.KEY.spmUtil]

    def setSpmUtil(self, value):
        assert self.KEY.spmUtil in self.perfNode
        self.perfNode[self.KEY.spmUtil] = value

    def getSpmPageUtil(self):
        assert self.KEY.spmPageUtil in self.perfNode
        return self.perfNode[self.KEY.spmPageUtil]
    
    def setSpmPageUtil(self, value):
        assert self.KEY.spmPageUtil in self.perfNode
        self.perfNode[self.KEY.spmPageUtil] = value
    
    def getAccelUtil(self) -> int:
        assert self.KEY.accelUtil in self.perfNode
        return self.perfNode[self.KEY.accelUtil]

    def getFactors(self) -> list[list[int]]:
        assert self.KEY.factors in self.mappingNode
        return self.mappingNode[self.KEY.factors]

    def setLgSpmUtil(self, value):
        assert self.KEY.lgSpmUtil in self.layerGroupNode
        self.layerGroupNode[self.KEY.lgSpmUtil] = value

    def setLgAccelUtil(self, value):
        assert self.KEY.lgAccelUtil in self.layerGroupNode
        self.layerGroupNode[self.KEY.lgAccelUtil] = value

    def getSpmTensorUtil(self) -> list[int]:
        assert self.KEY.spmTensorUtil in self.othersNode
        return self.othersNode[self.KEY.spmTensorUtil]

    def setSpmTensorUtil(self, value: list[int]):
        assert self.KEY.spmTensorUtil in self.othersNode
        self.othersNode[self.KEY.spmTensorUtil] = value

    def getSpmTensorPageCount(self):
        assert self.KEY.spmTensorPageCount in self.othersNode
        return self.othersNode[self.KEY.spmTensorPageCount]
    
    def setSpmTensorPageCount(self, value: list[int]):
        assert self.KEY.spmTensorPageCount in self.othersNode
        self.othersNode[self.KEY.spmTensorPageCount] = value

    def getFirstTensorPageNum(self):
        assert self.KEY.firstTensorPageNum in self.othersNode
        return self.othersNode[self.KEY.firstTensorPageNum]

    def setFirstTensorPageNum(self, value: list[int]):
        assert self.KEY.firstTensorPageNum in self.othersNode
        self.othersNode[self.KEY.firstTensorPageNum] = value
    
    def getSpmTensorAddr(self) -> list[int]:
        assert self.KEY.spmTensorAddr in self.othersNode
        return self.othersNode[self.KEY.spmTensorAddr]

    def setSpmTensorAddr(self, value: list[int]):
        assert self.KEY.spmTensorAddr in self.othersNode
        self.othersNode[self.KEY.spmTensorAddr] = value


class MTMRecord:
    class MapKey:
        def __init__(self, dram_bypass: list[int], spm_bypass: list[int], acc_num: int):
            self.dram_bypass = tuple(dram_bypass)  # 转换为元组，使其可哈希
            self.spm_bypass = tuple(spm_bypass)    # 转换为元组，使其可哈希
            self.acc_num = acc_num
        
        def __hash__(self):
            """基于所有属性计算哈希值"""
            return hash((self.dram_bypass, self.spm_bypass, self.acc_num))
        
        def __eq__(self, other):
            """定义相等性比较"""
            if not isinstance(other, MTMRecord.MapKey):
                return False
            return (self.dram_bypass == other.dram_bypass and 
                    self.spm_bypass == other.spm_bypass and 
                    self.acc_num == other.acc_num)
        
        def __repr__(self):
            """便于调试的字符串表示"""
            return f"MapKey(dram_bypass={self.dram_bypass}, spm_bypass={self.spm_bypass}, acc_num={self.acc_num})"
    
    class KEY:
        candidates = "candidates"
        pipeline = "pipeline"

    def __init__(self, pipeline=False):
        self.node = dict({self.KEY.candidates: [], self.KEY.pipeline: pipeline})
        self.candidateList: list[LayerMappingCandidate] = []
        self._map_has_created = False
        self._map_dict: dict[MTMRecord.MapKey, LayerMappingCandidate] = None

    def set(self, mapperList: list[Mapper]):
        for index, mapper in enumerate(mapperList):
            if mapper.success:
                can = LayerMappingCandidate()
                can.set(mapper, index)
                self.candidateList.append(can)
                self.node[self.KEY.candidates].append(can.node)
            else:
                assert False

    def write(self, path: str):
        assert os.path.exists(os.path.dirname(os.path.normpath(path)))
        with open(path, "w") as file:
            yaml.dump(self.node, file, Dumper=CustomYamlDumper)

    def read(self, path: str):
        assert os.path.exists(path)
        with open(path, "r") as file:
            self.node = yaml.safe_load(file)
        assert self.KEY.candidates in self.node
        for canNode in self.node[self.KEY.candidates]:
            can = LayerMappingCandidate()
            can.setNode(canNode)
            self.candidateList.append(can)

    def getNumCandidates(self) -> int:
        return len(self.candidateList)

    def getDramAccess(self, canIdx) -> int:
        return self.candidateList[canIdx].getDramAccess()

    def getSpmUtil(self, canIdx) -> int:
        return self.candidateList[canIdx].getSpmUtil()

    def setSpmUtil(self, canIdx, value):
        return self.candidateList[canIdx].setSpmUtil(value)

    def getAccelUtil(self, canIdx) -> int:
        return self.candidateList[canIdx].getAccelUtil()

    def getFactors(self, canIdx) -> list[list[int]]:
        return self.candidateList[canIdx].getFactors()

    def setLgSpmUtil(self, canIdx, value):
        self.candidateList[canIdx].setLgSpmUtil(value)

    def setLgAccelUtil(self, canIdx, value):
        self.candidateList[canIdx].setLgAccelUtil(value)

    def getSpmTensorUtil(self, canIdx) -> list[int]:
        return self.candidateList[canIdx].getSpmTensorUtil()

    def setSpmTensorUtil(self, canIdx, value: list[int]):
        self.candidateList[canIdx].setSpmTensorUtil(value)

    def getSpmTensorAddr(self, canIdx) -> list[int]:
        return self.candidateList[canIdx].getSpmTensorAddr()

    def setSpmTensorAddr(self, canIdx, value: list[int]):
        self.candidateList[canIdx].setSpmTensorAddr(value)

    def _generate_map(self):
        self._map_dict = {}
        for candidate in self.candidateList:
            key: MTMRecord.MapKey = MTMRecord.MapKey(
                dram_bypass=candidate.mappingNode[LayerMappingCandidate.KEY.dramBypass],
                spm_bypass=candidate.mappingNode[LayerMappingCandidate.KEY.spmBypass],
                acc_num=candidate.targetNode[LayerMappingCandidate.KEY.accel],
            )
            assert(self._map_dict.get(key, None) == None)
            self._map_dict[key] = candidate
        self._map_has_created = True
    
    def getMap(self, key: MapKey) -> LayerMappingCandidate | None:
        if not self._map_has_created:
            self._generate_map()
        
        return self._map_dict.get(key, None)
        
    def dump(self, path: str):
        with open(path, "wb") as f:
            pickle.dump(self, f)
        
    
