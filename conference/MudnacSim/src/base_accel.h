#ifndef MUDNACSIM_BASE_ACCEL_H
#define MUDNACSIM_BASE_ACCEL_H


#include "tools.h"
#include "tensor.h"


namespace mudnac {


    class AccelConfig {
    public:
        static const uint SPM_ADDR_TYPE_BLOCK_INTERLEAVED = 10;
        static const uint SPM_ADDR_TYPE_PAGE_INTERLEAVED = 20;
        static const uint SPM_ADDR_TYPE_CONTINUOUS = 30;

        uint accId;
        uint arrayH, arrayW;
        uint beatBytes;
        uint maxBurstBytes;

        uint nBanks;
        uint nPages;
        uint pageBytes;

        uint inputBufBytes, weightBufBytes, psumBufBytes;
        uint inputWordBytes, psumWordBytes;

        uint spmAddrType = SPM_ADDR_TYPE_BLOCK_INTERLEAVED;
        uint numAccels = 16;

        AccelConfig(uint accId_, uint arrayH_, uint arrayW_, uint beatBytes_, uint maxBurstBytes_,
                    uint nBanks_, uint nPages_, uint pageBytes_,
                    uint inputBufBytes_, uint weightBufBytes_, uint psumBufBytes_,
                    uint inputWordBytes_, uint psumWordBytes_) {
            accId = accId_, arrayH = arrayH_, arrayW = arrayW_, beatBytes = beatBytes_, maxBurstBytes = maxBurstBytes_;
            nBanks = nBanks_, nPages = nPages_, pageBytes = pageBytes_;
            inputBufBytes = inputBufBytes_, weightBufBytes = weightBufBytes_, psumBufBytes = psumBufBytes_;
            inputWordBytes = inputWordBytes_, psumWordBytes = psumWordBytes_;
        }

        AccelConfig(uint accId_, uint arrayH_, uint arrayW_, uint beatBytes_, uint maxBurstBytes_,
                    uint nBanks_, uint nPages_, uint pageBytes_)
                : AccelConfig(accId_, arrayH_, arrayW_, beatBytes_, maxBurstBytes_,
                              nBanks_, nPages_, pageBytes_,
                              32 * 1024, 32 * 1024, 64 * 1024,
                              1, 4) {}

        AccelConfig(uint accId_, uint arrayH_, uint arrayW_, uint beatBytes_, uint maxBurstBytes_, uint nBanks_)
                : AccelConfig(accId_, arrayH_, arrayW_, beatBytes_, maxBurstBytes_, nBanks_,
                              1024, 32 * 1024) {}

        AccelConfig() : AccelConfig(0, 0, 0, 0, 0, 0) {}
    };


    class SPMTensor {
    public:
        Tensor tensor;  // tensor layout in SPM
        bool bypass = false;  // bypass SPM
        bool reuse = false;  // reuse data in SPM (no need for memory access)
        uint groupId = 0;
        uint groupSize = 1;

        void set(const Tensor &tensor_, bool bypass_, bool reuse_,
                 uint groupId_, uint groupSize_) {
            tensor = tensor_;
            bypass = bypass_, reuse = reuse_, groupId = groupId_, groupSize = groupSize_;
        }

        void set(const Tensor &tensor_, bool bypass_, bool reuse_) {
            set(tensor_, bypass_, reuse_, 0, 1);
        }

        void set(bool bypass_, bool reuse_, uint groupId_, uint groupSize_) {
            bypass = bypass_, reuse = reuse_, groupId = groupId_, groupSize = groupSize_;
        }

        void set(bool bypass_, bool reuse_) {
            set(bypass_, reuse_, 0, 1);
        }

        std::string toString() {
            std::string s;
            s += "SPMTensor(addr=" + std::to_string(tensor.addr_) +
                 ",strides=" + mudnac::toString((tensor.strides_)) +
                 ",bypass=" + std::to_string(bypass) +
                 ",reuse=" + std::to_string(reuse) +
                 ",groupId=" + std::to_string(groupId) +
                 ",groupSize=" + std::to_string(groupSize) + ")";
            return s;
        }
    };


    class AccelTiledCMD {
    public:
        uint numInputs, numOutputs;

        std::deque<Tensor> inputTensors;
        std::deque<Tensor> outputTensors;

        std::deque<SPMTensor> inputSPMTensors;
        std::deque<SPMTensor> outputSPMTensors;

        std::deque<uint> inputReuse;
        std::deque<uint> outputReuse;

        // pipeline运行时使用
        TensorId tensorId;
        uint stageIdx;

