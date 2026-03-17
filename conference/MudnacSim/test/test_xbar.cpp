#include "xbar.h"


using namespace mudnac;


const uint NUM_SPM = 3;
const uint NUM_MEM = 2;
const uint64 BEAT_BYTES = 8;

/*
SPM0    SPM1     SPM2
MASTER  MASTER   MASTER

SLAVE   SLAVE    SLAVE
MEM0    MEM1     MEM2
*/

std::deque<Message> msgs;

std::deque<std::deque<Message *>> masterPortInput(NUM_SPM);
std::deque<std::deque<Message *>> masterPortOutput(NUM_SPM);
std::deque<std::deque<Message *>> slavePortInput(NUM_MEM);
std::deque<std::deque<Message *>> slavePortOutput(NUM_MEM);

std::vector<Port> spmMasterPort;
std::vector<Port> memSlavePort;

XBar xbar;


void test();

void downstreamTick();

bool upstreamTick();


int main() {
    spmMasterPort.resize(NUM_SPM);
    memSlavePort.resize(NUM_MEM);
    for (uint i = 0; i < NUM_SPM; i++) {
        spmMasterPort[i].assign(masterPortInput[i], masterPortOutput[i]);
        xbar.connectPort(PortIDManager::localToGlobalSPMMaster(i), spmMasterPort[i]);
    }
    for (uint i = 0; i < NUM_MEM; i++) {
        memSlavePort[i].assign(slavePortInput[i], slavePortOutput[i]);
        xbar.connectPort(PortIDManager::localToGlobalMEMSlave(i), memSlavePort[i]);        
    }

    test();
}


void test() {
//    for (uint i = 0; i < 10; i++) {
//        msgs.emplace_back(8, 8, i % NUM_UP, 0);
//    }

//    for (uint i = 0; i < 10; i++) {
//        msgs.emplace_back(8, 64, i % NUM_UP, 0);
//    }

//    for (uint i = 0; i < 10; i++) {
//        msgs.emplace_back(64, 8, i % NUM_UP, 0);
//    }

//    for (uint i = 0; i < 10; i++) {
//        msgs.emplace_back(8, 64, 0, i % NUM_DOWN);
//    }

//    for (uint i = 0; i < 10; i++) {
//        msgs.emplace_back(64, 8, 0, i % NUM_DOWN);
//    }

    for (uint i = 0; i < 100; i++) {
        msgs.emplace_back(64, 64, PortIDManager::localToGlobalSPMMaster(i % NUM_SPM), PortIDManager::localToGlobalMEMSlave(i % NUM_MEM), Message::META_TYPE_SPM);
    }

    while (true) {
        bool done = upstreamTick();
        xbar.tick();
        downstreamTick();
        if (done) { break; }
    }
}


bool upstreamTick() {
    static uint msgIdx = 0;
    static uint finished = 0;
    static uint64 cycles = 0;
    if (msgIdx < msgs.size()) {
        auto &msg = msgs[msgIdx];
        masterPortOutput[PortIDManager::globalToLocalSPM(msg.masterPortId)].push_back(&msg);
        msgIdx++;
    }
    for (uint i = 0; i < NUM_SPM; i++) {
        auto &masterInput = masterPortInput[i];
        if (not masterInput.empty()) {
            auto msg = masterInput.front();
            masterInput.pop_front();
            assert(msg->masterPortId == PortIDManager::localToGlobalSPMMaster(i));
            printf("%lu spm master port, finished a msg, s2mBytes=%#lx, m2sBytes=%#lx, masterPortId=%#x slavePort=%#x\n",
                   cycles, msg->s2mBytes, msg->m2sBytes, msg->masterPortId, msg->slavePortId);
            finished++;
        }
    }
    cycles++;
    return finished == msgs.size();
}


void downstreamTick() {
    static uint64 cycles = 0;
    for (uint i = 0; i < NUM_MEM; i++) {
        auto &slaveInput = slavePortInput[i];
        auto &slaveOutput = slavePortOutput[i];
        if (not slaveInput.empty()) {
            auto msg = slaveInput.front();
            printf("%lu mem slave port, receive a msg, s2mBytes=%#lx, m2sBytes=%#lx, masterPortId=%#x slavePortId=%#x\n",
                   cycles, msg->s2mBytes, msg->m2sBytes, msg->masterPortId, msg->slavePortId);
                   slaveInput.pop_front();
            assert(msg->slavePortId == PortIDManager::localToGlobalMEMSlave(i));
            slaveOutput.push_back(msg);
        }
    }
    cycles++;
}
