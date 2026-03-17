#include "mudnacsim.h"
#include "cxxopts.hpp"


int main(int argc, char *argv[]) {
    const std::string MODEL_ROOT = "models/";
    const std::string OUTPUT_ROOT = "expr/output/expr_multi_models/default/";

    // Part1: Parse input options

    ArgParser ap;
    ap.noc1_flit_size = 64;
//    ap.noc1FlitBytes = 32;
//    ap.noc1FlitBytes = 16;
    ap.noc1_vc_size = 64 / ap.noc1_flit_size;
    ap.accels = 16;
    ap.channels = 8;
    ap.cache = 16;
    ap.time = 100;
    ap.inf_count = 1;
    ap.output_root = OUTPUT_ROOT;
    ap.parse(argc, argv);
    std::stringstream ss;
    ss << "\n" << ap.getString() << "\n";

    // prepare output folder
    if (ap.output_root.back() != '/') {
        ap.output_root += "/";
    }
    std::string dramsim3OutputRoot = ap.output_root + "dramsim3/";
    std::string mudnacsimOutputRoot = ap.output_root + "mudnacsim/";
    int r = 1;
    r = system(("mkdir -p " + dramsim3OutputRoot).c_str());
    assert(r == 0);
    r = system(("mkdir -p " + mudnacsimOutputRoot).c_str());
    assert(r == 0);
    std::string dramsimOutputPath = dramsim3OutputRoot + ap.output;

    // Part2: Construct hardware system

    SystemConfig config;
    config.dramsim3ConfigPath = "DRAMsim3/configs/DDR4_8Gb_x16_3200_am1.ini";
    config.dramsim3OutputPath = dramsimOutputPath;
    config.memCtrlChannels = ap.channels;
    config.cacheTotalBytes = ap.cache * 1024 * 1024;
    config.cacheWays = 16;
    config.cacheBanks = 8;
    config.spmWays = ap.ways;
    config.spmPageBytes = 32 * 1024;
    config.spmMulticast = ap.spm_multicast;
    config.accelNum = ap.accels;
    config.accelArrayH = config.accelArrayW = 32;

    // reset cache banks for NoC
    if (ap.noc != "none") {
        config.cacheBanks = roundupToPowOfTwo(std::max(config.memCtrlChannels / 2, config.accelNum));
        printf("The number of cache banks is set to %u\n", config.cacheBanks);
    }

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

    uint profileScaleIndex = ceil(log2f((float) ap.threads));
    std::string scaleFolder = "scaling_" + ap.system;
    RT.gv.loadProfilingResults({ap.model}, profileScaleIndex, scaleFolder);

    // Part4: Load models

    uint64 addrOffset = 0;
    std::deque<Model> modelList(ap.threads);
    for (uint tid = 0; tid < ap.threads; tid++) {
        auto &model = modelList[tid];
        ModelLoader::load(model, MODEL_ROOT + ap.model, addrOffset, tid);
        printf("model: name=%s addr=[%lx,%lx)\n", ap.model.c_str(), model.addrLo, model.addrHi);
        addrOffset = model.addrHi;
//        addrOffset += 0x100000000;
    }

    // Part5: Create threads

    const uint64 maxCycles = ap.time * config.freqMHz * 1000000;
    std::deque<ThreadRunModel> threadList;
    for (uint tid = 0; tid < ap.threads; tid++) {
        threadList.emplace_back(tid, &modelList[tid], 0, ap.acc_alloc_idx, ap.spm_alloc_idx,
                                0, ap.inf_count, maxCycles);
        RT.threads.push_back(&threadList.back());
    }

    // Part6: Run

    RT.run();

    // Part7: Print and return

    ss << "\n########## Thread Info ##########\n\n";
    auto memCtrlCounter = system->memCtrl.getSummaryCounter();
    ss << "Total DRAM Access: " << memCtrlCounter.getTotalBytes() << "\n";
    ss << "DRAM Access(Bytes per Thread): " << memCtrlCounter.getTotalBytes() / ap.threads << "\n";
    uint64 totalModelCycles = 0, numRuns = 0;
    for (auto &t: threadList) {
        ss << "Thread " << t.tid << "\n";
        ss << "\tNum of Runs: " << t.getNumRuns() << "\n";
        ss << "\tAvg Model Latency: " << t.getAvgModelCycles() << "\n";
        ss << "\tCycles Waiting for Accel: " << RT.gv.threadWaitingAccCycles[t.tid]
           << "(" << (double) RT.gv.threadWaitingAccCycles[t.tid] / RT.getCycles() << ")" << "\n";
        ss << "\tCycles Waiting for SPM: " << RT.gv.threadWaitingSpmCycles[t.tid]
           << "(" << (double) RT.gv.threadWaitingSpmCycles[t.tid] / RT.getCycles() << ")" << "\n";
        ss << "\tModel Latency: " << mudnac::toString(t.modelCyclesList).c_str() << "\n";
        ss << "\tLayer Latency: " << mudnac::toString(t.layerCyclesList).c_str() << "\n";
        numRuns += t.getNumRuns();
        totalModelCycles += t.getTotalModelCycles();
    }
    ss << "\n";
    ss << "Global Num of Runs: " << numRuns << "\n";
    ss << "Global Avg Latency: " << ((numRuns == 0) ? 0 : (totalModelCycles / numRuns)) << "\n";

    ss << "\n########## Performance Info ##########\n\n";
    ss << RT.system->getPerformanceInfo().c_str() << "\n";

    // Print NoC Info
    if (ap.noc == "HomoTileMesh") {
        BaseNoCSystem *nocSys = dynamic_cast<BaseNoCSystem *>(system);
        ss << "\n########## NoC Performance Info ##########\n\n";
        ss << nocSys->getPerformanceString();
    }

    ss << "\n" << "Elapsed: " << RT.getFormatedRunElapsedTime() << "\n";

    std::cout << "\n" << ss.str() << "\n";

    std::ofstream ofs(mudnacsimOutputRoot + ap.output);
    assert(ofs.is_open());
    ofs << ss.str();
    ofs.close();

    delete RT.system;
    return 0;
}
