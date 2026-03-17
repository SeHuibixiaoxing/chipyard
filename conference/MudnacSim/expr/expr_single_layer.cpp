#include "mudnacsim.h"
#include "cxxopts.hpp"


int main(int argc, char *argv[]) {
    // Part1: Parse input options

    ArgParser ap;
    ap.accels = 2;
    ap.channels = 4;
    ap.cache = 4;
    ap.ways = 16;
    ap.layer = 1;
    ap.system = "spm";
    ap.parse(argc, argv);
    std::stringstream ss;
    ss << "\n" << ap.getString() << "\n";

    // Part2: Construct hardware system

    SystemConfig config;
    config.dramsim3ConfigPath = "DRAMsim3/configs/DDR4_8Gb_x16_3200_am1.ini";
    config.dramsim3OutputPath = "expr/output/expr_single_layer/default/dramsim3";
    config.memCtrlChannels = ap.channels;
    config.cacheBanks = ap.accels;
    config.cacheTotalBytes = ap.cache * 1024 * 1024;
    config.cacheWays = 16;
    config.spmWays = config.cacheWays;
    // config.spmPageBytes = 32 * 1024;
    config.spmPageBytes = 1 * 1024;
    config.spmMulticast = ap.spm_multicast;
    config.accelNum = ap.accels;
    config.accelArrayH = config.accelArrayW = 32;
    config.accelSpmAddrType = AccelConfig::SPM_ADDR_TYPE_CONTINUOUS;
    if (ap.spm_addr_type == "block") config.accelSpmAddrType = AccelConfig::SPM_ADDR_TYPE_BLOCK_INTERLEAVED;
    if (ap.spm_addr_type == "page") config.accelSpmAddrType = AccelConfig::SPM_ADDR_TYPE_PAGE_INTERLEAVED;

    NetworkInterfaceConfig noc0NiConfig;
    noc0NiConfig.flitBytes = ap.noc0_flit_size;
    noc0NiConfig.virtualChannels = ap.noc0_vc_num;
    noc0NiConfig.channelSize = ap.noc0_vc_size;
    RouterConfig noc0RouterConfig;
    noc0RouterConfig.virtualChannels = noc0NiConfig.virtualChannels;
    noc0RouterConfig.channelSize = noc0NiConfig.channelSize;

    NetworkInterfaceConfig noc1NiConfig;
    noc1NiConfig.flitBytes = ap.noc1_flit_size;
    noc1NiConfig.virtualChannels = ap.noc1_vc_num;
    noc1NiConfig.channelSize = ap.noc1_vc_size;
    RouterConfig noc1RouterConfig;
    noc1RouterConfig.virtualChannels = noc1NiConfig.virtualChannels;
    noc1RouterConfig.channelSize = noc1NiConfig.channelSize;

    BaseSystem *system = NULL;
    if (ap.system == "cache") {
        if (ap.noc == "HomoTileMesh") {
            system = new CacheNoCSystemHomoTileMesh(config, noc0RouterConfig, noc1RouterConfig,
                                                    noc0NiConfig, noc1NiConfig, ap.noc_shared);
        } else {
            system = new CacheSystem(config);
        }
    } else {
        if (ap.noc == "HomoTileMesh") {
            system = new SPMNoCSystemHomoTileMesh(config, noc0RouterConfig, noc1RouterConfig,
                                                  noc0NiConfig, noc1NiConfig, ap.noc_shared);
        } else {
            system = new SPMSystem(config);
        }
    }
    std::cout << "\nSystem creating is done.\n";

    // Part3: Initiate runtime

    RT.setSystem(system);
    RT.gv.accelAllocMode = RT.gv.ACCEL_ALLOC_MODE_STATIC;
    RT.gv.memoryAccessMode = RT.gv.MEMORY_ACCESS_MODE_UNLIMITED;
    RT.gv.spmAllocMode = RT.gv.SPM_ALLOC_MODE_STATIC;
    if (ap.spm_alloc_mode == "dynamic") {
        RT.gv.spmAllocMode = RT.gv.SPM_ALLOC_MODE_DYNAMIC;
    }

    // Part4: Load models

    Model model;
    ModelLoader::load(model, "models/" + ap.model);
    std::cout << "\nModel loading is done.\n";

    // Part5: Create threads

    ThreadRunLayer thread(0, &model, 0, ap.layer, ap.acc_alloc_idx, ap.spm_alloc_idx);
    RT.threads.push_back(&thread);
    std::cout << "\nThread creating is done.\n";

    // Part6: Run

    std::cout << "\nRunning...\n";
    RT.run();
    std::cout << "\nRunning is done.\n";

    // Part7: Print and return

    ss << "\n########## Performance Info ##########\n\n";
    ss << RT.system->getPerformanceInfo();

    // Print NoC Info
    if (ap.noc == "HomoTileMesh") {
        BaseNoCSystem *nocSys = dynamic_cast<BaseNoCSystem *>(system);
        ss << "\n########## NoC Performance Info ##########\n\n";
        ss << nocSys->getPerformanceString();
    }

    ss << "\n" << "Elapsed: " << RT.getFormatedRunElapsedTime() << "\n";

    std::cout << ss.str() << "\n";

    delete RT.system;
    return 0;
}
