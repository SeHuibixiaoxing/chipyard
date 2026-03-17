import argparse
import os

from HybridMapper.StageRecord import StageCandidate,StageRecord

def createResnet18_OneLayer_OneStage_2Acc(path: str):
    can0 = StageCandidate(
                globalStageId = 0,
                layerIdList = [1],
                tensorIdList = [1000002, 1000003, 1, 2],
                vAccIdxList = [[0,1]],
                
                dramBypassList = [[1,1,1,1]],
                spmBypassList = [[0,0,0,0]],
                 
                entryTensorIdList = [1],
                entryTensorTypeList = ["DRAM"],
                entryTensorDoubleBufferList = [0],
                 
                exportTensorIdList = [2],
                exportTensorTypeList = ["DRAM"],
                exportTensorDoubleBufferList = [0],
                 
                fixTensorDramBypassIdList = [1000002, 1000003],
                innerIsolateTensorId = [],
                innerSharedTensorId = [],
                
                tensorUseLazyFetch = [[0,0,0,0]],
                
                tensorUsageCountList = [[1, 1, 1, 1]],
                
                accUtil = 2)
    
    stageRecord = StageRecord()
    stageRecord.set([can0])
    stageRecord.write(path)

def createResnet18_TwoLayer_TwoStage(path: str, accNums: int, lazyFetch: int, doubleBuffer: int, ringBuffer: int):
    accNumPerStage = accNums // 2
    vAccIdxList = [[x for x in range(accNumPerStage)]]
    
    pAccOffset = 0
    
    can0 = StageCandidate(
                globalStageId = 0,
                layerIdList = [1],
                tensorIdList = [1000002, 1000003, 1, 2],
                vAccIdxList = vAccIdxList,
                pAccIdxList = [x+pAccOffset for x in range(accNumPerStage)],
                
                dramBypassList = [[1,1,1,1]],
                spmBypassList = [[0,0,0,0]],
                 
                entryTensorIdList = [1],
                entryTensorTypeList = ["DRAM"],
                entryTensorDoubleBufferList = [doubleBuffer],
                 
                exportTensorIdList = [2],
                exportTensorTypeList = ["ISOLATE_SPM"],
                exportTensorDoubleBufferList = [doubleBuffer],
                 
                fixTensorDramBypassIdList = [1000002, 1000003],
                innerIsolateTensorId = [],
                innerSharedTensorId = [],
                
                tensorUseLazyFetch = [[lazyFetch,lazyFetch,0,0]],
                
                tensorUsageCountList = [[1, 1, 1, 1]],
                
                accUtil = accNumPerStage)
    
    pAccOffset += len(vAccIdxList[0])
    
    can1 = StageCandidate(
                globalStageId = 1,
                layerIdList = [2],
                tensorIdList = [1000004, 1000005, 2, 3],
                vAccIdxList = vAccIdxList,
                pAccIdxList = [x+pAccOffset for x in range(accNumPerStage)],
                
                dramBypassList = [[1,1,1,1]],
                spmBypassList = [[0,0,0,0]],
                 
                entryTensorIdList = [2],
                entryTensorTypeList = ["ISOLATE_SPM"],
                entryTensorDoubleBufferList = [doubleBuffer],
                 
                exportTensorIdList = [3],
                exportTensorTypeList = ["DRAM"],
                exportTensorDoubleBufferList = [doubleBuffer],
                 
                fixTensorDramBypassIdList = [1000004, 1000005],
                innerIsolateTensorId = [],
                innerSharedTensorId = [],
                
                tensorUseLazyFetch = [[lazyFetch,lazyFetch,0,0]],
                
                tensorUsageCountList = [[1, 1, 1, 1]],
                
                accUtil = accNumPerStage)

    stageRecord = StageRecord()
    stageRecord.set([can0, can1])
    stageRecord.write(path)
    
