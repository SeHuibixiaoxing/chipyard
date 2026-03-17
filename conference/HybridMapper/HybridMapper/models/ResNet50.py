from HybridMapper.Model import *


def ResNet50(pipeline: bool=False) -> Model:
    N = 1

    model = Model()

    def resBlock1(IC, innerC, OC, IH, IW, s, iId):
        assert s == 1 or s == 2
        OH, OW = IH, IW
        if s == 2:
            assert IH % 2 == 0 and IW % 2 == 0
            OH, OW = int(IH / 2), int(IW / 2)
        model.appendLayer(LayerConv([N, IC, innerC, OH, OW, 1, 1, 1, s, s], [-1, -1, iId, iId + 1]))
        model.appendLayer(LayerConv([N, innerC, innerC, OH, OW, 3, 3, 1, 1, 1], [-1, -1, iId + 1, iId + 2]))
        model.appendLayer(LayerConv([N, innerC, OC, OH, OW, 1, 1, 1, 1, 1], [-1, -1, iId + 2, iId + 3]))
        model.appendLayer(LayerConv([N, IC, OC, OH, OW, 1, 1, 1, s, s], [-1, -1, iId, iId + 4]))
        model.appendLayer(LayerResadd([N, OC, OH, OW, 1], [iId + 3, iId + 4, iId + 5]))
        return iId + 5, OH, OW

    def resBlock2(C, innerC, H, W, iId, writeBackToDram=False):
        model.appendLayer(LayerConv([N, C, innerC, H, W, 1, 1, 1, 1, 1], [-1, -1, iId, iId + 1]))
        model.appendLayer(LayerConv([N, innerC, innerC, H, W, 3, 3, 1, 1, 1], [-1, -1, iId + 1, iId + 2]))
        model.appendLayer(LayerConv([N, innerC, C, H, W, 1, 1, 1, 1, 1], [-1, -1, iId + 2, iId + 3]))
        dramKeep = [0, 0, 1] if writeBackToDram else [0, 0, 0]
        lgType = Layer.LG_TYPE_TAIL if writeBackToDram else Layer.LG_TYPE_INTER
        model.appendLayer(LayerResadd([N, C, H, W, 1], [iId, iId + 3, iId + 4]))
        return iId + 4

    model.appendLayer(LayerConv([N, 3, 64, 112, 112, 7, 7, 1, 2, 2], [-1, -1, 0, 1]))
    # Pooling
    id_, H_, W_ = resBlock1(64, 64, 256, 56, 56, 1, 2)
    id_ = resBlock2(256, 64, H_, W_, id_)
    id_ = resBlock2(256, 64, H_, W_, id_, writeBackToDram=True)
    id_, H_, W_ = resBlock1(256, 128, 512, H_, W_, 2, id_)
    id_ = resBlock2(512, 128, H_, W_, id_)
    id_ = resBlock2(512, 128, H_, W_, id_)
    id_ = resBlock2(512, 128, H_, W_, id_, writeBackToDram=True)
    id_, H_, W_ = resBlock1(512, 256, 1024, H_, W_, 2, id_)
    id_ = resBlock2(1024, 256, H_, W_, id_)
    id_ = resBlock2(1024, 256, H_, W_, id_)
    id_ = resBlock2(1024, 256, H_, W_, id_)
    id_ = resBlock2(1024, 256, H_, W_, id_)
    id_ = resBlock2(1024, 256, H_, W_, id_, writeBackToDram=True)
    id_, H_, W_ = resBlock1(1024, 512, 2048, H_, W_, 2, id_)
    id_ = resBlock2(2048, 512, H_, W_, id_)
    id_ = resBlock2(2048, 512, H_, W_, id_, writeBackToDram=True)
    # Pooling
    model.appendLayer(LayerConv([N, 2048, 1024, 1, 1, 1, 1, 1, 1, 1], [-1, -1, id_ + 1, id_ + 2]))

    return model
