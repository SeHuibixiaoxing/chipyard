#ifndef MUDNACSIM_NET_INT_H
#define MUDNACSIM_NET_INT_H


#include "router.h"


namespace mudnac {


    class NetworkInterfaceConfig {
    public:
        uint64 virtualChannels = 1;
        uint64 channelSize = 8;
        uint64 flitBytes = 8;
    };


    class NetworkInterfaceCounter {
    public:
        uint64 cycles = 0;
        PeriodicalCounter<uint64> msgCounter;
        PeriodicalCounter<uint64> flitCounter;
        HistogramCounter<uint64, uint64> remoteIdHist;
        HistogramCounter<uint64, uint64> latencyHist;
        HistogramCounter<uint64, uint64> hopCountHist;

        NetworkInterfaceCounter() : msgCounter(1000), flitCounter(1000) {}

        inline void tick() {
            msgCounter.tick();
            flitCounter.tick();
            cycles++;
        }

        double getAvgMsgBW() { return (double) msgCounter.sum() / cycles; }

        double getMaxMsgBW() { return (double) msgCounter.max() / msgCounter.periodCycles; }

        double getAvgFlitBW() { return (double) flitCounter.sum() / cycles; }

        double getMaxFlitBW() { return (double) flitCounter.max() / flitCounter.periodCycles; }

        double getAvgLatency() {
            uint64 sumLatency = 0;
            for (uint64 latency = 0; latency < latencyHist.size(); latency++) {
                sumLatency += latency * latencyHist.counters[latency];
            }
            return (double) sumLatency / latencyHist.getTotalCount();
        }

        uint64 getMaxLatency() { return latencyHist.size() - 1; }

        double getAvgHops() {
            uint64 sumHops = 0;
            for (uint64 hops = 0; hops < hopCountHist.size(); hops++) {
                sumHops += hops * hopCountHist.counters[hops];
            }
            return (double) sumHops / hopCountHist.getTotalCount();
        }

        uint64 getMaxHops() { return hopCountHist.size() - 1; }

        void merge(NetworkInterfaceCounter &counter) {
            cycles = std::max(cycles, counter.cycles);
            msgCounter.merge(counter.msgCounter);
            flitCounter.merge(counter.flitCounter);
            remoteIdHist.merge(counter.remoteIdHist);
            latencyHist.merge(counter.latencyHist);
            hopCountHist.merge(counter.hopCountHist);
        }

        std::string toString() {
            std::stringstream ss;
            ss << "\tTotal Messages: " << msgCounter.sum() << "\n";
            ss << "\tTotal Flits: " << flitCounter.sum() << "\n";
            ss << "\tAvg Bandwidth: "
               << getAvgMsgBW() << "(msgs/cycle), "
               << getAvgFlitBW() << "(flits/cycle)\n";
            ss << "\tMax Bandwidth: "
               << getMaxMsgBW() << "(msgs/cycle), "
               << getMaxFlitBW() << "(flits/cycle)\n";
            ss << "\tAvg Latency: " << getAvgLatency() << "\n";
            ss << "\tMax Latency: " << getMaxLatency() << "\n";
            ss << "\tAvg Hops: " << getAvgHops() << "\n";
            ss << "\tMax Hops: " << getMaxHops() << "\n";
            ss << "\tRemote Id Hist: " << remoteIdHist.toString() << "\n";
            ss << "\tHop Hist: " << hopCountHist.toString() << "\n";
            ss << "\tLatency Hist: " << latencyHist.toString(50) << "\n";
            return ss.str();
        }

        void reset() {
            cycles = 0;
            msgCounter.reset();
            flitCounter.reset();
            remoteIdHist.reset();
            latencyHist.reset();
            hopCountHist.reset();
        }
    };


    class BaseNetworkInterface : public TickModule {
    public:
        uint64 id;
        uint64 cycles;
        NetworkInterfaceConfig cfg;
        RouterPort routerPort;

