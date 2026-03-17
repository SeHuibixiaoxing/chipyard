#include "mudnacsim.h"
#include "cxxopts.hpp"


int main(int argc, char *argv[]) {
    const std::string OUTPUT_ROOT = "expr/output/expr_profiling/default/";
    const std::string DRAMSIM3_CONFIG_FOLDER = "DRAMsim3/configs/";

    // Part1: Parse input options

    cxxopts::Options opt("expr_qos");
    opt.add_options()
            ("model", "model name", cxxopts::value<std::string>())
            ("channels", "num of memory channels", cxxopts::value<uint>())
            ("accels", "num of accels", cxxopts::value<uint>())
            ("output", "output name", cxxopts::value<std::string>())
            ("spm-size-per-bank", "SPM size per bank (KB)", cxxopts::value<uint>())
            ("noc-flit-size", "NoC flit size (bytes)", cxxopts::value<uint>())
            ("batch-size", "batch size", cxxopts::value<uint>())
            ("profiling-file-path", "profiling file path", cxxopts::value<std::string>())
            ("profiling-fix-acc-alloc-idx", "profiling with fixed accel alloc idx", cxxopts::value<uint>())
            ("output-root", "output root path", cxxopts::value<std::string>());

    auto arg = opt.parse(argc, argv);

    uint memChannels = 4;
    uint numAccels = 16;
    uint spm_size_per_bank_kb = 1024;
    uint noc_flit_size = 64;
    uint batch_size;
    std::string outputName = "";
    std::string outputRoot = OUTPUT_ROOT;
    std::string model_name;
    std::string profiling_file_path;
    uint fix_acc_idx = 0;


    if (arg.count("channels")) memChannels = arg["channels"].as<uint>();
    if (arg.count("accels")) numAccels = arg["accels"].as<uint>();
    if (arg.count("output")) outputName = arg["output"].as<std::string>();
    if (arg.count("spm_size_per_bank_kb")) spm_size_per_bank_kb = arg["spm-size-per-bank"].as<uint>();
    if (arg.count("noc-flit-size")) noc_flit_size = arg["noc-flit-size"].as<uint>();
    if (arg.count("output-root")) outputRoot = arg["output-root"].as<std::string>();
    if (arg.count("model")) model_name = arg["model"].as<std::string>();
    if (arg.count("batch-size")) batch_size = arg["batch-size"].as<uint>();
    if (arg.count("profiling-file-path")) profiling_file_path = arg["profiling-file-path"].as<std::string>();
    if (arg.count("profiling-fix-acc-alloc-idx")) fix_acc_idx = arg["profiling-fix-acc-alloc-idx"].as<uint>();

    assert(memChannels == 1 or memChannels == 2 or memChannels == 4 or memChannels == 8);
    assert(numAccels > 0);
    {
        std::stringstream ss;
        ss << "\n";
        ss << "expr_profiling2\n";
        ss << "\tmemory channels: " << memChannels << "\n";
        ss << "\tnum of accels: " << numAccels << "\n";
        ss << "\toutput name: " << outputName << "\n";
        ss << "\toutput root: " << outputRoot << "\n";
        ss << "\tmodel name: " << model_name << "\n";
        ss << "\tbatch size: " << batch_size << "\n";
        ss << "\tspm size per bank (KB): " << spm_size_per_bank_kb << "\n";
        ss << "\n";
        std::cout << ss.str();
    }

    // prepare output folder
    if (outputRoot.back() != '/') {
        outputRoot += '/';
    }
    std::string dramsim3OutputRoot = outputRoot + "dramsim3/";
    std::string mudnacsimOutputRoot = outputRoot + "mudnacsim/";
    int r = 1;
    r = system(("mkdir -p " + dramsim3OutputRoot).c_str());
    assert(r == 0);
    r = system(("mkdir -p " + mudnacsimOutputRoot).c_str());
    assert(r == 0);
    
    std::string dramsimOutputPath = dramsim3OutputRoot + outputName;

    // Part2: Construct hardware system

    SystemConfig config;
    config.dramsim3OutputPath = dramsimOutputPath;
    config.dramsim3ConfigPath = DRAMSIM3_CONFIG_FOLDER + "DDR4_4Gb_x4_2400.ini"; //  19 GB/s GB/s / channel
    config.memCtrlChannels = memChannels;
    config.cacheBanks = numAccels;
    config.cacheTotalBytes = numAccels * spm_size_per_bank_kb * 1024;
    config.cacheWays = 16;
    config.spmWays = config.cacheWays;
    config.spmPageBytes = 1 * 1024;
    config.accelNum = numAccels;
    config.accelArrayH = config.accelArrayW = 32;
    config.accelSpmAddrType = AccelConfig::SPM_ADDR_TYPE_PAGE_INTERLEAVED;

    uint noc0_flit_size = noc_flit_size;
    uint noc0_vc_num = 4;
    uint noc0_vc_size = 1;
    uint noc1_flit_size = noc0_flit_size;
    uint noc1_vc_num = noc0_vc_num;
    uint noc1_vc_size = noc0_vc_size;
    if (noc0_vc_size < std::ceil(64.0 / noc0_flit_size)) {
        noc0_vc_size = std::ceil(64.0 / noc0_flit_size);
    }
    if (noc1_vc_size < std::ceil(64.0 / noc1_flit_size)) {
        noc1_vc_size = std::ceil(64.0 / noc1_flit_size);
    }

    NetworkInterfaceConfig noc0NiConfig;
    noc0NiConfig.flitBytes = noc0_flit_size;
    noc0NiConfig.virtualChannels = noc0_vc_num;
    noc0NiConfig.channelSize = noc0_vc_size;
    RouterConfig noc0RouterConfig;
    noc0RouterConfig.virtualChannels = noc0NiConfig.virtualChannels;
    noc0RouterConfig.channelSize = noc0NiConfig.channelSize;

    NetworkInterfaceConfig noc1NiConfig;
    noc1NiConfig.flitBytes = noc1_flit_size;
    noc1NiConfig.virtualChannels = noc1_vc_num;
    noc1NiConfig.channelSize = noc1_vc_size;
    RouterConfig noc1RouterConfig;
    noc1RouterConfig.virtualChannels = noc1NiConfig.virtualChannels;
    noc1RouterConfig.channelSize = noc1NiConfig.channelSize;

    SPMNoCSystemHomoTileMesh *system = NULL;
    system = new SPMNoCSystemHomoTileMesh(config, noc0RouterConfig, noc1RouterConfig,
                                            noc0NiConfig, noc1NiConfig, true);

    // Part3: Initiate runtime

    RT.setSystem(system);

    RT.gv.accelAllocMode = RT.gv.ACCEL_ALLOC_MODE_STATIC;
    RT.gv.memoryAccessMode = RT.gv.MEMORY_ACCESS_MODE_UNLIMITED;
    RT.gv.spmAllocMode = RT.gv.SPM_ALLOC_MODE_STATIC;

    printf("runtime initiation is done\n");

    // Part4: Load models

    Model model;
    ModelLoader::load(model, "models/batch" + std::to_string(batch_size) + "/" + model_name);
    printf("model loading is done\n");

    // Part5+6: Create threads and run

    std::deque<ThreadRunModel> threadList;
    for (uint staticAccAllocIdx = 0; staticAccAllocIdx < RT.gv.NUM_ACCEL_ALLOCS; staticAccAllocIdx++) {
        if (staticAccAllocIdx != fix_acc_idx) {
            continue;
        }
        threadList.emplace_back(0, &model, 0, staticAccAllocIdx, 0);
        RT.threads.push_back(&threadList.back());
        RT.run();
        RT.threads.pop_back();
    }

    // Part7: Print and return

    uint numLayers = model.numLayers;
    std::stringstream ss;
    for (uint i = 0; i < numLayers; i++) {
        for (uint j = 0; j < RT.gv.NUM_ACCEL_ALLOCS; j++) {
            if (j != fix_acc_idx) {
                ss << 0 << ",";
            } else {
                ss << threadList[0].layerCyclesList[0][i] << ",";
            }
        }
        ss << "\n";
    }

    std::string ofsPath = profiling_file_path;
    std::ofstream ofs(ofsPath);
    assert(ofs.is_open());
    ofs << ss.str();
    ofs.close();

    delete RT.system;
    return 0;
}
