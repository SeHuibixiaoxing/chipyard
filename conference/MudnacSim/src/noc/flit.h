#ifndef MUDNACSIM_FLIT_H
#define MUDNACSIM_FLIT_H


#include "tools.h"


namespace mudnac {


    class Flit {
    public:
        Message *msg = NULL;
        uint64 srcId = 0;
        uint64 dstId = 0;

        uint64 numFlits = 0;
        uint64 flitId = 0;

        uint64 injectTime = 0;
        uint64 totalHops = 0;
        uint64 currentHops = 0;

        std::set<Flit *> multicastFlits;

        Flit(Message *msg_, uint64 srcId_, uint64 dstId_, uint64 numFlits_, uint64 flitId_,
             uint64 injectTime_, uint64 totalHops_) {
            assert(numFlits_ > 0);
            assert(flitId_ < numFlits_);
            msg = msg_;
            srcId = srcId_;
            dstId = dstId_;
            numFlits = numFlits_;
            flitId = flitId_;
            injectTime = injectTime_;
            totalHops = totalHops_;
            currentHops = 0;
        }

        inline bool isHead() { return flitId == 0; }

        inline bool isTail() { return flitId == numFlits - 1; }

        inline bool isMulticastFlits() { return not multicastFlits.empty(); }

        inline std::string toString() {
            std::stringstream ss;
            ss << "("
               << "srcId=" << srcId << ",";
            if (not isMulticastFlits()) {
                ss << "dstId=" << dstId << ",";
            } else {
                ss << "dstId=(";
                for (Flit *f: multicastFlits) {
                    ss << f->dstId << ",";
                }
                ss << "),";
            }
            ss << "numFlits=" << numFlits << ","
               << "flitId=" << flitId << ","
               << "injectTime=" << injectTime << ","
               << "totalHops=" << totalHops << ","
               << "currentHops=" << currentHops
               << ")";
            return ss.str();
        }
    };


}


#endif //MUDNACSIM_FLIT_H