        std::vector<std::vector<uint64>> *hopCountTable;

        NetworkInterfaceCounter sendCounter;
        NetworkInterfaceCounter recvCounter;

    protected:
        // send
        std::deque<Flit *> sendFlitQueue;
        bool sending;
        uint64 sendVcId;
        // recv
        std::deque<Message *> recvMsgQueue;
        bool recving;
        uint64 recvVcId;

    public:
        BaseNetworkInterface(uint64 id_, const NetworkInterfaceConfig &cfg_)
                : routerPort(cfg_.virtualChannels, cfg_.channelSize) {
            id = id_;
            cycles = 0;
            cfg = cfg_;
            hopCountTable = NULL;
            sending = false;
            sendVcId = 0;
            recving = false;
            recvVcId = 0;
        }

        void reset() {
            cycles = 0;
            sendCounter.reset();
            recvCounter.reset();
        }

    protected:
        inline void sendFlit() {
            if (not sendFlitQueue.empty()) {
                // If there's no currently sending package, find a VC that is available.
                if (not sending) {
                    // Find the VC with the most vacancy.
                    uint64 vcId = 0;
                    uint64 vcVacancy = 0;
                    for (uint64 vcId_ = 0; vcId_ < routerPort.vc; vcId_++) {
                        uint64 vcVacancy_ = routerPort.getVacancyInOutputVC(vcId_);
                        if (vcVacancy_ > vcVacancy) {
                            vcId = vcId_;
                            vcVacancy = vcVacancy_;
                        }
                    }
                    if (vcVacancy > 0) {
                        sending = true;
                        sendVcId = vcId;
                    }
                }
                // If there's a currently sending package, send it to VC "sendVcId".
                if (sending and routerPort.canPush(sendVcId)) {
                    Flit *flit = sendFlitQueue.front();
                    sendFlitQueue.pop_front();
                    // set inject time
                    flit->injectTime = cycles;
                    if (flit->isMulticastFlits()) {
                        for (Flit *f: flit->multicastFlits) {
                            f->injectTime = cycles;
                        }
                    }
                    routerPort.push(sendVcId, flit);
                    if (flit->isTail()) {
                        if (not flit->isMulticastFlits()) {  // unicast
                            sendCounter.flitCounter.add(flit->numFlits);
                            sendCounter.msgCounter.add(1);
                            sendCounter.remoteIdHist.add(flit->dstId, 1);
                            sendCounter.hopCountHist.add(flit->totalHops, 1);
                            sendCounter.latencyHist.add(0, 1);
                        } else {  // multicast
                            for (Flit *f: flit->multicastFlits) {
                                sendCounter.flitCounter.add(f->numFlits);
                                sendCounter.msgCounter.add(1);
                                sendCounter.remoteIdHist.add(f->dstId, 1);
                                sendCounter.hopCountHist.add(f->totalHops, 1);
                                sendCounter.latencyHist.add(0, 1);
                            }
                        }
                        sending = false;
//                        printf("%lu NetInt %lu: send tail flit, dstId=%lu, numFlits=%lu, numHops=%lu, multicast=%zu\n",
//                               cycles, id, flit->dstId, flit->numFlits, flit->totalHops, flit->multicastFlits.size());
                    }
                }
            }
        }

