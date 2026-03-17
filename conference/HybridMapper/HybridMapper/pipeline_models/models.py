from HybridMapper.Model import Model
from HybridMapper.pipeline_models.TestModel import TestModel
from HybridMapper.pipeline_models.ResNet18 import ResNet18
from HybridMapper.pipeline_models.ResNet50 import ResNet50
from HybridMapper.pipeline_models.MobileNetV2 import MobileNetV2
from HybridMapper.pipeline_models.ViT import ViTSmall16,ViTBase16, ViTBase32, ViTLarge16, ViTLarge32, ViTHuge14
from HybridMapper.pipeline_models.BERT import BERTBase, BERTLarge, BERTTiny, BERTMini, BERTSmall, BERTMedium
from HybridMapper.pipeline_models.GNMT import GNMT
from HybridMapper.pipeline_models.ONet import ONet
from HybridMapper.pipeline_models.PointTransformerV3 import PointTransformerV3
from HybridMapper.pipeline_models.UNet import UNet
from HybridMapper.pipeline_models.PointPillars import PointPillars
from HybridMapper.pipeline_models.Wave2Vec2 import Wave2Vec2Base, Wave2Vec2Large
from HybridMapper.pipeline_models.PipelineTestModel import BERT_MHA, BERT_FFN


def create(modelName: str, pipeline: bool=False) -> Model:
    modelName = modelName.lower()

    if modelName.startswith("effinet"):
        from HybridMapper.pipeline_models.EfficientNet import (EfficientNetB0, EfficientNetB1, EfficientNetB2, EfficientNetB3,
                                                      EfficientNetB4, EfficientNetB5, EfficientNetB6, EfficientNetB7,
                                                      EfficientNetV2S, EfficientNetV2M, EfficientNetV2L)
        nameToCreator = {
            "effinetb0": EfficientNetB0,
            "effinetb1": EfficientNetB1,
            "effinetb2": EfficientNetB2,
            "effinetb3": EfficientNetB3,
            "effinetb4": EfficientNetB4,
            "effinetb5": EfficientNetB5,
            "effinetb6": EfficientNetB6,
            "effinetb7": EfficientNetB7,
            "effinetv2s": EfficientNetV2S,
            "effinetv2m": EfficientNetV2M,
            "effinetv2l": EfficientNetV2L,
        }
        assert modelName in nameToCreator
        creator = nameToCreator[modelName]
    else:
        nameToCreator = {
            "testmodel": TestModel,
            "resnet18": ResNet18,
            "resnet50": ResNet50,
            "mobilenetv2": MobileNetV2,
            "vitsmall16": ViTSmall16,
            "vitb16": ViTBase16,
            "vitb32": ViTBase32,
            "vitl16": ViTLarge16,
            "vitl32": ViTLarge32,
            "vith14": ViTHuge14,
            "berttiny": BERTTiny,
            "bertmini": BERTMini,
            "bertsmall": BERTSmall,
            "bertmedium": BERTMedium,
            "bertbase": BERTBase,
            "bertlarge": BERTLarge,
            "gnmt": GNMT,
            "onet": ONet,
            "ptv3": PointTransformerV3,
            "unet": UNet,
            "pointpillars": PointPillars,
            "w2v2base": Wave2Vec2Base,
            "w2v2large": Wave2Vec2Large,
            "bert_mha": BERT_MHA,
            "bert_ffn": BERT_FFN,
        }
        assert modelName in nameToCreator
        creator = nameToCreator[modelName]

    return creator(pipeline)
