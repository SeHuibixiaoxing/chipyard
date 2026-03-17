from HybridMapper.pipeline_models import models
from HybridMapper.HybridMapper import *
from HybridMapper.Target import Target
from HybridMapper.PostProcess import PostProcess
from HybridMapper.PipelineTarget import get_mapping_target
import argparse
import os
import subprocess
import sys


def main():
    outputRootPath = "output"
    defaultModelName = "testmodel"

    parser = argparse.ArgumentParser()
    parser.add_argument("--model", type=str, required=False, default=defaultModelName)
    parser.add_argument("--no-mapping", action='store_true', help="Disable mapping generation")

    args = parser.parse_args()
    modelName = args.model

    model = models.create(modelName, True)
    print()
    assert model.checkLayerGroup()
    model.generateMemoryMapping()
    # print(model.getInfoString())
    # exit()
    
    pipelineTarget = get_mapping_target()

    outputFolder = os.path.join(outputRootPath, "pipeline", modelName)
        
    if not os.path.exists(outputFolder):
        os.makedirs(outputFolder)

    model.write(os.path.join(outputFolder, "layers.yaml"))
    model.dump(os.path.join(outputFolder, "layers.dump"))
    # exit()
    model_in_ids, model_out_ids = model.get_model_in_out_ids()
    print(f"model in ids={model_in_ids}")
    print(f"model out ids={model_out_ids}")
    
    gemmini_script = os.path.join(os.path.dirname(__file__), "create-gemmini-pipeline-runtime-artifacts.py")

    if args.no_mapping:
        subprocess.run([sys.executable, gemmini_script, "--model", modelName, "--skip-mapping", "--skip-canonical", "--skip-dummy"], check=True)
        return

    hm = HybridMapper(model.getProblemList(), pipelineTarget, os.path.join(outputFolder, "mapping"))
    hm.run()
    subprocess.run([sys.executable, gemmini_script, "--model", modelName, "--skip-canonical", "--skip-dummy"], check=True)
        


if __name__ == "__main__":
    main()