        uint64 globalCmdId;
        static uint64 globalCmdCount;

        bool is_pipeline_cmd;

        AccelTiledCMD(uint numInputs_, uint numOutputs_) {
            numInputs = numInputs_;
            numOutputs = numOutputs_;
            for (int i = 0; i < numInputs; i++) {
                inputTensors.emplace_back();
                inputSPMTensors.emplace_back();
                inputReuse.push_back(0);
            }
            for (int i = 0; i < numOutputs; i++) {
                outputTensors.emplace_back();
                outputSPMTensors.emplace_back();
                outputReuse.push_back(0);
            }

            globalCmdId = AccelTiledCMD::globalCmdCount;
            ++ AccelTiledCMD::globalCmdCount;

            is_pipeline_cmd = false;
        }

        virtual ~AccelTiledCMD() = default;

        
        /**
         * @brief Generates the number of computation cycles
         *
         * @param config The configuration of the accelerator.
         * @return The computation cycles.
         */

        virtual uint64 compute(AccelConfig &config) {
             return 0; 
        }
        
        /**
         * @brief Generated memory bursts for an input tensor.
         *
         * @param tensorId The id of the intput tensor to load. It is defined by the derived class.
         * @param config The configuration of the accelerator.
         * @param bQue The burst queue to store the generated memory bursts.
         * @return The number of bytes loaded.
         */
        virtual uint64 load(uint64 tensorId, AccelConfig &config, Tensor::BurstQue &bQue) { return 0; }

        /**
         * @brief Generated memory bursts and their corresponding SPM addresses for an input tensor.
         *
         * @param tensorId The id of the intput tensor to load. It is defined by the derived class.
         * @param config The configuration of the accelerator.
         * @param bQue The burst queue to store the generated memory bursts.
         * @param spmAddrQue Output queue of SPM addresses associated to each burst.
         * @return The number of bytes loaded.
         */
        virtual uint64 load(uint64 tensorId, AccelConfig &config, Tensor::BurstQue &bQue,
                            std::deque<uint64> &spmAddrQue) { return 0; }

        virtual uint64 store(uint64 tensorId, AccelConfig &config, Tensor::BurstQue &bQue) { return 0; }

        virtual uint64 store(uint64 tensorId, AccelConfig &config, Tensor::BurstQue &bQue,
                             std::deque<uint64> &spmAddrQue) { return 0; }
        
        virtual uint64 send(uint64 tensorId, AccelConfig &config, Tensor::BurstQue &bQue, std::deque<uint64> &spmSrcAddrQue, std::deque<uint64> &spmDstAddrQue) { return 0; }
        
        virtual std::string toString() {
            std::deque<uint> accReuse = inputReuse;
            for (auto r: outputReuse) { accReuse.push_back(r); }
            std::string memTensors = "{";
            for (auto &t: inputTensors) { memTensors += t.toString() + ","; }
            for (auto &t: outputTensors) { memTensors += t.toString() + ","; }
            memTensors += "}";
            std::string spmTensors = "{";
            for (auto &t: inputSPMTensors) { spmTensors += t.toString() + ","; }
            for (auto &t: outputSPMTensors) { spmTensors += t.toString() + ","; }
            spmTensors += "}";

            std::stringstream ss;
            ss << "AccelTiledCMD {\n";
            ss << "\tSpec=" << getSpecificInfoStr() << "\n";
            ss << "\tAccReuse=" << mudnac::toString(accReuse) << "\n";
            ss << "\tMemTensors=" << memTensors << "\n";
            ss << "\tSpmTensors=" << spmTensors << "\n";
            ss << "\tglobalCmdId=" << globalCmdId << "\n";
            ss << "}";
            return ss.str();
        };

        virtual std::string getSpecificInfoStr() { return ""; }
    };

    class AccelTiledConv : public AccelTiledCMD {
    public:
        Tensor &bias, &weight, &input, &output;
        SPMTensor &spmBias, &spmWeight, &spmInput, &spmOutput;

        uint n, ic, oc, oh, ow, kh, kw, g;
        uint strideH, strideW, KH, KW;

        AccelTiledConv() : AccelTiledCMD(3, 1),
                           bias(inputTensors[0]), weight(inputTensors[1]), input(inputTensors[2]),
                           output(outputTensors[0]),
                           spmBias(inputSPMTensors[0]), spmWeight(inputSPMTensors[1]), spmInput(inputSPMTensors[2]),
                           spmOutput(outputSPMTensors[0]) {
            setReuse(false, false, false, false);
            setDim(1, 1, 1, 1, 1, 1, 1, 1);
            setStride(1, 1);
            setKernel(1, 1);
        }

