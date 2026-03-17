#include <cstdio>
#include <cache.h>
#include <random>


using namespace mudnac;


std::deque<CacheMessage> msgs;
std::deque<std::deque<Message *>> slavePortInput;
std::deque<std::deque<Message *>> slavePortOutput;
std::deque<std::deque<Message *>> masterPortInput;
std::deque<std::deque<Message *>> masterPortOutput;

std::deque<Port> slavePort, masterPort;


bool naiveUpstreamTick(uint64 bankIdxMask = 0x0, uint64 bankIdxOffset = 6);

void naiveDownstreamTick();

void testSingleBankSingleSetSingleWay();

void testSingleBankSingleSetMultiWay();

void testSingleBankMultiSetSingleWay();

void testMultiBankSingleSetSingleWay();

void testAllIn();


int main() {
    printf("main starts\n");

    testSingleBankSingleSetSingleWay();
    testSingleBankSingleSetMultiWay();
    testSingleBankMultiSetSingleWay();
    testMultiBankSingleSetSingleWay();
    testAllIn();

    printf("main ends\n");
    return 0;
}


void testSingleBankSingleSetSingleWay() {
    CacheConfig config(1, 1, 1, 64,
                       8, 8);
    slavePortInput.resize(config.nBanks);
    slavePortOutput.resize(config.nBanks);
    masterPortInput.resize(config.nBanks);
    masterPortOutput.resize(config.nBanks);

    slavePort.resize(config.nBanks);
    masterPort.resize(config.nBanks);

    slavePort[0].assign(slavePortInput[0], slavePortOutput[0]);
    masterPort[0].assign(masterPortInput[0], masterPortOutput[0]);

    Cache cache(config);
    cache.assignPort(0, masterPort[0], slavePort[0]);
    printf("cache creating done\n");

    uint size = 64;
    uint incr = 64;
//    bool isWrite = false;
    bool isWrite = true;
    msgs.emplace_back(0 * incr, size, isWrite);
    msgs.emplace_back(0 * incr, size, isWrite);
    msgs.emplace_back(1 * incr, size, isWrite);
    msgs.emplace_back(1 * incr, size, isWrite);
    msgs.emplace_back(2 * incr, size, isWrite);
    msgs.emplace_back(2 * incr, size, isWrite);
    msgs.emplace_back(3 * incr, size, isWrite);
    msgs.emplace_back(3 * incr, size, isWrite);
    CacheMessage flush;
    flush.flush = true;
    msgs.push_back(flush);
    printf("msgs creating done\n");

    while (true) {
        bool done = naiveUpstreamTick();
        cache.tick();
        naiveDownstreamTick();
        if (done) { break; }
    }
    printf("run done\n");
}


void testSingleBankSingleSetMultiWay() {
    CacheConfig config(1, 2, 1, 64,
                       8, 8);
    slavePortInput.resize(config.nBanks);
    slavePortOutput.resize(config.nBanks);
    masterPortInput.resize(config.nBanks);
    masterPortOutput.resize(config.nBanks);

    slavePort.resize(config.nBanks);
    masterPort.resize(config.nBanks);

    slavePort[0].assign(slavePortInput[0], slavePortOutput[0]);
    masterPort[0].assign(masterPortInput[0], masterPortOutput[0]);

    Cache cache(config);
    cache.assignPort(0, masterPort[0], slavePort[0]);
    printf("cache creating done\n");

    uint size = 64;
    uint incr = 64;
//    bool isWrite = false;
    bool isWrite = true;
    msgs.emplace_back(0 * incr, size, isWrite);
    msgs.emplace_back(1 * incr, size, isWrite);
    msgs.emplace_back(0 * incr, size, isWrite);
    msgs.emplace_back(1 * incr, size, isWrite);
    msgs.emplace_back(2 * incr, size, isWrite);
    msgs.emplace_back(3 * incr, size, isWrite);
    msgs.emplace_back(4 * incr, size, isWrite);
    msgs.emplace_back(5 * incr, size, isWrite);
    msgs.emplace_back(4 * incr, size, isWrite);
    msgs.emplace_back(5 * incr, size, isWrite);
    CacheMessage flush;
    flush.flush = true;
    msgs.push_back(flush);
    printf("msgs creating done\n");

    while (true) {
        bool done = naiveUpstreamTick();
        cache.tick();
        naiveDownstreamTick();
        if (done) { break; }
    }
    printf("run done\n");
}


