from HybridMapper.Model import *


def TestModel(pipeline: bool=False) -> Model:
    model = Model()

    # 0-7: conv
    model.appendLayer(LayerConv([1, 3, 64, 112, 112, 7, 7, 1, 2, 2], [200, 100, 0, 1]))
    model.appendLayer(LayerConv([1, 128, 128, 56, 56, 3, 3, 1, 2, 2], [201, 101, 2, 3]))
    model.appendLayer(LayerConv([1, 256, 256, 28, 28, 3, 3, 1, 2, 2], [202, 102, 4, 5]))
    model.appendLayer(LayerConv([1, 1024, 1024, 7, 7, 3, 3, 1, 2, 2], [203, 103, 6, 7]))
    model.appendLayer(LayerConv([1, 64, 128, 112, 112, 1, 1, 1, 1, 1], [204, 104, 8, 9]))
    model.appendLayer(LayerConv([1, 128, 256, 56, 56, 1, 1, 1, 1, 1], [205, 105, 10, 11]))
    model.appendLayer(LayerConv([1, 512, 1024, 14, 14, 1, 1, 1, 1, 1], [206, 106, 12, 13]))
    model.appendLayer(LayerConv([1, 4096, 1024, 1, 1, 1, 1, 1, 1, 1], [207, 107, 14, 15]))

    # 8-14 vit
    model.appendLayer(LayerConv([1, 3, 768, 14, 14, 16, 16, 1, 16, 16], [400, 300, 30, 31]))
    model.appendLayer(LayerConv([200, 768, 2304, 1, 1, 1, 1, 1, 1, 1], [401, 301, 32, 33]))
    model.appendLayer(LayerConv([200, 64, 200, 1, 1, 1, 1, 12, 1, 1], [402, 302, 34, 35]))
    model.appendLayer(LayerConv([200, 200, 64, 1, 1, 1, 1, 12, 1, 1], [403, 303, 36, 37]))
    model.appendLayer(LayerConv([200, 768, 768, 1, 1, 1, 1, 1, 1, 1], [404, 304, 38, 39]))
    model.appendLayer(LayerConv([200, 768, 3072, 1, 1, 1, 1, 1, 1, 1], [405, 305, 40, 41]))
    model.appendLayer(LayerConv([200, 3072, 768, 1, 1, 1, 1, 1, 1, 1], [406, 306, 42, 43]))

    # 15-18 dw-conv
    model.appendLayer(LayerConv([1, 1, 1, 112, 112, 7, 7, 64, 2, 2], [600, 500, 70, 71]))
    model.appendLayer(LayerConv([1, 1, 1, 56, 56, 3, 3, 128, 2, 2], [601, 501, 72, 73]))
    model.appendLayer(LayerConv([1, 1, 1, 14, 14, 3, 3, 512, 2, 2], [602, 502, 74, 75]))
    model.appendLayer(LayerConv([1, 1, 1, 7, 7, 3, 3, 1024, 2, 2], [603, 503, 76, 79]))

    # 19-20 Occupancy Network
    model.appendLayer(LayerConv([1, 512, 1024, 2048, 1, 1, 1, 1, 1, 1], [-1, -1, -1, -1]))
    model.appendLayer(LayerConv([1, 1024, 512, 2048, 1, 1, 1, 1, 1, 1], [-1, -1, -1, -1]))

    # 21-24 ResAdd
    model.appendLayer(LayerResadd([1, 256, 56, 56, 1], [-1, -1, -1]))
    model.appendLayer(LayerResadd([1, 2048, 14, 14, 1], [-1, -1, -1]))
    model.appendLayer(LayerResadd([64, 1024, 7, 7, 1], [-1, -1, -1]))
    model.appendLayer(LayerResadd([64, 1, 7, 7, 1024], [-1, -1, -1]))

    return model
