#include "router.h"
#include <random>


using namespace mudnac;


class RouterTestMessage : public Message {
public:
    uint64 flits;
    uint64 srcId;
    uint64 dstId;
    uint64 hops;

    uint64 sendTime;
    uint64 recvTime;

    RouterTestMessage(uint64 flits_, uint64 srcId_, uint64 dstId_, uint64 hops_)
            : flits(flits_), srcId(srcId_), dstId(dstId_), hops(hops_) {
        sendTime = 0;
        recvTime = 0;
    };

    uint64 getLatency() {
        assert(recvTime > sendTime);
        return recvTime - sendTime;
    }
};


class RouterTestNode : public TickModule {
public:
    uint64 id;
    RouterConfig cfg;
    RouterPort port;
    uint64 cycles;

    // controlling injection rate
    uint64 injectionGapCycles;
    uint64 firstInjectionTime;
    uint64 gapCyclesCount;

    std::deque<RouterTestMessage *> msgSend;
    std::set<RouterTestMessage *> msgRecv;
    std::set<RouterTestMessage *> msgRecvExpected;

    RouterTestNode(RouterConfig cfg_, uint64 id_) : port(cfg_.virtualChannels, cfg_.channelSize) {
        id = id_;
        cfg = cfg_;
        cycles = 0;

        injectionGapCycles = 0;
        firstInjectionTime = 0;
        gapCyclesCount = 0;
    }

    void tick() {
        // Send a message.
        if (not msgSend.empty()) {
            if (cycles >= firstInjectionTime and gapCyclesCount >= injectionGapCycles) {
                RouterTestMessage *msg = msgSend.front();
                // Find a VC that is available for all flits.
                for (int vcId_ = 0; vcId_ < cfg.virtualChannels; vcId_++) {
                    int vcId = (vcId_ + cycles) % cfg.virtualChannels;
                    if (port.canPush(vcId, msg->flits)) {
                        msg->sendTime = cycles;
//                        printf("%lu RouterTestNode %lu: send msg, vcId=%d, srcId=%lu, dstId=%lu, flits=%lu\n",
//                               cycles, id, vcId, msg->srcId, msg->dstId, msg->flits);
                        // Push all flits into the VC.
                        for (int flitId = 0; flitId < msg->flits; flitId++) {
                            port.push(vcId,
                                      new Flit(msg, msg->srcId, msg->dstId, msg->flits, flitId, cycles, msg->hops));
                        }
                        msgSend.pop_front();
                        gapCyclesCount = 0;
                        break;
                    }
                }
            } else {
                gapCyclesCount++;
            }
        }

        // Receive a message.
        if (msgRecv.size() < msgRecvExpected.size()) {
            // Find a VC that contains all flits of a message.
            for (int vcId = 0; vcId < cfg.virtualChannels; vcId++) {
                if (port.canPop(vcId) and port.canPop(vcId, port.peek(vcId)->numFlits)) {
                    Flit *headFlit = port.peek(vcId);
                    assert(headFlit->isHead());
                    assert(headFlit->totalHops == headFlit->currentHops);
                    RouterTestMessage *msg = (RouterTestMessage *) headFlit->msg;
                    assert(headFlit->totalHops == msg->hops);
                    uint64 numFlits = headFlit->numFlits;
                    msg->recvTime = cycles;
//                    printf("%lu RouterTestNode %lu: recv msg, vcId=%d, srcId=%lu, dstId=%lu, flits=%lu, latency=%lu,"
//                           "progress=%lu/%lu\n",
//                           cycles, id, vcId, msg->srcId, msg->dstId, msg->flits, msg->getLatency(),
//                           msgRecv.size() + 1, msgRecvExpected.size());
                    assert(msgRecv.count(msg) == 0);
                    msgRecv.insert(msg);
                    for (int flitId = 0; flitId < numFlits; flitId++) {
                        Flit *flit = port.pop(vcId);
                        // Do some checks for each flit.
                        assert(flit->flitId == flitId);
                        assert(flit->dstId == id);
                        assert((flitId > 0) or flit->isHead());
                        assert((flitId < numFlits - 1) or flit->isTail());
                        delete flit;
                    }
                    break;
                }
            }
        }

        cycles++;
    }

    inline bool idle() { return msgSend.empty() and msgRecv.size() == msgRecvExpected.size(); }

    bool check() {
        bool correct = msgRecvExpected.size() == msgRecv.size();
        for (auto m: msgRecv) {
            correct &= msgRecvExpected.count(m) > 0;
        }
        return correct;
    }

    void reset() {
        cycles = 0;
        injectionGapCycles = 0;
        firstInjectionTime = 0;
        gapCyclesCount = 0;
        msgSend.clear();
        msgRecv.clear();
        msgRecvExpected.clear();
    }
};


