#include "noc.h"
#include <random>


using namespace mudnac;


class TestNocMessage : public Message {
public:
    uint64 &bytes;
    uint &dstId;
    uint &srcId;

    TestNocMessage() : bytes(m2sBytes), dstId(slavePortId), srcId(masterPortId) {}
};


class TestNocModule : public TickModule {
public:
    uint64 id;
    uint64 cycles;
    uint64 messagePeriod;
    uint64 firstMessageOffset;

    std::deque<TestNocMessage *> messages;
    std::set<TestNocMessage *> expectedReceivedMessages;

private:
    std::deque<TestNocMessage *> *input;
    std::deque<TestNocMessage *> *output;

public:
    TestNocModule(uint64 id_, uint64 messagePeriod_) {
        id = id_;
        cycles = 0;
        messagePeriod = messagePeriod_;
        firstMessageOffset = 0;

        input = NULL;
        output = NULL;
    }

    inline void tick() {
        if ((cycles % messagePeriod == firstMessageOffset) and (not messages.empty())) {
//            printf("%lu TestNocModule %lu, send a message, dst=%u\n", cycles, id, messages.front()->dstId);
            output->push_back(messages.front());
            messages.pop_front();
        }
        cycles++;
    }

    inline bool idle() {
        return input->size() >= expectedReceivedMessages.size();
    }

    void operator==(NetworkInterface<TestNocMessage> &ni) {
        assert(input == NULL and output == NULL);
        input = &ni.input;
        output = &ni.output;
    }

    void checkOutput() {
        assert(input->size() == expectedReceivedMessages.size());
        for (auto m: *input) {
            assert(m->dstId == id);
            assert(expectedReceivedMessages.count(m));
        }
    }

    void reset() {
        cycles = 0;
        messages.clear();
        input->clear();
        output->clear();
    }
};


void test();

void curve();


int main() {
//    test();
    curve();

    return 0;
}


