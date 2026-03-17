#ifndef MUDNACSIM_LAYER_RESADD_H
#define MUDNACSIM_LAYER_RESADD_H


#include "runtime.h"


namespace mudnac {


    class LayerResadd : public Layer {
    public:
        static const uint iN = 0, iC = 1, iH = 2, iW = 3, iG = 4;

        uint N, C, H, W, G;
        Tensor &input0, &input1, &output;
        Tensor &input0_2, &input1_2, &output_2;
        TensorId &input0Id, &input1Id, &outputId;

        LayerResadd() : Layer("resadd", 3),
                        input0(tensorList[0]), input1(tensorList[1]), output(tensorList[2]),
                        input0_2(tensorList2[0]), input1_2(tensorList2[1]), output_2(tensorList2[2]),
                        input0Id(tensorIds[0]), input1Id(tensorIds[1]), outputId(tensorIds[2]) {}

        void setDim(uint N_, uint C_, uint H_, uint W_, uint G_) {
            N = N_, C = C_, H = H_, W = W_, G = G_;
        }

        void setParam(const std::vector<uint> &v) {
            assert(v.size() == 5);
            paramList = v;
            setDim(v[0], v[1], v[2], v[3], v[4]);
        }
        
        std::vector<uint64> getTensorShape(TensorId tensorId) {
            if(tensorId == input0Id) {
                return {H, W, N, G, C};
            }
            else if(tensorId == input1Id) {
                return {H, W, N, G, C};
            }
            else if(tensorId == outputId) {
                return {H, W, N, G, C};
            }
            else {
                assert(false);
            }
        }

        std::vector<uint64> getTensorStrides(TensorId tensorId) override {
            if(tensorId == input0Id) {
                return input0.strides_;
            }
            else if(tensorId == input1Id) {
                return input1.strides_;
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
            assert(accSet.size() >= mapping->accUtil);
            LoopNest &ln = mapping->ln;
            ln.reset();
//            printf("SPM alloc: %lu (required: %lu)\n",
//                   RT.gv.threadSpmPageAlloc[tid].size() * RT.hw.pageBytes, mapping->spmUtil);

            // SPM-level tile dimensions.
            uint spmN = mapping->spmDimensions[iN];
            uint spmC = mapping->spmDimensions[iC];
            uint spmH = mapping->spmDimensions[iH];
            uint spmW = mapping->spmDimensions[iW];
            uint spmG = mapping->spmDimensions[iG];
            // SPM-level tile strides.
            Tensor::Vector spmStrides = {spmW * spmN * spmG * spmC * RT.hw.inputWordBytes,
                                         spmN * spmG * spmC * RT.hw.inputWordBytes,
                                         spmG * spmC * RT.hw.inputWordBytes,
                                         spmC * RT.hw.inputWordBytes,
                                         RT.hw.inputWordBytes};
            // SPM-level tensors.
            Tensor spmInput0(mapping->spmTensorAddr[0], spmStrides);
            Tensor spmInput1(mapping->spmTensorAddr[1], spmStrides);
            Tensor spmOutput(mapping->spmTensorAddr[2], spmStrides);

            uint acc = 0;
            while (ln.valid) {
                // Create a new command.
                AccelTiledResadd *cmd = new AccelTiledResadd;
                cmd->setDim(mapping->accDimensions);
                cmd->setTensors(input0, input1, output, ln.dists);

                // If it's SPM system, set additional info for the command
                if (RT.isSPMSystem()) {
                    // Get in-SPM dimensions of the current accel-level tile
                    std::vector<uint64> inSpmDims;
                    ln.getRelativeDistance(inSpmDims, 1);
                    for (int i = 0; i < 5; i++) {
                        assert((inSpmDims[i] + mapping->accDimensions[i]) <= mapping->spmDimensions[i]);
                    }
//                    printf("inSpmDims: %s\n", mudnac::toString(inSpmDims).c_str());
                    std::vector<uint64> offset = {inSpmDims[iH],
                                                  inSpmDims[iW],
                                                  inSpmDims[iN],
                                                  inSpmDims[iG],
                                                  inSpmDims[iC]};
                    // Input0
                    spmInput0.getOffsetTensor(offset, cmd->spmInput0.tensor);
                    cmd->spmInput0.bypass = mapping->spmBypass[0];
                    cmd->spmInput0.reuse = mapping->dramBypass[0];
                    // Input1
                    spmInput1.getOffsetTensor(offset, cmd->spmInput1.tensor);
                    cmd->spmInput1.bypass = mapping->spmBypass[1];
                    cmd->spmInput1.reuse = mapping->dramBypass[1];
                    // Output
                    spmOutput.getOffsetTensor(offset, cmd->spmOutput.tensor);
                    cmd->spmOutput.bypass = mapping->spmBypass[2];
                    cmd->spmOutput.reuse = mapping->dramBypass[2];
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
                } else {
                    acc++;
                }
            }
            assert(acc == 0);
        }
    };


} // mudnac


#endif //MUDNACSIM_LAYER_RESADD_H
