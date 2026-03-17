from HybridMapper.Model import *


def BERT(S, N, numLayers, numHeads, hiddenDim) -> Model:
    assert N == 1
    assert numLayers in  {2, 4, 8, 12, 24}
    assert numHeads in {4, 8, 12, 16}
    assert hiddenDim in {128, 256, 512, 768, 1024}
    assert(hiddenDim % numHeads == 0)
    mlpDim = 4 * hiddenDim
    headDim = int(hiddenDim / numHeads)

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

    id_ = 0
    for idxLayer in range(numLayers):
        id_ = encoderBlock(id_)

    return model

# BERT-tiny，L=2，H=128

# BERT-mini，L=4，H=256

# BERT-small，L=4，H=512

# BERT-medium，L=8，H=512

def BERTTiny(pipeline: bool=False) -> Model:
    N = 1
    # S = 512
    S = 256
    return BERT(S, N, 2, 4, 128)

def BERTMini(pipeline: bool=False) -> Model:
    N = 1
    # S = 512
    S = 256
    return BERT(S, N, 4, 8, 256)

def BERTSmall(pipeline: bool=False) -> Model:
    N = 1
    # S = 512
    S = 256
    return BERT(S, N, 4, 8, 512)

def BERTMedium(pipeline: bool=False) -> Model:
    N = 1
    # S = 512
    S = 256
    return BERT(S, N, 8, 8, 512)

def BERTBase(pipeline: bool=False) -> Model:
    N = 1
    # S = 512
    S = 256
    return BERT(S, N, 12, 12, 768)


def BERTLarge(pipeline: bool=False) -> Model:
    N = 1
    # S = 512
    S = 256
    return BERT(S, N, 24, 16, 1024)