void test() {
//    uint64 H = 1;
    uint64 H = 4;
//    uint64 W = 1;
    uint64 W = 4;
//    uint64 W = 8;
//    uint64 W = 16;

    uint64 MESSAGE_BYTES = 64;
    uint64 MESSAGE_PERIOD = 1;
//    bool ENABLE_MULTICAST = false;
    bool ENABLE_MULTICAST = true;
//    uint64 MULTICAST_GROUP_SIZE = 1;
//    uint64 MULTICAST_GROUP_SIZE = 2;
    uint64 MULTICAST_GROUP_SIZE = 4;
//    uint64 MULTICAST_GROUP_SIZE = 8;
//    uint64 NUM_MULTICAST_GROUPS = 10;
//    uint64 NUM_MULTICAST_GROUPS = 100;
//    uint64 NUM_MULTICAST_GROUPS = 1000;
    uint64 NUM_MULTICAST_GROUPS = 10000;

    uint64 NUM_NODES = H * W;
    uint64 NUM_MESSAGES = MULTICAST_GROUP_SIZE * NUM_MULTICAST_GROUPS;
    assert(NUM_NODES >= MULTICAST_GROUP_SIZE + 1);

    NetworkInterfaceConfig niCfg;
//    niCfg.flitBytes = 64;
    niCfg.flitBytes = 16;
//    niCfg.virtualChannels = 1;
    niCfg.virtualChannels = 4;
//    niCfg.virtualChannels = 16;
    niCfg.channelSize = std::ceil((double) MESSAGE_BYTES / niCfg.flitBytes);
//    niCfg.channelSize = std::ceil((double) MESSAGE_BYTES / niCfg.flitBytes) * 2;
//    niCfg.channelSize = std::ceil((double) MESSAGE_BYTES / niCfg.flitBytes) * 4;

    RouterConfig routerCfg;
    routerCfg.portNum = 5;
    routerCfg.virtualChannels = niCfg.virtualChannels;
    routerCfg.channelSize = niCfg.channelSize;

    MeshNoC meshNoc;
    meshNoc.setup(H, W, routerCfg);
    printf("Mesh NoC creation is done.\n");

    std::vector<uint64> downPortId2DstId(H * W, 0);
    std::vector<std::vector<NetworkInterface<TestNocMessage>>> nodes(H);
    for (uint64 h = 0; h < H; h++) {
        nodes[h].reserve(W);
        for (uint64 w = 0; w < W; w++) {
            uint64 id = h * W + w;
            downPortId2DstId[id] = id;
            nodes[h].emplace_back(id, niCfg, true, downPortId2DstId);
            meshNoc.addNode(nodes[h][w], id, h, w, 4);
        }
    }
    meshNoc.generate();
    printf("Network interface creation is down.\n");

    std::vector<std::vector<TestNocModule>> modules(H);
    for (uint64 h = 0; h < H; h++) {
        modules[h].reserve(W);
        for (uint64 w = 0; w < W; w++) {
            uint64 id = h * W + w;
            modules[h].emplace_back(id, MESSAGE_PERIOD);
            modules[h][w] == nodes[h][w];
        }
    }
    printf("Module creation is down.\n");

    std::default_random_engine re;
    std::uniform_int_distribution<> nodeIdDistri(0, NUM_NODES - 1);
    std::vector<TestNocMessage> messages;
    messages.reserve(NUM_MESSAGES);
    for (uint64 grpId = 0; grpId < NUM_MULTICAST_GROUPS; grpId++) {
        uint64 srcId = nodeIdDistri(re);
        TestNocMessage *multicastMsg = NULL;
        std::vector<bool> availableDstIdMask(NUM_NODES, true);
        availableDstIdMask[srcId] = false;
        if (ENABLE_MULTICAST) {
            multicastMsg = new TestNocMessage();
            multicastMsg->bytes = MESSAGE_BYTES;
        }
        for (uint64 i = 0; i < MULTICAST_GROUP_SIZE; i++) {
            uint64 dstId = nodeIdDistri(re);
            while (not availableDstIdMask[dstId]) {
                dstId = nodeIdDistri(re);
            }
            availableDstIdMask[dstId] = false;
            messages.emplace_back();
            messages.back().bytes = MESSAGE_BYTES;
            messages.back().dstId = dstId;
//            printf("create message %lu, srcId=%lu, dstId=%lu\n", grpId * NUM_MULTICAST_GROUPS + i, srcId, dstId);
            modules[dstId / W][dstId % W].expectedReceivedMessages.insert(&messages.back());
            if (ENABLE_MULTICAST) {
                multicastMsg->multicastMsgs.push_back(&messages.back());
            } else {
                modules[srcId / W][srcId % W].messages.push_back(&messages.back());
            }
        }
        if (ENABLE_MULTICAST) {
            modules[srcId / W][srcId % W].messages.push_back(multicastMsg);
        }
    }
    printf("Messages creation is done\n");

    bool idle = false;
    while (not idle) {
        idle = true;
        for (auto &row: modules) {
            for (auto &m: row) {
                m.tick();
                idle &= m.idle();
            }
        }
        meshNoc.tick();
    }
    printf("Running is done\n");

    for (auto &row: modules) {
        for (auto &m: row) {
            m.checkOutput();
        }
    }
    printf("Correct.\n");

    uint64 cycles = modules.front().front().cycles;
    printf("\nCycles: %lu\n", cycles);
    NetworkInterfaceCounter sumRecvCounter;
    for (auto ni: meshNoc.nodes) {
        sumRecvCounter.merge(ni->recvCounter);
    }
    printf("Summary\n");
    printf("%s", sumRecvCounter.toString().c_str());
    printf("\n");
//    for (uint64 nodeId = 0; nodeId < meshNoc.nodes.size(); nodeId++) {
//        printf("Interface %lu\n", nodeId);
//        printf("  SEND:\n");
//        printf("%s", meshNoc.nodes[nodeId]->sendCounter.toString().c_str());
//        printf("  RECV:\n");
//        printf("%s", meshNoc.nodes[nodeId]->recvCounter.toString().c_str());
//    }
}