void testSingleBankMultiSetSingleWay() {
    CacheConfig config(2, 1, 1, 64,
                       8, 8);
    slavePortInput.resize(config.nBanks);
    slavePortOutput.resize(config.nBanks);
    masterPortInput.resize(config.nBanks);
    masterPortOutput.resize(config.nBanks);

    slavePort.resize(config.nBanks);
    masterPort.resize(config.nBanks);

    slavePort[0].assign(slavePortInput[0], slavePortOutput[0]);
    masterPort[0].assign(masterPortInput[0], masterPortOutput[0]);

    Cache cache(config);
    cache.assignPort(0, masterPort[0], slavePort[0]);
    printf("cache creating done\n");

    uint size = 64;
    uint incr = 64;
    bool isWrite = false;
//    bool isWrite = true;
    msgs.emplace_back(0 * incr, size, isWrite);
    msgs.emplace_back(1 * incr, size, isWrite);
    msgs.emplace_back(2 * incr, size, isWrite);
    msgs.emplace_back(3 * incr, size, isWrite);
    msgs.emplace_back(2 * incr, size, isWrite);
    msgs.emplace_back(3 * incr, size, isWrite);
    msgs.emplace_back(4 * incr, size, isWrite);
    msgs.emplace_back(5 * incr, size, isWrite);
    msgs.emplace_back(2 * incr, size, isWrite);
    msgs.emplace_back(3 * incr, size, isWrite);
    msgs.emplace_back(6 * incr, size, isWrite);
    msgs.emplace_back(7 * incr, size, isWrite);
    CacheMessage flush;
    flush.flush = true;
    msgs.push_back(flush);
    printf("msgs creating done\n");

    while (true) {
        bool done = naiveUpstreamTick();
        cache.tick();
        naiveDownstreamTick();
        if (done) { break; }
    }
    printf("run done\n");
}


void testMultiBankSingleSetSingleWay() {
    CacheConfig config(1, 1, 2, 64,
                       8, 8);
    slavePortInput.resize(config.nBanks);
    slavePortOutput.resize(config.nBanks);
    masterPortInput.resize(config.nBanks);
    masterPortOutput.resize(config.nBanks);

    slavePort.resize(config.nBanks);
    masterPort.resize(config.nBanks);

    Cache cache(config);
    for (uint i = 0; i < config.nBanks; i++) {        
        slavePort[i].assign(slavePortInput[i], slavePortOutput[i]);
        masterPort[i].assign(masterPortInput[i], masterPortOutput[i]);

        cache.assignPort(i, masterPort[i], slavePort[i]);
    }
    printf("cache creating done\n");

    uint size = 64;
    uint incr = 64;
//    bool isWrite = false;
    bool isWrite = true;
    msgs.emplace_back(0 * incr, size, isWrite);
    msgs.emplace_back(1 * incr, size, isWrite);
    msgs.emplace_back(2 * incr, size, isWrite);
    msgs.emplace_back(3 * incr, size, isWrite);
    msgs.emplace_back(0 * incr, size, isWrite);
    msgs.emplace_back(1 * incr, size, isWrite);
    msgs.emplace_back(0 * incr, size, isWrite);
    msgs.emplace_back(1 * incr, size, isWrite);
    msgs.emplace_back(2 * incr, size, isWrite);
    msgs.emplace_back(3 * incr, size, isWrite);
    msgs.push_back(CacheMessage::getFlushMsg(0 * config.lineBytes));
    msgs.push_back(CacheMessage::getFlushMsg(1 * config.lineBytes));
    msgs.push_back(CacheMessage::getFlushMsg(2 * config.lineBytes));
    msgs.push_back(CacheMessage::getFlushMsg(3 * config.lineBytes));
    printf("msgs creating done\n");

    while (true) {
        bool done = naiveUpstreamTick(0x1);
        cache.tick();
        naiveDownstreamTick();
        if (done) { break; }
    }
    printf("run done\n");
}


