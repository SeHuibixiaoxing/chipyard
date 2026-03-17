#ifndef MUDNACSIM_LOOPNEST_H
#define MUDNACSIM_LOOPNEST_H


#include "tools.h"


namespace mudnac {


    class LoopNest {
    public:
        uint64 numLevels = 0;
        uint64 numDims = 0;

        bool valid = false;
        uint64 count = 0;

        /**
         * Shape: (numLevels, numDims). \n
         * Format:
         * [ [dim00, dim01, ...],
         *   [dim10, dim11, ...], ... ]. \n
         * Value: for a factor "dimXY", X is levelId, Y is dimId. \n
         */
        std::vector<std::vector<uint64>> factors;

        /**
         * Shape: (numLevels, numLoops(numDims)). \n
         * Format:
         * [ [dimId00, dimId01, ...],
         *   [dimId10, dimId11, ...], ...] (from outer loop to inner loop). \n
         * Value: for a dimId "dimIdXZ", X is levelId, Z is loopId. \n
         */
        std::vector<std::vector<uint64>> loops;

        /**
         * Shape: (numLevels, numDims). \n
         * Format:
         * [ [stride00, stride01, ...],
         *   [stride10, stride11, ...], ...] (from outer loop to inner loop). \n
         * Value: for a stride "strideXY", X is levelId, Y is dimId.
         */
        std::vector<std::vector<uint64>> strides;

        /**
         * Shape: (numLevels, numDims). \n
         * Format:
         * [ [dim00, dim01, ...],
         *   [dim10, dim11, ...], ... ]. \n
         * Value: for a iter "dimXY", X is levelId, Y is dimId. \n
         */
        std::vector<std::vector<uint64>> iters;

        /**
         * Shape: (numDims). \n
         * Format: [distance0, distance1, ...]. \n
         * Value: for a distance "distanceY", Y is dimId. \n
         */
        std::vector<uint64> dists;

        inline void init(const std::vector<std::vector<uint64>> &factors_,
                         const std::vector<std::vector<uint64>> &loops_,
                         const std::vector<uint64> &innermostStride_) {
            assert(factors_.size() > 0);
            assert(factors_.size() == loops_.size());
            assert(factors_[0].size() > 0);
            numLevels = factors_.size();
            numDims = factors_[0].size();

            for (int levelId = 0; levelId < numLevels; levelId++) {
                assert(factors_[levelId].size() == numDims);
                assert(loops_[levelId].size() == numDims);
            }
            assert(innermostStride_.size() == numDims);

            factors = factors_;
            loops = loops_;

            strides.resize(numLevels);
            for (auto &i : strides) {
                i.resize(numDims, 1);
            }
            for (int levelId = 0; levelId < numLevels; levelId++) {
                for (int dimId = 0; dimId < numDims; dimId++) {
                    strides[levelId][dimId] = innermostStride_[dimId];
                    for (int i = levelId + 1; i < numLevels; i++) {
                        strides[levelId][dimId] *= factors[i][dimId];
                    }
                }
            }

            reset();
        }

        inline void reset() {
            assert(numLevels > 0);
            assert(numDims > 0);
            valid = true;
            count = 0;

            iters.resize(numLevels);
            for (auto &i : iters) {
                i.resize(numDims, 0);
            }

            dists.resize(numDims, 0);
        }

        inline void next() {
            assert(valid);
            for (int levelId = numLevels - 1; levelId >= 0; levelId--) {
                for (int loopId = numDims - 1; loopId >= 0; loopId--) {
                    uint64 dimId = loops[levelId][loopId];
                    if ((iters[levelId][dimId] + 1) < factors[levelId][dimId]) {
                        iters[levelId][dimId]++;
                        dists[dimId] += strides[levelId][dimId];
                        count++;
                        return;
                    } else {
                        iters[levelId][dimId] = 0;
                        dists[dimId] -= strides[levelId][dimId] * (factors[levelId][dimId] - 1);
                    }
                }
            }
            valid = false;
        }

        inline void getRelativeDistance(std::vector<uint64> &dists_, uint64 targetLevelId = 0) {
            std::vector<uint64> targetLevelDists(numDims, 0);
            for (int levelId = 0; levelId < targetLevelId; levelId++) {
                for (int loopId = numDims - 1; loopId >= 0; loopId--) {
                    uint64 dimId = loops[levelId][loopId];
                    targetLevelDists[dimId] += iters[levelId][dimId] * strides[levelId][dimId];
                }
            }
            dists_.resize(numDims, 0);
            for (int dimId = 0; dimId < numDims; dimId++) {
                assert(dists[dimId] >= targetLevelDists[dimId]);
                dists_[dimId] = dists[dimId] - targetLevelDists[dimId];
            }
        }
    };


}


#endif //MUDNACSIM_LOOPNEST_H
