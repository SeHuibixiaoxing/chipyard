from HybridMapper.Model import *
import torchvision.models.efficientnet as effn
import copy
from typing import Any, Callable, Dict, List, Optional, Sequence, Tuple, Union


def roundup64(v) -> int:
    if v <= 16:
        return 16
    elif v <= 32:
        return 32
    elif v <= 64:
        return 64
    else:
        return effn._make_divisible(v, 64)


def MBConv(model, cnf: effn.MBConvConfig, N, H, W, iId: int, ir: int = 0):
    input_channels = roundup64(cnf.input_channels)
    expanded_channels = roundup64(cnf.adjust_channels(cnf.input_channels, cnf.expand_ratio))
    output_channels = roundup64(cnf.out_channels)
    stride = cnf.stride
    kernel = cnf.kernel

    assert stride in {1, 2}
    use_res_connect = stride == 1 and input_channels == output_channels
    use_expaned_layer = expanded_channels != input_channels
    print(f"MBConv: ResAdd={use_res_connect}, Expand={use_expaned_layer}, IR={ir}")

    dramKeep = None
    lgType = Layer.LG_TYPE_NONE

    # expand
    id_ = iId
    if use_expaned_layer:
        if ir != 0:
            dramKeep = [1, 1, 1, 0] if (ir == 1 or ir == 4) else [1, 1, 0, 0]
            lgType = Layer.LG_TYPE_HEAD if (ir == 1 or ir == 4) else Layer.LG_TYPE_INTER
        model.appendLayer(LayerConv([N, input_channels, expanded_channels, H, W, 1, 1, 1, 1, 1],
                                    [-1, -1, iId, iId + 1],
                                    dramKeep, lgType))
        print(f"\t0 dramKeep={dramKeep} lgType={lgType}")
        id_ = iId + 1

    # depthwise
    OH, OW = int(H / stride), int(W / stride)
    if ir != 0:
        dramKeep = [1, 1, 1, 0] if ((ir == 1 or ir == 4) and (not use_expaned_layer)) else [1, 1, 0, 0]
        lgType = Layer.LG_TYPE_HEAD if ((ir == 1 or ir == 4) and (not use_expaned_layer)) else Layer.LG_TYPE_INTER
    model.appendLayer(LayerConv([N, 1, 1, OH, OW, kernel, kernel, expanded_channels, stride, stride],
                                [-1, -1, id_, id_ + 1],
                                dramKeep, lgType))
    print(f"\t1 dramKeep={dramKeep} lgType={lgType}")

    # project
    if ir != 0:
        dramKeep = [1, 1, 0, 1] if ((ir == 3 or ir == 4) and (not use_res_connect)) else [1, 1, 0, 0]
        lgType = Layer.LG_TYPE_TAIL if ((ir == 3 or ir == 4) and (not use_res_connect)) else Layer.LG_TYPE_INTER
    model.appendLayer(LayerConv([N, expanded_channels, output_channels, OH, OW, 1, 1, 1, 1, 1],
                                [-1, -1, id_ + 1, id_ + 2],
                                dramKeep, lgType))
    print(f"\t2 dramKeep={dramKeep} lgType={lgType}")

    if use_res_connect:
        if ir != 0:
            dramKeep = [0, 0, 0]
            if ir == 3 or ir == 4:
                dramKeep[2] = 1
            if ir == 1 or ir == 4:
                dramKeep[0] = 1
            lgType = Layer.LG_TYPE_TAIL if (ir == 3 or ir == 4) else Layer.LG_TYPE_INTER
        model.appendLayer(LayerResadd([N, output_channels, OH, OW, 1], [iId, id_ + 2, id_ + 3],
                                      dramKeep, lgType))
        print(f"\t3 dramKeep={dramKeep} lgType={lgType}")
        return id_ + 3, OH, OW
    else:
        return id_ + 2, OH, OW


def FusedMBConv(model, cnf: effn.FusedMBConvConfig, N, H, W, iId: int):
    input_channels = roundup64(cnf.input_channels)
    expanded_channels = roundup64(cnf.adjust_channels(cnf.input_channels, cnf.expand_ratio))
    output_channels = roundup64(cnf.out_channels)
    stride = cnf.stride
    kernel = cnf.kernel

    assert stride in {1, 2}
    use_res_connect = stride == 1 and input_channels == output_channels
    print(f"FusedMBConv: ResAdd={use_res_connect}")

    OH, OW = int(H / stride), int(W / stride)
    if expanded_channels != input_channels:
        # fused expand
        model.appendLayer(LayerConv([N, input_channels, expanded_channels, OH, OW, kernel, kernel, 1,
                                     stride, stride],
                                    [-1, -1, iId, iId + 1]))
        # preject
        model.appendLayer(LayerConv([N, expanded_channels, output_channels, OH, OW, 1, 1, 1, 1, 1],
                                    [-1, -1, iId + 1, iId + 2]))
    else:
        model.appendLayer(LayerConv([N, input_channels, output_channels, OH, OW, kernel, kernel, 1,
                                     stride, stride],
                                    [-1, -1, iId, iId + 2]))
    if use_res_connect:
        model.appendLayer(LayerResadd([N, output_channels, OH, OW, 1], [iId, iId + 2, iId + 3]))
        return iId + 3, OH, OW
    else:
        return iId + 2, OH, OW