        inline void setReuse(bool reuseBias, bool reuseWeight, bool reuseInput, bool reuseOutput) {
            inputReuse = {reuseBias, reuseWeight, reuseInput};
            outputReuse = {reuseOutput};
        }

        inline void setDim(uint n_, uint ic_, uint oc_, uint oh_, uint ow_, uint kh_, uint kw_, uint g_ = 1) {
            assert(n_ > 0 and ic_ > 0 and oc_ > 0 and oh_ > 0 and ow_ > 0 and kh_ > 0 and kw_ > 0 and g_ > 0);
            n = n_, ic = ic_, oc = oc_, oh = oh_, ow = ow_, kh = kh_, kw = kw_, g = g_;
        }

        inline void setDim(std::vector<uint64> d) {
            assert(d.size() == 8);
            setDim(d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
        }

        inline void setStride(uint strideH_, uint strideW_) {
            assert(strideH_ > 0 and strideW_ > 0);
            strideH = strideH_, strideW = strideW_;
        }

        inline void setKernel(uint kh_, uint kw_) {
            assert(kh_ > 0 and kw_ > 0);
            KH = kh_, KW = kw_;
        }

        inline void setTensors(Tensor &bias_, Tensor &weight_, Tensor &input_, Tensor &output_,
                               uint n_, uint ic_, uint oc_, uint oh_, uint ow_, uint kh_, uint kw_, uint g_ = 1) {
            bias_.getOffsetTensor({g_, oc_}, bias);
            weight_.getOffsetTensor({kh_, kw_, g_, ic_, oc_}, weight);
            input_.getOffsetTensor({oh_ * strideH, ow_ * strideW, n_, g_, ic_}, input);
            output_.getOffsetTensor({oh_, ow_, n_, g_, oc_}, output);
        }

        inline void setTensors(Tensor &bias_, Tensor &weight_, Tensor &input_, Tensor &output_, std::vector<uint64> d) {
            assert(d.size() == 8);
            setTensors(bias_, weight_, input_, output_,
                       d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
        }

        inline uint ih() { return (oh - 1) * strideH + KH; }

        inline uint iw() { return (ow - 1) * strideW + KW; }

        uint64 compute(AccelConfig &config);

        uint64 load(uint64 tensorId, AccelConfig &config, Tensor::BurstQue &bQue);

        uint64 load(uint64 tensorId, AccelConfig &config, Tensor::BurstQue &bQue,
                    std::deque<uint64> &spmAddrQue);

        uint64 store(uint64 tensorId, AccelConfig &config, Tensor::BurstQue &bQue);

        uint64 store(uint64 tensorId, AccelConfig &config, Tensor::BurstQue &bQue,
                     std::deque<uint64> &spmAddrQue);

        std::string getSpecificInfoStr() {
            return "Conv<" + mudnac::toString(std::vector<uint>{n, ic, oc, oh, ow, kh, kw, g}) + "," +
                   mudnac::toString(std::vector<uint>{strideH, strideW, KH, KW}) + ">";
        }
    };


    class AccelTiledResadd : public AccelTiledCMD {
    public:
        Tensor &input0, &input1, &output;
        SPMTensor &spmInput0, &spmInput1, &spmOutput;

        uint n, c, h, w, g;

        AccelTiledResadd() : AccelTiledCMD(2, 1),
                             input0(inputTensors[0]), input1(inputTensors[1]),
                             output(outputTensors[0]),
                             spmInput0(inputSPMTensors[0]), spmInput1(inputSPMTensors[1]),
                             spmOutput(outputSPMTensors[0]) {
            setDim(1, 1, 1, 1, 1);
        }

        inline void setDim(uint n_, uint c_, uint h_, uint w_, uint g_) {
            assert(n_ > 0 and c_ > 0 and h_ > 0 and w_ > 0 and g_ > 0);
            n = n_, c = c_, h = h_, w = w_, g = g_;
        }

        inline void setDim(std::vector<uint64> d) {
            assert(d.size() == 5);
            setDim(d[0], d[1], d[2], d[3], d[4]);
        }

        inline void setTensors(Tensor &input0_, Tensor &input1_, Tensor &output_,
                               uint n_, uint c_, uint h_, uint w_, uint g_) {
            input0_.getOffsetTensor({h_, w_, n_, g_, c_}, input0);
            input1_.getOffsetTensor({h_, w_, n_, g_, c_}, input1);
            output_.getOffsetTensor({h_, w_, n_, g_, c_}, output);
        }

        inline void setTensors(Tensor &input0_, Tensor &input1_, Tensor &output_, std::vector<uint64> d) {
            assert(d.size() == 5);
            setTensors(input0_, input1_, output_,
                       d[0], d[1], d[2], d[3], d[4]);
        }

        uint64 load(uint64 tensorId, AccelConfig &config, Tensor::BurstQue &bQue);

        uint64 load(uint64 tensorId, AccelConfig &config, Tensor::BurstQue &bQue,
                    std::deque<uint64> &spmAddrQue);

        uint64 store(uint64 tensorId, AccelConfig &config, Tensor::BurstQue &bQue);

        uint64 store(uint64 tensorId, AccelConfig &config, Tensor::BurstQue &bQue,
                     std::deque<uint64> &spmAddrQue);

        std::string getSpecificInfoStr() {
            return "Resadd<" + mudnac::toString(std::vector<uint>{n, c, h, w, g}) + ">";
        }
    };


    class AccelFlushCache : public AccelTiledCMD {
    public:
        uint64 addr;

        AccelFlushCache(uint64 addr_) : AccelTiledCMD(0, 0) { addr = addr_; }
    };


    class AccelSetPageTable : public AccelTiledCMD {
    public:
        uint vPage, pPage;
        bool valid;

        AccelSetPageTable(uint vPage_, uint pPage_, bool valid_) : AccelTiledCMD(0, 0) {
            vPage = vPage_, pPage = pPage_, valid = valid_;
        }
    };
    
    class AccelSPMFetch : public AccelTiledCMD {
        public:
            Tensor &tensor;
            SPMTensor &spmTensor;
            Tensor::Vector shape;
            AccelSPMFetch() : AccelTiledCMD(1 ,0), tensor(inputTensors[0]), spmTensor(inputSPMTensors[0]) {
                inputReuse = {false};
            }


            uint64 load(uint64 tensorId, AccelConfig &config, Tensor::BurstQue &bQue, std::deque<uint64> &spmAddrQue) override;

            std::string toString() override {
                std::stringstream ss;
                ss << AccelTiledCMD::toString() << ", shape=" << mudnac::toString(shape);
                return ss.str();
            }
    };

    class AccelSPMFlush : public AccelTiledCMD {
        public:
            Tensor &tensor;
            SPMTensor &spmTensor;
            Tensor::Vector shape;
    
            AccelSPMFlush() : AccelTiledCMD(0 ,1), tensor(outputTensors[0]), spmTensor(outputSPMTensors[0]) {
                inputReuse = {false};
            }
    
            uint64 store(uint64 tensorId, AccelConfig &config, Tensor::BurstQue &bQue) override;
            uint64 store(uint64 tensorId, AccelConfig &config, Tensor::BurstQue &bQue, std::deque<uint64> &spmAddrQue) override;

            std::string toString() override {
                std::stringstream ss;
                ss << AccelTiledCMD::toString() << ", shape=" << mudnac::toString(shape);
                return ss.str();
            }
    };

    class AccelSPMSend : public AccelTiledCMD {
        public:
            SPMTensor &spmTensorSrc, &spmTensorDst;
            Tensor::Vector shape;
            AccelSPMSend() : AccelTiledCMD(1, 1), spmTensorSrc(inputSPMTensors[0]), spmTensorDst(outputSPMTensors[0]) {
                inputReuse = {false};
                outputReuse = {false};
            }

            uint64 send(uint64 tensorId, AccelConfig &config, Tensor::BurstQue &bQue, std::deque<uint64> &spmSrcAddrQue, std::deque<uint64> &spmDstAddrQue) override;

            std::string toString() override {
                std::stringstream ss;
                ss << AccelTiledCMD::toString() << ", shape=" << mudnac::toString(shape);
                return ss.str();
            }


    };


    class BaseAccel : public TickModule {
    public:
        class Counter {
        public:
            class TimeSlice {
            public:
                uint64 startCycles, endCycles, storeCycles, loadCycles, computeCycles;

                void clear() {
                    startCycles = endCycles = storeCycles = loadCycles = computeCycles = 0;
                }

                inline uint64 getCycles() { return endCycles - startCycles; }

                inline double getMemoryAccessRatio() { return (double) (loadCycles + storeCycles) / getCycles(); }

                inline double getComputeRatio() { return (double) computeCycles / getCycles(); }
            };

            uint64 idealLoadBytes, idealStoreBytes, realLoadBytes, realStoreBytes, idealFetchBytes, idealFlushBytes, idealSendBytes, realFetchBytes, realFlushBytes, realSendBytes;
            uint64 sendBytes, flushBytes, fetchBytes;
            std::deque<TimeSlice> tsQueue;
            std::deque<TimeSlice> sendFlushQueue;
            std::deque<TimeSlice> fetchQueue;
            uint64 cycles;

            Counter() {
                idealLoadBytes = idealFetchBytes = idealSendBytes = 0;
                idealStoreBytes = idealFlushBytes = 0;
                realLoadBytes = realFetchBytes = realSendBytes = 0;
                realStoreBytes = realFlushBytes = 0;

                sendBytes = 0;
                flushBytes = 0;
                fetchBytes = 0;

                cycles = 0;
            }

            inline void tick() { cycles++; }

            uint64 getTotalMemoryAccessCycles() {
                uint64 c = 0;
                for (auto &ts: tsQueue) {
                    c += ts.storeCycles + ts.loadCycles;
                }
                return c;
            }

            uint64 getTotalComputeCycles() {
                uint64 c = 0;
                for (auto &ts: tsQueue) {
                    c += ts.computeCycles;
                }
                return c;
            }

            uint64 getTotalBusyCycles() {
                uint64 c = 0;
                for (auto &ts: tsQueue) {
                    c += ts.getCycles();
                }
                return c;
            }

            double getAvgMemoryAccessRatio() { return (double) getTotalMemoryAccessCycles() / cycles; }

            double getAvgComputeRatio() { return (double) getTotalComputeCycles() / cycles; }

            double getMaxMemoryAccessRatio() {
                double m = 0;
                for (auto &ts: tsQueue) {
                    m = std::max(m, ts.getMemoryAccessRatio());
                }
                return m;
            }

            double getMaxComputeAccessRatio() {
                double m = 0;
                for (auto &ts: tsQueue) {
                    m = std::max(m, ts.getComputeRatio());
                }
                return m;
            }

            double getMinMemoryAccessRatio() {
                double m = DBL_MAX;
                for (auto &ts: tsQueue) {
                    m = std::min(m, ts.getMemoryAccessRatio());
                }
                return m;
            }

            double getMinComputeRatio() {
                double m = DBL_MAX;
                for (auto &ts: tsQueue) {
                    m = std::min(m, ts.getComputeRatio());
                }
                return m;
            }

            double getAvgBW() { return (double) (realStoreBytes + realLoadBytes) / cycles; }

            std::string getPerformanceInfo() {
                std::stringstream ss;
                ss << "\tLoad Bytes: " << realLoadBytes << "\n";
                ss << "\tStore Bytes: " << realStoreBytes << "\n";
                ss << "\tTotal Bytes: " << realLoadBytes + realStoreBytes << "\n";
                ss << "\tMemory Bandwidth: " << getAvgBW() << "\n";
                ss << "\tBusy Ratio: " << (double) getTotalBusyCycles() / cycles << "\n";
                ss << "\tCompute Ratio: " << getAvgComputeRatio() << "\n";
                ss << "\tMemory Access Ratio: " << getAvgMemoryAccessRatio() << "\n";
                return ss.str();
            }
        };

        Counter counter, sendCounter, fetchCounter, flushCounter;;
        AccelConfig config;

        uint64 memAccessCtrlEpoch;
        uint64 memAccessCtrlMaxReqs;
        uint64 memAccessCtrlCycleCounter;
        uint64 memAccessCtrlBytesCounter;

        BaseAccel(const AccelConfig &config_) {
            assert(config_.arrayH > 0 and config_.arrayW > 0);
            assert(config_.beatBytes > 0 and isPowOfTwo(config_.beatBytes));
            assert(config_.maxBurstBytes > 0 and isPowOfTwo(config_.maxBurstBytes));
            assert(config_.nPages > 0);
            assert(config_.pageBytes > 0 and isPowOfTwo(config_.pageBytes));
            assert(config_.nBanks > 0 and isPowOfTwo(config_.nBanks));

            config = config_;

            memAccessCtrlEpoch = UINT64_MAX;
            memAccessCtrlMaxReqs = UINT64_MAX;
            memAccessCtrlCycleCounter = 0;
            memAccessCtrlBytesCounter = 0;
        }

        virtual ~BaseAccel() = default;

        void setMemAccessCtrl(uint64 epoch, uint64 maxReqs) {
            memAccessCtrlEpoch = epoch;
            memAccessCtrlMaxReqs = maxReqs;
        }

        int accel_id_;
    };

} // mudnac


#endif //MUDNACSIM_BASE_ACCEL_H
