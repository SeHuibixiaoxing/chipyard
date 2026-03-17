from HybridMapper.Model import *


def UNetBlock(model, N, H, W, in_channels, features, id_in):
    model.appendLayer(LayerConv([N, in_channels, features, H, W, 3, 3, 1, 1, 1],
                                [-1, -1, id_in, id_in + 1]))
    model.appendLayer(LayerConv([N, features, features, H, W, 3, 3, 1, 1, 1],
                                [-1, -1, id_in + 1, id_in + 2]))
    return id_in + 2


def UNet(pipeline: bool=False) -> Model:
    N = 1
    H = 224
    W = 224
    in_channels = 3
    out_channels = 1
    features = 64

    model = Model()

    id_ = UNetBlock(model, N, H, W, in_channels, features, 0)
    # MaxPool2d (ignored)

    id_ = UNetBlock(model, N, int(H / 2), int(W / 2), features, features * 2, id_ + 1)
    # MaxPool2d (ignored)

    id_ = UNetBlock(model, N, int(H / 4), int(W / 4), features * 2, features * 4, id_ + 1)
    # MaxPool2d (ignored)

    id_ = UNetBlock(model, N, int(H / 8), int(W / 8), features * 4, features * 8, id_ + 1)
    # MaxPool2d (ignored)

    id_ = UNetBlock(model, N, int(H / 16), int(W / 16), features * 8, features * 16, id_ + 1)

    model.appendLayer(LayerConv([N, features * 16, features * 8, int(H / 8), int(W / 8), 2, 2, 1, 1, 1],
                                [-1, -1, id_, id_ + 1]))
    id_ = UNetBlock(model, N, int(H / 8), int(W / 8), features * 8 * 2, features * 8, id_ + 2)

    model.appendLayer(LayerConv([N, features * 8, features * 4, int(H / 4), int(W / 4), 2, 2, 1, 1, 1],
                                [-1, -1, id_, id_ + 1]))
    id_ = UNetBlock(model, N, int(H / 4), int(W / 4), features * 4 * 2, features * 4, id_ + 2)

    model.appendLayer(LayerConv([N, features * 4, features * 2, int(H / 2), int(W / 2), 2, 2, 1, 1, 1],
                                [-1, -1, id_, id_ + 1]))
    id_ = UNetBlock(model, N, int(H / 2), int(W / 2), features * 2 * 2, features * 2, id_ + 2)

    model.appendLayer(LayerConv([N, features * 2, features, H, W, 2, 2, 1, 1, 1],
                                [-1, -1, id_, id_ + 1]))
    id_ = UNetBlock(model, N, H, W, features * 2, features, id_ + 2)

    model.appendLayer(LayerConv([N, features, out_channels, H, W, 1, 1, 1, 1, 1],
                                [-1, -1, id_, id_ + 1]))

    return model
