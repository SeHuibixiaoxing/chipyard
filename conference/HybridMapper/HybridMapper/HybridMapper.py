import os
import functools
import itertools
from multiprocessing import Pool
from HybridMapper.Target import Target
from HybridMapper.Problem import Problem, ProblemEmpty, ProblemConv, ProblemResadd
from HybridMapper.Mapper import getMapper
from HybridMapper.MTMRecord import MTMRecord


def processFunc(prob: Problem, targetList: list[Target], dramKeep: list[int], path: str, dumpPath: str, spmKeep: list[int]):
    mapperList = []
    
    # pipeline (dramKeep, spmKeep)对应表
    pipeKeepConvWeight = [[0, 1], [1,0], [1, 1]]
    pipeKeepConvBias = [[0, 1], [1, 0], [1, 1]]
    pipeKeepConvInput = [[0, 1], [1, 0], [1, 1]]
    pipeKeepConvOutput = [[0, 1], [1, 0], [1, 1]]
    
    pipeKeepResaddInput0 = [[1, 0], [0, 1], [1, 1]]
    pipeKeepResaddInput1 = [[0, 1], [1, 0], [1, 1]]
    pipeKeepResaddOutput = [[0, 1], [1, 0], [1, 1]]
    
    
    # pipeKeepConvWeight = [[0, 1], [1,0]]
    # pipeKeepConvBias = [[0, 1], [1, 0]]
    # pipeKeepConvInput = [[0, 1], [1, 0]]
    # pipeKeepConvOutput = [[0, 1], [1, 0]]
    
    # pipeKeepResaddInput0 = [[1, 0], [0, 1]]
    # pipeKeepResaddInput1 = [[0, 1], [1, 0]]
    # pipeKeepResaddOutput = [[0, 1], [1, 0]]
    
    def processKeep(*lists):
        cartesian_product = list(itertools.product(*lists))
        
        # 对每个笛卡尔积结果进行合并处理
        result = []
        for combination in cartesian_product:
            # 提取每个列表的前两个元素（0位和1位）
            positions_0 = []
            positions_1 = []
            
            # 遍历每个小列表
            for sublist in combination:
                # 0位数据（每个子列表的第一个元素）
                positions_0.append(sublist[0])
                # 1位数据（每个子列表的第二个元素）
                positions_1.append(sublist[1])
            
            # 组合成新的列表格式
            new_combination = [positions_0, positions_1]
            result.append(new_combination)
            
        
        return result
    
    pipeKeepConv = processKeep(pipeKeepConvBias, pipeKeepConvWeight, pipeKeepConvInput, pipeKeepConvOutput)
    pipeKeepRessadd = processKeep(pipeKeepResaddInput0, pipeKeepResaddInput1, pipeKeepResaddOutput)
    
    for target in targetList:
        if target.enablePipeline:
            keepCan = None
            if isinstance(prob, ProblemConv):
                keepCan = pipeKeepConv
            elif isinstance(prob, ProblemResadd):
                keepCan = pipeKeepRessadd
            else:
                assert False
            for keep in keepCan:
                mapperList.append(getMapper(prob, target, keep[0], False, keep[1]))
        else:
            mapperList.append(getMapper(prob, target, dramKeep, False, spmKeep))
    for mapper in mapperList:
        mapper.run()
    record = MTMRecord(pipeline=functools.reduce(lambda acc, x: acc or x.enablePipeline, targetList, False))
    record.set(mapperList)
    record.write(path)
    record.dump(dumpPath)
    # print([mapper.opt.objVal for mapper in mapperList])


class HybridMapper:
    def __init__(self, probList: list[Problem], targetList: list[Target], outputPath: str, maxProcesses: int = 60):
        assert len(targetList) > 0
        assert maxProcesses > 0

        self.probList = probList
        self.targetList = targetList
        self.outputPath = outputPath
        self.maxProcesses = maxProcesses

    def run(self):
        os.makedirs(self.outputPath, exist_ok=True)

        argsList = []
        for i in range(len(self.probList)):
            prob = self.probList[i]
            if isinstance(prob, ProblemEmpty):
                continue
            
            targetList = self.targetList
            dramKeep = prob.dramKeep
            spmKeep = prob.spmKeep
            path = os.path.join(self.outputPath, f"{i}.yaml")
            dumpPath = os.path.join(self.outputPath, f"{i}.dump")
            argsList.append((prob, targetList, dramKeep, path, dumpPath, spmKeep))

        with Pool() as pool:
            pool.starmap(processFunc, argsList)