def createBert_FFN_TwoLayer_TwoStage(path: str, accNums: int, lazyFetch: int, doubleBuffer: int, ringBuffer: int):
    accNumPerStage = accNums // 2
    vAccIdxList = [[x for x in range(accNumPerStage)]]
    
    pAccOffset = 0
    
    can0 = StageCandidate(
                globalStageId = 0,
                layerIdList = [0],
                tensorIdList = [1000000, 1000001, 0, 1],
                vAccIdxList = vAccIdxList,
                pAccIdxList = [x+pAccOffset for x in range(accNumPerStage)],
                
                dramBypassList = [[1,1,1,1]],
                spmBypassList = [[0,0,0,0]],
                 
                entryTensorIdList = [0],
                entryTensorTypeList = ["DRAM"],
                entryTensorDoubleBufferList = [doubleBuffer],
                 
                exportTensorIdList = [1],
                exportTensorTypeList = ["ISOLATE_SPM"],
                exportTensorDoubleBufferList = [doubleBuffer],
                 
                fixTensorDramBypassIdList = [1000000, 1000001],
                innerIsolateTensorId = [],
                innerSharedTensorId = [],
                
                tensorUseLazyFetch = [[lazyFetch,lazyFetch,0,0]],
                
                tensorUsageCountList = [[1, 1, 1, 1]],
                
                accUtil = accNumPerStage)
    
    pAccOffset += len(vAccIdxList[0])
    
    can1 = StageCandidate(
                globalStageId = 0,
                layerIdList = [1],
                tensorIdList = [1000002, 1000003, 1, 2],
                vAccIdxList = vAccIdxList,
                pAccIdxList = [x+pAccOffset for x in range(accNumPerStage)],
                
                dramBypassList = [[1,1,1,1]],
                spmBypassList = [[0,0,0,0]],
                 
                entryTensorIdList = [1],
                entryTensorTypeList = ["ISOLATE_SPM"],
                entryTensorDoubleBufferList = [doubleBuffer],
                 
                exportTensorIdList = [2],
                exportTensorTypeList = ["DRAM"],
                exportTensorDoubleBufferList = [doubleBuffer],
                 
                fixTensorDramBypassIdList = [1000002, 1000003],
                innerIsolateTensorId = [],
                innerSharedTensorId = [],
                
                tensorUseLazyFetch = [[lazyFetch,lazyFetch,0,0]],
                
                tensorUsageCountList = [[1, 1, 1, 1]],
                
                accUtil = accNumPerStage)

    stageRecord = StageRecord()
    stageRecord.set([can0, can1])
    stageRecord.write(path)

def createResnet50_Resblock2_16acc(path: str, lazyFetch: int, doubleBuffer: int, ringBuffer: int):
    can0 = StageCandidate(
                globalStageId = 0,
                layerIdList = [13],
                tensorIdList = [11, 14, 15],
                vAccIdxList = [[0]],
                pAccIdxList = [12],
                
                dramBypassList = [[1,1,1]],
                spmBypassList = [[0,0,0]],
                 
                entryTensorIdList = [11,14],
                entryTensorTypeList = ["DRAM","DRAM"],
                entryTensorDoubleBufferList = [doubleBuffer, doubleBuffer],
                 
                exportTensorIdList = [15],
                exportTensorTypeList = ["ISOLATE_SPM"],
                exportTensorDoubleBufferList = [doubleBuffer],
                 
                fixTensorDramBypassIdList = [],
                innerIsolateTensorId = [],
                innerSharedTensorId = [],
                
                tensorUseLazyFetch = [[0,0,0]],
                
                tensorUsageCountList = [[1, 1, 1]],
                
                accUtil = 1)
    
    can1 = StageCandidate(
                globalStageId = 1,
                layerIdList = [14],
                tensorIdList = [1000022, 1000023, 15, 16],
                vAccIdxList = [[0,1,2,3]],
                pAccIdxList = [8,9,10,11],
                
                dramBypassList = [[1,1,1,1]],
                spmBypassList = [[0,0,0,0]],
                 
                entryTensorIdList = [15],
                entryTensorTypeList = ["ISOLATE_SPM"],
                entryTensorDoubleBufferList = [doubleBuffer],
                 
                exportTensorIdList = [16],
                exportTensorTypeList = ["ISOLATE_SPM"],
                exportTensorDoubleBufferList = [doubleBuffer],
                 
                fixTensorDramBypassIdList = [1000022,1000023],
                innerIsolateTensorId = [],
                innerSharedTensorId = [],
                
                tensorUseLazyFetch = [[lazyFetch,lazyFetch,0,0]],
                
                tensorUsageCountList = [[1, 1, 1, 1]],
                
                accUtil = 4)

    can2 = StageCandidate(
                globalStageId = 2,
                layerIdList = [15],
                tensorIdList = [1000024, 1000025, 16, 17],
                vAccIdxList = [[0,1,2,3]],
                pAccIdxList = [4,5,6,7],
                
                dramBypassList = [[1,1,1,1]],
                spmBypassList = [[0,0,0,0]],
                 
                entryTensorIdList = [16],
                entryTensorTypeList = ["ISOLATE_SPM"],
                entryTensorDoubleBufferList = [doubleBuffer],
                 
                exportTensorIdList = [17],
                exportTensorTypeList = ["ISOLATE_SPM"],
                exportTensorDoubleBufferList = [doubleBuffer],
                 
                fixTensorDramBypassIdList = [1000024,1000025],
                innerIsolateTensorId = [],
                innerSharedTensorId = [],
                
                tensorUseLazyFetch = [[lazyFetch,lazyFetch,0,0]],
                
                tensorUsageCountList = [[1, 1, 1, 1]],
                
                accUtil = 4)
    
    can3 = StageCandidate(
                globalStageId = 3,
                layerIdList = [16],
                tensorIdList = [1000026, 1000027, 17, 18],
                vAccIdxList = [[0,1,2,3]],
                pAccIdxList = [0,1,2,3],
                
                dramBypassList = [[1,1,1,1]],
                spmBypassList = [[0,0,0,0]],
                 
                entryTensorIdList = [17],
                entryTensorTypeList = ["ISOLATE_SPM"],
                entryTensorDoubleBufferList = [doubleBuffer],
                 
                exportTensorIdList = [18],
                exportTensorTypeList = ["ISOLATE_SPM"],
                exportTensorDoubleBufferList = [doubleBuffer],
                 
                fixTensorDramBypassIdList = [1000026,1000027],
                innerIsolateTensorId = [],
                innerSharedTensorId = [],
                
                tensorUseLazyFetch = [[lazyFetch,lazyFetch,0,0]],
                
                tensorUsageCountList = [[1, 1, 1, 1]],
                
                accUtil = 4)
    
    can4 = StageCandidate(
                globalStageId = 4,
                layerIdList = [17],
                tensorIdList = [1000028, 1000029, 15, 19],
                vAccIdxList = [[0,1]],
                pAccIdxList = [13,14],
                
                dramBypassList = [[1,1,1,1]],
                spmBypassList = [[0,0,0,0]],
                 
                entryTensorIdList = [15],
                entryTensorTypeList = ["ISOLATE_SPM"],
                entryTensorDoubleBufferList = [doubleBuffer],
                 
                exportTensorIdList = [19],
                exportTensorTypeList = ["DRAM_DEPEN"],
                exportTensorDoubleBufferList = [doubleBuffer],
                 
                fixTensorDramBypassIdList = [1000028,1000029],
                innerIsolateTensorId = [],
                innerSharedTensorId = [],
                
                tensorUseLazyFetch = [[lazyFetch,lazyFetch,0,0]],
                
                tensorUsageCountList = [[1, 1, 1, 1]],
                
                accUtil = 2)
    
    can5 = StageCandidate(
                globalStageId = 5,
                layerIdList = [18],
                tensorIdList = [18, 19, 20],
                vAccIdxList = [[0]],
                pAccIdxList = [15],
                
                dramBypassList = [[0,1,1]],
                spmBypassList = [[0,0,0]],
                 
                entryTensorIdList = [18,19],
                entryTensorTypeList = ["ISOLATE_SPM", "DRAM_DEPEN"],
                entryTensorDoubleBufferList = [doubleBuffer, doubleBuffer],
                 
                exportTensorIdList = [20],
                exportTensorTypeList = ["DRAM"],
                exportTensorDoubleBufferList = [doubleBuffer],
                 
                fixTensorDramBypassIdList = [],
                innerIsolateTensorId = [],
                innerSharedTensorId = [],
                
                tensorUseLazyFetch = [[0,0,0]],
                
                tensorUsageCountList = [[1, 1, 1]],
                
                accUtil = 1)
    
    stageRecord = StageRecord()
    stageRecord.set([can0, can1, can2, can3, can4, can5])
    if ringBuffer == 1:
        stageRecord.generate_ringbuffer([[19, 3, 1]])
    stageRecord.write(path)

