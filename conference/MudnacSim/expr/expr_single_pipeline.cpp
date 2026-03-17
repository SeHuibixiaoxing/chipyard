#include "mudnacsim.h"
#include "cxxopts.hpp"
#include "thread_run_pipeline.h"


int main(int argc, char *argv[]) {
    const std::string MODEL_ROOT = "models/pipeline/";
    const std::string OUTPUT_ROOT = "expr/output/expr_single_stage/default/";

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
    // config.dramsim3ConfigPath = "DRAMsim3/configs/DDR4_8Gb_x16_3200_am1.ini"; // 25.6GB/s / channel
    // config.dramsim3ConfigPath = "DRAMsim3/configs/DDR3_8Gb_x8_1600.ini"; //  12.8 GB/s / channel
    config.dramsim3ConfigPath = "DRAMsim3/configs/DDR4_4Gb_x4_2400.ini"; //  19 GB/s GB/s / channel
    
    config.dramsim3OutputPath = dramsimOutputPath;
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

    SPMNoCSystemHomoTileMesh *system = NULL;
    system = new SPMNoCSystemHomoTileMesh(config, noc0RouterConfig, noc1RouterConfig,
                                            noc0NiConfig, noc1NiConfig, ap.noc_shared);

    // Part3: Initiate runtime

    RT.setSystem(system);
    AcceleratorManager::PipelineAccAllocStrategy acc_manager_strategy;
    if (ap.pipeAccStrategy == "free") {
        acc_manager_strategy = AcceleratorManager::PipelineAccAllocStrategy::FREE;
    } else if (ap.pipeAccStrategy == "hilbert") {
        acc_manager_strategy = AcceleratorManager::PipelineAccAllocStrategy::HILBERT;
    } else {
        assert(false && "Unknown pipeAccStrategy");
    }
    
    PipelineRuntime::PipelineSpmAllocStrategy spm_manager_strategy;
    if (ap.pipeSpmStrategy == "allbank") {
        spm_manager_strategy = PipelineRuntime::PipelineSpmAllocStrategy::ALL_BANK_PAGE_INTER;
    } else if (ap.pipeSpmStrategy == "mindis") {
        spm_manager_strategy = PipelineRuntime::PipelineSpmAllocStrategy::MIN_DIS;
    } else {
        assert(false && "Unknown pipeSpmStrategy");
    }


    RT.PipelineRuntimeInit(system->getAccW(), system->getAccH(),
                              ap.noc0_flit_size,
                              acc_manager_strategy,
                              spm_manager_strategy);

    // Part4: Load models

    uint64 addrOffset = 0;
    Model model;
    ModelLoader::load(model, MODEL_ROOT + ap.model, addrOffset, 0, 1);

    // 暂时将所有subbatch设置为1

    // Part5: Create threads
    // ThreadRunModel thread(0, &model, 0, ap.acc_alloc_idx, ap.spm_alloc_idx);
   
    std::cout << "total spm page: " << config.getSPMTotalPages() << "\n";

    std::unique_ptr<ThreadRunPipeline> threadPtr;
    uint64 maxCycles = UINT64_MAX;
    uint totalBatch = ap.totalBatch;
    auto pipeline_mapping = PipelineMappingParser::pipelineCandidateToPipelineMapping(&model, ap.pipelineMappingPath);
    assert(!pipeline_mapping.segmentCandidates.empty());

    threadPtr = std::make_unique<ThreadRunPipeline>(0, &model, std::make_shared<PipelineMapping>(pipeline_mapping), maxCycles, totalBatch);
    RT.threads.push_back(threadPtr.get());

    // Part6: Run

    printf("\nrunning...\n");
    RT.run();

    // Part7: Print and return

    // ss << "Model Latency: " << thread.modelCyclesList[0] << "\n";
    // ss << "Layer Latency: " << mudnac::toString(thread.layerCyclesList[0]).c_str() << "\n";
    ss << "Total Latency: " << RT.getCycles() << std::endl;
    ss << "\n########## Performance Info ##########\n\n";
    ss << RT.system->getPerformanceInfo().c_str() << "\n";
    // ss << threadPtr->getPerformance() << "\n";

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