void test();

void curve();


int main() {
    // TODO
//    test();
    curve();

    return 0;
}


void test() {
    /**
     * Mesh:
     *   N0  N1  N2  N3
     *    |   |   |   |
     *   R0--R1--R2--R3
     *    |   |   |   |
     *   R4--R5--R6--R7
     *    |   |   |   |
     *   N4  N5  N6  N7
     *
     * Router ports:
     *   port0: local node port
     *   port1: left router port2
     *   port2: right router port1
     *   port3: up router port4
     *   port4: below router port3
     */

//    uint64 H = 1;
//    uint64 H = 2;
    uint64 H = 4;
//    uint64 W = 1;
//    uint64 W = 2;
    uint64 W = 4;
//    uint64 W = 8;
//    uint64 W = 16;

    uint64 numNodes = H * W;

    RouterConfig cfg;
    cfg.portNum = 5;
//    cfg.virtualChannels = 1;
    cfg.virtualChannels = 4;
//    cfg.virtualChannels = 16;
//    cfg.virtualChannels = 128;
    cfg.channelSize = 8;
//    cfg.channelSize = 32;
//    cfg.channelSize = 128;

    // generate nodes
    uint64 injectionGapCycles = 0;
    std::deque<RouterTestNode> nodes;
    for (int i = 0; i < numNodes; i++) {
        nodes.emplace_back(cfg, i);
        nodes.back().injectionGapCycles = injectionGapCycles;
        nodes.back().firstInjectionTime = (injectionGapCycles / numNodes) * i;
    }

    // generate routers
    std::deque<Router> routers;
    for (int i = 0; i < numNodes; i++) {
        routers.emplace_back(cfg);
    }

    // connect ports
    for (int i = 0; i < numNodes; i++) {
        routers[i].ports[0] == nodes[i].port;
        if (i > 0) { routers[i].ports[1] == routers[i - 1].ports[2]; }
        if (i >= W) { routers[i].ports[3] == routers[i - W].ports[4]; }
    }
    for (auto router: routers) {
        router.updateConnectedPortIdList();
    }
    printf("Connecting done.\n");

    // write routing tables (XY routing)
    for (int i = 0; i < numNodes; i++) {
        for (int dstId = 0; dstId < numNodes; dstId++) {
            if ((dstId % W) < (i % W)) {  // to left port
                routers[i].rt.set(dstId, 1);
            } else if ((dstId % W) > (i % W)) {  // to right port
                routers[i].rt.set(dstId, 2);
            } else {
                if (dstId < i) {  // to up port
                    routers[i].rt.set(dstId, 3);
                } else if (dstId > i) {  // to down port
                    routers[i].rt.set(dstId, 4);
                } else {  // to local port
                    routers[i].rt.set(dstId, 0);
                }
            }
        }
    }
    printf("Writing routingMeshXY tables done.\n");

    // compute hops. <srcId, dstId> -> hops
    std::vector<std::vector<uint>> hopsTable(numNodes);
    for (auto &r: hopsTable) { r.resize(numNodes, 0); }
    for (int h = 0; h < H; h++) {
        for (int w = 0; w < W; w++) {
            uint srcId = h * W + w;
            for (int h_ = 0; h_ < H; h_++) {
                for (int w_ = 0; w_ < W; w_++) {
                    uint dstId = h_ * W + w_;
                    uint hops = 2 + abs(h - h_) + abs(w - w_);
                    hopsTable[srcId][dstId] = hops;
                }
            }
        }
    }

    // create message
//    std::string mode = "manual";
    std::string mode = "random";
    std::deque<RouterTestMessage> msgList;
    if (mode == "manual") {
//        std::deque<int> dstIdList = {0};
//        std::deque<int> dstIdList = {0, 1};
//        std::deque<int> dstIdList = {1, 0};
//        std::deque<int> dstIdList = {0, 1, 2, 3};
//        std::deque<int> dstIdList = {1, 0, 3, 2};
        std::deque<int> dstIdList = {2, 3, 1, 0};
//        std::deque<int> numMsgsList(numNodes, 1);
//        std::deque<int> numMsgsList(numNodes, 10);
//        std::deque<int> numMsgsList(numNodes, 20);
//        std::deque<int> numMsgsList(numNodes, 50);
        std::deque<int> numMsgsList(numNodes, 100);
//        std::deque<int> numFlitsList(numNodes, 1);
//        std::deque<int> numFlitsList(numNodes, 7);
        std::deque<int> numFlitsList(numNodes, 8);
        for (int srcId = 0; srcId < numNodes; srcId++) {
            for (int msgId = 0; msgId < numMsgsList[srcId]; msgId++) {
                int dstId = dstIdList[srcId];
                msgList.emplace_back(numFlitsList[srcId], srcId, dstId, hopsTable[srcId][dstId]);
            }
        }
    } else if (mode == "random") {
        std::default_random_engine gen;
        std::uniform_int_distribution<> dstIdDtrb(0, numNodes - 1);
//        std::uniform_int_distribution<> flitsDtrb(1, 8);
        std::uniform_int_distribution<> flitsDtrb(8, 8);
//        std::deque<int> numMsgsList(numNodes, 10);
//        std::deque<int> numMsgsList(numNodes, 100);
//        std::deque<int> numMsgsList(numNodes, 200);
        std::deque<int> numMsgsList(numNodes, 500);
        for (int srcId = 0; srcId < numNodes; srcId++) {
            for (int msgId = 0; msgId < numMsgsList[srcId]; msgId++) {
//                int dstId = srcId;
                int dstId = dstIdDtrb(gen);
                msgList.emplace_back(flitsDtrb(gen), srcId, dstId, hopsTable[srcId][dstId]);
            }
        }
    }
    printf("\n");
    for (auto &msg: msgList) {
//        printf("Message: srcId=%lu, dstId=%lu, flits=%lu\n", msg.srcId, msg.dstId, msg.flits);
        nodes[msg.srcId].msgSend.push_back(&msg);
        nodes[msg.dstId].msgRecvExpected.insert(&msg);
    }
    printf("\n");
    printf("Creating messages done.\n");

    // run
    uint64 cycles = 0;
    bool busy = true;
    while (busy) {
        for (auto &router: routers) {
            router.tick();
        }
        busy = false;
        for (auto &node: nodes) {
            node.tick();
            busy |= not node.idle();
        }
        cycles++;
    }
    printf("Running done.\n");

    // check correctness
    bool correct = true;
    for (auto &node: nodes) {
        correct &= node.check();
    }
    assert(correct);

    // perfromance
    printf("\n");
    printf("Cycles: %lu\n", cycles);
    uint64 sumFlits = 0;
    uint64 sumLatency = 0;
    uint64 minLatency = UINT64_MAX;
    uint64 maxLatency = 0;
    for (auto &msg: msgList) {
        sumFlits += msg.flits;
        sumLatency += msg.getLatency();
        minLatency = std::min(minLatency, msg.getLatency());
        maxLatency = std::max(maxLatency, msg.getLatency());
    }
    double avgLatency = (double) sumLatency / msgList.size();
    double bandwidth = (double) sumFlits / cycles;
    printf("Latency: min=%lu, avg=%.2lf, max=%lu\n", minLatency, avgLatency, maxLatency);
    printf("Bandwidth: %.2lf flits/cycle\n", bandwidth);
}


