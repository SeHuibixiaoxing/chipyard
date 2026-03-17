from HybridMapper.Model import *


def _PointPillars(model):
    num_classes = 3
    num_batches = 1

    H, W = 448, 512

    channels = 9
    features = 8
    num_points = 25
    num_pillars = 150

    def _pillar_encoder(in_channels, out_channels, id_):
        model.appendLayer(LayerConv([num_batches * num_pillars, in_channels, out_channels, num_points,
                                     1, 1, 1, 1, 1, 1],
                                    [-1, -1, id_, id_ + 1]))
        return id_ + 1

    def _backbone(in_channels, out_channels_list: list, num_layers_list: list, id_):
        oh, ow = int(H / 2), int(W / 2)
        for i, num_layers in enumerate(num_layers_list):
            out_channels = out_channels_list[i]
            model.appendLayer(LayerConv([num_batches, in_channels, out_channels, oh, ow, 3, 3, 1, 2, 2],
                                        [-1, -1, id_, id_ + 1],
                                        [1, 1, 1, 0], Layer.LG_TYPE_HEAD))
            id_ += 1
            for k in range(num_layers):
                dramKeep = [1, 1, 0, 0] if k < num_layers - 1 else [1, 1, 0, 1]
                lgType = Layer.LG_TYPE_INTER if k < num_layers - 1 else Layer.LG_TYPE_TAIL
                model.appendLayer(LayerConv([num_batches, out_channels, out_channels, oh, ow, 3, 3, 1, 1, 1],
                                            [-1, -1, id_, id_ + 1],
                                            dramKeep, lgType))
                id_ += 1
            in_channels = out_channels
            oh, ow = int(oh / 2), int(ow / 2)
        return id_

    def _neck(in_channels_list: list, out_channels, id_):
        oh, ow = int(H / 2), int(W / 2)
        kh, kw = 1, 1
        for i, in_channels in enumerate(in_channels_list):
            model.appendLayer(LayerConv([num_batches, in_channels, out_channels, oh, ow, kh, kw, 1, 1, 1],
                                        [-1, -1, id_, id_ + 1]))  # deconv
            id_ += 1
            kh, kw = kh * 2, kw * 2
        return id_

    def _head(in_channels, n_anchors, n_classes, id_):
        oh, ow = int(H / 2), int(W / 2)
        model.appendLayer(LayerConv([num_batches, in_channels, n_anchors * n_classes, oh, ow, 1, 1, 1, 1, 1],
                                    [-1, -1, id_, id_ + 1]))
        model.appendLayer(LayerConv([num_batches, in_channels, n_anchors * 7, oh, ow, 1, 1, 1, 1, 1],
                                    [-1, -1, id_, id_ + 2]))
        model.appendLayer(LayerConv([num_batches, in_channels, n_anchors * 2, oh, ow, 1, 1, 1, 1, 1],
                                    [-1, -1, id_, id_ + 3]))
        return id_ + 3

    id_ = _pillar_encoder(channels, features, 0)
    id_ = _backbone(features, [features, features * 2, features * 4], [3, 5, 5], id_)
    id_ = _neck([features, features * 2, features * 4], features * 2, id_)
    id_ = _head(features * 2 * 3, num_classes * 2, num_classes, id_)



def PointPillars(pipeline: bool=False) -> Model:
    model = Model()
    _PointPillars(model)
    return model