void testAllIn() {
    CacheConfig config(256, 4, 2, 64,
                       8, 8);
    printf("total capacity: %lfKB\n", config.getTotalCapacity() / 1024.0);
    slavePortInput.resize(config.nBanks);
    slavePortOutput.resize(config.nBanks);
    masterPortInput.resize(config.nBanks);
    masterPortOutput.resize(config.nBanks);

    slavePort.resize(config.nBanks);
    masterPort.resize(config.nBanks);

    Cache cache(config);
    for (uint i = 0; i < config.nBanks; i++) {
        slavePort[i].assign(slavePortInput[i], slavePortOutput[i]);
        masterPort[i].assign(masterPortInput[i], masterPortOutput[i]);

        cache.assignPort(i, masterPort[i], slavePort[i]);
    }
    printf("cache creating done\n");

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> readWriteDist(0, 1);
//    std::uniform_int_distribution<> blockIdxDist(0, 1024);
//    std::uniform_int_distribution<> blockIdxDist(0, 2048);
    std::uniform_int_distribution<> blockIdxDist(0, 4096);
//    std::uniform_int_distribution<> blockIdxDist(0, 16384);
    std::uniform_int_distribution<> byteOffsetDist(0, 64);
    std::uniform_int_distribution<> sizeDist(0, 64);

//    uint num = 10;
//    uint num = 100;
    uint num = 1000;
//    uint num = 10000;
    for (int i = 0; i < num; i++) {
        while (true) {
            bool isWrite = readWriteDist(gen);
            uint64 blockIdx = blockIdxDist(gen);
            uint64 byteOffset = byteOffsetDist(gen);
            uint64 size = sizeDist(gen);
            if (byteOffset + size <= 64) {
                msgs.emplace_back((blockIdx << 6) + byteOffset, size, isWrite);
                auto &msg = msgs.back();
                break;
            }
        }
    }
    for (int i = 0; i < config.nBanks; i++) {
        msgs.push_back(CacheMessage::getFlushMsg(i * config.lineBytes));
    }
    printf("msgs creating done\n");

    while (true) {
        bool done = naiveUpstreamTick(0x1);
        cache.tick();
        naiveDownstreamTick();
        if (done) { break; }
    }
    printf("run done\n");

    auto counter = cache.getTotalCounter();
    printf("%lu %lu %lu %lu %lu %lu %lu %lu\n",
           counter.readHit, counter.readMiss, counter.writeHit, counter.writeMiss,
           counter.readBytes, counter.writeBytes, counter.flushBytes, counter.fetchBytes);
    printf("totalAccess=%lu\n", counter.totalAccess());
    printf("totalAccessBytes=%lu\n", counter.totalAccessBytes());
    printf("totalMemAccessBytes=%lu\n", counter.totalMemAccessBytes());
    printf("hitRate=%lf\n", counter.hitRate());

    uint64 totalBytes = 0;
    for (auto &msg : msgs) {
        totalBytes += msg.size;
    }
    printf("bw=%lf\n", (double)totalBytes / counter.cycles);
}


bool naiveUpstreamTick(uint64 bankIdxMask, uint64 bankIdxOffset) {
    static uint msgIdx = 0;
    static uint finished = 0;
    static uint64 cycles = 0;
    if (msgIdx < msgs.size()) {
        auto &msg = msgs[msgIdx];
        uint bankIdx = (msg.addr >> bankIdxOffset) & bankIdxMask;
        slavePortInput[bankIdx].push_back(&msg);
        msgIdx++;
    }
    for (uint i = 0; i < slavePortOutput.size(); i++) {
        auto &outputPort = slavePortOutput[i];
        if (not outputPort.empty()) {
            auto msg = dynamic_cast<CacheMessage*>(outputPort.front());
            printf("%lu upstream, addr=%lx, size=%ld, isWrite=%d\n",
                   cycles, msg->addr, msg->size, msg->isWrite);
            outputPort.pop_front();
            finished++;
        }
    }
    cycles++;
    return finished == msgs.size();
}


void naiveDownstreamTick() {
    static uint64 cycles = 0;
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
