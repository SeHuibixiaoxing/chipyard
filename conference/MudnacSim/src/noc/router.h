#ifndef MUDNACSIM_ROUTER_H
#define MUDNACSIM_ROUTER_H


#include "flit.h"


namespace mudnac {


    class RouterConfig {
    public:
        uint64 portNum = 1;
        uint64 virtualChannels = 1;
        uint64 channelSize = 8;
    };


    class RouterVirtualChannel {
    private:
        SizedFifo<Flit *> flits;

    public:
        RouterVirtualChannel(uint64 size_) {
            assert(size_ > 0);
            flits.resize(size_);
        }

        inline bool canPush(uint64 n = 1) { return flits.canPush(n); }

        inline void push(Flit *flit) {
            flits.push(flit);
            if (not flit->isMulticastFlits()) {
                flit->currentHops++;
            } else {
                for (Flit *f: flit->multicastFlits) {
                    f->currentHops++;
                }
            }
        }

        inline bool canPop(uint64 n = 1) { return flits.canPop(n); }

        inline Flit *pop() { return flits.pop(); }

        inline Flit *peek() { return flits.peek(); }

        inline uint64 getOccupied() { return flits.has(); }

        inline uint64 getVacancy() { return flits.size() - flits.has(); }

        inline bool empty() { return flits.has() == 0; }
    };


    class RouterPort {
    public:
        uint64 vc;

    private:
        std::vector<RouterVirtualChannel> inputVCs;
        std::vector<RouterVirtualChannel *> outputVCs;

    public:
        RouterPort(uint64 vc_, uint64 size_) {
            assert(vc_ > 0);
            assert(size_ > 0);
            vc = vc_;
            inputVCs.resize(vc, {size_});
            outputVCs.resize(vc, NULL);
        }

        inline void operator==(RouterPort &other) {
            assert(vc == other.vc);
            for (int i = 0; i < vc; i++) {
                assert(outputVCs[i] == NULL);
                assert(other.outputVCs[i] == NULL);
                outputVCs[i] = &other.inputVCs[i];
                other.outputVCs[i] = &inputVCs[i];
            }
        }

        inline bool connected() { return outputVCs[0] != NULL; }

        inline bool canPush(int id, uint64 n = 1) { return outputVCs[id]->canPush(n); }

        inline void push(int id, Flit *flit) { outputVCs[id]->push(flit); }

        inline bool canPop(int id, uint64 n = 1) { return inputVCs[id].canPop(n); }

        inline Flit *pop(int id) { return inputVCs[id].pop(); }

        inline Flit *peek(int id) { return inputVCs[id].peek(); }

        inline uint64 getOccupiedInOutputVC(int id) { return outputVCs[id]->getOccupied(); }

        inline uint64 getOccupiedInInputVC(int id) { return inputVCs[id].getOccupied(); }

        inline uint64 getVacancyInOutputVC(int id) { return outputVCs[id]->getVacancy(); }

        inline uint64 getVacancyInInputVC(int id) { return inputVCs[id].getVacancy(); }

        inline uint64 inputVCEmpty(int id) { return inputVCs[id].empty(); }

        inline uint64 outputVCEmpty(int id) { return outputVCs[id]->empty(); }
    };


    class RouterPackageIdentifier {
    public:
        uint64 inputPortId;
        uint64 inputVCId;
        std::set<uint64> outputPortIdSet;
        uint64 outputVCId;

        RouterPackageIdentifier(uint64 inputPortId_, uint64 inputVCId_)
                : inputPortId(inputPortId_), inputVCId(inputVCId_), outputVCId(0) {}

        RouterPackageIdentifier(uint64 inputPortId_, uint64 inputVCId_, const std::set<uint64> &outputPortIdSet_)
                : RouterPackageIdentifier(inputPortId_, inputVCId_) {
            outputPortIdSet = outputPortIdSet_;
        }

