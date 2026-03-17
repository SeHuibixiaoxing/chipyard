#include <iostream>
#include <tools.h>


using namespace mudnac;

int main() {


    for(uint i = 0;i < 10;i ++) {
        uint memMaster = PortIDManager::localToGlobalMEMMaster(i);
        uint memSlave = PortIDManager::localToGlobalMEMSlave(i);
        uint spmMaster = PortIDManager::localToGlobalSPMMaster(i);
        uint spmSlave = PortIDManager::localToGlobalSPMSlave(i);
        uint accelMaster = PortIDManager::localToGlobalAccelMaster(i);
        uint accelSlave = PortIDManager::localToGlobalAccelSlave(i);
        uint cacheMaster = PortIDManager::localToGlobalCacheMaster(i);
        uint cacheSlave = PortIDManager::localToGlobalCacheSlave(i);
        
        printf("===========================\n");
        printf("local id: %#x\n", i);
        printf("Mem global id: master %#x, slave %#x\n", memMaster, memSlave);
        printf("SPM global id: master %#x, slave %#x\n", spmMaster, spmSlave);
        printf("Accelerator global id: master %#x, slave %#x\n", accelMaster, accelSlave);
        printf("Cache global id: master %#x, slave %#x\n", cacheMaster, cacheSlave);

        assert(PortIDManager::isAccel(accelMaster));
        assert(PortIDManager::isAccel(accelSlave));

        assert(PortIDManager::isSPM(spmMaster));
        assert(PortIDManager::isSPM(spmSlave));

        assert(PortIDManager::isMemCtrlChannel(memMaster));
        assert(PortIDManager::isMemCtrlChannel(memSlave));

        assert(PortIDManager::isCache(cacheMaster));
        assert(PortIDManager::isCache(cacheSlave));

        assert(PortIDManager::isMaster(memMaster));
        assert(PortIDManager::isMaster(spmMaster));
        assert(PortIDManager::isMaster(accelMaster));
        assert(PortIDManager::isMaster(cacheMaster));

        assert(PortIDManager::isSlave(memSlave));
        assert(PortIDManager::isSlave(spmSlave));
        assert(PortIDManager::isSlave(accelSlave));
        assert(PortIDManager::isSlave(cacheSlave));

        assert(PortIDManager::globalToLocalAccel(accelMaster) == i);
        assert(PortIDManager::globalToLocalSPM(spmMaster) == i);
        assert(PortIDManager::globalToLocalMEM(memMaster) == i);
        assert(PortIDManager::globalToLocalCache(cacheMaster) == i);

        assert(PortIDManager::globalToLocalAccel(accelSlave) == i);
        assert(PortIDManager::globalToLocalSPM(spmSlave) == i);
        assert(PortIDManager::globalToLocalMEM(memSlave) == i);
        assert(PortIDManager::globalToLocalCache(cacheSlave) == i);

        assert(PortIDManager::masterToSlave(memMaster) == memSlave);
        assert(PortIDManager::masterToSlave(spmMaster) == spmSlave);
        assert(PortIDManager::masterToSlave(accelMaster) == accelSlave);
        assert(PortIDManager::masterToSlave(cacheMaster) == cacheSlave);

        assert(PortIDManager::slaveToMaster(memSlave) == memMaster);
        assert(PortIDManager::slaveToMaster(spmSlave) == spmMaster);
        assert(PortIDManager::slaveToMaster(accelSlave) == accelMaster);
        assert(PortIDManager::slaveToMaster(cacheSlave) == cacheMaster);
    }
    printf("\ntest pass\n");
}
