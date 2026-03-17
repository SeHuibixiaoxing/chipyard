import math
import functools
from HybridMapper.Problem import Problem, ProblemConv, ProblemResadd
from HybridMapper.Target import Target
from HybridMapper.Optimizer import Optimizer


def getMapper(prob: Problem, target: Target, dramKeep: list[int] = None, showGurobiOutput: bool = True, spmKeep: list[int] = None):
    if isinstance(prob, ProblemConv):
        return MapperConv(prob, target, dramKeep, showGurobiOutput, spmKeep)
    elif isinstance(prob, ProblemResadd):
        return MapperResadd(prob, target, dramKeep, showGurobiOutput, spmKeep)
    else:
        assert False


class Mapper:
    """
    Attributes:
        target: Target hardware configuration.
        mappingForPipeline: 是否采用pipeline。如果采用，spm地址应该与页面对齐，并计算各tensor需要的页面数等数据
        showGurobiOutput: Show Gurobi output during execution.
        tileShape: Shape of accel-level tile. [D0, D1, D2, ...]
        dramFactors: DRAM-level loop factors. [D0, D1, D2, ...]
        spmFactors: SPM-level loop factors. [D0, D1, D2, ...]
        spatialFactors: Multi-accel-level loop factors. [D0, D1, D2, ...]. \
                        Element-wise production of tileShape, dramFactors, spmFactors and spatialFactors should equal \
                        to the layer dimensions.
        dramPerm: DRAM-level loop permutation (from outer to inner). [2, 1, 0, ...]
        spmPerm: SPM-level loop permutaion (from outer to inner). [2, 1, 0, ...]
        spatialPerm: Multi-accel-level loop permutation (from outer to inner). [2, 1, 0, ...]
        dramKeep: If a tensor is stored in DRAM. [1, 1, 1, 0]
        spmKeep: If a tensor is stored in SPM. [1, 1, 1, 0]
        dramAccessBytes: Total DRAM access in bytes of the mapping result.
        spmUtilBytes: Total SPM utilization of the mapping result.
        spmPageUtil 以页为单位的spm使用
        accelUtil: Total number of utilized accels of the mapping result.
        spmTensorUtil: SPM space allocated to each tensor. [1024, 1024, 2048, ...]
        spmDimensions: SPM-level tile dimensions. [D0, D1, D2, ...]
        spmTensorTileCount: The number of tiles that stored in SPM for each tensor. [2, 2, 1, ...]
        psumAccumCount: The number of tiles that accumulation dimensions are divided into.
        tensorMulticastCount: The number of multicast targets of each tensor. [1, 1, 2, ...]
        totalTiles: The number of tiles.
    """

    def __init__(self, target: Target, showGurobiOutput: bool = True):
        self.target = target
        self.mappingForPipeline = target.enablePipeline
        self.showGurobiOutput = showGurobiOutput
        # Tiling factors.
        self.tileShape = None
        self.dramFactors = None
        self.spmFactors = None
        self.spatialFactors = None
        # Loop permutations.
        self.dramPerm = None
        self.spmPerm = None
        self.spatialPerm = None
        # Tensor storage.
        self.dramKeep = None
        self.spmKeep = None
        # Performance of the mapping result.
        self.dramAccessBytes = 0
        self.spmUtilBytes = 0
        self.spmPageUtil = 0 
        self.accelUtil = 0
        # Others
        self.spmTensorUtil = None
        self.spmDimensions = None
        self.spmTensorTileCount = None
        self.psumAccumCount = 0
        self.tensorMulticastCount = None
        self.totalTiles = 0
        self.success = False

    def run(self):
        raise NotImplementedError


