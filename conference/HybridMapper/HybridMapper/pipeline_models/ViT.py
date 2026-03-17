from HybridMapper.Model import *


def ViT(N, numLayers, numHeads, patchDim, hiddenDim, mlpDim) -> Model:
    assert N == 1
    assert numLayers in {12, 24, 32}
    assert numHeads in {6, 12, 16, 16}
    assert patchDim in {14, 16, 32}
    assert hiddenDim in {384, 768, 1024, 1280}
    assert mlpDim in {1536, 3072, 4096, 5120}
    headDim = int(hiddenDim / numHeads)
    # IH=IW=224
    if patchDim == 14:
        oh = ow = 16
        S = 260  # 16 * 16 + 1 = 257
    elif patchDim == 16:
        oh = ow = 14
        S = 200  # 14 * 14 + 1 = 197
    else:  # patchDim == 32
        oh = ow = 7
        S = 50  # 7 * 7 + 1 = 50

    model = Model()

    def ffn(iId):
        model.appendLayer(LayerConv([N, hiddenDim, mlpDim, S, 1, 1, 1, 1, 1, 1], [-1, -1, iId, iId + 1],
                                    [1, 1, 1, 0], Layer.LG_TYPE_HEAD))
        model.appendLayer(LayerConv([N, mlpDim, hiddenDim, S, 1, 1, 1, 1, 1, 1], [-1, -1, iId + 1, iId + 2],
                                    [1, 1, 0, 1], Layer.LG_TYPE_TAIL))
        return iId + 2

    def mha(iId):
        model.appendLayer(LayerConv([N, hiddenDim, hiddenDim, S, 1, 1, 1, 1, 1, 1],
                                    [-1, -1, iId, iId + 2],
                                    [1, 1, 1, 0], Layer.LG_TYPE_HEAD))  # Q
        model.appendLayer(LayerConv([N, hiddenDim, hiddenDim, S, 1, 1, 1, 1, 1, 1],
                                    [-1, -1, iId, iId + 3],
                                    [1, 1, 1, 0], Layer.LG_TYPE_INTER))  # K
        model.appendLayer(LayerConv([N, hiddenDim, hiddenDim, S, 1, 1, 1, 1, 1, 1],
                                    [-1, -1, iId, iId + 4],
                                    [1, 1, 1, 0], Layer.LG_TYPE_INTER))  # V
        model.appendLayer(LayerConv([N, headDim, S, S, 1, 1, 1, numHeads, 1, 1],
                                    [-1, iId + 2, iId + 3, iId + 6],
                                    [1, 0, 0, 0], Layer.LG_TYPE_INTER))  # QK
        model.appendLayer(LayerConv([N, S, headDim, S, 1, 1, 1, numHeads, 1, 1],
                                    [-1, iId + 4, iId + 6, iId + 7],
                                    [1, 0, 0, 0], Layer.LG_TYPE_INTER))  # QKV
        model.appendLayer(LayerConv([N, hiddenDim, hiddenDim, S, 1, 1, 1, 1, 1, 1],
                                    [-1, -1, iId + 7, iId + 8],
                                    [1, 1, 0, 1], Layer.LG_TYPE_TAIL))
        return iId + 8

    def encoderBlock(iId):
        # norm_layer (jumped for now)
        oId_mha = mha(iId)  # multihead attention
        model.appendLayer(LayerResadd([N, hiddenDim, S, 1, 1], [iId, oId_mha, oId_mha + 1]))  # resadd
        # norm_layer (jumped for now)
        oId_ffn = ffn(oId_mha + 1)  # ffn
        model.appendLayer(LayerResadd([N, hiddenDim, S, 1, 1], [oId_mha + 1, oId_ffn, oId_ffn + 1]))  # resadd
        return oId_ffn + 1

    model.appendLayer(LayerConv([N, 3, hiddenDim, oh, ow, patchDim, patchDim, 1, patchDim, patchDim],
                                [-1, -1, 0, 1]))
    # reshape and add a category token
    id_ = 1
    for idxLayer in range(numLayers):
        id_ = encoderBlock(id_)
    # norm_layer (jumped for now)
    # get the category token and put to the last layer
    model.appendLayer(LayerConv([1, hiddenDim, 1024, 1, 1, 1, 1, 1, 1, 1],
                                [-1, -1, id_, id_ + 1]))  # class_num = 1000

    return model

def ViTSmall16(pipeline: bool=False) -> Model:
    N = 1
    return ViT(N, 12, 6, 16, 384, 1536)


def ViTBase16(pipeline: bool=False) -> Model:
    N = 1
    return ViT(N, 12, 12, 16, 768, 3072)


def ViTBase32(pipeline: bool=False) -> Model:
    N = 1
    return ViT(N, 12, 12, 32, 768, 3072)


def ViTLarge16(pipeline: bool=False) -> Model:
    N = 1
    return ViT(N, 24, 16, 16, 1024, 4096)


def ViTLarge32(pipeline: bool=False) -> Model:
    N = 1
    return ViT(N, 24, 16, 32, 1024, 4096)


def ViTHuge14(pipeline: bool=False) -> Model:
    N = 1
    return ViT(N, 32, 16, 14, 1280, 5120)
