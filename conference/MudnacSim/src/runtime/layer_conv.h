#ifndef MUDNACSIM_LAYER_CONV_H
#define MUDNACSIM_LAYER_CONV_H


#include "runtime.h"


namespace mudnac {


    class LayerConv : public Layer {
    public:
        /**
         *  bias: [g, oc]
         *  weight: [kh, kw, g, ic, oc]
         *  input: [ih, iw, n, g, ic]
         *  output: [oh, ow, n, g, oc]
         */
        static const uint iN = 0, iIC = 1, iOC = 2, iOH = 3, iOW = 4, iKH = 5, iKW = 6, iG = 7;

        static const bool isBiasDim(uint d) { return d == iOC or d == iG; }

        static const bool isWeightDim(uint d) { return d == iIC or d == iOC or d == iKH or d == iKW or d == iG; }

        static const bool isInputDim(uint d) { return d == iN or d == iIC or d == iOH or d == iOW or d == iG; }

        static const bool isOutputDim(uint d) { return d == iN or d == iOC or d == iOH or d == iOW or d == iG; }

        static const bool isAccumulatedDim(uint d) { return d == iIC or d == iKH or d == iKW; }

        uint N, IC, OC, OH, OW, KH, KW, G;
        uint strideH, strideW;
        Tensor &bias, &weight, &input, &output;
        Tensor &bias2, &weight2, &input2, &output2;
        TensorId &biasId, &weightId, &inputId, &outputId;

        LayerConv() : Layer("conv", 4),
                      bias(tensorList[0]), weight(tensorList[1]), input(tensorList[2]), output(tensorList[3]),
                      bias2(tensorList2[0]), weight2(tensorList2[1]), input2(tensorList2[2]), output2(tensorList2[3]),
                      biasId(tensorIds[0]), weightId{tensorIds[1]}, inputId(tensorIds[2]), outputId(tensorIds[3]) {}

        void setStride(uint strideH_, uint strideW_) {
            strideH = strideH_, strideW = strideW_;
        }

        void setDim(uint N_, uint IC_, uint OC_, uint OH_, uint OW_, uint KH_, uint KW_, uint G_) {
            N = N_, IC = IC_, OC = OC_, OH = OH_, OW = OW_, KH = KH_, KW = KW_, G = G_;
        }

