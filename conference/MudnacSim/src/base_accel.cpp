#include "base_accel.h"


namespace mudnac {
    uint64 AccelTiledCMD::globalCmdCount = 0;


    /**
     * @brief Compute the cycles for the tiled convolution.
     *
     * This is assuming weight-stationary (WS) dataflow in Gemmini.
     *
     * @param config The accelerator configuration.
     *
     * @return The number of cycles.
     */
    uint64 AccelTiledConv::compute(mudnac::AccelConfig &config) {
        // assuming WS dataflow in Gemmini <<IC, G>, <OC, G>>
        uint nTilesH, nTilesW, nTilesG;
        if (ic >= config.arrayH or oc >= config.arrayW) {
            nTilesH = divCeil(ic, config.arrayH) * kh * kw;
            nTilesW = divCeil(oc, config.arrayW);
            nTilesG = g;
        } else {
            nTilesH = kh * kw;
            nTilesW = 1;
            nTilesG = divCeil(g, std::min(divFloor(config.arrayH, ic), divFloor(config.arrayW, oc)));
        }
        uint nTiles = nTilesH * nTilesW * nTilesG;
        uint temporal = oh * ow * n;
#ifdef BASE_ACCEL_DBG
        printf("AccelTiledConv::compute, nTiles=(%u,%u,%u), temporal=%u\n", nTilesH, nTilesW, nTilesG, temporal);
#endif
        return config.arrayH + ((nTiles - 1) * std::max(temporal, config.arrayH) + temporal) +
               (config.arrayW + config.arrayH);
    }

    uint64 AccelTiledConv::load(uint64 tensorId, AccelConfig &config, Tensor::BurstQue &bQue) {
        if (tensorId == 0) {
            return bias.getBurstQue(config.maxBurstBytes, {0, 0}, {g, oc},
                                    bQue);
        } else if (tensorId == 1) {
            return weight.getBurstQue(config.maxBurstBytes, {0, 0, 0, 0, 0}, {kh, kw, g, ic, oc},
                                      bQue);
        } else if (tensorId == 2) {
            return input.getBurstQue(config.maxBurstBytes, {0, 0, 0, 0, 0}, {ih(), iw(), n, g, ic},
                                     bQue);
        } else {
            assert(false && "tensorId should be 0, 1, or 2 in AccelTiledConv::load");
        }
    }

    uint64 AccelTiledConv::load(uint64 tensorId, AccelConfig &config, Tensor::BurstQue &bQue,
                                std::deque<uint64> &spmAddrQue) {
        if (tensorId == 0) {
            return bias.getBurstQue(config.maxBurstBytes, {0, 0}, {g, oc},
                                    bQue, spmBias.tensor, spmAddrQue);
        } else if (tensorId == 1) {
            return weight.getBurstQue(config.maxBurstBytes, {0, 0, 0, 0, 0}, {kh, kw, g, ic, oc},
                                      bQue, spmWeight.tensor, spmAddrQue);
        } else if (tensorId == 2) {
            return input.getBurstQue(config.maxBurstBytes, {0, 0, 0, 0, 0}, {ih(), iw(), n, g, ic},
                                     bQue, spmInput.tensor, spmAddrQue);
        } else {
            assert(false && "tensorId should be 0, 1, or 2 in AccelTiledConv::load");
        }
    }

    uint64 AccelTiledConv::store(uint64 tensorId, AccelConfig &config, Tensor::BurstQue &bQue) {
        if (tensorId == 0) {
            return output.getBurstQue(config.maxBurstBytes, {0, 0, 0, 0, 0}, {oh, ow, n, g, oc},
                                      bQue);
        } else {
            assert(false);
        }
    }

    uint64 AccelTiledConv::store(uint64 tensorId, AccelConfig &config, Tensor::BurstQue &bQue,
                                 std::deque<uint64> &spmAddrQue) {
        if (tensorId == 0) {
            return output.getBurstQue(config.maxBurstBytes, {0, 0, 0, 0, 0}, {oh, ow, n, g, oc},
                                      bQue, spmOutput.tensor, spmAddrQue);
        } else {
            assert(false);
        }
    }


    uint64 AccelTiledResadd::load(uint64 tensorId, AccelConfig &config, Tensor::BurstQue &bQue) {
        assert(tensorId == 0 or tensorId == 1);
        return inputTensors[tensorId].getBurstQue(config.maxBurstBytes, {0, 0, 0, 0, 0}, {h, w, n, g, c},
                                                  bQue);
    }

    uint64 AccelTiledResadd::load(uint64 tensorId, AccelConfig &config, Tensor::BurstQue &bQue,
                                  std::deque<uint64> &spmAddrQue) {
        assert(tensorId == 0 or tensorId == 1);
        return inputTensors[tensorId].getBurstQue(config.maxBurstBytes, {0, 0, 0, 0, 0}, {h, w, n, g, c},
                                                  bQue, inputSPMTensors[tensorId].tensor, spmAddrQue);
    }

    uint64 AccelTiledResadd::store(uint64 tensorId, AccelConfig &config, Tensor::BurstQue &bQue) {
        assert(tensorId == 0);
        return outputTensors[tensorId].getBurstQue(config.maxBurstBytes, {0, 0, 0, 0, 0}, {h, w, n, g, c},
                                                   bQue);
    }

    uint64 AccelTiledResadd::store(uint64 tensorId, AccelConfig &config, Tensor::BurstQue &bQue,
                                   std::deque<uint64> &spmAddrQue) {
        assert(tensorId == 0);
        return outputTensors[tensorId].getBurstQue(config.maxBurstBytes, {0, 0, 0, 0, 0}, {h, w, n, g, c},
                                                   bQue, outputSPMTensors[tensorId].tensor, spmAddrQue);
    }

    uint64 AccelSPMFetch::load(uint64 tensorId, AccelConfig &config, Tensor::BurstQue &bQue, std::deque<uint64> &spmAddrQue)
    {
        assert(tensorId == 0);        
        return this->tensor.getBurstQue(config.maxBurstBytes, shape, bQue, spmTensor.tensor,spmAddrQue);
    }

    uint64 AccelSPMFlush::store(uint64 tensorId, AccelConfig &config, Tensor::BurstQue &bQue)
    {
        assert(tensorId == 0);
        return this->tensor.getBurstQue(config.maxBurstBytes, shape, bQue);
    }

    uint64 AccelSPMFlush::store(uint64 tensorId, AccelConfig &config, Tensor::BurstQue &bQue, std::deque<uint64> &spmAddrQue)
    {
        assert(tensorId == 0);
        return this->tensor.getBurstQue(config.maxBurstBytes, shape, bQue, spmTensor.tensor, spmAddrQue);
    }

    uint64 AccelSPMSend::send(uint64 tensorId, AccelConfig &config, Tensor::BurstQue &bQue, std::deque<uint64> &spmSrcAddrQue, std::deque<uint64> &spmDstAddrQue)
    {
        assert(tensorId == 0);
        Tensor::BurstQue bQue_t;
        auto sizeSrc = this->spmTensorSrc.tensor.getBurstQue(config.maxBurstBytes, shape, bQue, spmTensorSrc.tensor, spmSrcAddrQue);
        auto sizeDst = this->spmTensorDst.tensor.getBurstQue(config.maxBurstBytes, shape, bQue_t, spmTensorDst.tensor, spmDstAddrQue);
        assert(sizeSrc == sizeDst);
        assert(bQue.size() == bQue_t.size());

        return sizeSrc;
    }

} // mudnac