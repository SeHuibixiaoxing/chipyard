#include <mem_ctrl.h>
#include <random>


using namespace mudnac;


static bool naiveAccelTick();

uint64 numMsg;
static std::vector<std::deque<Message *>> slavePortInput;
static std::vector<std::deque<Message *>> slavePortOutput;

static std::vector<Port> slavePorts;


int main() {
    printf("main starts\n");

//    uint64 freqMHz = 100;
    uint64 freqMHz = 1000;
//    uint64 freqMHz = 4000;

//    uint64 slavePortBeatBytes = 64;
//    uint64 slavePortBeatBytes = 32;
//    uint64 slavePortBeatBytes = 8;
    uint64 slavePortBeatBytes = 1;

//    uint channels = 1;
//    uint channels = 2;
//    uint channels = 4;
    uint channels = 8;

    slavePortInput.resize(channels);
    slavePortOutput.resize(channels);
    slavePorts.resize(channels);

    std::string dramsim3ConfigPath = "DRAMsim3/configs/DDR4_8Gb_x16_3200_am1.ini";
//    std::string dramsim3ConfigPath = "DRAMsim3/configs/DDR4_8Gb_x16_3200.ini";

    MemCtrlConfig cfg = {
            dramsim3ConfigPath,
            "test/output_test_mem_ctrl",
            freqMHz,
            slavePortBeatBytes,
            channels
    };

    mudnac::MemController mc(cfg);
    for (int i = 0; i < channels; i++) {
        slavePorts[i].assign(slavePortInput[i], slavePortOutput[i]);
        mc.assignPort(i, slavePorts[i]);
    }

    uint64 burstBytes = MemCtrlConfig::DRAM_BURST_BYTES;
    uint64 channelIdxMask = channels - 1;
    uint64 channelIdxOffset = MemCtrlConfig::DRAM_CHANNEL_IDX_OFFSET;

    bool isWrite = false;
//    bool isWrite = true;

    std::default_random_engine rd;
    std::mt19937 gen(rd());
//    std::uniform_int_distribution<> addrDist(0, 1024 * 16);  // 16 * 64 KB = 1MB
//    std::uniform_int_distribution<> addrDist(0, 1024 * 128);  // 128 * 64KB = 8MB
//    std::uniform_int_distribution<> addrDist(0, 1024 * 1024);  // 1024 * 64KB = 64MB
    std::uniform_int_distribution<> addrDist(0, 1024 * 8024);  // 1024 * 512KB = 512MB

    std::uniform_int_distribution<> isWriteDist(0, 9);

//    numMsg = 100;
//    numMsg = 10000;
    numMsg = 100000;

    for (uint i = 0; i < numMsg; i++) {
        uint64 addr = i * burstBytes;
//        uint64 addr = addrDist(gen) * burstBytes;

        MemCtrlMessage *msg = new MemCtrlMessage(addr, isWrite);        
        msg->setTrans(isWrite ? 1 : burstBytes,
                      isWrite ? burstBytes : 1,
                      PortIDManager::localToGlobalSPMMaster(0),
                      PortIDManager::localToGlobalMEMSlave((addr >> channelIdxOffset) & channelIdxMask));
        
        // printf("spm master port, send a msg %u, s2mBytes=%#lx, m2sBytes=%#lx, masterPortId=%#x slavePort=%#x\n",
        //             i, msg->s2mBytes, msg->m2sBytes, msg->masterPortId, msg->slavePortId);
        slavePortInput[PortIDManager::globalToLocalMEM(msg->slavePortId)].push_back(msg);
    }
    printf("Message creation is done\n");

    uint64 cycles = 0;
    while (true) {
        bool isDone = naiveAccelTick();
        mc.tick();
        cycles++;
        if (isDone) {
            break;
        }
    }
    printf("Simulation is done\n");

    auto counter = mc.getSummaryCounter();
    printf("Trans=%lu, Cycles=%lu\n", numMsg, cycles);
    printf("Bandwidth=%lf(Bytes/Cycles)\n", counter.getAvgBW());
    printf("Bandwidth=%lf(GB/s)\n", counter.getAvgBW() * (freqMHz / 1000.0));

    printf("\n%s\n", mc.getPerformanceInfo().c_str());

    printf("main ends\n");
    return 0;
}


bool naiveAccelTick() {
    static uint64 finishedMsgCount = 0;
    for (auto &port: slavePortOutput) {
        if (not port.empty()) {
            auto msg = port.front();
            port.pop_front();
            finishedMsgCount++;
            delete msg;
        }
    }
    return finishedMsgCount == numMsg;
}
