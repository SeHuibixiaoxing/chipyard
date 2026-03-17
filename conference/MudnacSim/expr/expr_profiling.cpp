#include "mudnacsim.h"
#include "cxxopts.hpp"


int main(int argc, char *argv[]) {
    // Part1: Parse input options

    ArgParser ap;
    ap.accels = 4;
    ap.accels = 4;
    ap.channels = 4;
    ap.cache = 4;
    ap.parse(argc, argv);
    std::cout << ap.getString() << "\n";

    // prepare output folder
    std::string outputPath = "expr/output/expr_profiling/" + ap.output;
    std::string dramsimOutputPath = outputPath + "/dramsim3";

    // Part2: Construct hardware system

    SystemConfig config;
    config.dramsim3ConfigPath = "DRAMsim3/configs/DDR4_8Gb_x16_3200_am1.ini";
    config.dramsim3OutputPath = dramsimOutputPath;
    config.memCtrlChannels = ap.channels;
    config.cacheTotalBytes = ap.cache * 1024 * 1024;
    config.cacheWays = 16;
    config.cacheBanks = 4;
    config.spmWays = ap.ways;
    config.spmPageBytes = 32 * 1024;
    config.spmMulticast = ap.spm_multicast;
    config.accelNum = ap.accels;
    config.accelArrayH = config.accelArrayW = 32;
    config.accelSpmAddrType = AccelConfig::SPM_ADDR_TYPE_CONTINUOUS;
    if (ap.spm_addr_type == "block") config.accelSpmAddrType = AccelConfig::SPM_ADDR_TYPE_BLOCK_INTERLEAVED;
    if (ap.spm_addr_type == "page") config.accelSpmAddrType = AccelConfig::SPM_ADDR_TYPE_PAGE_INTERLEAVED;

    BaseSystem *system = NULL;
    if (ap.system == "cache") {
        system = new CacheSystem(config);
    } else {
        system = new SPMSystem(config);
    }

    // Part3: Initiate runtime

    RT.setSystem(system);
    RT.gv.accelAllocMode = RT.gv.ACCEL_ALLOC_MODE_STATIC;
    RT.gv.memoryAccessMode = RT.gv.MEMORY_ACCESS_MODE_UNLIMITED;
    RT.gv.spmAllocMode = RT.gv.SPM_ALLOC_MODE_STATIC;
    if (ap.spm_alloc_mode == "dynamic") {
        RT.gv.spmAllocMode = RT.gv.SPM_ALLOC_MODE_DYNAMIC;
    } else if (ap.spm_alloc_mode == "fix") {
        RT.gv.spmAllocMode = RT.gv.SPM_ALLOC_MODE_FIX;
    }
printf("runtime initiation is done\n");

    // Part4: Load models

    Model model;
    ModelLoader::load(model, "models/" + ap.model);
    printf("model loading is done\n");

    // Part5+6: Create threads and run

    std::deque<ThreadRunModel> threadList;
    for (uint staticAccAllocIdx = 0; staticAccAllocIdx < RT.gv.NUM_ACCEL_ALLOCS; staticAccAllocIdx++) {
        threadList.emplace_back(0, &model, 0, staticAccAllocIdx, ap.spm_alloc_idx);
        RT.threads.push_back(&threadList.back());
        RT.run();
        RT.threads.pop_back();
    }

    // Part7: Print and return

    uint numLayers = model.numLayers;
    std::stringstream ss;
    for (uint i = 0; i < numLayers; i++) {
        for (uint j = 0; j < 3; j++) {
            ss << threadList[j].layerCyclesList[0][i] << ",";
        }
        ss << "\n";
    }

    std::string ofsPath = outputPath + "/profiling";
    std::ofstream ofs(ofsPath);
    assert(ofs.is_open());
    ofs << ss.str();
    ofs.close();

    delete RT.system;
    return 0;
}
