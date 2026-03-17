#include "spm.h"
#include <random>


using namespace mudnac;


std::deque<SPMMessage> msgs;
std::deque<std::deque<Message *>> slavePortInput;
std::deque<std::deque<Message *>> slavePortOutput;
std::deque<std::deque<Message *>> masterPortInput;
std::deque<std::deque<Message *>> masterPortOutput;

std::deque<Port> slavePort, masterPort;

bool naiveUpstreamTick(uint64 bankIdxMask = 0x0, uint64 bankIdxOffset = 4);

void naiveDownstreamTick();

void test();

void testGroupedMsg();

void testMultiBank();

void testRandom();


int main() {
    test();
    // testGroupedMsg();
    // testMultiBank();
    // testRandom();

    return 0;
}


void test() {
    SPMConfig config({1, 64,
                      8, 8});                      
    config.multicast = false;

    slavePortInput.resize(config.nBanks);
    slavePortOutput.resize(config.nBanks);
    masterPortInput.resize(config.nBanks);
    masterPortOutput.resize(config.nBanks);
    
    slavePort.resize(config.nBanks);
    masterPort.resize(config.nBanks);

    SPM spm(config);
    for (uint i = 0; i < config.nBanks; i++) {        
        slavePort[i].assign(slavePortInput[i], slavePortOutput[i]);
        masterPort[i].assign(masterPortInput[i], masterPortOutput[i]);

        spm.assignPort(i, masterPort[i], slavePort[i]);
    }

    msgs.emplace_back(SPMMSG_LOAD, 0, 64, 0);
    msgs.emplace_back(SPMMSG_LOAD, 0, 64, 0);
    msgs.emplace_back(SPMMSG_STORE, 15, 64, 0);
    msgs.emplace_back(SPMMSG_STORE, 15, 64, 0);
    msgs.emplace_back(SPMMSG_FETCH_LOAD, 0, 10, 0x1000);
    msgs.emplace_back(SPMMSG_FETCH_LOAD, 0, 30, 0x2000);
    msgs.emplace_back(SPMMSG_LOAD_BYPASS, 0, 64, 0x5000);
    msgs.emplace_back(SPMMSG_LOAD_BYPASS, 0, 64, 0x6000);
    msgs.emplace_back(SPMMSG_STORE_BYPASS, 0, 64, 0x7000);
    msgs.emplace_back(SPMMSG_STORE_BYPASS, 0, 64, 0x8000);

    
    msgs.emplace_back(SPMMSG_FETCH, 0, 64, 0x9000);
    msgs.emplace_back(SPMMSG_FETCH, 0, 64, 0x9000);

    msgs.emplace_back(SPMMSG_FLUSH, 64, 0, 0x10000);
    msgs.emplace_back(SPMMSG_FLUSH, 64, 0, 0x10000);

    while (true) {
        bool done = naiveUpstreamTick();
        spm.tick();
        naiveDownstreamTick();
        if (done) { break; }
    }
}


void testGroupedMsg() {
    SPMConfig config({1, 64,
                      8, 8});
    config.multicast = false;

    slavePortInput.resize(config.nBanks);
    slavePortOutput.resize(config.nBanks);
    masterPortInput.resize(config.nBanks);
    masterPortOutput.resize(config.nBanks);
    
    slavePort.resize(config.nBanks);
    masterPort.resize(config.nBanks);

    SPM spm(config);
    for (uint i = 0; i < config.nBanks; i++) {
        slavePort[i].assign(slavePortInput[i], slavePortOutput[i]);
        masterPort[i].assign(masterPortInput[i], masterPortOutput[i]);

        spm.assignPort(i, masterPort[i], slavePort[i]);
    }

    msgs.emplace_back(SPMMSG_FETCH_LOAD, 0, 64, 0x0, 0, 4);
    msgs.emplace_back(SPMMSG_FETCH_LOAD, 0, 64, 0x0, 0, 4);
    msgs.emplace_back(SPMMSG_FETCH_LOAD, 0, 64, 0x0, 0, 4);
    msgs.emplace_back(SPMMSG_FETCH_LOAD, 0, 64, 0x0, 0, 4);

    msgs.emplace_back(SPMMSG_FETCH_LOAD, 0, 10, 0x1000, 1, 2);
    msgs.emplace_back(SPMMSG_FETCH_LOAD, 0, 10, 0x1000, 1, 2);

    msgs.emplace_back(SPMMSG_FETCH_LOAD, 0, 20, 0x2000, 2, 2);
    msgs.emplace_back(SPMMSG_FETCH_LOAD, 0, 20, 0x2000, 2, 2);
    

    while (true) {
        bool done = naiveUpstreamTick();
        spm.tick();
        naiveDownstreamTick();
        if (done) { break; }
    }
}