        void setParam(const std::vector<uint> &v) {
            assert(v.size() == 10);
            paramList = v;
            setDim(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
            setStride(v[8], v[9]);
        }

        inline uint IH() { return (OH - 1) * strideH + KH; }

        inline uint IW() { return (OW - 1) * strideW + KW; }

        // std::vector<TensorId> getInputTensorId() final {
        //     return {biasId, weightId, inputId};
        // }

        // std::vector<TensorId> getOutputTensorId() final {
        //     return {outputId};
        // }

        std::vector<uint> getInputTensorIdxInLayer() {
            return {0, 1, 2};
        }

        std::vector<uint> getOutputTensorIdxInLayer() {
            return {3};
        }

        std::vector<uint64> getTensorShape(TensorId tensorId) override {
            if(tensorId == inputId) {
                return {IH(), IW(), N, G, IC};
            }
            else if(tensorId == weightId) {
                return {KH, KW, G, IC, OC};
            }
            else if(tensorId == biasId) {
                return {G, OC};
            }
            else if(tensorId == outputId) {
                return {OH, OW, N, G, OC};
            }
            else {
                assert(false);
            }
        }

        std::vector<uint64> getTensorStrides(TensorId tensorId) override {
            if(tensorId == inputId) {
                return input.strides_;
            }
            else if(tensorId == weightId) {
                return weight.strides_;
            }
            else if(tensorId == biasId) {
                return bias.strides_;
            }
            else if(tensorId == outputId) {
                return output.strides_;
            }
            else {
                assert(false);
            }
        }

    private:

        void loop(const std::vector<uint>& accSet, bool isPipeline) override {
            assert(tid < 128);
            assert(accSet.size() >= mapping->accUtil);
            LoopNest &ln = mapping->ln;
            ln.reset();
//            printf("SPM alloc: %lu (required: %lu)\n",
//                   RT.gv.threadSpmPageAlloc[tid].size() * RT.hw.pageBytes, mapping->spmUtil);

            // SPM-level tile dimensions
            uint spmN = mapping->spmDimensions[iN];
            uint spmIC = mapping->spmDimensions[iIC];
            uint spmOC = mapping->spmDimensions[iOC];
            uint spmOH = mapping->spmDimensions[iOH];
            uint spmOW = mapping->spmDimensions[iOW];
            uint spmKH = mapping->spmDimensions[iKH];
            uint spmKW = mapping->spmDimensions[iKW];
            uint spmG = mapping->spmDimensions[iG];
            uint spmIH = (spmOH - 1) * strideH + KH;
            uint spmIW = (spmOW - 1) * strideW + KW;
            // SPM-level tile strides
            Tensor::Vector spmBiasStrides = {spmOC * RT.hw.psumWordBytes,
                                             RT.hw.psumWordBytes};
            Tensor::Vector spmWeightStrides = {spmKW * spmG * spmIC * spmOC * RT.hw.inputWordBytes,
                                               spmG * spmIC * spmOC * RT.hw.inputWordBytes,
                                               spmIC * spmOC * RT.hw.inputWordBytes,
                                               spmOC * RT.hw.inputWordBytes,
                                               RT.hw.inputWordBytes};
            Tensor::Vector spmInputStrides = {spmIW * spmN * spmG * spmIC * RT.hw.inputWordBytes,
                                              spmN * spmG * spmIC * RT.hw.inputWordBytes,
                                              spmG * spmIC * RT.hw.inputWordBytes,
                                              spmIC * RT.hw.inputWordBytes,
                                              RT.hw.inputWordBytes};
            Tensor::Vector spmOutputStrides = {spmOW * spmN * spmG * spmOC * RT.hw.inputWordBytes,
                                               spmN * spmG * spmOC * RT.hw.inputWordBytes,
                                               spmG * spmOC * RT.hw.inputWordBytes,
                                               spmOC * RT.hw.inputWordBytes,
                                               RT.hw.inputWordBytes};
            // SPM-level tensors.
            Tensor spmBias(mapping->spmTensorAddr[0], spmBiasStrides);
            Tensor spmWeight(mapping->spmTensorAddr[1], spmWeightStrides);
            Tensor spmInput(mapping->spmTensorAddr[2], spmInputStrides);
            Tensor spmOutput(mapping->spmTensorAddr[3], spmOutputStrides);

            // Group ID offset: 区分不同tid
            // pipeline模式下，一个tid可能并行多个stage，因此，使用层唯一编号groupIdOffset标记
            static const uint MAX_GROUP_ID_OFFSET_EXP = 9;
            // uint groupIdOffset = (isPipeline ? this->groupIdOffsetBase : tid) << MAX_GROUP_ID_OFFSET_EXP;
            // Multi-accel distribution
            uint acc = 0;
            // In-accel psum accumulation.
            std::vector<uint> psumAccumCount(mapping->accUtil, 1);
            // In-accel tile records. Accel index -> set of physical address (size=2 for double buffering)
            std::vector<std::set<uint64>> accBiasBuffer(mapping->accUtil);
            std::vector<std::set<uint64>> accWeightBuffer(mapping->accUtil);
            std::vector<std::set<uint64>> accInputBuffer(mapping->accUtil);
            // In-SPM tile records. Physical address -> multicast count
            std::map<uint64, uint64> spmBiasTileMap;
            std::map<uint64, uint64> spmWeightTileMap;
            std::map<uint64, uint64> spmInputTileMap;
            // Multicast records. Physical address -> group ID
            std::map<uint64, uint> multicastGroupIdMap;

            // ##wzydebug
            std::set<uint> bias_group_id;
            std::set<uint> input_group_id;
            std::set<uint> output_group_id;
            std::set<uint> weight_group_id;

            while (ln.valid) {
                // Compute physical addresses of tiles.
                uint64 biasTilePaddr = bias.getOffsetAddress(
                        {ln.dists[iG], ln.dists[iOC]});
                uint64 weightTilePaddr = weight.getOffsetAddress(
                        {ln.dists[iKH], ln.dists[iKW], ln.dists[iG], ln.dists[iIC], ln.dists[iOC]});
                uint64 inputTilePaddr = input.getOffsetAddress(
                        {ln.dists[iOH] * strideH, ln.dists[iOW] * strideW, ln.dists[iN], ln.dists[iG], ln.dists[iIC]});
                uint64 outputTilePaddr = output.getOffsetAddress(
                        {ln.dists[iOH], ln.dists[iOW], ln.dists[iN], ln.dists[iG], ln.dists[iOC]});
                // std::cout << "wzydebug: ln.dists[iG]=" << ln.dists[iG] << ", ln.dists[iOC]=" << ln.dists[iOC] << ", biasTilePaddr=" << biasTilePaddr << std::endl;
                // std::cout << "wzydebug: ln.dists[iKH]=" << ln.dists[iKH] << ", ln.dists[iKW]=" << ln.dists[iKW] << ", ln.dists[iG]=" << ln.dists[iG] << ", ln.dists[iIC]=" << ln.dists[iIC] << ", ln.dists[iOC]=" << ln.dists[iOC] << ", weightTilePaddr=" << weightTilePaddr << std::endl;
                // std::cout << "wzydebug: ln.dists[iOH]*strideH=" << ln.dists[iOH]*strideH << ", ln.dists[iOW]*strideW=" << ln.dists[iOW]*strideW << ", ln.dists[iN]=" << ln.dists[iN] << ", ln.dists[iG]=" << ln.dists[iG] << ", ln.dists[iIC]=" << ln.dists[iIC] << ", inputTilePaddr=" << inputTilePaddr << std::endl;
                // std::cout << "wzydebug: ln.dists[iOH]=" << ln.dists[iOH] << ", ln.dists[iOW]=" << ln.dists[iOW] << ", ln.dists[iN]=" << ln.dists[iN] << ", ln.dists[iG]=" << ln.dists[iG] << ", ln.dists[iOC]=" << ln.dists[iOC] << ", outputTilePaddr=" << outputTilePaddr << std::endl;
                

                // Create a new command.
                AccelTiledConv *cmd = new AccelTiledConv;
                cmd->setDim(mapping->accDimensions);
                cmd->setStride(strideH, strideW);
                cmd->setKernel(KH, KW);
                cmd->setTensors(bias, weight, input, output, ln.dists);

                // Check in-accel tile reuse
                bool accReuseBias = accBiasBuffer[acc].count(biasTilePaddr);
                if (not accReuseBias) {
                    if (accBiasBuffer[acc].size() == 2) { accBiasBuffer[acc].clear(); }
                    accBiasBuffer[acc].insert(biasTilePaddr);
                }
                bool accReuseWeight = accWeightBuffer[acc].count(weightTilePaddr);
                if (not accReuseWeight) {
                    if (accWeightBuffer[acc].size() == 2) { accWeightBuffer[acc].clear(); }
                    accWeightBuffer[acc].insert(weightTilePaddr);
                }
                bool accReuseInput = accInputBuffer[acc].count(inputTilePaddr);
                if (not accReuseInput) {
                    if (accInputBuffer[acc].size() == 2) { accInputBuffer[acc].clear(); }
                    accInputBuffer[acc].insert(inputTilePaddr);
                }
                bool accReusePsum = psumAccumCount[acc] < mapping->psumAccumCount;
                psumAccumCount[acc] = accReusePsum ? psumAccumCount[acc] + 1 : 1;
                cmd->setReuse(accReuseBias, accReuseWeight, accReuseInput, accReusePsum); // reuse in accel

                if (RT.isSPMSystem()) {
                    // Check in-SPM tile reuse
                    // Bias
                    bool spmReuseBias = false;
                    if (not spmBiasTileMap.count(biasTilePaddr)) {
                        if (spmBiasTileMap.size() == mapping->spmTensorTileCount[0]) { spmBiasTileMap.clear(); }
                        spmBiasTileMap[biasTilePaddr] = 1;
                    } else {
                        spmReuseBias = spmBiasTileMap[biasTilePaddr] == mapping->tensorMulticastCount[0];
                        if (not spmReuseBias) { spmBiasTileMap[biasTilePaddr]++; }
                    }
                    // Weight
                    bool spmReuseWeight = false;
                    if (not spmWeightTileMap.count(weightTilePaddr)) {
                        if (spmWeightTileMap.size() == mapping->spmTensorTileCount[1]) { spmWeightTileMap.clear(); }
                        spmWeightTileMap[weightTilePaddr] = 1;
                    } else {
                        spmReuseWeight = spmWeightTileMap[weightTilePaddr] == mapping->tensorMulticastCount[1];
                        if (not spmReuseWeight) { spmWeightTileMap[weightTilePaddr]++; }
                    }
                    // Input
                    bool spmReuseInput = false;
                    if (not spmInputTileMap.count(inputTilePaddr)) {
                        if (spmInputTileMap.size() == mapping->spmTensorTileCount[2]) { spmInputTileMap.clear(); }
                        spmInputTileMap[inputTilePaddr] = 1;
                    } else {
                        spmReuseInput = spmInputTileMap[inputTilePaddr] == mapping->tensorMulticastCount[2];
                        if (not spmReuseInput) { spmInputTileMap[inputTilePaddr]++; }
                    }

                    // Multicast
                    for (auto paddr: {biasTilePaddr, weightTilePaddr, inputTilePaddr}) {
                        if (not multicastGroupIdMap.count(paddr)) {
                            uint nextGroupId = Layer::getNxtGroupIdx();
                            multicastGroupIdMap[paddr] = nextGroupId;
                        }
                    }
                    assert(multicastGroupIdMap.size() <= (1 << MAX_GROUP_ID_OFFSET_EXP));

                    // Get in-SPM dimensions of the current accel-level tile
                    std::vector<uint64> inSpmDims;
                    ln.getRelativeDistance(inSpmDims, 1);
                    for (int i = 0; i < 8; i++) {
                        assert((inSpmDims[i] + mapping->accDimensions[i]) <= mapping->spmDimensions[i]);
                    }
//                    printf("inSpmDims: %s\n", mudnac::toString(inSpmDims).c_str());

                    // Setup the conmand
                    // Bias
                    spmBias.getOffsetTensor({inSpmDims[iG],
                                             inSpmDims[iOC]},
                                            cmd->spmBias.tensor);
                    cmd->spmBias.bypass = mapping->spmBypass[0];
                    cmd->spmBias.reuse = mapping->dramBypass[0] or spmReuseBias;
                    cmd->spmBias.groupId = multicastGroupIdMap[biasTilePaddr];
                    cmd->spmBias.groupSize = mapping->tensorMulticastCount[0];
                    // Weight
                    spmWeight.getOffsetTensor({inSpmDims[iKH],
                                               inSpmDims[iKW],
                                               inSpmDims[iG],
                                               inSpmDims[iIC],
                                               inSpmDims[iOC]},
                                              cmd->spmWeight.tensor);
                    cmd->spmWeight.bypass = mapping->spmBypass[1];
                    cmd->spmWeight.reuse = mapping->dramBypass[1] or spmReuseWeight;
                    cmd->spmWeight.groupId = multicastGroupIdMap[weightTilePaddr];
                    cmd->spmWeight.groupSize = mapping->tensorMulticastCount[1];
                    // Input
                    spmInput.getOffsetTensor({inSpmDims[iOH] * strideH,
                                              inSpmDims[iOW] * strideW,
                                              inSpmDims[iN],
                                              inSpmDims[iG],
                                              inSpmDims[iIC]},
                                             cmd->spmInput.tensor);
                    cmd->spmInput.bypass = mapping->spmBypass[2];
                    cmd->spmInput.reuse = mapping->dramBypass[2] or spmReuseInput; //dramBypass：不写回dram
                    cmd->spmInput.groupId = multicastGroupIdMap[inputTilePaddr];
                    cmd->spmInput.groupSize = mapping->tensorMulticastCount[2];
                    // Output
                    spmOutput.getOffsetTensor({inSpmDims[iOH],
                                               inSpmDims[iOW],
                                               inSpmDims[iN],
                                               inSpmDims[iG],
                                               inSpmDims[iOC]}, cmd->spmOutput.tensor);
                    cmd->spmOutput.bypass = mapping->spmBypass[3];
                    cmd->spmOutput.reuse = mapping->dramBypass[3];

                    cmd->spmOutput.groupId = 0;
                    cmd->spmOutput.groupSize = 1;
                }

//                printf("%lu %s\n", ln.count, cmd->toString().c_str());
                // Add the command to the accel
                // uint accId = RT.gv.threadAccelAlloc[tid][acc];
                uint accId = accSet[acc];
                RT.addCmd(accId, cmd);
                cmdCounter++;

                // Iterate next
                ln.next();
                if (acc + 1 == mapping->accUtil) {
                    acc = 0;
                    multicastGroupIdMap.clear();
                } else {
                    acc++;
                }
            }
            assert(acc == 0);
        }
    };


} // mudnac

#endif //MUDNACSIM_LAYER_CONV_H