void curve() {
//    uint64 H = 1;
//    uint64 H = 2;
    uint64 H = 4;
//    uint64 W = 1;
//    uint64 W = 2;
    uint64 W = 4;
//    uint64 W = 8;
//    uint64 W = 16;

    uint64 numNodes = H * W;

    RouterConfig cfg;
    cfg.portNum = 5;
//    cfg.virtualChannels = 1;
//    cfg.virtualChannels = 2;
    cfg.virtualChannels = 4;
//    cfg.virtualChannels = 8;
//    cfg.virtualChannels = 16;
//    cfg.virtualChannels = 128;
    cfg.channelSize = 8;
//    cfg.channelSize = 16;
//    cfg.channelSize = 32;
//    cfg.channelSize = 64;
//    cfg.channelSize = 128;

    // generate nodes
    std::deque<RouterTestNode> nodes;
    for (int i = 0; i < numNodes; i++) {
        nodes.emplace_back(cfg, i);
    }

    // generate routers
    std::deque<Router> routers;
    for (int i = 0; i < numNodes; i++) {
        routers.emplace_back(cfg);
    }

    // connect ports
    for (int i = 0; i < numNodes; i++) {
        routers[i].ports[0] == nodes[i].port;
        if (i > 0) { routers[i].ports[1] == routers[i - 1].ports[2]; }
        if (i >= W) { routers[i].ports[3] == routers[i - W].ports[4]; }
    }
    for (auto router: routers) {
        router.updateConnectedPortIdList();
    }

    // write routing tables (XY routing)
    for (int i = 0; i < numNodes; i++) {
        for (int dstId = 0; dstId < numNodes; dstId++) {
            if ((dstId % W) < (i % W)) {  // to left port
                routers[i].rt.set(dstId, 1);
            } else if ((dstId % W) > (i % W)) {  // to right port
                routers[i].rt.set(dstId, 2);
            } else {
                if (dstId < i) {  // to up port
                    routers[i].rt.set(dstId, 3);
                } else if (dstId > i) {  // to down port
                    routers[i].rt.set(dstId, 4);
                } else {  // to local port
                    routers[i].rt.set(dstId, 0);
                }
            }
        }
    }

    // compute hops. <srcId, dstId> -> hops
    std::vector<std::vector<uint>> hopsTable(numNodes);
    for (auto &r: hopsTable) { r.resize(numNodes, 0); }
    for (int h = 0; h < H; h++) {
        for (int w = 0; w < W; w++) {
            uint srcId = h * W + w;
            for (int h_ = 0; h_ < H; h_++) {
                for (int w_ = 0; w_ < W; w_++) {
                    uint dstId = h_ * W + w_;
                    uint hops = 2 + abs(h - h_) + abs(w - w_);
                    hopsTable[srcId][dstId] = hops;
                }
            }
        }
    }

    // create message
    std::deque<RouterTestMessage> msgList;
    std::default_random_engine gen;
    std::uniform_int_distribution<> dstIdDtrb(0, numNodes - 1);
//        std::uniform_int_distribution<> flitsDtrb(1, 8);
    std::uniform_int_distribution<> flitsDtrb(8, 8);
//        std::deque<int> numMsgsList(numNodes, 10);
//        std::deque<int> numMsgsList(numNodes, 100);
//        std::deque<int> numMsgsList(numNodes, 200);
    std::deque<int> numMsgsList(numNodes, 500);
    for (int srcId = 0; srcId < numNodes; srcId++) {
        for (int msgId = 0; msgId < numMsgsList[srcId]; msgId++) {
            int dstId = dstIdDtrb(gen);
            msgList.emplace_back(flitsDtrb(gen), srcId, dstId, hopsTable[srcId][dstId]);
        }
    }

    std::vector<uint64> injectGapList = {
            0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
            21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 35, 40, 45, 50, 60, 70, 80, 100, 150, 200};
//    std::vector<uint64> injectGapList = {0, 5, 10, 20, 25, 30, 35, 40, 45, 50, 60, 70, 80, 100, 150, 200};
    std::deque<double> bandwidthList;
    std::deque<double> avgLatencyList;
    std::deque<uint64> minLatencyList;
    std::deque<uint64> maxLatencyList;

    for (auto gap : injectGapList) {
        // reset status
        for (int i = 0; i < numNodes; i++) {
            routers[i].reset();
            nodes[i].reset();
            nodes[i].injectionGapCycles = gap;
            nodes[i].firstInjectionTime = (gap / numNodes) * i;
        }
        for (auto &msg: msgList) {
            nodes[msg.srcId].msgSend.push_back(&msg);
            nodes[msg.dstId].msgRecvExpected.insert(&msg);
        }

        // run
        uint64 cycles = 0;
        bool busy = true;
        while (busy) {
            for (auto &router: routers) {
                router.tick();
            }
            busy = false;
            for (auto &node: nodes) {
                node.tick();
                busy |= not node.idle();
            }
            cycles++;
        }

        // check correctness
        bool correct = true;
        for (auto &node: nodes) {
            correct &= node.check();
        }
        assert(correct);

        // perfromance

        uint64 sumFlits = 0;
        uint64 sumLatency = 0;
        uint64 minLatency = UINT64_MAX;
        uint64 maxLatency = 0;
        for (auto &msg: msgList) {
            sumFlits += msg.flits;
            sumLatency += msg.getLatency();
            minLatency = std::min(minLatency, msg.getLatency());
            maxLatency = std::max(maxLatency, msg.getLatency());
        }
        double avgLatency = (double) sumLatency / msgList.size();
        double bandwidth = (double) sumFlits / cycles;
        printf("\n");
        printf("Gap: %lu\n", gap);
        printf("Cycles: %lu\n", cycles);
        printf("Latency: min=%lu, avg=%.2lf, max=%lu\n", minLatency, avgLatency, maxLatency);
        printf("Bandwidth: %.2lf flits/cycle\n", bandwidth);

        bandwidthList.push_back(bandwidth);
        avgLatencyList.push_back(avgLatency);
        minLatencyList.push_back(minLatency);
        maxLatencyList.push_back(maxLatency);
    }

    // print curve
    printf("\n");
    printf("%8s%8s%8s%8s%8s\n", "gap", "bw", "avg_lat", "min_lat", "max_lat");
    for (int i = 0; i < injectGapList.size(); i++) {
        printf("%8lu%8.2lf%8.2lf%8lu%8lu\n",
               injectGapList[i], bandwidthList[i], avgLatencyList[i], minLatencyList[i], maxLatencyList[i]);
    }
}
