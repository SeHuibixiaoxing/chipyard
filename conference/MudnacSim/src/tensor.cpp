#include "tensor.h"


namespace mudnac {

    Tensor::Tensor(uint64 addr, const Vector &strides) {
        set(addr, strides);
    }

    void Tensor::set(uint64 addr, const mudnac::Tensor::Vector &strides) {
        assert(strides.size() > 0);
        for (int i = 0; i < strides.size(); i++) {
            if (i + 1 < strides.size()) {
                assert(strides[i] >= strides[i + 1]);
            } else {
                assert(strides[i] > 0);
            }
        }

        addr_ = addr;
        strides_ = strides;
    }


    /**
     * @brief Get a set of addresses that cover the range of indices given by [indicesLo, indicesHi].
     * 
     * @param indicesLo lower bound of the indices range (inclusive)
     * @param indicesHi upper bound of the indices range (exclusive)
     * @param aVec output set of addresses
     * 
     * @return the total number of bytes in the output vector
     * 
     * @pre aVec.empty() and indicesLo.size() == getNumDims() and indicesHi.size() == getNumDims()
     * @pre all elements in indicesLo are less than the corresponding elements in indicesHi
     * @post aVec.size() == size and all elements in aVec are valid addresses of the tensor
     */
    uint64 Tensor::getAddrVector(const Vector &indicesLo, const Vector &indicesHi, AddrVector &aVec) {
        assert(aVec.empty());
        assert(indicesLo.size() == getNumDims() and indicesHi.size() == getNumDims());
        uint64 size = 1;
        for (int i = 0; i < getNumDims(); i++) {
            assert(indicesLo[i] < indicesHi[i]);
            size *= indicesHi[i] - indicesLo[i];
        }
        aVec.reserve(size);

        uint64 addr = getOffsetAddress(indicesLo);
        aVec.push_back(addr);
        Vector iter = indicesLo;
        while (aVec.size() < size) {
            for (int i = getNumDims() - 1; i >= 0; i--) {
                if ((iter[i] + 1) < indicesHi[i]) {
                    iter[i]++;
                    addr += strides_[i];
                    aVec.push_back(addr);
                    break;
                } else {
                    iter[i] = indicesLo[i];
                    addr -= (indicesHi[i] - indicesLo[i] - 1) * strides_[i];
                }
            }
        }

#ifdef TENSOR_DBG
        printf("Tensor::getAddrVector, size=%lu, data=[", size);
        for (int i = 0; i < 10; i++) {
            if (i < aVec.size()) {
                printf("%lu,", aVec[i]);
            }
        }
        if (aVec.size() > 10) {
            printf("...,%lu", aVec.back());
        }
        printf("]\n");
#endif

        return size * getWordBytes();
    }

    /**
     * @brief Generates a queue of memory bursts from the tensor addressing range.
     *
     * @param maxBurstBytes Maximum number of bytes per burst (must be a power of two).
     * @param indicesLo Lower bound of the indices range (inclusive).
     * @param indicesHi Upper bound of the indices range (exclusive).
     * @param bQue Output queue of memory bursts, each represented as a pair of starting address and size.
     *
     * @return The total number of bytes in the generated bursts.
     *
     * @details This function calculates and organizes the addresses of the tensor elements
     *          within the specified index range into bursts that do not exceed the specified
     *          maximum burst size. The addresses are stored in the output burst queue `bQue`,
     *          with each burst being represented as a pair of starting address and the number
     *          of bytes it covers.
     */
    uint64 Tensor::getBurstQue(uint maxBurstBytes, const Vector &indicesLo, const Vector &indicesHi,
                               BurstQue &bQue) {
        assert(isPowOfTwo(maxBurstBytes) && "maxBurstBytes must be a power of two");

        AddrVector aVec;
        uint64 bytes = getAddrVector(indicesLo, indicesHi, aVec);  // sorted results

        uint64 wordBytes = getWordBytes();
        uint64 burstAddrLo = aVec[0];
        uint64 burstAddrHi = burstAddrLo;
        uint64 burstAddrUB = addrFloor(burstAddrLo, maxBurstBytes) + maxBurstBytes; // current burst address upper bound
        for (auto wordAddrLo: aVec) {
            uint64 wordAddrHi = wordAddrLo + wordBytes;
            if (wordAddrHi <= burstAddrUB) {  // the word can be merged into current burst  
                burstAddrHi = wordAddrHi;
            } else {  // at least part of the word is out of the current burst
                if (wordAddrLo >= burstAddrUB) {  // the word is out of the current burst
                    bQue.push_back({burstAddrLo, burstAddrHi - burstAddrLo}); 
                    burstAddrLo = wordAddrLo;
                    burstAddrUB = addrFloor(burstAddrLo, maxBurstBytes) + maxBurstBytes;
                }
                while (wordAddrHi > burstAddrUB) {
                    bQue.push_back({burstAddrLo, burstAddrUB - burstAddrLo});
                    burstAddrLo = burstAddrUB;
                    burstAddrUB = burstAddrLo + maxBurstBytes;
                }
                burstAddrHi = wordAddrHi;
            }
        }
        bQue.push_back({burstAddrLo, burstAddrHi - burstAddrLo});

        return bytes;
    }

