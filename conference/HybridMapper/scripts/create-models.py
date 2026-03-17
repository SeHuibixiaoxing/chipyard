import os
import argparse


def main():
    modelList = [
        "resnet50",
        "mobilenetv2",
        "effinetb0",
        "vitb16",
        "vitsmall16",
        "pointpillars",
        "w2v2base",
        "bertbase",
        "berttiny",
        "bertmini",
        "bertsmall",
        "bertmedium",
        "gnmt",

        "resnet18",
        # "ptv3",
        # "onet",
        # "testmodel",
        # "unet",
        # "effinetb0",
        
        # "bert_mha",
        # "bert_ffn"
    ]
    
    parser = argparse.ArgumentParser()
    parser.add_argument("--pipeline",  action="store_true")
    parser.add_argument("--batch-size",  type=int, default = 1)
    args = parser.parse_args()

    for modelName in modelList:
        print(f"\nCreating {modelName}...")
        os.system(f"python scripts/create-model-mapping.py --model {modelName}" + (" --pipeline" if args.pipeline else "") + f" --batch-size {args.batch_size}")


if __name__ == "__main__":
    main()
