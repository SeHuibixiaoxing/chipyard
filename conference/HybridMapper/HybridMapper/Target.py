import math
class Target:
    """
    Attributes:
        isCache: Cache-non-aware mapping used in "Gemmini" runtime.
        enableILR: Put intermediate data fully in SPM to reduce memory access.
    """

    def __init__(self, numArrays=1, spmKB=1024, spmPageKB = 1, isCache=False, enableILR=False, enablePipeline=False, num_macs_per_array = 1024, dram_bw_per_cycle=10, noc_bw_per_cycle = 64):
        # macs
        self.num_macs_per_array = num_macs_per_array
        
        # dram
        self.dram_bw_per_cycle = dram_bw_per_cycle
        
        # noc
        self.noc_bw_per_cycle = noc_bw_per_cycle
        
        # SPM capacity
        self.spmBytes = spmKB * 1024
        self.spmBytesPerArray = self.spmBytes // numArrays
        
        # SPM page size
        self.spmPageSizeBytes= spmPageKB * 1024

        # accel capacity
        self.accelWeightBytes = 32 * 1024
        self.accelInputBytes = 32 * 1024
        self.accelPsumBytes = 64 * 1024

        # word bytes
        self.inputWordBytes = 1
        self.psumWordBytes = 4

        # compute array
        self.numArrays = numArrays
        self.allowed_acc_nums: list[int] = [pow(2, i) for i in range(int(math.log2(self.numArrays) + 2) + 1) if pow(2, i) <= self.numArrays]

        # mapping strategy
        self.isCache = isCache
        self.enableILR = enableILR
        self.enablePipeline = enablePipeline
    
    def ceilPage(self, bytes: int) -> int:
        return (bytes + self.spmPageSizeBytes - 1) // self.spmPageSizeBytes * self.spmPageSizeBytes

    def getPageNum(self, bytes: int) -> int:
        return self.ceilPage(bytes) // self.spmPageSizeBytes