        inline void recvFlit() {
            // If there's no currently receiving package, find a VC that is available.
            if (not recving) {
                uint64 vcId = 0;
                uint64 vcOccupied = 0;
                for (uint64 i = 1; i <= routerPort.vc; i++) {
                    uint64 vcId_ = (recvVcId + i) % routerPort.vc;
                    uint64 vcOccupied_ = routerPort.getOccupiedInInputVC(vcId_);
                    if (vcOccupied_ > vcOccupied) {
                        vcId = vcId_;
                        vcOccupied = vcOccupied_;
                    }
                }
                if (vcOccupied > 0) {
                    recving = true;
                    recvVcId = vcId;
                }
            }
            // If there is a currently receiving package, receive the next flit from VC "recvVcId"
            if (recving and routerPort.canPop(recvVcId)) {
                Flit *flit = routerPort.pop(recvVcId);
                if (flit->isTail()) {
                    if (not flit->isMulticastFlits()) {
                        recvCounter.flitCounter.add(flit->numFlits);
                        recvCounter.msgCounter.add(1);
                        recvCounter.remoteIdHist.add(flit->srcId, 1);
                        recvCounter.hopCountHist.add(flit->totalHops, 1);
                        recvCounter.latencyHist.add(std::max((uint64) 0, cycles - flit->injectTime), 1);
                        assert(flit->currentHops == flit->totalHops);
                        assert(flit->msg != NULL);
                        recvMsgQueue.push_back(flit->msg);
                    } else {
                        assert(flit->multicastFlits.size() == 1);
                        Flit *f = *flit->multicastFlits.begin();
                        recvCounter.flitCounter.add(f->numFlits);
                        recvCounter.msgCounter.add(1);
                        recvCounter.remoteIdHist.add(f->srcId, 1);
                        recvCounter.hopCountHist.add(f->totalHops, 1);
                        recvCounter.latencyHist.add(std::max((uint64) 0, cycles - f->injectTime), 1);
                        assert(f->currentHops == f->totalHops);
                        assert(f->msg != NULL);
                        recvMsgQueue.push_back(f->msg);
                        delete f;
                    }
                    recving = false;
//                    printf("%lu NetInt %lu: recv tail flit, srcId=%lu, numFlits=%lu, numHops=%lu\n",
//                           cycles, id, flit->srcId, flit->numFlits, flit->totalHops);
                }
                delete flit;
            }
        }
    };


    template<typename M>
    class NetworkInterface : public BaseNetworkInterface {
    public:
        std::deque<M *> input;
        std::deque<M *> output;
        std::vector<uint64> &dstPortId2DstNodeId;

        // To support direct transfer between two interfaces (Messages will not go into NoC)
        std::map<uint64, NetworkInterface<M> *> dstNodeId2DstNode;

        NetworkInterface(uint64 id_,
                         const NetworkInterfaceConfig &cfg_,
                         bool isMaster_,
                         std::vector<uint64> &portId2DstId)
                : BaseNetworkInterface(id_, cfg_),
                  isMaster(isMaster_),
                  dstPortId2DstNodeId(portId2DstId) {}

        inline uint64 getDstPortId(Message *m) {
            if(isMaster) return m->slavePortId;
            else return m->masterPortId;
        }
          
        inline uint64 getBytes(Message *m) {
            if(isMaster) return m->m2sBytes;
            else return m->s2mBytes;
        }

        inline void tick() {
            getFlitsFromMsg();
            sendFlit();
            recvFlit();
            getMsgFromFlits();
            sendCounter.tick();
            recvCounter.tick();
            cycles++;
        }

        void createDirectTransferConnection(NetworkInterface<M> &dstNode) {
            assert(not dstNodeId2DstNode.count(dstNode.id));
            dstNodeId2DstNode[dstNode.id] = &dstNode;
            assert(not dstNode.dstNodeId2DstNode.count(id));
            dstNode.dstNodeId2DstNode[id] = this;
        }

    private:
        bool isMaster;