class MapperConv(Mapper):
    # N, IC, OC, OH, OW, KH, KW, G
    # 0,  1,  2,  3,  4,  5,  6, 7
    DIMS = ["N", "IC", "OC", "OH", "OW", "KH", "KW", "G"]

    # [Bias, Weight, Input, Output]
    SPM_KEEP_LIST = [[0, 1, 1, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 0]]

    # Dimensions are diveded into 4 groups: [G(7)], [OC(2)], [OH(3), OW(4), N(0)], [IC(1), KH(5), KW(6)]
    PERMUTATION_LIST = [[7, 3, 4, 0, 1, 5, 6, 2],
                        [7, 2, 1, 5, 6, 3, 4, 0],
                        [7, 2, 3, 4, 0, 1, 5, 6]]

    DRAM_REPEAT_LIST = [
        [  # ["G", "OH", "OW", "N", "IC", "KH", "KW", "OC"], IC=1, KH=1, KW=1
            ["G", "OH", "OW", "N", "OC"],  # Bias
            ["G", "OH", "OW", "N", "OC"],  # Weight
            ["G", "OH", "OW", "N"],  # Input
            ["G", "OH", "OW", "N", "OC"],  # Output
        ],
        [  # ["G", "OC", "IC", "KH", "KW", "OH", "OW", "N"], IC=1, KH=1, KW=1
            ["G", "OC"],  # Bias
            ["G", "OC"],  # Weight
            ["G", "OC", "OH", "OW", "N"],  # Input
            ["G", "OC", "OH", "OW", "N"],  # Output
        ],
        [  # ["G", "OC", "OH", "OW", "N", "IC", "KH", "KW"], IC=1, KH=1, KW=1
            ["G", "OC"],  # Bias
            ["G", "OC"],  # Weight
            ["G", "OC", "OH", "OW", "N"],  # Input
            ["G", "OC", "OH", "OW", "N"],  # Output
        ],
    ]

    SPM_REPEAT_LIST = [
        [  # ["G", "OH", "OW", "N", "IC", "KH", "KW", "OC"], IC=1, KH=1, KW=1
            ["G", "OH", "OW", "N", "OC"],  # Bias
            ["G", "OH", "OW", "N", "OC"],  # Weight
            ["G", "OH", "OW", "N"],  # Input
            ["G", "OH", "OW", "N", "OC"],  # Output
        ],
        [  # ["G", "OC", "IC", "KH", "KW", "OH", "OW", "N"], IC=1, KH=1, KW=1
            ["G", "OC"],  # Bias
            ["G", "OC"],  # Weight
            ["G", "OC", "OH", "OW", "N"],  # Input
            ["G", "OC", "OH", "OW", "N"],  # Output
        ],
        [  # ["G", "OC", "OH", "OW", "N", "IC", "KH", "KW"]
            ["G", "OC"],  # Bias
            ["G", "OC", "OH", "OW", "N", "IC", "KH", "KW"],  # Weight
            ["G", "OC", "OH", "OW", "N", "IC"],  # Input
            ["G", "OC", "OH", "OW", "N"],  # Output
        ],
    ]

    def __init__(self, prob: ProblemConv, target: Target, dramKeep: list[int] = None, showGurobiOutput: bool = True, spmKeep: list[int] = None):
        super().__init__(target, showGurobiOutput)
        self.prob = prob

        self.opt: Optimizer | None = None
        self.optIndices = (0, 0, 0)  # (spmKeepIndex, dramReuseIndex, spmReuseIdx)
        self.subOptList: list[tuple[Optimizer, tuple]] = []

        if (dramKeep is None) or ((not self.target.enableILR) and (not self.target.enablePipeline)) or self.target.isCache:
            self.dramKeep = [1, 1, 1, 1]
        else:
            assert len(dramKeep) == 4
            self.dramKeep = list(dramKeep)
        
        self.spmKeepInput = spmKeep
        if spmKeep is not None:
            assert len(spmKeep) == 4
        
        self.spatialPerm = list(self.PERMUTATION_LIST[2])

    def run(self):
        self.tileAndDistribute()

        if self.target.isCache:
            spmKeepId, dramRepeatId, spmRepeatId = 3, 2, 2
            dramKeep = [1, 1, 1, 1]
            spmKeep = self.SPM_KEEP_LIST[spmKeepId]
            dramRepeat = self.DRAM_REPEAT_LIST[dramRepeatId]
            spmRepeat = self.SPM_REPEAT_LIST[spmRepeatId]
            opt = Optimizer(self.prob, self.target,
                            self.tileShape, self.spatialFactors,
                            dramRepeat, spmRepeat,
                            dramKeep, spmKeep,
                            self.showGurobiOutput)
            for d in self.DIMS:
                opt.setVarEqualToValue(0, d, 1)
            opt.run()

            self.subOptList.append((opt, (spmKeepId, dramRepeatId, spmRepeatId)))
            if opt.valid():
                self.success = True
            else:
                self.success = False
                return
                
            self.opt = opt
            self.optIndices = (spmKeepId, dramRepeatId, spmRepeatId)
            self.spmKeep = list(spmKeep)

        else:
            for spmKeepId in range(len(self.SPM_KEEP_LIST)):
                # 如果指定了spmKeep，则只考虑指定的spmKeep
                if self.spmKeepInput is None:
                    if self.dramKeep[-1] == 0:
                        # SPM doesn't keep output by default
                        # Set spm to keep output when dram doesn't keep output
                        spmKeep = self.SPM_KEEP_LIST[spmKeepId][: -1] + [1]
                    else:
                        spmKeep = self.SPM_KEEP_LIST[spmKeepId]
                else:
                    spmKeep = list(self.spmKeepInput)

                # At least one of SPM and DRAM should keep a tensor
                illegal = False
                for keepIter in range(len(self.dramKeep)):
                    if self.dramKeep[keepIter] == 0 and spmKeep[keepIter] == 0:
                        illegal = True
                if illegal:
                    continue

                for dramRepeatId, dramRepeat in enumerate(self.DRAM_REPEAT_LIST[: 2]):
                    for spmRepeatId, spmRepeat in enumerate(self.SPM_REPEAT_LIST):
                        opt = Optimizer(self.prob, self.target,
                                        self.tileShape, self.spatialFactors,
                                        dramRepeat, spmRepeat,
                                        self.dramKeep, spmKeep,
                                        self.showGurobiOutput)
                        opt.setVarEqualToValue(0, "IC", 1)
                        opt.setVarEqualToValue(0, "KH", 1)
                        opt.setVarEqualToValue(0, "KW", 1)
                        if spmRepeatId < 2:
                            opt.setVarEqualToValue(1, "IC", 1)
                            opt.setVarEqualToValue(1, "KH", 1)
                            opt.setVarEqualToValue(1, "KW", 1)
                        opt.run()

                        self.subOptList.append((opt, (spmKeepId, dramRepeatId, spmRepeatId)))
                        if opt.valid() and ((self.opt is None) or opt.isBetterThan(self.opt)):
                            self.opt = opt
                            self.optIndices = (spmKeepId, dramRepeatId, spmRepeatId)
                            self.spmKeep = list(spmKeep)
                
                # 如果指定了spmKeep，则只考虑指定的spmKeep，仅执行一次
                if self.spmKeepInput is not None:
                    break

        if self.opt is None:
            self.success = False
            return
        else:
            self.success = True
            
        if self.mappingForPipeline:
            assert self.opt.spmUtilBytesTotal % (self.target.spmPageSizeBytes) == 0
        
        self.dramFactors = self.opt.vars[0]
        self.spmFactors = self.opt.vars[1]
        self.dramPerm = list(self.PERMUTATION_LIST[self.optIndices[1]])
        self.spmPerm = list(self.PERMUTATION_LIST[self.optIndices[2]])
        self.dramAccessBytes = self.opt.dramAccessBytesTotal
        self.spmUtilBytes = self.opt.spmUtilBytesTotal
        self.spmPageUtil = self.opt.spmUtilBytesTotal // (self.target.spmPageSizeBytes) if self.mappingForPipeline else 0
        self.accelUtil = self.opt.accelUtil
        self.spmTensorUtil = self.opt.spmUtilBytes
        self.spmDimensions = self.opt.spmTileDims
        self.spmTensorTileCount = [1, 1, 1, 1]
        self.psumAccumCount = 1
        self.tensorMulticastCount = [self.accelUtil, self.accelUtil, self.accelUtil, self.accelUtil]
        self.totalTiles = 1
        # N, IC, OC, OH, OW, KH, KW, G
        # 0,  1,  2,  3,  4,  5,  6, 7
        ACCUM_DIMS = {1, 5, 6}  # IC, KH, KW
        BIAS_DIMS = {2, 7}  # OC, G
        WEIGHT_DIMS = {1, 2, 5, 6, 7}  # IC, OC, KH, KW, G
        INPUT_DIMS = {0, 1, 3, 4, 7}  # N, IC, OH, OW, G
        OUTPUT_DIMS = {0, 2, 3, 4, 7}  # N, OC, OH, OW, G
        TENSOR_ID_TO_DIMS = [BIAS_DIMS, WEIGHT_DIMS, INPUT_DIMS, OUTPUT_DIMS]
        for dimId, factor in enumerate(self.dramFactors):
            self.totalTiles *= factor
            if dimId in ACCUM_DIMS:
                assert factor == 1
        for dimId, factor in enumerate(self.spmFactors):
            self.totalTiles *= factor
            if dimId in ACCUM_DIMS:
                self.psumAccumCount *= factor
            for t in range(4):
                if dimId in TENSOR_ID_TO_DIMS[t]:
                    self.spmTensorTileCount[t] *= factor
        for dimId, factor in enumerate(self.spatialFactors):
            self.totalTiles *= factor
            if dimId in ACCUM_DIMS:
                assert factor == 1
            for t in range(4):
                if dimId in TENSOR_ID_TO_DIMS[t]:
                    self.spmTensorTileCount[t] *= factor
                    assert self.tensorMulticastCount[t] % factor == 0
                    self.tensorMulticastCount[t] = int(self.tensorMulticastCount[t] / factor)

    # 产生accel-tile shape和spatial-tile factor
    def tileAndDistribute(self):
        cacheLineBytes = 64
        words = int(cacheLineBytes / self.target.inputWordBytes)
        numArrays = self.target.numArrays

        sh, sw = self.prob.strideH, self.prob.strideW
        N, IC, OC, OH, OW, KH, KW, G = [self.prob[i] for i in [i for i in range(8)]]
        n, ic, oc, oh, ow, kh, kw, g = 1, 1, 1, 1, 1, 1, 1, 1 # accel-tile shape
        sp_n, sp_oc, sp_oh, sp_ow, sp_g = 1, 1, 1, 1, 1 # spatial-tile factor

        # maximize cache line bytes
        # cache line:[n_max, g_max, ic, oc]
        for oc in range(words, 0, -1):
            if OC % oc == 0:
                break
        for ic in range(words, 0, -1): # TODO: 为什么不是words / ic
            if IC % ic == 0:
                break
        g_max = math.ceil(words / (ic * oc))
        for g in range(g_max, 0, -1):
            if G % g == 0:
                break
        n_max = math.ceil(words / (ic * oc * g))
        for n in range(n_max, 0, -1):
            if N % n == 0:
                break

        # multi-accel spatial distribution
        # spatial(splited by numArrays):[sp_ow, sp_oh, sp_n, sp_g, sp_oc]
        for sp_oc in range(numArrays, 0, -1):
            if OC % (oc * sp_oc) == 0:
                break
        for sp_g in range(math.floor(numArrays / sp_oc), 0, -1):
            if G % (g * sp_g) == 0:
                break
        for sp_n in range(math.floor(numArrays / (sp_oc * sp_g)), 0, -1):
            if N % (n * sp_n) == 0:
                break
        for sp_oh in range(math.floor(numArrays / (sp_oc * sp_n * sp_g)), 0, -1):
            if OH % sp_oh == 0:
                break
        for sp_ow in range(math.floor(numArrays / (sp_oc * sp_n * sp_g * sp_oh)), 0, -1):
            if OW % sp_ow == 0:
                break

        def weightTileSize():
            return kh * kw * ic * g * oc * self.target.inputWordBytes

        def inputTileSize():
            return ((oh - 1) * sh + KH) * ((ow - 1) * sw + KW) * n * g * ic * self.target.inputWordBytes

        def outputTileSize():
            return oh * ow * n * g * oc * self.target.psumWordBytes

        def satisfy():
            return (weightTileSize() <= self.target.accelWeightBytes and
                    inputTileSize() <= self.target.accelInputBytes and
                    outputTileSize() <= self.target.accelPsumBytes)

        # check in-accel weight tile size
        kh, kw = KH, KW
        while not satisfy():
            if kh > 1:
                kh_ = kh - 1
                for kh in range(kh_, 0, -1):
                    if KH % kh == 0:
                        break
            elif kw > 1:
                kw_ = kw - 1
                for kw in range(kw_, 0, -1):
                    if KW % kw == 0:
                        break
            else:
                assert False

        # check in-accel input/output tile size
        oh, ow = math.ceil(OH / sp_oh), math.ceil(OW / sp_ow)
        while not satisfy():
            if oh > 1 and oh >= ow:
                oh_ = oh - 1
                for oh in range(oh_, 0, -1):
                    if OH % (sp_oh * oh) == 0:
                        break
            elif ow > 1:
                ow_ = ow - 1
                for ow in range(ow_, 0, -1):
                    if OW % (sp_ow * ow) == 0:
                        break
            else:
                assert False

        # 前面要求整除，可能造成利用率低；尝试增加tile大小
        # increase N
        n = math.ceil(N / sp_n)
        while not satisfy():
            assert n > 1
            n_ = n - 1
            for n in range(n_, 0, -1):
                if N % (sp_n * n) == 0:
                    break

        # increase IC
        ic = IC
        while not satisfy():
            assert ic > 1
            ic_ = ic - 1
            for ic in range(ic_, 0, -1):
                if IC % ic == 0:
                    break

        # increase OC
        oc = math.ceil(OC / sp_oc)
        while not satisfy():
            assert oc > 1
            oc_ = oc - 1
            for oc in range(oc_, 0, -1):
                if OC % (sp_oc * oc) == 0:
                    break

        # increase G
        g = math.ceil(G / sp_g)
        while not satisfy():
            assert g > 1
            g_ = g - 1
            for g in range(g_, 0, -1):
                if G % (sp_g * g) == 0:
                    break

        self.tileShape = [n, ic, oc, oh, ow, kh, kw, g]
        self.spatialFactors = [sp_n, 1, sp_oc, sp_oh, sp_ow, 1, 1, sp_g]


