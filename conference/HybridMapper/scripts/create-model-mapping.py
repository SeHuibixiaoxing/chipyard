from HybridMapper.models import models
from HybridMapper.HybridMapper import *
from HybridMapper.Target import Target
from HybridMapper.PostProcess import PostProcess
from HybridMapper.Model import Layer
from HybridMapper.PipelineTarget import get_mapping_target
import argparse
import os


def main():
    outputRootPath = "output"
    defaultModelName = "testmodel"

    parser = argparse.ArgumentParser()
    parser.add_argument("--model", type=str, required=False, default=defaultModelName)
    parser.add_argument("--pipeline",  action="store_true")
    parser.add_argument("--batch-size",  type=int, default = 1)

    args = parser.parse_args()
    modelName = args.model
    
    if args.batch_size != 1:
        Layer.BATCH_SIZE = args.batch_size

    model = models.create(modelName, args.pipeline)
    print()
    assert model.checkLayerGroup()
    model.generateMemoryMapping()
    # print(model.getInfoString())
    # exit()

    targetList = [
        Target(1, 0, isCache=True), # 0
        Target(1, 0), # 1
        Target(1, 256), # 2
        Target(1, 512), # 3
        Target(1, 1024), # 4
        Target(1, 2048), # 5
        Target(1, 4096), # 6
        Target(1, 8192), # 7
        Target(1, 8192*2), # 8
        Target(1, 8192*4), # 9
        Target(1, 1024 * 1024, enableILR=True), # 10

        Target(2, 0, isCache=True),
        Target(2, 0),
        Target(2, 256),
        Target(2, 512),
        Target(2, 1024),
        Target(2, 2048),
        Target(2, 4096),
        Target(2, 8192),
        Target(2, 8192*2),
        Target(2, 8192*4),
        Target(2, 1024 * 1024, enableILR=True),

        Target(4, 0, isCache=True),
        Target(4, 0),
        Target(4, 256),
        Target(4, 512),
        Target(4, 1024),
        Target(4, 2048),
        Target(4, 4096),
        Target(4, 8192),
        Target(4, 8192*2),
        Target(4, 8192*4),
        Target(4, 1024 * 1024, enableILR=True),
        
        Target(8, 0, isCache=True),
        Target(8, 0),
        Target(8, 256),
        Target(8, 512),
        Target(8, 1024),
        Target(8, 2048),
        Target(8, 4096),
        Target(8, 8192),
        Target(8, 8192*2),
        Target(8, 8192*4),
        Target(8, 1024 * 1024, enableILR=True),
        
        Target(16, 0, isCache=True),
        Target(16, 0),
        Target(16, 256),
        Target(16, 512),
        Target(16, 1024),
        Target(16, 2048),
        Target(16, 4096),
        Target(16, 8192),
        Target(16, 8192*2),
        Target(16, 8192*4),
        Target(16, 1024 * 1024, enableILR=True),
        
        Target(32, 0, isCache=True),
        Target(32, 0),
        Target(32, 256),
        Target(32, 512),
        Target(32, 1024),
        Target(32, 2048),
        Target(32, 4096),
        Target(32, 8192),
        Target(32, 8192*2),
        Target(32, 8192*4),
        Target(32, 1024 * 1024, enableILR=True),
    ]
    
    pipelineTarge = get_mapping_target()

    if args.pipeline:
        outputFolder = os.path.join(outputRootPath, "pipeline", modelName)
        # model.setSpmKeepAllConv([1, 1, 1, 1])
        # model.setSpmKeepAllResadd([1, 1, 1])
        # model.setDramKeepAllConv([0, 0, 0, 0])
        # model.setDramKeepAllResadd([0, 0, 0])
    else:
        if Layer.BATCH_SIZE != 1:
            outputFolder = os.path.join(outputRootPath, f"batch{Layer.BATCH_SIZE}", modelName)
        else:   
            outputFolder = os.path.join(outputRootPath, modelName)

    if not os.path.exists(outputFolder):
        os.makedirs(outputFolder)
        
    model.write(os.path.join(outputFolder, "layers.yaml"))
    model.dump(os.path.join(outputFolder, "layers.dump"))
    # exit()

    mappingFolder = os.path.join(outputFolder, "mapping")
    if os.path.exists(mappingFolder):
        os.system(f"rm -rf {mappingFolder}")
    os.makedirs(mappingFolder)

    if args.pipeline:
        hm = HybridMapper(model.getProblemList(), pipelineTarge, mappingFolder)
        hm.run()
        
    else:
        hm = HybridMapper(model.getProblemList(), targetList, mappingFolder)
        hm.run()
        # exit()

        pp = PostProcess(model, targetList)
        pp.read(outputFolder)
        pp.resetLayerGroupAccelUtil()
        pp.resetLayerGroupSPMTensorAddr()
        pp.write(outputFolder)


if __name__ == "__main__":
    main()