void curve() {
//    uint64 H = 1;
    uint64 H = 4;
//    uint64 W = 1;
//    uint64 W = 2;
    uint64 W = 4;
//    uint64 W = 8;
//    uint64 W = 16;

    uint64 MESSAGE_BYTES = 64;
    uint64 MESSAGE_PERIOD = 1;
//    bool ENABLE_MULTICAST = false;
    bool ENABLE_MULTICAST = true;
//    uint64 MULTICAST_GROUP_SIZE = 1;
//    uint64 MULTICAST_GROUP_SIZE = 2;
    uint64 MULTICAST_GROUP_SIZE = 4;
//    uint64 NUM_MULTICAST_GROUPS = 10;
//    uint64 NUM_MULTICAST_GROUPS = 100;
//    uint64 NUM_MULTICAST_GROUPS = 1000;
    uint64 NUM_MULTICAST_GROUPS = 10000;

    uint64 NUM_NODES = H * W;
    uint64 NUM_MESSAGES = MULTICAST_GROUP_SIZE * NUM_MULTICAST_GROUPS;
    assert(NUM_NODES >= MULTICAST_GROUP_SIZE + 1);

    NetworkInterfaceConfig niCfg;
//    niCfg.flitBytes = 64;
    niCfg.flitBytes = 16;
//    niCfg.flitBytes = 8;
//    niCfg.virtualChannels = 1;
    niCfg.virtualChannels = 4;
//    niCfg.virtualChannels = 16;
    niCfg.channelSize = std::ceil((double) MESSAGE_BYTES / niCfg.flitBytes);
//    niCfg.channelSize = std::ceil((double) MESSAGE_BYTES / niCfg.flitBytes) * 2;
//    niCfg.channelSize = std::ceil((double) MESSAGE_BYTES / niCfg.flitBytes) * 4;

    RouterConfig routerCfg;
    routerCfg.portNum = 5;
    routerCfg.virtualChannels = niCfg.virtualChannels;
    routerCfg.channelSize = niCfg.channelSize;

    MeshNoC meshNoc;
    meshNoc.setup(H, W, routerCfg);
    printf("Mesh NoC creation is done.\n");

    std::vector<uint64> downPortId2DstId(H * W, 0);
    std::vector<std::vector<NetworkInterface<TestNocMessage>>> nodes(H);
    for (uint64 h = 0; h < H; h++) {
        nodes[h].reserve(W);
        for (uint64 w = 0; w < W; w++) {
            uint64 id = h * W + w;
            downPortId2DstId[id] = id;
            nodes[h].emplace_back(id, niCfg, true, downPortId2DstId);
            meshNoc.addNode(nodes[h][w], id, h, w, 4);
        }
    }
    meshNoc.generate();
    printf("Network interface creation is down.\n");

    std::vector<std::vector<TestNocModule>> modules(H);
    for (uint64 h = 0; h < H; h++) {
        modules[h].reserve(W);
        for (uint64 w = 0; w < W; w++) {
            uint64 id = h * W + w;
            modules[h].emplace_back(id, MESSAGE_PERIOD);
            modules[h][w] == nodes[h][w];
        }
    }
    printf("Module creation is down.\n");

    std::default_random_engine re;
    std::uniform_int_distribution<> nodeIdDistri(0, NUM_NODES - 1);
    std::vector<TestNocMessage> messages;
    messages.reserve(NUM_MESSAGES);
    for (uint64 grpId = 0; grpId < NUM_MULTICAST_GROUPS; grpId++) {
        uint64 srcId = nodeIdDistri(re);
        std::vector<bool> availableDstIdMask(NUM_NODES, true);
        availableDstIdMask[srcId] = false;
        for (uint64 i = 0; i < MULTICAST_GROUP_SIZE; i++) {
            uint64 dstId = nodeIdDistri(re);
            while (not availableDstIdMask[dstId]) {
                dstId = nodeIdDistri(re);
            }
            availableDstIdMask[dstId] = false;
            messages.emplace_back();
            messages.back().bytes = MESSAGE_BYTES;
            messages.back().dstId = dstId;
            messages.back().srcId = srcId;
//            printf("create message %lu, srcId=%lu, dstId=%lu\n", grpId * NUM_MULTICAST_GROUPS + i, srcId, dstId);
            modules[dstId / W][dstId % W].expectedReceivedMessages.insert(&messages.back());
        }
    }
    printf("Messages creation is done\n");

//    std::vector<uint64> MESSAGE_PERIOD_LIST = { 1, 2, 4, 8, 16};
    std::vector<uint64> MESSAGE_PERIOD_LIST = {
            1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
            21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 35, 40, 45, 50, 60, 70, 80, 100, 150, 200};
    std::deque<double> avgBandwidthList;
    std::deque<double> maxBandwidthList;
    std::deque<double> avgLatencyList;
    std::deque<double> maxLatencyList;

    for (uint64 testId = 0; testId < MESSAGE_PERIOD_LIST.size(); testId++) {
        uint64 messagePeriod = MESSAGE_PERIOD_LIST[testId];

        // reset status
        for (int h = 0; h < H; h++) {
            for (int w = 0; w < W; w++) {
                int id = h * W + w;
                modules[h][w].reset();
                modules[h][w].messagePeriod = messagePeriod;
                modules[h][w].firstMessageOffset = (uint) (std::ceil((float) messagePeriod / NUM_NODES) * id)
                                                   % messagePeriod;
                nodes[h][w].reset();
                meshNoc.routerMesh[h][w].reset();
            }
        }
//        printf("Status reset is done\n");

        // reset messages
        for (uint64 grpId = 0; grpId < NUM_MULTICAST_GROUPS; grpId++) {
            TestNocMessage *multicastMsg = NULL;
            uint64 srcId = messages[grpId * MULTICAST_GROUP_SIZE].srcId;
            if (ENABLE_MULTICAST) {
                multicastMsg = new TestNocMessage();
                multicastMsg->bytes = MESSAGE_BYTES;
                multicastMsg->srcId = srcId;
            }
            for (uint64 i = 0; i < MULTICAST_GROUP_SIZE; i++) {
                uint64 msgId = grpId * MULTICAST_GROUP_SIZE + i;
                if (ENABLE_MULTICAST) {
                    multicastMsg->multicastMsgs.push_back(&messages[msgId]);
                } else {
                    modules[srcId / W][srcId % W].messages.push_back(&messages[msgId]);
                }
            }
            if (ENABLE_MULTICAST) {
                modules[srcId / W][srcId % W].messages.push_back(multicastMsg);
            }
        }
//        printf("Message reset is done\n");

        // run
        bool idle = false;
        while (not idle) {
            idle = true;
            for (auto &row: modules) {
                for (auto &m: row) {
                    m.tick();
                    idle &= m.idle();
                }
            }
            meshNoc.tick();
        }
//        printf("Run is done\n");

        // check
        for (auto &row: modules) {
            for (auto &m: row) {
                m.checkOutput();
            }
        }

        // performance
        NetworkInterfaceCounter sumRecvCounter;
        for (auto ni: meshNoc.nodes) {
            sumRecvCounter.merge(ni->recvCounter);
        }
        avgBandwidthList.push_back(sumRecvCounter.getAvgFlitBW());
        maxBandwidthList.push_back(sumRecvCounter.getMaxFlitBW());
        avgLatencyList.push_back(sumRecvCounter.getAvgLatency());
        maxLatencyList.push_back(sumRecvCounter.getMaxLatency());
        std::cout << "\n";
        std::cout << "Period: " << messagePeriod << "\n";
        std::cout << "Cycles: " << sumRecvCounter.cycles << "\n";
        std::cout << "Avg BW: " << avgBandwidthList.back() << "\n";
        std::cout << "Max BW: " << maxBandwidthList.back() << "\n";
        std::cout << "Avg Lat: " << avgLatencyList.back() << "\n";
        std::cout << "Max Lat: " << maxLatencyList.back() << "\n";
    }

    // print curve
    printf("\n");
    printf("%8s%8s%8s%8s%8s\n", "period", "avg_bw", "max_bw", "avg_lat", "max_lat");
    for (int i = 0; i < MESSAGE_PERIOD_LIST.size(); i++) {
        printf("%8lu%8.2lf%8.2lf%8.2lf%8.2lf\n",
               MESSAGE_PERIOD_LIST[i], avgBandwidthList[i], maxBandwidthList[i], avgLatencyList[i], maxLatencyList[i]);
    }
}