        inline void getFlitsFromMsg() {
            // If there is a new message to send to NoC
            if (not output.empty()) {
                M *msg = output.front();
                output.pop_front();
                if (not msg->isMulticastRespMsg()) {  // unicast
                    uint64 dstId = dstPortId2DstNodeId[getDstPortId(msg)];
                    uint64 numFlits = std::ceil((double) std::max(getBytes(msg), (uint64) 1) / cfg.flitBytes);
                    if (dstNodeId2DstNode.count(dstId)) {  // direct transfer
                        dstNodeId2DstNode[dstId]->input.push_back(msg);
                        // record
                        sendCounter.flitCounter.add(numFlits);
                        sendCounter.msgCounter.add(1);
                        sendCounter.remoteIdHist.add(dstId, 1);
                        sendCounter.hopCountHist.add(0, 1);
                        sendCounter.latencyHist.add(0, 1);
                        NetworkInterfaceCounter &recvCnt = dstNodeId2DstNode[dstId]->recvCounter;
                        recvCnt.flitCounter.add(numFlits);
                        recvCnt.msgCounter.add(1);
                        recvCnt.remoteIdHist.add(id, 1);
                        recvCnt.hopCountHist.add(0, 1);
                        recvCnt.latencyHist.add(0, 1);
                    } else {  // send to NoC
                        uint64 numHops = (*hopCountTable)[id][dstId];
                        for (uint64 flitId = 0; flitId < numFlits; flitId++) {
                            sendFlitQueue.push_back(new Flit(msg, id, dstId, numFlits, flitId, cycles, numHops));
                        }
//                        printf("%lu NetInt %lu: create unicast flits, dstId=%lu, numFlits=%lu, numHops=%lu\n",
//                               cycles, id, dstId, numFlits, numHops);
                    }
                } else {  // multicast
                    uint64 numFlits = std::ceil((double) std::max(getBytes(msg), (uint64) 1) / cfg.flitBytes);
                    std::vector<Flit *> multicastFlits(numFlits, NULL);
                    for (Message *m: msg->multicastMsgs) {
                        uint64 dstId = dstPortId2DstNodeId[getDstPortId(m)];
                        if (dstNodeId2DstNode.count(dstId)) {  // direct transfer
                            dstNodeId2DstNode[dstId]->input.push_back(dynamic_cast<M *>(m));
                            // record
                            sendCounter.flitCounter.add(numFlits);
                            sendCounter.msgCounter.add(1);
                            sendCounter.remoteIdHist.add(dstId, 1);
                            sendCounter.hopCountHist.add(0, 1);
                            sendCounter.latencyHist.add(0, 1);
                            NetworkInterfaceCounter &recvCnt = dstNodeId2DstNode[dstId]->recvCounter;
                            recvCnt.flitCounter.add(numFlits);
                            recvCnt.msgCounter.add(1);
                            recvCnt.remoteIdHist.add(id, 1);
                            recvCnt.hopCountHist.add(0, 1);
                            recvCnt.latencyHist.add(0, 1);
                        } else {  // send to NoC
                            if (multicastFlits.front() == NULL) {  // create multicast flits
                                for (uint64 flitId = 0; flitId < numFlits; flitId++) {
                                    multicastFlits[flitId] = new Flit(NULL, id, 0, numFlits, flitId, cycles, 0);
                                }
                            }
                            uint64 numHops = (*hopCountTable)[id][dstId];
                            for (uint64 flitId = 0; flitId < numFlits; flitId++) {
                                // create unicast flits and add them into multicast flits
                                multicastFlits[flitId]->multicastFlits.insert(
                                        new Flit(m, id, dstId, numFlits, flitId, cycles, numHops)
                                );
                            }
                        }
                    }
                    if (multicastFlits.front() != NULL) {
                        for (Flit *f: multicastFlits) {
                            sendFlitQueue.push_back(f);
                        }
//                        printf("%lu NetInt %lu: create multicast flits, numFlits=%lu, multicast=%zu\n",
//                               cycles, id, numFlits, multicastFlits.front()->multicastFlits.size());
                    }
                    delete msg;
                }
            }
        }

        inline void getMsgFromFlits() {
            // If there is a new message to receive from NoC
            if (not recvMsgQueue.empty()) {
                M *msg = static_cast<M *>(recvMsgQueue.front());
                recvMsgQueue.pop_front();
                input.push_back(msg);
//                printf("%lu NetInt %lu: get message\n", cycles, id);
            }
        }
    };
}


#endif //MUDNACSIM_NET_INT_H