    /**
     * @brief Generates a queue of memory bursts from the tensor addressing range and their corresponding SPM addresses.
     *
     * @param maxBurstBytes Maximum number of bytes per burst (must be a power of two).
     * @param indicesLo Lower bound of the indices range (inclusive).
     * @param indicesHi Upper bound of the indices range (exclusive).
     * @param bQue Output queue of memory bursts, each represented as a pair of starting address and size.
     * @param spmTensor The tensor which memory bursts are generated from.
     * @param spmAddrQue Output queue of SPM addresses associated to each burst.
     *
     * @return The total number of bytes in the generated bursts.
     *
     * @details This function calculates and organizes the addresses of the tensor elements
     *          within the specified index range into bursts that do not exceed the specified
     *          maximum burst size. The addresses are stored in the output burst queue `bQue`,
     *          with each burst being represented as a pair of starting address and the number
     *          of bytes it covers. The corresponding SPM addresses are stored in the output
     *          queue `spmAddrQue`.
     */
    uint64 Tensor::getBurstQue(uint maxBurstBytes, const Vector &indicesLo, const Vector &indicesHi,
                               BurstQue &bQue, Tensor &spmTensor, std::deque<uint64> &spmAddrQue) {
        // get a queue of bursts from memory spmTensor and a queue of SPM addresses associated to each burst
        assert(isPowOfTwo(maxBurstBytes));

        AddrVector aVec, aVec2;
        uint64 bytes = getAddrVector(indicesLo, indicesHi, aVec);  // sorted results
        uint64 bytes2 = spmTensor.getAddrVector(indicesLo, indicesHi, aVec2);
        assert(bytes == bytes2);

        uint64 wordBytes = getWordBytes();
        uint64 spmAddr = aVec2[0];
        uint64 burstAddrLo = aVec[0];
        uint64 burstAddrHi = burstAddrLo;
        uint64 burstAddrUB = addrFloor(burstAddrLo, maxBurstBytes) + maxBurstBytes;
        for (int i = 0; i < aVec.size(); i++) {
            uint64 wordAddrLo = aVec[i];
            uint64 wordAddrHi = wordAddrLo + wordBytes;
            if (wordAddrHi <= burstAddrUB) {  // the word can be merged into current burst
                burstAddrHi = wordAddrHi;
            } else {  // at least part of the word is out of the current burst
                if (wordAddrLo >= burstAddrUB) {  // the word is out of the current burst
                    bQue.push_back({burstAddrLo, burstAddrHi - burstAddrLo});
                    spmAddrQue.push_back(spmAddr);
                    spmAddr = aVec2[i];
                    burstAddrLo = wordAddrLo;
                    burstAddrUB = addrFloor(burstAddrLo, maxBurstBytes) + maxBurstBytes;
                }
                while (wordAddrHi > burstAddrUB) {
                    bQue.push_back({burstAddrLo, burstAddrUB - burstAddrLo});
                    spmAddrQue.push_back(spmAddr);
                    spmAddr += burstAddrUB - burstAddrLo;
                    burstAddrLo = burstAddrUB;
                    burstAddrUB = burstAddrLo + maxBurstBytes;
                }
                burstAddrHi = wordAddrHi;
            }
        }
        bQue.push_back({burstAddrLo, burstAddrHi - burstAddrLo});
        spmAddrQue.push_back(spmAddr);

        return bytes;
    }

    // TODO: Tensor没有shape，应该至少有一个high。
    uint64 Tensor::getBurstQue(uint maxBurstBytes, const Vector &indicesHi, BurstQue &bQue, Tensor &spmTensor, std::deque<uint64> &spmAddrQue)
    {
        return this->getBurstQue(maxBurstBytes, Vector(this->getNumDims(), 0), indicesHi, bQue, spmTensor, spmAddrQue);
    }

    uint64 Tensor::getBurstQue(uint maxBurstBytes, const Vector &indicesHi, BurstQue &bQue) 
    {
        return this->getBurstQue(maxBurstBytes, Vector(this->getNumDims(), 0), indicesHi, bQue);
    }

    uint64 Tensor::getOffsetAddress(const Vector &indices) {
        assert(indices.size() == strides_.size());
        uint64 addr = addr_;
        for (int i = 0; i < getNumDims(); i++) {
            addr += indices[i] * strides_[i];
        }
        return addr;
    }

    /**
     * @brief Get a tensor with an offset address.
     *
     * @param indices offset indices
     * @param tensor output tensor
     *
     * @details Set the address of the output tensor to the address of the current
     *          tensor plus the offset address calculated by the given indices.
     *          The strides of the output tensor are the same as the current tensor.
     */
    void Tensor::getOffsetTensor(const Vector &indices, Tensor &tensor) {
        tensor.addr_ = getOffsetAddress(indices);
        tensor.strides_ = strides_;
#ifdef TENSOR_DBG
        printf("Tensor::getOffsetTensor, indices=%s, addrOld=%lu, addrNew=%lu\n",
               toString(indices).c_str(), addr_, tensor.addr_);
#endif
    }


} // mudnac