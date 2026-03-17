class Problem:
    """
    Attributes:
        dramKeep: If a tensor is stored in DRAM memory.
        spmKeep: If a tensor is stored in SPM memory.
    """

    def __init__(self, dramKeep: list[int] = None, spmKeep: list[int] = None):
        self.dramKeep = dramKeep
        self.spmKeep = spmKeep


class ProblemConv(Problem):
    PARAM_IDX_TO_NAME = ["N", "IC", "OC", "OH", "OW", "KH", "KW", "G"]
    PARAM_NAME_TO_IDX = {p: i for i, p in enumerate(PARAM_IDX_TO_NAME)}

    def __init__(self, dimList: list[int] = None, stride: list[int] = None, dramKeep: list[int] = None, spmKeep: list[int] = None):
        super().__init__()
        self.N = 1
        self.IC = 1
        self.OC = 1
        self.OH = 1
        self.OW = 1
        self.KH = 1
        self.KW = 1
        self.G = 1
        self.strideH = 1
        self.strideW = 1

        if dimList is None:
            dimList = [1, 1, 1, 1, 1, 1, 1, 1]
        if stride is None:
            stride = [1, 1]
        if dramKeep is None:
            dramKeep = [1, 1, 1, 1]
        self.setDimList(dimList)
        self.setStride(stride[0], stride[1])
        self.dramKeep = dramKeep
        self.spmKeep = spmKeep

    def setDimList(self, dimList: list[int]):
        assert len(dimList) == 8
        self.N, self.IC, self.OC, self.OH, self.OW, self.KH, self.KW, self.G = dimList

    def setStride(self, strideH, strideW):
        self.strideH = strideH
        self.strideW = strideW

    def getIH(self):
        return (self.OH - 1) * self.strideH + self.KH

    def getIW(self):
        return (self.OW - 1) * self.strideW + self.KW

    def getBiasWords(self):
        return self.OC * self.G

    def getWeightWords(self):
        return self.KH * self.KW * self.OC * self.IC * self.G

    def getInputWords(self):
        return self.getIH() * self.getIW() * self.N * self.IC * self.G

    def getOutputWords(self):
        return self.OH * self.OW * self.N * self.OC * self.G

    def __getitem__(self, i: int | str):
        if isinstance(i, int):
            i = self.PARAM_IDX_TO_NAME[i]
        assert hasattr(self, i)
        return getattr(self, i)

    def __str__(self):
        s = f"{self.__class__.__name__}("
        for dim in self.PARAM_IDX_TO_NAME:
            s += f"{dim}={self[dim]},"
        s += f"strideH={self.strideH},strideW={self.strideW})"
        return s


class ProblemResadd(Problem):
    PARAM_IDX_TO_NAME = ["N", "C", "H", "W", "G"]
    PARAM_NAME_TO_IDX = {p: i for i, p in enumerate(PARAM_IDX_TO_NAME)}

    def __init__(self, dimList: list[int] = None, dramKeep: list[int] = None, spmKeep: list[int] = None):
        super().__init__()
        self.N = 1
        self.C = 1
        self.H = 1
        self.W = 1
        self.G = 1

        if dimList is None:
            dimList = [1, 1, 1, 1, 1]
        if dramKeep is None:
            dramKeep = [1, 1, 1]
        self.setDimList(dimList)
        self.dramKeep = dramKeep
        self.spmKeep = spmKeep

    def setDimList(self, dimList: list[int]):
        assert len(dimList) == 5
        self.N, self.C, self.H, self.W, self.G = dimList

    def getTensorWords(self):
        return self.N * self.C * self.H * self.W * self.G

    def __getitem__(self, i: int | str):
        if isinstance(i, int):
            i = self.PARAM_IDX_TO_NAME[i]
        assert hasattr(self, i)
        return getattr(self, i)

    def __str__(self):
        s = f"{self.__class__.__name__}("
        for dim in self.PARAM_IDX_TO_NAME:
            s += f"{dim}={self[dim]},"
        s += ")"
        return s

class ProblemEmpty(Problem):
    def __init__(self):
        super().__init__()