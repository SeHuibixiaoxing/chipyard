from HybridMapper.Model import *


def ResNet18(pipeline=False) -> Model:
    N = 1

    model = Model()

    def resBlock1(C: int, H: int, W: int, iId: int):
        model.appendLayer(LayerConv([N, C, C, H, W, 3, 3, 1, 1, 1], [-1, -1, iId, iId + 1]))
        model.appendLayer(LayerConv([N, C, C, H, W, 3, 3, 1, 1, 1], [-1, -1, iId + 1, iId + 2]))
        model.appendLayer(LayerResadd([N, C, H, W, 1], [iId, iId + 2, iId + 3]))
        # model.appendLayer(LayerResadd([N, C, H, W, 1], [iId + 3, iId + 2, iId + 4]))
        return iId + 3
        # return iId + 4

    def resBlock2(IC: int, OC: int, OH: int, OW: int, iId: int):
        model.appendLayer(LayerConv([N, IC, OC, OH, OW, 3, 3, 1, 2, 2], [-1, -1, iId, iId + 1]))
        model.appendLayer(LayerConv([N, OC, OC, OH, OW, 3, 3, 1, 1, 1], [-1, -1, iId + 1, iId + 2]))
        model.appendLayer(LayerConv([N, IC, OC, OH, OW, 1, 1, 1, 2, 2], [-1, -1, iId, iId + 3]))
        model.appendLayer(LayerResadd([N, OC, OH, OW, 1], [iId + 2, iId + 3, iId + 4]))
        return iId + 4

    model.appendLayer(LayerConv([N, 3, 64, 112, 112, 7, 7, 1, 2, 2], [-1, -1, 0, 1])) # input:[3,229,229]
    if pipeline:
        # model.appendLayer(LayerPool([N, 64, 56, 56, 3, 3, 2, 2], [1, 2]))
        pass
        
    oId = resBlock1(64, 56, 56, 1 if pipeline else 2) # input: [64,58,58]
    oId = resBlock1(64, 56, 56, oId)
    oId = resBlock2(64, 128, 28, 28, oId)
    oId = resBlock1(128, 28, 28, oId)
    oId = resBlock2(128, 256, 14, 14, oId)
    oId = resBlock1(256, 14, 14, oId)
    oId = resBlock2(256, 512, 7, 7, oId)
    oId = resBlock1(512, 7, 7, oId)
    # model.appendLayer(LayerPool([N * 512, 1, 1, 7, 7, 1, 1], [oId, oId + 1]))  # TODO

    # model.appendLayer(LayerConv([N, 512, 1000, 1, 1, 1, 1, 1, 1, 1], [-1, -1, oId + 1, oId + 2]))  # note: roundup
    oId = oId - 1 if pipeline else oId
    model.appendLayer(LayerConv([N, 512, 1024, 1, 1, 1, 1, 1, 1, 1], [-1, -1, oId + 1, oId + 2]))

    return model
