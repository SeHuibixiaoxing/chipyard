import typing
import gurobipy as gu
from collections.abc import Iterable

from HybridMapper.ConMul import ConMul, Ceil
from HybridMapper.Problem import ProblemConv
from HybridMapper.Target import Target


class Optimizer:

    # dims
    PARAM_IDX_TO_NAME = ["N", "IC", "OC", "OH", "OW", "KH", "KW", "G"]
    PARAM_NAME_TO_IDX = {p: i for i, p in enumerate(PARAM_IDX_TO_NAME)}
    DIMS = PARAM_IDX_TO_NAME

    NUM_DIMS = len(DIMS)

    # [Bias, Weight, Input, Output]
    TENSOR_DIM = [["G", "OC"],
                  ["KH", "KW", "G", "IC", "OC"],
                  ["OH", "OW", "N", "G", "IC"],
                  ["OH", "OW", "N", "G", "OC"]]

    # levels
    I_DRAM = 0
    I_SPM = 1
    I_MA = 2
    I_ACC = 3
    NUM_LEVELS = 4  # [DRAM, SPM, MultiAccel, Accel]

    # tensors
    I_BIAS = 0
    I_WEIGHT = 1
    I_INPUT = 2
    I_OUTPUT = 3
    NUM_TENSORS = 4

    def __init__(self, prob: ProblemConv, target: Target,
                 tileShape: list[int], spatialFactors: list[int],
                 dramRepeat: list[list[str]], spmRepeat: list[list[str]],
                 dramKeep: list[int], spmKeep: list[int],
                 showGurobiOutput: bool = True, gurobiThreads: int = 1):
        self.prob = prob
        self.target = target
        self.tileShape = tileShape
        self.spatialFactors = spatialFactors

        assert len(dramRepeat) == self.NUM_TENSORS
        self.dramRepeat = dramRepeat
        assert len(spmRepeat) == self.NUM_TENSORS
        self.spmRepeat = spmRepeat

        assert len(dramKeep) == self.NUM_TENSORS
        self.dramKeep = dramKeep
        assert len(spmKeep) == self.NUM_TENSORS
        self.spmKeep = spmKeep

        # Tensor bytes [Bias, Weight, Input, Output]
        self.tensorBytes = [
            self.prob.getBiasWords() * self.target.psumWordBytes,
            self.prob.getWeightWords() * self.target.inputWordBytes,
            self.prob.getInputWords() * self.target.inputWordBytes,
            self.prob.getOutputWords() * self.target.inputWordBytes
        ]
        if target.enablePipeline:
            self.tensorBytes = [target.ceilPage(x) for x in self.tensorBytes]

        # Setup gurobi environment
        self.env = gu.Env(empty=True)
        self.env.setParam("OutputFlag", showGurobiOutput)
        self.env.setParam("Threads", gurobiThreads)
        self.env.start()
        self.m = gu.Model(env=self.env)
        self.cm = ConMul(self.m)
        self.m_ceil = Ceil(self.m)

        self.objVal = 0
        self.runtime = 0
        self.numSolutions = 0

        # Loop factor variables
        # [levelId][dimId] = level.dim.(factor or tilesize)
        # 前(NUM_LEVELS-2)*NUM_DIMS个变量是DRAM和SPM level的loop factor var
        # 后2*NUM_DIMS个变量是multi-accel level的loop factor，以及accel level的tile shape
        
        self.vars = ([[self.m.addVar(lb=1, ub=self.prob[d], vtype=gu.GRB.INTEGER) for d in range(self.NUM_DIMS)]
                      for _ in range(self.NUM_LEVELS - 2)] +
                     [self.spatialFactors, self.tileShape]) 

        # Compute accel utilization
        self.accelUtil = self.cm(self.getVar(self.I_MA, self.DIMS))

        # Accel-level tile bytes [Bias, Weight, Input, Output]
        self.accelTileBytes = [
            self.cm(self.getVar(self.I_ACC, self.TENSOR_DIM[self.I_BIAS]) +
                    [self.target.psumWordBytes]),
            self.cm(self.getVar(self.I_ACC, self.TENSOR_DIM[self.I_WEIGHT]) +
                    [self.target.inputWordBytes]),
            self.cm([(self.getVar(self.I_ACC, "OH")[0] - 1) * self.prob.strideH + self.prob.KH,
                     (self.getVar(self.I_ACC, "OW")[0] - 1) * self.prob.strideW + self.prob.KW] +
                    self.getVar(self.I_ACC, ["N", "IC", "G"]) +
                    [self.target.inputWordBytes]),
            self.cm(self.getVar(self.I_ACC, self.TENSOR_DIM[self.I_OUTPUT]) +
                    [self.target.inputWordBytes])
        ]

        # SPM-level tile dimensions
        self.spmTileDims = [self.cm(self.getVar([self.I_SPM, self.I_MA, self.I_ACC], d))
                            for d in range(self.NUM_DIMS)]

        # SPM-level tile bytes [Bias, Weight, Input, Output]
        self.spmTileBytes = [
            self.cm(self.get([self.spmTileDims], 0, self.TENSOR_DIM[self.I_BIAS]) +
                    [self.target.psumWordBytes]),
            self.cm(self.get([self.spmTileDims], 0, self.TENSOR_DIM[self.I_WEIGHT]) +
                    [self.target.inputWordBytes]),
            self.cm([(self.get([self.spmTileDims], 0, "OH")[0] - 1) * self.prob.strideH + self.prob.KH,
                     (self.get([self.spmTileDims], 0, "OW")[0] - 1) * self.prob.strideW + self.prob.KW] +
                    self.get([self.spmTileDims], 0, ["N", "IC", "G"]) +
                    [self.target.inputWordBytes]),
            self.cm(self.get([self.spmTileDims], 0, self.TENSOR_DIM[self.I_OUTPUT]) +
                    [self.target.inputWordBytes]),
        ]
        
        if target.enablePipeline:
            self.ceilSpmTensorPage = [self.m_ceil(x / float(target.spmPageSizeBytes)) for x in self.spmTileBytes]
            self.spmTileBytes = [x * target.spmPageSizeBytes for x in self.ceilSpmTensorPage]
        else:
            self.ceilSpmTensorPage = None

        # SPM utilization per tensor
        self.spmUtilBytes = [0 for _ in range(self.NUM_TENSORS)]
        for t in range(self.NUM_TENSORS):
            if self.spmKeep[t]:
                if self.dramKeep[t]:
                    self.spmUtilBytes[t] = self.spmTileBytes[t]
                else:
                    self.spmUtilBytes[t] = self.tensorBytes[t]

        # SPM utilization
        self.spmUtilBytesTotal = sum(self.spmUtilBytes)

        # DRAM access
        self.dramAccessBytes = [0, 0, 0, 0]
        for t in range(self.NUM_TENSORS):
            if self.spmKeep[t]:
                if self.dramKeep[t]:
                    self.dramAccessBytes[t] = self.cm(self.getVar(self.I_DRAM, self.dramRepeat[t]) +
                                                      self.getVar(self.I_SPM, self.TENSOR_DIM[t]) +
                                                      self.getVar(self.I_MA, self.TENSOR_DIM[t]) +
                                                      [self.accelTileBytes[t]])
                else:
                    self.dramAccessBytes[t] = 0
            else:
                if self.dramKeep[t]:
                    self.dramAccessBytes[t] = self.cm(self.getVar(self.I_DRAM, self.DIMS) +
                                                      self.getVar(self.I_SPM, self.spmRepeat[t]) +
                                                      self.getVar(self.I_MA, self.TENSOR_DIM[t]) +
                                                      [self.accelTileBytes[t]])
                else:
                    assert False
        self.dramAccessBytesTotal = sum(self.dramAccessBytes)
        
        # spm access？

        # set constaint: dimensions
        for d in range(self.NUM_DIMS):
            self.m.addConstr(self.spmTileDims[d] * self.vars[self.I_DRAM][d] == self.prob[int(d)])

        # set constaint: SPM capacity
        self.m.addConstr(self.spmUtilBytesTotal <= self.target.spmBytes)

        # set objective
        self.m.setObjective(self.dramAccessBytesTotal, gu.GRB.MINIMIZE)

    def run(self):
        self.m.update()
        self.m.optimize()
        if self.m.Status != gu.GRB.OPTIMAL:
            # print(f"Optimizer: gurobi model status is {str(self.m.Status)} instead of OPTIMAL.")
            pass
        else:
            self.objVal = int(self.m.ObjVal)
            self.runtime = self.m.Runtime
            self.numSolutions = self.m.SolCount
            self.turnVarToValue()

    
    def get(self, table: list[list],
            levelIdx: typing.Union[int, Iterable[int]], dim: typing.Union[int, str, Iterable]) -> list:
        """
        Get values from table.

        Args:
        - table (list[list]): 2D table.
        - levelIdx (int or Iterable[int]): Row index/indices in the table.
        - dim (int or str or Iterable): Column index/indices in the table. If str, convert to int using PARAM_NAME_TO_IDX.

        Returns:
        - list: The values in the table at the given row and column indices.
        """
        levelIdx_ = levelIdx
        if isinstance(levelIdx_, int):
            levelIdx_ = [levelIdx_]

        dim_ = dim
        if isinstance(dim_, int) or isinstance(dim_, str):
            dim_ = [dim_]

        for i in range(len(dim_)):
            if isinstance(dim_[i], str):
                assert dim_[i] in self.PARAM_NAME_TO_IDX
                dim_[i] = self.PARAM_NAME_TO_IDX[dim_[i]]

        t = []
        for rowIdx in levelIdx_:
            assert 0 <= rowIdx < len(table)
            for dimIdx in dim_:
                assert 0 <= dimIdx < len(table[rowIdx])
                t.append(table[rowIdx][dimIdx])
        return t

    def getVar(self, levelIdx: typing.Union[int, Iterable[int]], dim: typing.Union[int, str, Iterable]) -> list:
        return self.get(self.vars, levelIdx, dim)

    def setVarEqualToValue(self, levelIdx: int, dim: int | str, value: int):
        self.m.addConstr(self.getVar(levelIdx, dim)[0] == value)

    def turnVarToValue(self):
        self.vars = [[self.getValue(v) for v in varList] for varList in self.vars]
        self.spmTileDims = [self.getValue(v) for v in self.spmTileDims]
        self.spmTileBytes = [self.getValue(v) for v in self.spmTileBytes]
        self.accelTileBytes = [self.getValue(v) for v in self.accelTileBytes]
        self.spmUtilBytes = [self.getValue(v) for v in self.spmUtilBytes]
        self.spmUtilBytesTotal = self.getValue(self.spmUtilBytesTotal)
        self.dramAccessBytes = [self.getValue(v) for v in self.dramAccessBytes]
        self.dramAccessBytesTotal = self.getValue(self.dramAccessBytesTotal)
        self.accelUtil = self.getValue(self.accelUtil)

    def resultsToString(self):
        return (f"numSolutions={self.numSolutions}\n"
                f"vars={self.vars}\n"
                f"spmTileDims={self.spmTileDims}\n"
                f"tensorBytes={self.tensorBytes}\n"
                f"spmTileBytes={self.spmTileBytes}\n"
                f"accelTileBytes={self.accelTileBytes}\n"
                f"spmUtilBytes={self.spmUtilBytes}\n"
                f"spmUtilBytesTotal={self.spmUtilBytesTotal}\n"
                f"dramAccessBytes={self.dramAccessBytes}\n"
                f"dramAccessBytesTotal={self.dramAccessBytesTotal}\n"
                f"accelUtil={self.accelUtil}\n"
                )

    def valid(self):
        return self.numSolutions > 0

    @staticmethod
    def getValue(v) -> int:
        if isinstance(v, int) or isinstance(v, float):
            return int(v)
        else:
            return int((v + 0).getValue())

    def isBetterThan(self, opt):
        assert isinstance(opt, Optimizer)
        return ((self.dramAccessBytesTotal < opt.dramAccessBytesTotal) or
                (self.dramAccessBytesTotal == opt.dramAccessBytesTotal and
                 self.spmUtilBytesTotal < opt.spmUtilBytesTotal))
