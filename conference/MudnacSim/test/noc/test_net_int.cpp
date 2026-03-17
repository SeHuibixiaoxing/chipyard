#include "net_int.h"
#include <random>


using namespace mudnac;


class NetIntModule : public TickModule {
public:
    std::deque<Message *> *input;
    std::deque<Message *> *output;

    std::set<Message *> expectedRecvMsgs;

    void tick() {}

    inline bool idle() {
        return output->empty() and (input->size() == expectedRecvMsgs.size());
    }

    bool check() {
        bool correct = input->size() == expectedRecvMsgs.size();
        for (auto m : *input) {
            correct &= expectedRecvMsgs.count(m);
        }
        return correct;
    }

    void clear() {
        input->clear();
        output->clear();
        expectedRecvMsgs.clear();
    }
};


void testTwoModuleOneRouter();


int main() {
    testTwoModuleOneRouter();

    return 0;
}


void testTwoModuleOneRouter() {
    NetworkInterfaceConfig niCfg;
    niCfg.virtualChannels = 1;
//    niCfg.flitBytes = 64;
    niCfg.flitBytes = 16;
    niCfg.channelSize = std::ceil(64.0 / niCfg.flitBytes);

    RouterConfig routerCfg;
    routerCfg.virtualChannels = niCfg.virtualChannels;
    routerCfg.channelSize = niCfg.channelSize;
    routerCfg.portNum = 2;

    std::vector<std::vector<uint64>> hopCountTable = {{2, 2}, {2, 2}};
    std::vector<uint64> module0DownPortId2DstId = {1};
    std::vector<uint64> module1UpPortId2DstId = {0};

    std::cout << "Create modules\n";
    NetIntModule module0, module1;

    std::cout << "Create network interfaces\n";
    NetworkInterface<Message> ni0(0, niCfg, true, module0DownPortId2DstId);
    NetworkInterface<Message> ni1(1, niCfg, false,  module1UpPortId2DstId);
    ni0.hopCountTable = &hopCountTable;
    ni1.hopCountTable = &hopCountTable;

    std::cout << "Connect interfaces with modules\n";
    module0.input = &ni0.input;
    module0.output = &ni0.output;
    module1.input = &ni1.input;
    module1.output = &ni1.output;

    std::cout << "Create routers\n";
    Router router(routerCfg);
    router.rt.set(0, 0);
    router.rt.set(1, 1);

    std::cout << "Connect interfaces with routers\n";
    ni0.routerPort == router.ports[0];
    ni1.routerPort == router.ports[1];
    router.updateConnectedPortIdList();

    std::cout << "Generate messages\n";
//    uint64 numMsgs = 100;
    uint64 numMsgs = 1000;
    std::vector<Message> msgs;
    msgs.reserve(numMsgs);
    for (uint i = 0; i < numMsgs; i++) {
        msgs.emplace_back(64, 64, 0, 0);
        module0.output->emplace_back(&msgs[i]);
        module1.expectedRecvMsgs.insert(&msgs[i]);
    }

    std::cout << "Running\n";
    uint64 cycles = 0;
    while (true) {
        module0.tick();
        module1.tick();
        ni0.tick();
        ni1.tick();
        router.tick();
        cycles++;
        if (module0.idle() and module1.idle()) {
            break;
        }
    }

    assert(module0.check());
    assert(module1.check());

    std::cout << "Cycles: " << cycles << "\n";
    std::cout << "NI 0: \n";
    std::cout << "  SEND: \n";
    std::cout << ni0.sendCounter.toString() << "\n";
    std::cout << "  RECV: \n";
    std::cout << ni0.recvCounter.toString() << "\n";
    std::cout << "NI 1: \n";
    std::cout << "  SEND: \n";
    std::cout << ni1.sendCounter.toString() << "\n";
    std::cout << "  RECV: \n";
    std::cout << ni1.recvCounter.toString() << "\n";
}
