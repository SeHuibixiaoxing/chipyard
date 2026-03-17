from HybridMapper.Model import *


def Wave2Vec2(
        extractor_conv_layer_config,
        encoder_pos_conv_kernel,
        encoder_pos_conv_groups,
        encoder_embed_dim,
        encoder_ff_interm_features,
        encoder_num_layers,
        encoder_num_heads,
        interLayerReuseSetting,
) -> Model:
    N = 1
    inputChannels = 1
    audioLength = 20480

    model = Model()

    IC = inputChannels
    IW = audioLength
    iId_ = 0
    for idx, (OC, kernel, stride) in enumerate(extractor_conv_layer_config):
        OW = int(IW / stride)
        dramKeep, lgType = interLayerReuseSetting[idx]
        print(f"convIdx={idx}, IC={IC}, OC={OC}, IW={IW}, OW={OW}, kernel={kernel}, stride={stride}")
        model.appendLayer(LayerConv([N, IC, OC, 1, OW, 1, kernel, 1, 1, stride], [-1, -1, iId_, iId_ + 1],
                                    dramKeep, lgType))
        IC = OC
        IW = OW
        iId_ += 1

    G = encoder_pos_conv_groups
    IC = int(IC / G)
    OC = int(encoder_embed_dim / G)
    kernel = encoder_pos_conv_kernel
    stride = 1
    OW = int(IW / stride)
    dramKeep, lgType = interLayerReuseSetting[-1]
    print(f"positionEncoding, IC={IC}, OC={OC}, IW={IW}, OW={OW}, kernel={kernel}, stride={stride}")
    model.appendLayer(LayerConv([N, IC, OC, 1, OW, 1, kernel, G, 1, stride], [-1, -1, iId_, iId_ + 1],
                                dramKeep, lgType))
    iId_ += 1

    hiddenDim = encoder_embed_dim
    mlpDim = encoder_ff_interm_features
    numHeads = encoder_num_heads
    headDim = int(hiddenDim / numHeads)
    S = OW
    print(f"transformer: hiddenDim={hiddenDim}, mlpDim={mlpDim}, numHeads={numHeads}, headDim={headDim}, S={S}")

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

    for _ in range(encoder_num_layers):
        iId_ = encoderBlock(iId_)

    return model


def Wave2Vec2Base(pipeline: bool=False) -> Model:
    interLayerReuseSetting = [
        ([1, 1, 1, 0], Layer.LG_TYPE_HEAD),
        ([1, 1, 0, 0], Layer.LG_TYPE_INTER),
        ([1, 1, 0, 0], Layer.LG_TYPE_INTER),
        ([1, 1, 0, 1], Layer.LG_TYPE_TAIL),
        ([1, 1, 1, 0], Layer.LG_TYPE_HEAD),
        ([1, 1, 0, 0], Layer.LG_TYPE_INTER),
        ([1, 1, 0, 0], Layer.LG_TYPE_INTER),
        ([1, 1, 0, 1], Layer.LG_TYPE_TAIL)
    ]
    extractor_conv_layer_config = [
        (512, 10, 5),
        (512, 3, 2),
        (512, 3, 2),
        (512, 3, 2),
        (512, 3, 2),
        (512, 2, 2),
        (512, 2, 2)
    ]  # (channels, kernel, stride)
    encoder_pos_conv_kernel = 128
    encoder_pos_conv_groups = 16
    encoder_embed_dim = 768
    encoder_ff_interm_features = 3072
    encoder_num_layers = 12
    encoder_num_heads = 12
    return Wave2Vec2(
        extractor_conv_layer_config,
        encoder_pos_conv_kernel,
        encoder_pos_conv_groups,
        encoder_embed_dim,
        encoder_ff_interm_features,
        encoder_num_layers,
        encoder_num_heads,
        interLayerReuseSetting
    )


def Wave2Vec2Large(pipeline: bool=False) -> Model:
    interLayerReuseSetting = [
        ([1, 1, 1, 0], Layer.LG_TYPE_HEAD),
        ([1, 1, 0, 0], Layer.LG_TYPE_INTER),
        ([1, 1, 0, 0], Layer.LG_TYPE_INTER),
        ([1, 1, 0, 1], Layer.LG_TYPE_TAIL),
        ([1, 1, 1, 0], Layer.LG_TYPE_HEAD),
        ([1, 1, 0, 0], Layer.LG_TYPE_INTER),
        ([1, 1, 0, 0], Layer.LG_TYPE_INTER),
        ([1, 1, 0, 1], Layer.LG_TYPE_TAIL)
    ]
    extractor_conv_layer_config = [
        (512, 10, 5),
        (512, 3, 2),
        (512, 3, 2),
        (512, 3, 2),
        (512, 3, 2),
        (512, 2, 2),
        (512, 2, 2)
    ]  # (channels, kernel, stride)
    encoder_pos_conv_kernel = 128
    encoder_pos_conv_groups = 16
    encoder_embed_dim = 1024
    encoder_ff_interm_features = 4096
    encoder_num_layers = 24
    encoder_num_heads = 16
    return Wave2Vec2(
        extractor_conv_layer_config,
        encoder_pos_conv_kernel,
        encoder_pos_conv_groups,
        encoder_embed_dim,
        encoder_ff_interm_features,
        encoder_num_layers,
        encoder_num_heads,
        interLayerReuseSetting
    )
