#ifndef MUDNACSIM_TENSOR_H
#define MUDNACSIM_TENSOR_H


#include "tools.h"


namespace mudnac {


    /**
     * @brief A multi-dimensional tensor, storing its starting address and strides for each dimension.
     * 
     */    
    class Tensor {
    public:
        typedef std::vector<uint64> Vector;
        typedef std::vector<uint64> AddrVector;
        typedef std::deque<std::array<uint64, 2>> BurstQue; // A queue of memory bursts of a range of tensor, each represented as a pair of starting address and size. 

        uint64 addr_;
        Vector strides_;  // [outer dim, ..., inner dim]

        Tensor(uint64 addr, const Vector &strides);

        Tensor() : Tensor(0, {1}) {};

        void set(uint64 addr, const Vector &strides);

        inline uint64 getNumDims() { return strides_.size(); }

        inline uint64 getWordBytes() { return strides_[getNumDims() - 1]; }

        uint64 getOffsetAddress(const Vector &indices);

        void getOffsetTensor(const Vector &indices, Tensor &tensor);

        uint64 getAddrVector(const Vector &indicesLo, const Vector &indicesHi, AddrVector &aVec);

        uint64 getBurstQue(uint maxBurstBytes, const Vector &indicesLo, const Vector &indicesHi, BurstQue &bQue);

        uint64 getBurstQue(uint maxBurstBytes, const Vector &indicesLo, const Vector &indicesHi,
                           BurstQue &bQue, Tensor &spmTensor, std::deque<uint64> &spmAddrQue);

        uint64 getBurstQue(uint maxBurstBytes, const Vector &indicesHi, BurstQue &bQue, Tensor &spmTensor, std::deque<uint64> &spmAddrQue);

        uint64 getBurstQue(uint maxBurstBytes, const Vector &indicesHi, BurstQue &bQue);
        
        nlohmann::json toJson() const {
            nlohmann::json j;
#ifdef JSON_DBG
            j["addr"] = addr_;
            j["strides"] = strides_;
#endif
            return j;
        }

        std::string toString() const {
            return toJson().dump(JSON_DUMP_INDENTATION);
        }
    };


} // mudnac


#endif //MUDNACSIM_TENSOR_H