class MapperResadd(Mapper):
    # N, C, H, W, G
    # 0, 1, 2, 3, 4
    PERMUTATION = [2, 3, 0, 4, 1]

    def __init__(self, prob: ProblemResadd, target: Target, dramKeep: list[int] = None, showGurobiOutput: bool = True, spmKeep: list[int] = None):
        super().__init__(target, showGurobiOutput)
        self.prob = prob

        self.dramPerm = list(self.PERMUTATION)
        self.spmPerm = list(self.PERMUTATION)
        self.spatialPerm = list(self.PERMUTATION)

        if (dramKeep is None) or (not self.target.enableILR and not self.target.enablePipeline) or self.target.isCache:
            self.dramKeep = [1, 1, 1]
        else:
            assert len(dramKeep) == 3
            self.dramKeep = list(dramKeep)
            
        if spmKeep is None:
            self.spmKeep = [1 - i for i in self.dramKeep]
        else:
            assert len(spmKeep) == 3
            self.spmKeep = list(spmKeep)
    def run(self):
        self.tileAndDistribute()

        self.dramFactors = [1, 1, 1, 1, 1]
        self.spmFactors = [int(self.prob[i] / (self.tileShape[i] * self.spatialFactors[i])) for i in range(5)]
        self.spmTensorUtil = [self.prob.getTensorWords() * self.target.inputWordBytes * self.spmKeep[i]
                               for i in range(3)]
        if self.mappingForPipeline:
            self.ceilSpmTensorPage = [int(math.ceil(x / float(self.target.spmPageSizeBytes))) for x in self.spmTensorUtil]
            self.spmTensorUtil = [x * self.target.spmPageSizeBytes for x in self.ceilSpmTensorPage]
        
        self.dramAccessBytes = sum([self.prob.getTensorWords() * self.target.inputWordBytes * self.dramKeep[i]
                                    for i in range(3)])
        self.spmUtilBytes = sum(self.spmTensorUtil)
        
        self.accelUtil = 1
        for i in self.spatialFactors:
            self.accelUtil *= i
        self.spmDimensions = [self.prob.N, self.prob.C, self.prob.H, self.prob.W, self.prob.G]
        self.spmTensorTileCount = [functools.reduce(lambda a, b: a * b, self.spmFactors + self.spatialFactors)
                                   for _ in range(3)]
        self.psumAccumCount = 1
        self.tensorMulticastCount = [1, 1, 1]
        self.totalTiles = functools.reduce(lambda a, b: a * b, self.spmFactors + self.spatialFactors)
        
        self.spmPageUtil = self.spmUtilBytes // (self.target.spmPageSizeBytes) if self.mappingForPipeline else 0
        
        self.success = True

    def tileAndDistribute(self):
        cacheLineBytes = 64
        words = int(cacheLineBytes / self.target.inputWordBytes)
        numArrays = self.target.numArrays

        N, C, H, W, G = [self.prob[i] for i in [i for i in range(5)]]
        n, c, h, w, g = 1, 1, 1, 1, 1
        sp_n, sp_c, sp_h, sp_w, sp_g = 1, 1, 1, 1, 1

        # maximize cache line bytes
        for c in range(words, 0, -1):
            if C % c == 0:
                break
        g_max = math.ceil(words / c)
        for g in range(g_max, 0, -1):
            if G % g == 0:
                break
        n_max = math.ceil(words / (c * g))
        for n in range(n_max, 0, -1):
            if N % n == 0:
                break

        # multi-accel spatial distribution
        for sp_c in range(numArrays, 0, -1):
            if C % (c * sp_c) == 0:
                break
        for sp_g in range(math.floor(numArrays / sp_c), 0, -1):
            if G % (g * sp_g) == 0:
                break
        for sp_n in range(math.floor(numArrays / (sp_c * sp_g)), 0, -1):
            if N % (n * sp_n) == 0:
                break
        for sp_h in range(math.floor(numArrays / (sp_c * sp_n * sp_g)), 0, -1):
            if H % sp_h == 0:
                break
        for sp_w in range(math.floor(numArrays / (sp_c * sp_n * sp_g * sp_h)), 0, -1):
            if W % sp_w == 0:
                break

        def tensorTileSize():
            return h * w * c * g * n * self.target.inputWordBytes

        def satisfy():
            return tensorTileSize() <= min(self.target.accelInputBytes, self.target.accelWeightBytes,
                                           self.target.accelPsumBytes)

        # increase C
        c = math.ceil(C / sp_c)
        while not satisfy():
            assert c > 1
            c_ = c - 1
            for c in range(c_, 0, -1):
                if C % (sp_c * c) == 0:
                    break

        # increase G
        g = math.ceil(G / sp_g)
        while not satisfy():
            assert g > 1
            g_ = g - 1
            for g in range(g_, 0, -1):
                if G % (sp_g * g) == 0:
                    break

        # increase N
        n = math.ceil(N / sp_n)
        while not satisfy():
            assert n > 1
            n_ = n - 1
            for n in range(n_, 0, -1):
                if N % (sp_n * n) == 0:
                    break

        # increase W
        w = math.ceil(W / sp_w)
        while not satisfy():
            assert w > 1
            w_ = w - 1
            for w in range(w_, 0, -1):
                if W % (sp_w * w) == 0:
                    break

        # increase H
        h = math.ceil(H / sp_h)
        while not satisfy():
            assert h > 1
            h_ = h - 1
            for h in range(h_, 0, -1):
                if H % (sp_h * h) == 0:
                    break

        self.tileShape = [n, c, h, w, g]
        self.spatialFactors = [sp_n, sp_c, sp_h, sp_w, sp_g]
