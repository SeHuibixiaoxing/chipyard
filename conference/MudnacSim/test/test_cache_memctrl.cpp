#include <cstdio>
#include <cache.h>
#include <xbar.h>
#include <random>


using namespace mudnac;


static uint64 numMsgs = 0;

static std::deque<std::deque<Message *>> memSlavePortInput;
static std::deque<std::deque<Message *>> memSlavePortOutput;
static std::deque<Port> memSlavePort;

static std::deque<std::deque<Message *>> cacheSlavePortInput;
static std::deque<std::deque<Message *>> cacheSlavePortOutput;
static std::deque<std::deque<Message *>> cacheMasterPortInput;
static std::deque<std::deque<Message *>> cacheMasterPortOutput;
static std::deque<Port> cacheSlavePort;
static std::deque<Port> cacheMasterPort;


bool naiveAccelTick();


int main() {
    printf("main starts\n");

//    uint channels = 1;
//    uint channels = 2;
//    uint channels = 4;
    uint channels = 8;

    std::string dramsim3ConfigPath = "DRAMsim3/configs/DDR4_8Gb_x16_3200_am1.ini";
    MemCtrlConfig memCfg = {
            dramsim3ConfigPath,
            "test/output_test_default",
            1000,
            64,
            channels
    };
    MemController memCtrl(memCfg);

    CacheConfig cacheCfg;
    cacheCfg.nSets = 128;
    cacheCfg.nWays = 16;
//    cacheCfg.nBanks = 1;
//    cacheCfg.nBanks = 2;
//    cacheCfg.nBanks = 4;
//    cacheCfg.nBanks = 8;
    cacheCfg.nBanks = 16;
    cacheCfg.lineBytes = 64;
    cacheCfg.memCtrlChannels = channels;
    cacheCfg.upstreamBeatBytes = 1;
    cacheCfg.downstreamBeatBytes = 1;
    cacheCfg.writeThrough = false;
//    cacheCfg.writeThrough = true;
//    cacheCfg.writeAllocation = false;
    cacheCfg.writeAllocation = true;

    Cache cache(cacheCfg);
    printf("total capacity: %lfKB\n", cacheCfg.getTotalCapacity() / 1024.0);

    memSlavePortInput.resize(channels);
    memSlavePortOutput.resize(channels);
    memSlavePort.resize(channels);

    cacheSlavePortInput.resize(cacheCfg.nBanks);
    cacheSlavePortOutput.resize(cacheCfg.nBanks);
    cacheSlavePort.resize(cacheCfg.nBanks);

    cacheMasterPortInput.resize(cacheCfg.nBanks);
    cacheMasterPortOutput.resize(cacheCfg.nBanks);
    cacheMasterPort.resize(cacheCfg.nBanks);

    XBar memCtrlXbar;
    for (int i = 0; i < memCfg.channels; i++) {
        memSlavePort[i].assign(memSlavePortInput[i], memSlavePortOutput[i]);
        memCtrl.assignPort(i, memSlavePort[i]);
        memCtrlXbar.connectPort(PortIDManager::localToGlobalMEMSlave(i), memSlavePort[i]);
    }
    for (uint i = 0; i < cacheCfg.nBanks; i++) {
        cacheSlavePort[i].assign(cacheSlavePortInput[i], cacheSlavePortOutput[i]);
        cacheMasterPort[i].assign(cacheMasterPortInput[i], cacheMasterPortOutput[i]);
        cache.assignPort(i, cacheMasterPort[i], cacheSlavePort[i]);
        memCtrlXbar.connectPort(PortIDManager::localToGlobalCacheMaster(i), cacheMasterPort[i]);
    }

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> readWriteDist(0, 1);
//    std::uniform_int_distribution<> blockIdxDist(0, 1024);
//    std::uniform_int_distribution<> blockIdxDist(0, 2048);
//    std::uniform_int_distribution<> blockIdxDist(0, 4096);
//    std::uniform_int_distribution<> blockIdxDist(0, 16384);
//    std::uniform_int_distribution<> blockIdxDist(0, 0x1000);
    std::uniform_int_distribution<> blockIdxDist(0, 0x10000);
//    std::uniform_int_distribution<> blockIdxDist(0, 0x100000);

//    numMsgs = 1;
//    numMsgs = 10;
//    numMsgs = 100;
//    numMsgs = 1000;
    numMsgs = 10000;
//    numMsgs = 100000;
//    numMsgs = 1000000;

    bool isWrite = false;
//    bool isWrite = true;
    uint64 blockIdx = 0;
    uint bankIdxOffset = log2(cache.config.lineBytes);
    uint bankIdxMask = cache.config.nBanks - 1;
    for (uint64 i = 0; i < numMsgs; i++) {
//        isWrite = readWriteDist(gen);
//        blockIdx = blockIdxDist(gen);
        auto msg = new CacheMessage(blockIdx << bankIdxOffset, cache.config.lineBytes, isWrite);
        msg->setTrans(msg->isWrite ? 1 : msg->size,
                     msg->isWrite ? msg->size : 1,
                     PortIDManager::localToGlobalAccelMaster(0),
                     PortIDManager::localToGlobalCacheSlave((msg->addr >> bankIdxOffset) & bankIdxMask));
        cacheSlavePortInput[PortIDManager::globalToLocalCache(msg->slavePortId)].push_back(msg);
        blockIdx++;
    }
//    for (int i = 0; i < cacheCfg.nBanks; i++) {
//        msgs.push_back(CacheMessage::getFlushMsg(i * cacheCfg.lineBytes));
//    }
    printf("msgs creating done\n");

    while (true) {
        bool done = naiveAccelTick();
        cache.tick();
        memCtrl.tick();
        memCtrlXbar.tick();
        if (done) { break; }
    }
    printf("run done\n");

    auto counter = cache.getTotalCounter();
    printf("\nCycles: %lu\n\n", counter.cycles);

    printf("%lu %lu %lu %lu %lu %lu %lu %lu\n",
           counter.readHit, counter.readMiss, counter.writeHit, counter.writeMiss,
           counter.readBytes, counter.writeBytes, counter.flushBytes, counter.fetchBytes);
    printf("totalAccess=%lu\n", counter.totalAccess());
    printf("totalAccessBytes=%lu\n", counter.totalAccessBytes());
    printf("totalMemAccessBytes=%lu\n", counter.totalMemAccessBytes());
    printf("hitRate=%lf\n", counter.hitRate());

    auto memCounter = memCtrl.getSummaryCounter();
    printf("Memory Controller Info: \n");
    printf("Bandwidth=%lf(Bytes/Cycles)\n", memCounter.getAvgBW());
    printf("Bandwidth=%lf(GB/s)\n", memCounter.getAvgBW() * (memCtrl.config.freqMHz / 1000.0));

    std::cout << "\n";
    std::cout << "Cache: " << "\n";
    std::cout << cache.getPerformanceInfo() << "\n";
    std::cout << "\n";
    std::cout << "DRAM: " << "\n";
    std::cout << memCtrl.getPerformanceInfo() << "\n";
    std::cout << "\n";

    printf("main ends\n");
    return 0;
}


bool naiveAccelTick() {
    static uint msgIdx = 0;
    static uint finished = 0;
    static uint64 cycles = 0;
    for (uint i = 0; i < cacheSlavePortOutput.size(); i++) {
        auto &outputPort = cacheSlavePortOutput[i];
        while (not outputPort.empty()) {
            auto msg = dynamic_cast<CacheMessage*>(outputPort.front());
//            printf("%lu upstream, addr=%lx, size=%ld, isWrite=%d, bankIdx=%u\n",
//                   cycles, msg->addr, msg->size, msg->isWrite, msg->downPortId);
            outputPort.pop_front();
            delete msg;
            finished++;
        }
    }
    cycles++;
    return finished == numMsgs;
}
