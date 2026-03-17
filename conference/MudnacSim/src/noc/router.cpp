#include "router.h"


namespace mudnac {


    Router::Router(RouterConfig cfg_, uint64 id_) {
        assert(cfg_.portNum > 0);
        assert(cfg_.virtualChannels > 0);
        assert(cfg_.channelSize > 0);

        id = id_;
        cfg = cfg_;
        ports.resize(cfg.portNum, {cfg.virtualChannels, cfg.channelSize});
        arbiters.resize(cfg.portNum, {cfg.portNum, cfg.virtualChannels});
        cycles = 0;
    }


    void Router::tick() {
        // Reset temporal data structures
        pkgWaitingQueue.clear();
        for (uint64 outputPortId: connectedPortIdList) {
            auto &arbiter = arbiters[outputPortId];
            arbiter.availOutputVCQueue.clear();
            for (uint64 outputVCId = 0; outputVCId < cfg.virtualChannels; outputVCId++) {
                if ((not arbiter.outputVCIsAllocated[outputVCId]) and ports[outputPortId].outputVCEmpty(outputVCId)) {
                    arbiter.availOutputVCQueue.push_back(outputVCId);
                }
            }
        }

        // Add packages to "pkgWaitingQueue"
        for (uint64 inputPortId: connectedPortIdList) {
            for (uint64 inputVCId = 0; inputVCId < cfg.virtualChannels; inputVCId++) {
                // If there is a flit in this input channel
                if (ports[inputPortId].canPop(inputVCId)) {
                    Flit *flit = ports[inputPortId].peek(inputVCId);
//                    printf("%lu Router %lu: input flit at inputPortId=%lu, inputVCId=%lu, flitInfo=%s\n",
//                           cycles, id, inputPortId, inputVCId, flit->toString().c_str());
                    if (not flit->isHead()) {
                        continue;  // If it's not a head flit, then the package is already allocated a VC
                    }
                    if (not flit->isMulticastFlits()) {  // Unicast package
                        uint64 outputPortId = rt.get(flit->dstId);
                        // If it's not allocated with an output VC, then insert it to "pkgWaitingQueue"
                        if (not arbiters[outputPortId].pkgIsAllocated[inputPortId][inputVCId]) {
                            int priority = getPkgPriority(flit);
                            pkgWaitingQueue.insert({priority, {inputPortId, inputVCId, {outputPortId}}});
                        }
                    } else {  // Multicast package
                        // Check if the multicast package is already allocated output VCs
                        // If any package in this multicast package is allocated an output VC,
                        // then all are already allocated with VCs. Otherwise, insert it into waiting queue.
                        uint64 outputPortId = rt.get((*flit->multicastFlits.begin())->dstId);
                        if (not arbiters[outputPortId].pkgIsAllocated[inputPortId][inputVCId]) {
                            int priority = INT32_MAX;
                            std::set<uint64> outputPortIdSet;
                            for (Flit *f: flit->multicastFlits) {
                                outputPortIdSet.insert(rt.get(f->dstId));
                                priority = std::min(priority, getPkgPriority(f));
                            }
                            pkgWaitingQueue.insert({priority, {inputPortId, inputVCId, outputPortIdSet}});
                        }
                    }
                }
            }
        }

        // Allocate an available VC for each waiting package
        for (auto i = pkgWaitingQueue.begin(); i != pkgWaitingQueue.end(); i++) {
            RouterPackageIdentifier &pkgId = i->second;
            bool available = true;
            for (uint64 outputPortId: pkgId.outputPortIdSet) {
                available &= not arbiters[outputPortId].availOutputVCQueue.empty();
            }
            if (available) {
                for (uint64 outputPortId: pkgId.outputPortIdSet) {
                    arbiters[outputPortId].allocateOutputVC(pkgId);
                }
            }
        }

        // Print allocated packages
//        for (uint64 outputPortId: connectedPortIdList) {
//            for (auto &pkgId: arbiters[outputPortId].allocatedPkgQueue) {
//                printf("%lu Router %lu: package is allocated with a VC, "
//                       "inputPortId=%lu, inputVCId=%lu, outputPortId=%lu, outputVCId=%lu\n",
//                       cycles, id, pkgId.inputPortId, pkgId.inputVCId, outputPortId, pkgId.outputVCId);
//            }
//        }

        // Try to forward a flit at each output port
        for (uint64 outputPortId: connectedPortIdList) {
            auto &arbiter = arbiters[outputPortId];
            // Check if there is a flit able to be forwarded
            for (auto i = arbiter.allocatedPkgQueue.begin(); i != arbiter.allocatedPkgQueue.end(); i++) {
                RouterPackageIdentifier pkgId = *i;
                // If the input VC has a new flit, and the output VC is able to receive
                if (ports[pkgId.inputPortId].canPop(pkgId.inputVCId) and
                    ports[outputPortId].canPush(pkgId.outputVCId)) {
                    Flit *flit = ports[pkgId.inputPortId].peek(pkgId.inputVCId);
                    if (not flit->isMulticastFlits()) {  // Unicast flit
                        ports[pkgId.inputPortId].pop(pkgId.inputVCId);
                        ports[outputPortId].push(pkgId.outputVCId, flit);
                        // If it's the last flit, release VC
                        if (flit->isTail()) {
                            arbiter.releaseOutputVC(i);
                        }
//                        printf("%lu Router %lu: forward a unicast flit, inputPortId=%lu, inputVCId=%lu, "
//                               "outputPortId=%lu, outputVCId=%lu, flitInfo=%s\n",
//                               cycles, id, pkgId.inputPortId, pkgId.inputVCId,
//                               outputPortId, pkgId.outputVCId, flit->toString().c_str());
                        break;
                    } else {  // Multicast flit
                        // Create a new partial multicast flit
                        Flit *partialCastFlit = NULL;
                        for (Flit *f: flit->multicastFlits) {
                            if (outputPortId == rt.get(f->dstId)) {
                                if (partialCastFlit == NULL) {
                                    partialCastFlit = new Flit(flit->msg, flit->srcId, flit->dstId, flit->numFlits,
                                                               flit->flitId, flit->injectTime, flit->totalHops);
                                    partialCastFlit->currentHops = flit->currentHops;
                                }
                                partialCastFlit->multicastFlits.insert(f);
                            }
                        }
                        // "partialCastFlit == NULL" means the flit is already forwarded at this output port,
                        // but it's not forwarded yet at another output port.
                        if (partialCastFlit != NULL) {
                            for (Flit *f: partialCastFlit->multicastFlits) {
                                flit->multicastFlits.erase(f);
                            }
                            ports[outputPortId].push(pkgId.outputVCId, partialCastFlit);
                            // If it's the last flit, release VC
                            if (flit->isTail()) {
                                arbiter.releaseOutputVC(i);
                            }
                            // If it's multicast to all ports, delete the input flit
                            if (flit->multicastFlits.empty()) {
                                ports[pkgId.inputPortId].pop(pkgId.inputVCId);
                                delete flit;
                            }
//                            printf("%lu Router %lu: forward a multicast flit, inputPortId=%lu, inputVCId=%lu, "
//                                   "outputPortId=%lu, outputVCId=%lu, flitInfo=%s\n",
//                                   cycles, id, pkgId.inputPortId, pkgId.inputVCId,
//                                   outputPortId, pkgId.outputVCId, partialCastFlit->toString().c_str());
                            break;
                        }  // Otherwise, try to forward a flit at another VC
                    }
                }
            }
        }

        cycles++;
    }


}

