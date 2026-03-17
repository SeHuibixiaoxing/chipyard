from HybridMapper.Model import *

def ffn(iId, model, S, N, hiddenDim, mlpDim):
        # model.appendLayer(LayerConv([N, hiddenDim, mlpDim, S, 1, 1, 1, 1, 1, 1], [-1, -1, iId, iId + 1],
        #                             [1, 1, 1, 0], Layer.LG_TYPE_HEAD))
        # model.appendLayer(LayerConv([N, mlpDim, hiddenDim, S, 1, 1, 1, 1, 1, 1], [-1, -1, iId + 1, iId + 2],
        #                             [1, 1, 0, 1], Layer.LG_TYPE_TAIL))
        model.appendLayer(LayerConv([N, hiddenDim, mlpDim, S, 1, 1, 1, 1, 1, 1], [-1, -1, iId, iId + 1],
                                    [1, 1, 1, 0]))
        model.appendLayer(LayerConv([N, mlpDim, hiddenDim, S, 1, 1, 1, 1, 1, 1], [-1, -1, iId + 1, iId + 2],
                                    [1, 1, 0, 1]))
        return iId + 2

def mha(iId, model, S, N, hiddenDim, numHeads, headDim):
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

def encoderBlock(iId, model, S, N, numHeads, hiddenDim, headDim, mlpDim):
    # norm_layer (jumped for now)
    oId_mha = mha(iId + 1, model, S, N, hiddenDim, numHeads, headDim)  # multihead attention
    model.appendLayer(LayerResadd([N, hiddenDim, S, 1, 1], [iId, oId_mha, oId_mha + 1]))  # resadd
    # norm_layer (jumped for now)
    oId_ffn = ffn(oId_mha + 2, model, S, N, hiddenDim, mlpDim)  # ffn
    model.appendLayer(LayerResadd([N, hiddenDim, S, 1, 1], [oId_mha + 1, oId_ffn, oId_ffn + 1]))  # resadd
    return oId_ffn + 1

def BERT(S, N, numLayers, numHeads, hiddenDim) -> Model:
    assert N == 1
    assert numLayers in {12, 24}
    assert numHeads in {12, 16}
    assert hiddenDim in {768, 1024}
    mlpDim = 4 * hiddenDim
    headDim = int(hiddenDim / numHeads)

    model = Model()

    id_ = 0
    for idxLayer in range(numLayers):
        id_ = encoderBlock(id_, model, S, N, numHeads, hiddenDim, headDim, mlpDim)

    return model

def BERT_MHA(pipeline: bool=False):
    N = 1
    S = 256
    numHeads = 12
    hiddenDim = 768
    mlpDim = 4 * hiddenDim
    headDim = int(hiddenDim / numHeads)

    model = Model()

    id_ = 0
    oId_mha = mha(id_, model, S, N, hiddenDim, numHeads, headDim)  # multihead attention
    
    return model
    
def BERT_FFN(pipeline: bool=False):
    N = 1
    S = 256
    numHeads = 12
    hiddenDim = 768
    mlpDim = 4 * hiddenDim
    headDim = int(hiddenDim / numHeads)

    model = Model()

    id_ = 0
    oId_mha = ffn(id_, model, S, N, hiddenDim, mlpDim)
    
    return model

# def BERTBase() -> Model:
#     N = 1
#     # S = 512
#     S = 256
#     return BERT_Pipeline(S, N, 12, 12, 768)


# def BERTLarge() -> Model:
#     N = 1
#     # S = 512
#     S = 256
#     return BERT_Pipeline(S, N, 24, 16, 1024)
