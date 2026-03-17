#include "cache_noc_system.h"
#include "spm_noc_system.h"


using namespace mudnac;


static void test();


int main() {
    test();

    return 0;
}


static void test() {
    SystemConfig sysCfg;
    sysCfg.cacheTotalBytes = 16 * 1024 * 1024;

//    sysCfg.memCtrlChannels = 1;
//    sysCfg.memCtrlChannels = 2;
//    sysCfg.memCtrlChannels = 4;
//    sysCfg.memCtrlChannels = 8;
    sysCfg.memCtrlChannels = 16;

//    sysCfg.cacheBanks = 1;
//    sysCfg.cacheBanks = 2;
//    sysCfg.cacheBanks = 4;
//    sysCfg.cacheBanks = 8;
//    sysCfg.cacheBanks = 16;
//    sysCfg.cacheBanks = 32;
    sysCfg.cacheBanks = 64;

//    sysCfg.accelNum = 1;
//    sysCfg.accelNum = 2;
//    sysCfg.accelNum = 3;
    sysCfg.accelNum = sysCfg.cacheBanks;
//    sysCfg.accelNum = sysCfg.cacheBanks * 2;

    RouterConfig routerCfg;
    NetworkInterfaceConfig niCfg;

    bool shared = true;
//    bool shared = false;

//    CacheNoCSystemHomoTileMesh system(sysCfg, routerCfg, routerCfg, niCfg, niCfg, shared);
    SPMNoCSystemHomoTileMesh system(sysCfg, routerCfg, routerCfg, niCfg, niCfg, shared);

    std::cout << "\nNoC 0 hopCountTable\n";
    std::cout << toString(system.noc0->hopCountTable).c_str();
    if (not shared) {
        std::cout << "\nNoC 1 hopCountTable\n";
        std::cout << toString(system.noc1->hopCountTable).c_str();
    }
}
