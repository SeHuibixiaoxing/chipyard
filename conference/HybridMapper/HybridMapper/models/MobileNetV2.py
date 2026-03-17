from HybridMapper.Model import *


def MobileNetV2(pipeline: bool=False) -> Model:
    N = 1

    model = Model()

    def downsampleBlock(IC: int, OC: int, G: int, H: int, W: int, s: int, iId: int, writeBackToDram=False):
        assert s == 1 or s == 2
        if s == 1:
            OH = H
            OW = W
        else:
            assert H % 2 == 0 and W % 2 == 0
            OH = int(H / 2)
            OW = int(W / 2)
        model.appendLayer(LayerConv([N, IC, G, H, W, 1, 1, 1, 1, 1], [-1, -1, iId, iId + 1],
                                    [1, 1, 1, 0], Layer.LG_TYPE_HEAD))
        model.appendLayer(LayerConv([N, 1, 1, OH, OW, 3, 3, G, s, s], [-1, -1, iId + 1, iId + 2],
                                    [1, 1, 0, 0], Layer.LG_TYPE_INTER))
        dramKeep = [1, 1, 0, 1] if writeBackToDram else [1, 1, 0, 0]
        lgType = Layer.LG_TYPE_TAIL if writeBackToDram else Layer.LG_TYPE_INTER
        model.appendLayer(LayerConv([N, G, OC, H, W, 1, 1, 1, 1, 1], [-1, -1, iId + 2, iId + 3],
                                    dramKeep, lgType))
        # print(f"shape changes: {(H, W)} -> {(OH, OW)}")
        return iId + 3, OH, OW

    def resBlock(C: int, G: int, H: int, W: int, iId: int, readFromDram=False, writeBackToDram=False):
        dramKeep = [1, 1, 1, 0] if readFromDram else [1, 1, 0, 0]
        lgType = Layer.LG_TYPE_HEAD if readFromDram else Layer.LG_TYPE_INTER
        model.appendLayer(LayerConv([N, C, G, H, W, 1, 1, 1, 1, 1], [-1, -1, iId, iId + 1],
                                    dramKeep, lgType))
        model.appendLayer(LayerConv([N, 1, 1, H, W, 3, 3, G, 1, 1], [-1, -1, iId + 1, iId + 2],
                                    [1, 1, 0, 0], Layer.LG_TYPE_INTER))
        model.appendLayer(LayerConv([N, G, C, H, W, 1, 1, 1, 1, 1], [-1, -1, iId + 2, iId + 3],
                                    [1, 1, 0, 0], Layer.LG_TYPE_INTER))
        dramKeep = [0, 0, 1] if writeBackToDram else [0, 0, 0]
        if readFromDram:
            dramKeep[0] = 1
        lgType = Layer.LG_TYPE_TAIL if writeBackToDram else Layer.LG_TYPE_INTER
        model.appendLayer(LayerResadd([N, C, H, W, 1], [iId, iId + 3, iId + 4],
                                      dramKeep, lgType))
        return iId + 4

    # model.appendLayer(LayerConv([N, 3, 32, 112, 112, 3, 3, 1, 2, 2], [-1, -1, 0, 1]))
    # model.appendLayer(LayerConv([N, 1, 1, 112, 112, 3, 3, 32, 1, 1], [-1, -1, 1, 2]))
    # model.appendLayer(LayerConv([N, 32, 16, 112, 112, 1, 1, 1, 1, 1], [-1, -1, 2, 3]))
    # interId, interH, interW = downsampleBlock(16, 24, 96, 112, 112, 2, 3)
    # interId = resBlock(24, 144, interH, interW, interId)
    # interId, interH, interW = downsampleBlock(24, 32, 144, interH, interW, 2, interId)
    # interId = resBlock(32, 192, interH, interW, interId)
    # interId = resBlock(32, 192, interH, interW, interId)
    # interId, interH, interW = downsampleBlock(32, 64, 192, interH, interW, 2, interId)
    # interId = resBlock(64, 384, interH, interW, interId)
    # interId = resBlock(64, 384, interH, interW, interId)
    # interId = resBlock(64, 384, interH, interW, interId)
    # interId, interH, interW = downsampleBlock(64, 96, 384, interH, interW, 1, interId)
    # interId = resBlock(96, 576, interH, interW, interId)
    # interId = resBlock(96, 576, interH, interW, interId)
    # interId, interH, interW = downsampleBlock(96, 160, 576, interH, interW, 2, interId)
    # interId = resBlock(160, 960, interH, interW, interId)
    # interId = resBlock(160, 960, interH, interW, interId)
    # interId, interH, interW = downsampleBlock(160, 320, 960, interH, interW, 1, interId)
    # model.appendLayer(LayerConv([N, 320, 1280, interH, interW, 1, 1, 1, 1, 1], [-1, -1, interId, interId + 1]))
    # Pooling
    # model.appendLayer(LayerConv([N, 1280, 1000, 1, 1, 1, 1, 1, 1, 1], [-1, -1, interId + 2, interId + 3]))

    model.appendLayer(LayerConv([N, 3, 32, 112, 112, 3, 3, 1, 2, 2], [-1, -1, 0, 1],
                                [1, 1, 1, 0], Layer.LG_TYPE_HEAD))
    model.appendLayer(LayerConv([N, 1, 1, 112, 112, 3, 3, 32, 1, 1], [-1, -1, 1, 2],
                                [1, 1, 0, 0], Layer.LG_TYPE_INTER))
    model.appendLayer(LayerConv([N, 32, 16, 112, 112, 1, 1, 1, 1, 1], [-1, -1, 2, 3],
                                [1, 1, 0, 1], Layer.LG_TYPE_TAIL))
    interId, interH, interW = downsampleBlock(16, 32, 128, 112, 112, 2, 3, writeBackToDram=True)
    interId = resBlock(32, 192, interH, interW, interId, readFromDram=True, writeBackToDram=True)
    interId, interH, interW = downsampleBlock(32, 32, 192, interH, interW, 2, interId)
    interId = resBlock(32, 192, interH, interW, interId)
    interId = resBlock(32, 192, interH, interW, interId, writeBackToDram=True)
    interId, interH, interW = downsampleBlock(32, 64, 192, interH, interW, 2, interId)
    interId = resBlock(64, 384, interH, interW, interId)
    interId = resBlock(64, 384, interH, interW, interId)
    interId = resBlock(64, 384, interH, interW, interId, writeBackToDram=True)
    interId, interH, interW = downsampleBlock(64, 128, 384, interH, interW, 1, interId)
    interId = resBlock(128, 576, interH, interW, interId)
    interId = resBlock(128, 576, interH, interW, interId, writeBackToDram=True)
    interId, interH, interW = downsampleBlock(128, 192, 576, interH, interW, 2, interId)
    interId = resBlock(192, 960, interH, interW, interId)
    interId = resBlock(192, 960, interH, interW, interId, writeBackToDram=True)
    interId, interH, interW = downsampleBlock(192, 320, 960, interH, interW, 1, interId)
    model.appendLayer(LayerConv([N, 320, 1280, interH, interW, 1, 1, 1, 1, 1], [-1, -1, interId, interId + 1],
                                [1, 1, 0, 1], Layer.LG_TYPE_TAIL))
    # Pooling
    model.appendLayer(LayerConv([N, 1280, 1024, 1, 1, 1, 1, 1, 1, 1], [-1, -1, interId + 2, interId + 3]))

    return model