def EfficientNet(inverted_residual_setting: Sequence[Union[effn.MBConvConfig, effn.FusedMBConvConfig]],
                 num_classes: int = 1024,
                 last_channel: Optional[int] = None,
                 ir_list: list[int] = None) -> Model:
    N = 1
    H = 224
    W = 224

    model = Model()

    # building first layer
    firstconv_output_channels = roundup64(inverted_residual_setting[0].input_channels)
    OH, OW = int(H / 2), int(W / 2)
    model.appendLayer(LayerConv([N, 3, firstconv_output_channels, OH, OW, 3, 3, 1, 2, 2],
                                [-1, -1, 0, 1]))
    id_ = 1

    # building inverted residual blocks
    ir_list_idx = 0
    for cnf in inverted_residual_setting:
        stage = []
        for _ in range(cnf.num_layers):
            # copy to avoid modifications. shallow copy is enough
            block_cnf = copy.copy(cnf)
            # overwrite info if not the first conv in the stage
            if stage:
                block_cnf.input_channels = block_cnf.out_channels
                block_cnf.stride = 1
            if block_cnf.block == effn.MBConv:
                ir = ir_list[ir_list_idx] if isinstance(ir_list, list) else 0
                ir_list_idx += 1
                id_, OH, OW = MBConv(model, block_cnf, N, OH, OW, id_, ir)
            elif block_cnf.block == effn.FusedMBConv:
                id_, OH, OW = FusedMBConv(model, block_cnf, N, OH, OW, id_)
            else:
                assert False
            stage.append(1)

    # building last several layers
    lastconv_input_channels = inverted_residual_setting[-1].out_channels
    lastconv_output_channels = last_channel if last_channel is not None else 4 * lastconv_input_channels
    model.appendLayer(LayerConv([N, lastconv_input_channels, lastconv_output_channels, OH, OW, 1, 1, 1, 1, 1],
                                [-1, -1, id_, id_ + 1]))

    # avgpool (ignored)

    # classifier
    model.appendLayer(LayerConv([N, lastconv_output_channels, num_classes, 1, 1, 1, 1, 1, 1, 1],
                                [-1, -1, id_ + 2, id_ + 3]))
    return model


def EfficientNetB0(enablePipeline: bool = False) -> Model:
    ir_list = [4,
               4,
               4,
               4,
               4,
               1, 2, 3,
               1, 2, 3,
               1, 2, 2, 2, 3]
    inverted_residual_setting, last_channel = effn._efficientnet_conf("efficientnet_b0",
                                                                      width_mult=1.0, depth_mult=1.0)
    return EfficientNet(inverted_residual_setting, last_channel=last_channel, ir_list=ir_list)


def EfficientNetB1() -> Model:
    inverted_residual_setting, last_channel = effn._efficientnet_conf("efficientnet_b1",
                                                                      width_mult=1.0, depth_mult=1.1)
    return EfficientNet(inverted_residual_setting, last_channel=last_channel)


def EfficientNetB2() -> Model:
    inverted_residual_setting, last_channel = effn._efficientnet_conf("efficientnet_b2",
                                                                      width_mult=1.1, depth_mult=1.2)
    return EfficientNet(inverted_residual_setting, last_channel=last_channel)


def EfficientNetB3() -> Model:
    inverted_residual_setting, last_channel = effn._efficientnet_conf("efficientnet_b3",
                                                                      width_mult=1.2, depth_mult=1.4)
    return EfficientNet(inverted_residual_setting, last_channel=last_channel)


def EfficientNetB4() -> Model:
    inverted_residual_setting, last_channel = effn._efficientnet_conf("efficientnet_b4",
                                                                      width_mult=1.4, depth_mult=1.8)
    return EfficientNet(inverted_residual_setting, last_channel=last_channel)


def EfficientNetB5() -> Model:
    inverted_residual_setting, last_channel = effn._efficientnet_conf("efficientnet_b5",
                                                                      width_mult=1.6, depth_mult=2.2)
    return EfficientNet(inverted_residual_setting, last_channel=last_channel)


def EfficientNetB6() -> Model:
    inverted_residual_setting, last_channel = effn._efficientnet_conf("efficientnet_b6",
                                                                      width_mult=1.8, depth_mult=2.6)
    return EfficientNet(inverted_residual_setting, last_channel=last_channel)


def EfficientNetB7() -> Model:
    inverted_residual_setting, last_channel = effn._efficientnet_conf("efficientnet_b7",
                                                                      width_mult=2.0, depth_mult=3.1)
    return EfficientNet(inverted_residual_setting, last_channel=last_channel)


def EfficientNetV2S() -> Model:
    inverted_residual_setting, last_channel = effn._efficientnet_conf("efficientnet_v2_s")
    return EfficientNet(inverted_residual_setting, last_channel=last_channel)


def EfficientNetV2M() -> Model:
    inverted_residual_setting, last_channel = effn._efficientnet_conf("efficientnet_v2_m")
    return EfficientNet(inverted_residual_setting, last_channel=last_channel)


def EfficientNetV2L() -> Model:
    inverted_residual_setting, last_channel = effn._efficientnet_conf("efficientnet_v2_l")
    return EfficientNet(inverted_residual_setting, last_channel=last_channel)