modelList = ["resnet18", "bert_ffn", "resnet50"]
lazyFetchList = [0, 1]
accList = [2, 4, 8, 16]
doubleBufferList = [0, 1]
ringBufferList = [0, 1]

def process(rootPath: str, modelName: str, lazyFetch: int, accNum: int, doubleBuffer: int, ringBuffer: int):
    rootPath = os.path.join(rootPath, f"{modelName}/stage/")
    dir_path = os.path.dirname(rootPath)
    if dir_path and not os.path.exists(dir_path):
        os.makedirs(dir_path, exist_ok=True)
    
    doubleBufferStr = "_doublebuffer" if doubleBuffer == 1 else ""
    lazyFetchStr = "_lazyfetch" if lazyFetch == 1 else ""
    ringBufferStr = "_ringbuffer" if ringBuffer == 1 else ""
        
    if modelName == "resnet18":
        file_name = f"{modelName}-1-2_{accNum}acc{doubleBufferStr}{lazyFetchStr}{ringBufferStr}.yaml"
        createResnet18_TwoLayer_TwoStage(os.path.join(dir_path, file_name), accNum, lazyFetch, doubleBuffer, ringBuffer)
    elif modelName == "bert_ffn":
        file_name = f"{modelName}-0-1_{accNum}acc{doubleBufferStr}{lazyFetchStr}{ringBufferStr}.yaml"
        createBert_FFN_TwoLayer_TwoStage(os.path.join(dir_path, file_name), accNum, lazyFetch, doubleBuffer, ringBuffer)
    elif modelName == "resnet50":
        if accNum == 16:
            file_name = f"{modelName}-13-14-15-16-17-18_16acc{doubleBufferStr}{lazyFetchStr}{ringBufferStr}.yaml"
            createResnet50_Resblock2_16acc(os.path.join(dir_path, file_name), lazyFetch, doubleBuffer, ringBuffer)
        
def main():
    rootPath = "output/pipeline"
    
    for modelName in modelList:
        for lazyFetch in lazyFetchList:
            for accNum in accList:
                for doubleBuffer in doubleBufferList:
                    for ringBuffer in ringBufferList:
                        process(rootPath, modelName, lazyFetch, accNum, doubleBuffer, ringBuffer)
    
if __name__ == "__main__":
    main()
    
    
    
    