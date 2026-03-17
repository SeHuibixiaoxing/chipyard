#include "mudnacsim.h"
#include "cxxopts.hpp"


int main(int argc, char *argv[]) {
    const std::string MODEL_ROOT = "models/";
    const std::string OUTPUT_ROOT = "expr/output/expr_single_model/default/";

    // Part1: Parse input options

    ArgParser ap;
    ap.accels = 8;
    ap.channels = 1;
    ap.cache = 18;
    ap.output_root = OUTPUT_ROOT;
    ap.parse(argc, argv);

    if(ap.spmSizePerBank > 0) {
        assert(ap.spmSizePerBankBytes < 0);
        ap.cache = ap.accels * ap.spmSizePerBank;
    }
    if (ap.spmSizePerBankBytes > 0) {
        ap.cache = ap.accels * ap.spmSizePerBankBytes / 1024;
    }

    std::stringstream ss;
    ss << "\n" << ap.getString() << "\n";

    // prepare output folder
    if (ap.output_root.back() != '/') {
        ap.output_root += '/';
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
    config.dramsim3ConfigPath = "DRAMsim3/configs/DDR4_4Gb_x4_2400.ini"; //  19 GB/s GB/s / channel
    // config.dramsim3ConfigPath = "DRAMsim3/configs/DDR3_8Gb_x8_1600.ini"; //  12.8 GB/s / channel
    config.dramsim3OutputPath = dramsimOutputPath;
    config.memCtrlChannels = ap.channels;
    config.cacheTotalBytes = ap.cache * 1024 * 1024;
    config.cacheWays = 16;
    config.cacheBanks = ap.accels;
    config.spmWays = config.cacheWays;
    config.spmPageBytes = 1 * 1024;
    config.spmMulticast = ap.spm_multicast;
    config.accelNum = ap.accels;
    config.accelArrayH = config.accelArrayW = 32;
    config.accelSpmAddrType = AccelConfig::SPM_ADDR_TYPE_CONTINUOUS;
    config.accelSpmAddrType = AccelConfig::SPM_ADDR_TYPE_PAGE_INTERLEAVED;

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
    system = new SPMNoCSystemHomoTileMesh(config, noc0RouterConfig, noc1RouterConfig,
                                            noc0NiConfig, noc1NiConfig, ap.noc_shared);
    // Part3: Initiate runtime

    RT.setSystem(system);
    RT.gv.accelAllocMode = RT.gv.ACCEL_ALLOC_MODE_STATIC;
    RT.gv.memoryAccessMode = RT.gv.MEMORY_ACCESS_MODE_UNLIMITED;
    RT.gv.spmAllocMode = RT.gv.SPM_ALLOC_MODE_FIX;
//    RT.gv.loadProfilingResults({modelName});

    uint64 epoch = 10000;
//    uint64 maxReq = epoch * 64;
    uint64 maxReq = epoch * 8;
//    uint64 maxReq = epoch * 1;
//    RT.gv.bw_threadEpoch[0] = epoch;
//    RT.gv.bw_threadMaxReq[0] = maxReq;

    // Part4: Load models

    uint64 addrOffset = 0;
//    uint64 addrOffset = 0x0ffffff00;
//    uint64 addrOffset = 0xfffffff00;
    Model model;
    if (ap.totalBatch == 1) {
        ModelLoader::load(model, MODEL_ROOT + ap.model, addrOffset);
    } else {
        ModelLoader::load(model, MODEL_ROOT + "batch" + std::to_string(ap.totalBatch) + "/" + ap.model, addrOffset);
    }

    // Part5: Create threads

    auto extOption = ap.extOption;
    auto pos = extOption.find(',');
    
    int startLayerIdx = extOption.empty() ? 0 : std::stoi(extOption.substr(0, pos));
    int endLayerIdx = extOption.empty() ? -1 : std::stoi(extOption.substr(pos + 1));
    std::cout << "startLyaerIdx=" << startLayerIdx << ", endLayerIdx=" << endLayerIdx << std::endl;

    uint accIdx = 0;
    for (;accIdx < ap.accels; accIdx++) {
        if (1 << accIdx == ap.accels) {
            break;
        }
        if (1 << accIdx > ap.accels) {
            std::cout << "accIdx=" << accIdx << ", 1<<accIdx=" << (1<<accIdx) << ", ap.accels=" << ap.accels << std::endl;
            assert(false);
        }
    }
    ThreadRunModel thread(0, &model, 0, accIdx, ap.spm_alloc_idx, startLayerIdx, 1, UINT64_MAX, endLayerIdx);
    RT.threads.push_back(&thread);

    // Part6: Run

    printf("\nrunning...\n");
    RT.run();

    // Part7: Print and return

    ss << "Model Latency: " << thread.modelCyclesList[0] << "\n";
    ss << "Layer Latency: " << mudnac::toString(thread.layerCyclesList[0]).c_str() << "\n";
    ss << "Layer Latency Details: \n";
    for (uint i = 0;i < thread.layerCyclesList[0].size();i++) {
        ss << "Layer " << i << ": " << thread.layerCyclesList[0][i] << "\n";
    }
    ss << "\n########## Performance Info ##########\n\n";
    ss << RT.system->getPerformanceInfo().c_str();

    // Print NoC Info
    // if (ap.noc == "HomoTileMesh") {
    //     BaseNoCSystem *nocSys = dynamic_cast<BaseNoCSystem *>(system);
    //     ss << "\n########## NoC Performance Info ##########\n\n";
    //     ss << nocSys->getPerformanceString();
    // }

    ss << "\n" << "Elapsed: " << RT.getFormatedRunElapsedTime() << "\n";

    std::cout << "\n" << ss.str() << "\n";

    std::ofstream ofs(mudnacsimOutputRoot + ap.output);
    assert(ofs.is_open());
    ofs << ss.str();
    ofs.close();

    delete RT.system;
    return 0;
}
