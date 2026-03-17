from HybridMapper.Model import *


def ONet(pipeline: bool=False) -> Model:
    N = 1
    P = 320
    # P = 2048

    model = Model()

    def encoderBlock(M, Cin, Ch, Cout, iId):
        model.appendLayer(LayerConv([N, Cin, Ch, M, 1, 1, 1, 1, 1, 1], [-1, -1, iId, iId + 1]))
        model.appendLayer(LayerConv([N, Ch, Cout, M, 1, 1, 1, 1, 1, 1], [-1, -1, iId + 1, iId + 2]))
        if Cin != Cout:
            model.appendLayer(LayerConv([N, Cin, Cout, M, 1, 1, 1, 1, 1, 1], [-1, -1, iId, iId + 3]))
            model.appendLayer(LayerResadd([N, Cout, M, 1, 1], [iId + 2, iId + 3, iId + 4]))
            return iId + 4
        else:
            model.appendLayer(LayerResadd([N, Cout, M, 1, 1], [iId, iId + 2, iId + 3]))
            return iId + 3

    def decoderBlock(M, Cin, Ch, Cout, iId, middleId):
        model.appendLayer(LayerConv([N, Cin, Cin, M, 1, 1, 1, 1, 1, 1], [-1, -1, middleId, iId + 1]))
        model.appendLayer(LayerConv([N, Cin, Cin, M, 1, 1, 1, 1, 1, 1], [-1, -1, middleId, iId + 2]))
        model.appendLayer(LayerConv([N, Cin, Ch, M, 1, 1, 1, 1, 1, 1], [-1, -1, iId + 3, iId + 4]))
        model.appendLayer(LayerConv([N, Cin, Ch, M, 1, 1, 1, 1, 1, 1], [-1, -1, middleId, iId + 5]))
        model.appendLayer(LayerConv([N, Cin, Ch, M, 1, 1, 1, 1, 1, 1], [-1, -1, middleId, iId + 6]))
        model.appendLayer(LayerConv([N, Ch, Cout, M, 1, 1, 1, 1, 1, 1], [-1, -1, iId + 7, iId + 8]))
        if Cin != Cout:
            model.appendLayer(LayerConv([N, Cin, Cout, M, 1, 1, 1, 1, 1, 1], [-1, -1, iId, iId + 9]))
            model.appendLayer(LayerResadd([N, Cout, M, 1, 1], [iId + 8, iId + 9, iId + 10]))
            return iId + 10
        else:
            model.appendLayer(LayerResadd([N, Cout, M, 1, 1], [iId, iId + 8, iId + 9]))
            return iId + 9

    def PointNet(M, Cin, Ch, Cout, iId):
        model.appendLayer(LayerConv([N, Cin, 2 * Ch, M, 1, 1, 1, 1, 1, 1], [-1, -1, iId, iId + 1]))
        id_ = encoderBlock(M, 2 * Ch, Ch, Ch, iId + 1)
        id_ = encoderBlock(M, 2 * Ch, Ch, Ch, id_ + 1)
        id_ = encoderBlock(M, 2 * Ch, Ch, Ch, id_ + 1)
        id_ = encoderBlock(M, 2 * Ch, Ch, Ch, id_ + 1)
        id_ = encoderBlock(M, 2 * Ch, Ch, Ch, id_ + 1)
        model.appendLayer(LayerConv([N, Ch, Cout, M, 1, 1, 1, 1, 1, 1], [-1, -1, id_ + 1, id_ + 2]))
        return id_ + 2

    def Decoder(M, Cin, Ch, Cout, iId, middleId):
        model.appendLayer(LayerConv([N, Cin, Ch, M, 1, 1, 1, 1, 1, 1], [-1, -1, iId, iId + 1]))
        id_ = decoderBlock(M, Ch, Ch, Ch, iId + 1, middleId)
        id_ = decoderBlock(M, Ch, Ch, Ch, id_ + 1, middleId)
        id_ = decoderBlock(M, Ch, Ch, Ch, id_ + 1, middleId)
        id_ = decoderBlock(M, Ch, Ch, Ch, id_ + 1, middleId)
        id_ = decoderBlock(M, Ch, Ch, Ch, id_ + 1, middleId)
        model.appendLayer(LayerConv([N, Ch, Cout, M, 1, 1, 1, 1, 1, 1], [-1, -1, id_, id_ + 1]))
        return id_ + 1

    middleId = PointNet(P, 3, 512, 512, 0)
    Decoder(P, 512, 512, 1, 1000, middleId)

    return model
