from HybridMapper.Model import *


def PointTransformerV3(pipeline: bool=False) -> Model:
    # patch_size = 1024
    patch_size = 256

    enc_depths = [2, 2, 2, 6, 2]
    enc_channels = [32, 64, 128, 256, 512]
    enc_num_head = [2, 4, 8, 16, 32]
    enc_patch_size = [patch_size, patch_size, patch_size, patch_size, patch_size]
    dec_depths = [2, 2, 2, 2]
    dec_channels = [64, 64, 128, 256] + [enc_channels[-1]]
    dec_num_head = [4, 4, 8, 16]
    dec_patch_size = [patch_size, patch_size, patch_size, patch_size]
    mlp_ratio = 4
    batches = 1

    num_stages = len(enc_depths)

    model = Model()

    def CPE(N, H, K, C, iId):
        # Linear: C -> C
        model.appendLayer(LayerConv([N, C, C, H * K, 1, 1, 1, 1, 1, 1],
                                    [-1, -1, iId, iId + 1],
                                    [1, 1, 1, 0], Layer.LG_TYPE_HEAD))
        return iId + 1

    def Attn(N, H, K, C, iId):
        # Linear QKV, C -> 3C
        model.appendLayer(LayerConv([N, C, C, H * K, 1, 1, 1, 1, 1, 1],
                                    [-1, -1, iId, iId + 2],
                                    [1, 1, 1, 0], Layer.LG_TYPE_HEAD))  # Q
        model.appendLayer(LayerConv([N, C, C, H * K, 1, 1, 1, 1, 1, 1],
                                    [-1, -1, iId, iId + 3],
                                    [1, 1, 1, 0], Layer.LG_TYPE_INTER))  # K
        model.appendLayer(LayerConv([N, C, C, H * K, 1, 1, 1, 1, 1, 1],
                                    [-1, -1, iId, iId + 4],
                                    [1, 1, 1, 0], Layer.LG_TYPE_INTER))  # V
        model.appendLayer(LayerConv([1, C, K, K, 1, 1, 1, H * N, 1, 1],
                                    [-1, iId + 3, iId + 2, iId + 6],
                                    [1, 0, 0, 0], Layer.LG_TYPE_INTER))  # QK
        # softmax (jump for now)
        model.appendLayer(LayerConv([1, K, C, K, 1, 1, 1, H * N, 1, 1],
                                    [-1, iId + 4, iId + 6, iId + 7],
                                    [1, 0, 0, 0], Layer.LG_TYPE_INTER))  # QKV
        # Linear Project, C -> C
        model.appendLayer(LayerConv([N, C, C, H * K, 1, 1, 1, 1, 1, 1],
                                    [-1, -1, iId + 7, iId + 8],
                                    [1, 1, 0, 1], Layer.LG_TYPE_TAIL))
        return iId + 8

    def MLP(N, H, K, Cin, Ch, Cout, iId):
        # Linear
        model.appendLayer(LayerConv([N, Cin, Ch, H * K, 1, 1, 1, 1, 1, 1],
                                    [-1, -1, iId, iId + 1],
                                    [1, 1, 1, 0], Layer.LG_TYPE_HEAD))
        # Linear
        model.appendLayer(LayerConv([N, Ch, Cout, H * K, 1, 1, 1, 1, 1, 1],
                                    [-1, -1, iId + 1, iId + 2],
                                    [1, 1, 0, 1], Layer.LG_TYPE_TAIL))
        return iId + 2

    def Block(N, H, K, C, mlpRatio, iId):
        cpeOutId = CPE(N, H, K, C, iId)
        model.appendLayer(LayerResadd([N, C, H * K, 1, 1], [iId, cpeOutId, cpeOutId + 1],
                                      [1, 0, 1], Layer.LG_TYPE_TAIL))
        # norm: cpeOutId+2 <- cpeOutId+1
        attnOutId = Attn(N, H, K, C, cpeOutId + 2)
        model.appendLayer(LayerResadd([N, C, H * K, 1, 1], [cpeOutId + 1, attnOutId, attnOutId + 1]))
        # norm: attnOutId+2 <- attnOutId+1
        mlpOutId = MLP(N, H, K, C, C * mlpRatio, C, attnOutId + 2)
        model.appendLayer(LayerResadd([N, C, H * K, 1, 1], [attnOutId + 1, mlpOutId, mlpOutId + 1]))
        return mlpOutId + 1

    def SerializedPooling(Cin, Cout, iId):
        return iId + 1

    def SerializedUnpooling(Cin, Cskip, Cout, iId):
        return iId + 1

    id_ = 0
    # Encoder
    for s in range(num_stages):
        if s > 0:
            id_ = SerializedPooling(enc_channels[s - 1], enc_channels[s], id_)
        for i in range(enc_depths[s]):
            id_ = Block(batches, enc_num_head[s], enc_patch_size[s], enc_channels[s], mlp_ratio, id_)
    # Decoder
    for s in reversed(range(num_stages - 1)):
        id_ = SerializedUnpooling(dec_channels[s + 1], enc_channels[s], dec_channels[s], id_)
        for i in range(dec_depths[s]):
            id_ = Block(batches, dec_num_head[s], dec_patch_size[s], dec_channels[s], mlp_ratio, id_)

    return model