        RouterPackageIdentifier() : RouterPackageIdentifier(0, 0) {}
    };


    class RouterOutputArbiter {
    public:
        uint64 numPorts;
        uint64 numVCs;
        // If a package is already allocated with an output VC. <inputPortId, inputVCId> -> true/false.
        std::vector<std::vector<bool>> pkgIsAllocated;
        // If an output VC is allocated. outputVCId -> true/false.
        std::vector<uint64> outputVCIsAllocated;
        // Available output VCs.
        std::deque<uint64> availOutputVCQueue;
        // Packages that are already allocated an output VC in FIFO order.
        std::list<RouterPackageIdentifier> allocatedPkgQueue;

        RouterOutputArbiter(uint64 ports_, uint64 vc_) {
            assert(ports_ > 0);
            assert(vc_ > 0);
            numPorts = ports_;
            numVCs = vc_;
            pkgIsAllocated.resize(numPorts);
            for (auto &row: pkgIsAllocated) {
                row.resize(numVCs, false);
            }
            outputVCIsAllocated.resize(numVCs, false);
        }

        inline void allocateOutputVC(RouterPackageIdentifier &pkgId) {
            assert(not availOutputVCQueue.empty());
            uint64 outputVCId = availOutputVCQueue.front();
            availOutputVCQueue.pop_front();
            assert(not outputVCIsAllocated[outputVCId]);
            outputVCIsAllocated[outputVCId] = true;
            assert(not pkgIsAllocated[pkgId.inputPortId][pkgId.inputVCId]);
            pkgIsAllocated[pkgId.inputPortId][pkgId.inputVCId] = true;
            allocatedPkgQueue.push_back(pkgId);
            allocatedPkgQueue.back().outputVCId = outputVCId;
        }

        // TODO: ??
        inline void releaseOutputVC(std::list<RouterPackageIdentifier>::iterator &iter) {
            assert(outputVCIsAllocated[iter->outputVCId]);
            outputVCIsAllocated[iter->outputVCId] = false;
            assert(pkgIsAllocated[iter->inputPortId][iter->inputVCId]);
            pkgIsAllocated[iter->inputPortId][iter->inputVCId] = false;
            allocatedPkgQueue.erase(iter);
        }
    };


    class RouterRoutingTable {
    public:
        void set(uint64 dstId, uint64 portId) {
            if (dstId >= table.size()) {
                table.resize(dstId + 1, -1);
            }
            table[dstId] = portId;
        }

        uint64 get(uint64 dstId) {
//            if (dstId >= table.size()) {
//                printf("RouterRoutingTable::get : dstId=%lu, size=%lu\n", dstId, table.size());
//            }
            assert(dstId < table.size());
            int64 portId = table[dstId];
            assert(portId >= 0);
            return portId;
        }

    private:
        std::deque<int64> table;
    };


    class Router : public TickModule {
    public:
        uint64 id;
        RouterConfig cfg;
        RouterRoutingTable rt;
        std::vector<RouterPort> ports;

    private:
        std::vector<RouterOutputArbiter> arbiters;
        std::vector<uint64> connectedPortIdList;
        uint64 cycles;

        std::multimap<int, RouterPackageIdentifier> pkgWaitingQueue;

    public:
        Router(RouterConfig cfg_, uint64 id_ = 0);

        void tick();

        void reset() {
            cycles = 0;
        }

        void updateConnectedPortIdList() {
            assert(connectedPortIdList.empty());
            for (uint64 portId = 0; portId < ports.size(); portId++) {
                if (ports[portId].connected()) {
                    connectedPortIdList.push_back(portId);
                }
            }
        }

    private:
        inline int getPkgPriority(Flit *flit) {
            return (int) (10000 * ((int) (100 * flit->currentHops) / (int) flit->totalHops)) /
                   (int) (1 + cycles - flit->injectTime);
        }
    };


}


#endif //MUDNACSIM_ROUTER_H