void testMultiBank() {
    SPMConfig config({4, 64,
                      8, 8});
    config.multicast = false;

    slavePortInput.resize(config.nBanks);
    slavePortOutput.resize(config.nBanks);
    masterPortInput.resize(config.nBanks);
    masterPortOutput.resize(config.nBanks);
    
    slavePort.resize(config.nBanks);
    masterPort.resize(config.nBanks);

    SPM spm(config);
    
    for (uint i = 0; i < config.nBanks; i++) {
        slavePort[i].assign(slavePortInput[i], slavePortOutput[i]);
        masterPort[i].assign(masterPortInput[i], masterPortOutput[i]);

        spm.assignPort(i, masterPort[i], slavePort[i]);
    }

    msgs.emplace_back(SPMMSG_STORE, 0, 64, 0);
    msgs.emplace_back(SPMMSG_STORE, 16, 64, 0);
    msgs.emplace_back(SPMMSG_STORE, 32, 64, 0);
    msgs.emplace_back(SPMMSG_STORE, 48, 64, 0);
    msgs.emplace_back(SPMMSG_LOAD, 0, 64, 0);
    msgs.emplace_back(SPMMSG_LOAD, 0, 64, 0);
    msgs.emplace_back(SPMMSG_LOAD, 16, 64, 0);
    msgs.emplace_back(SPMMSG_LOAD, 16, 64, 0);
    msgs.emplace_back(SPMMSG_LOAD, 32, 64, 0);
    msgs.emplace_back(SPMMSG_LOAD, 32, 64, 0);
    msgs.emplace_back(SPMMSG_LOAD, 48, 64, 0);
    msgs.emplace_back(SPMMSG_LOAD, 48, 64, 0);

    while (true) {
        bool done = naiveUpstreamTick(config.nBanks - 1);
        spm.tick();
        naiveDownstreamTick();
        if (done) { break; }
    }
}


void testRandom() {
    std::vector<uint> typeArr({SPMMSG_FETCH_LOAD, SPMMSG_LOAD, SPMMSG_LOAD_BYPASS,
                               SPMMSG_STORE, SPMMSG_STORE_BYPASS});

    uint nBanks = 4;
    uint nPages = 16;
    uint lineBytes = 64;
    uint upstreamBeatBytes = 8;
    uint downstreamBeatBytes = 8;

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> typeDist(0, typeArr.size() - 1);
    std::uniform_int_distribution<> addrDist(0x0000, 0x3);
    std::uniform_int_distribution<> addrDist2(0x0000, 0x3f);
    std::uniform_int_distribution<> sizeDist(1, 64);
    std::uniform_int_distribution<> pageDist(0, nBanks * nPages - 1);

//    uint num = 100;
//    uint num = 1000;
    uint num = 10000;
    for (int i = 0; i < num; i++) {
        auto size = sizeDist(gen);
        msgs.emplace_back(
                typeArr[typeDist(gen)],
                pageDist(gen),
                size,
                (addrDist(gen)<<12) | (addrDist2(gen) % (64 - size + 1)));
    }

    SPMConfig config({nBanks, lineBytes,
                      upstreamBeatBytes, downstreamBeatBytes});
    config.multicast = false;

    slavePortInput.resize(config.nBanks);
    slavePortOutput.resize(config.nBanks);
    masterPortInput.resize(config.nBanks);
    masterPortOutput.resize(config.nBanks);
    
    slavePort.resize(config.nBanks);
    masterPort.resize(config.nBanks);

    SPM spm(config);
    for (uint i = 0; i < config.nBanks; i++) {
        slavePort[i].assign(slavePortInput[i], slavePortOutput[i]);
        masterPort[i].assign(masterPortInput[i], masterPortOutput[i]);

        spm.assignPort(i, masterPort[i], slavePort[i]);
    }

    while (true) {
        bool done = naiveUpstreamTick(config.nBanks - 1);
        spm.tick();
        naiveDownstreamTick();
        if (done) { break; }
    }

    auto counter = spm.getTotalCounter();

    uint64 totalBytes = 0;
    for (auto &msg : msgs) {
        totalBytes += msg.size;
    }
    printf("bw=%lf\n", (double)totalBytes / counter.cycles);
}


bool naiveUpstreamTick(uint64 bankIdxMask, uint64 bankIdxOffset) {
    static uint msgIdx = 0;
    static uint finished = 0;
    static uint64_t cycles = 0;
    if (msgIdx < msgs.size()) {
        auto &msg = msgs[msgIdx];
        uint bankIdx = (msg.page >> bankIdxOffset) & bankIdxMask;
        slavePortInput[bankIdx].push_back(&msg);
        msgIdx++;
    }
    for (uint i = 0; i < slavePortOutput.size(); i++) {
        auto &outputPort = slavePortOutput[i];
        if (not outputPort.empty()) {
            auto msg = dynamic_cast<SPMMessage*>(outputPort.front());
            printf("%lu upstream, type=%u, page=%u, addr=%lx, size=%ld\n",
                   cycles, msg->type, msg->page, msg->addr, msg->size);
            outputPort.pop_front();
            finished++;
        }
    }
    cycles++;
    return finished == msgs.size();
}


void naiveDownstreamTick() {
    static uint64_t cycles = 0;
    for (uint i = 0; i < masterPortOutput.size(); i++) {
        auto &outputPort = masterPortOutput[i];
        auto &inputPort = masterPortInput[i];
        if (not outputPort.empty()) {
            auto msg = dynamic_cast<MemCtrlMessage*>(outputPort.front());
            printf("%lu Downstream: addr=%lx, isWrite=%d\n",
                   cycles, msg->addr, msg->isWrite);
            outputPort.pop_front();
            inputPort.push_back(msg);
        }
    }
    cycles++;
}
