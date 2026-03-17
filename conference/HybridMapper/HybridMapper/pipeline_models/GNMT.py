from HybridMapper.Model import *


def LSTM(model, N, Hin, Hout, bi, dataId):
    biasId, weightId, inputId, outputId = dataId
    G = 2
    if bi:
        G = 4
    model.appendLayer(LayerConv([N, Hin, Hout * 4, 1, 1, 1, 1, G, 1, 1],
                                [biasId, weightId, inputId, outputId]))


def GNMT(pipeline: bool=False) -> Model:
    N = 1
    S = 256  # sequnce length
    H = 512
    numLayersEncoder = 4
    numLayersDecoder = 4

    model = Model()

    encoderIdList = [[model.reserveDataId() for _ in range(4)] for _ in range(numLayersEncoder + 1)]
    contextId = model.reserveDataId()
    attIdList = [model.reserveDataId() for _ in range(20)]
    decoderIdList = [[model.reserveDataId() for _ in range(4)] for _ in range(numLayersDecoder + 1)]
    classIdList = [model.reserveDataId() for _ in range(3)]

    # keep tensor ids continuous by reusing outputs as the next inputs for skipped ops
    encoderIdList[1][2] = encoderIdList[0][3]
    encoderIdList[2][2] = encoderIdList[1][3]
    decoderIdList[0][2] = contextId
    decoderIdList[1][2] = decoderIdList[0][3]
    attIdList[8] = attIdList[5]
    attIdList[11] = attIdList[9]

    def encoder():
        LSTM(model, N, H, H, True, encoderIdList[0])
        LSTM(model, N, H * 2, H, False, encoderIdList[1])
        for i in range(2, numLayersEncoder):
            LSTM(model, N, H, H, False, encoderIdList[i])
            next_input_id = contextId if i == numLayersEncoder - 1 else encoderIdList[i + 1][2]
            model.appendLayer(LayerResadd([N, H, 1, 1, 1],
                                          [encoderIdList[i][3], encoderIdList[i][2], next_input_id]))

    def attention():
        model.appendLayer(LayerConv([N, H, H, 1, 1, 1, 1, 1, 1, 1],
                                    [attIdList[0], attIdList[1], decoderIdList[0][3], attIdList[2]]))
        model.appendLayer(LayerConv([N, H, H, S, 1, 1, 1, 1, 1, 1],
                                    [attIdList[3], attIdList[4], contextId, attIdList[5]]))
        model.appendLayer(LayerConv([N, H, 1, S, 1, 1, 1, 1, 1, 1],
                                    [attIdList[6], attIdList[7], attIdList[8], attIdList[9]]))
        model.appendLayer(LayerConv([N, H, H, S, 1, 1, 1, 1, 1, 1],
                                    [attIdList[10], contextId, attIdList[11], attIdList[12]]))

    def decoder():
        LSTM(model, N, H, H, False, decoderIdList[0])
        attention()
        for i in range(1, numLayersDecoder):
            LSTM(model, N, H * 2, H, False, decoderIdList[i])
            model.appendLayer(LayerResadd([N, H, 1, 1, 1],
                                          [decoderIdList[i][3], decoderIdList[i][2], decoderIdList[i + 1][2]]))

    # for s in range(S):
    #     encoder()
    # for s in range(S):
    #     decoder()
    #     model.appendLayer(LayerConv([N, H, 32000, 1, 1, 1, 1, 1, 1, 1],
    #                                 [classIdList[0], classIdList[1], decoderIdList[-1][2], classIdList[2]]))

    # note: only infer one token instead of the whole sequence
    encoder()
    decoder()
    # model.appendLayer(LayerConv([N, H, 32000, 1, 1, 1, 1, 1, 1, 1],
    #                             [classIdList[0], classIdList[1], decoderIdList[-1][2], classIdList[2]]))

    return model
