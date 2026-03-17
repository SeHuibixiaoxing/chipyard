#include "mudnacsim.h"
#include "cxxopts.hpp"
#include "thread_run_pipeline.h"


int main(int argc, char *argv[]) {
    const std::string MODEL_ROOT = "models/pipeline/";
    const std::string OUTPUT_ROOT = "expr/output/expr_multi_pipeline/default/";

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

    // Part4: Load models (support comma-separated list in `ap.model`)

    // Split ap.model by comma and trim spaces
    std::vector<std::string> modelNames;
    {
        std::string modelsStr = ap.model;
        std::stringstream mss(modelsStr);
        std::string name;
        while (std::getline(mss, name, ',')) {
            // trim leading/trailing whitespace
            size_t l = name.find_first_not_of(" \t\n\r");
            if (l == std::string::npos) continue;
            size_t r = name.find_last_not_of(" \t\n\r");
            modelNames.push_back(name.substr(l, r - l + 1));
        }
    }

    // Load each model and create a pipeline mapping for it.
    std::vector<std::unique_ptr<Model>> models;
    std::vector<std::shared_ptr<PipelineMapping>> pipelineMappings;
    uint64 addrOffsetLocal = 0;
    // Split ap.pipelineMappingPath by comma to support per-model mapping files
    std::vector<std::string> pipelineMappingNames;
    {
        std::string mapsStr = ap.pipelineMappingPath;
        std::stringstream mss(mapsStr);
        std::string mname;
        while (std::getline(mss, mname, ',')) {
            size_t l = mname.find_first_not_of(" \t\n\r");
            if (l == std::string::npos) continue;
            size_t r = mname.find_last_not_of(" \t\n\r");
            pipelineMappingNames.push_back(mname.substr(l, r - l + 1));
        }
    }
    std::vector<uint64> model_addr_local;
    for (size_t i = 0; i < modelNames.size(); ++i) {
        auto mptr = std::make_unique<Model>();
        ModelLoader::load(*mptr, MODEL_ROOT + modelNames[i], addrOffsetLocal, 0, 1);
        model_addr_local.push_back(addrOffsetLocal);
        addrOffsetLocal = mptr->addrHi;
        models.push_back(std::move(mptr));
        std::string mappingPathForModel = pipelineMappingNames[i];

        std::cout << "Loaded model " << modelNames[i] << " with mapping " << mappingPathForModel << "\n";

        auto pm = PipelineMappingParser::pipelineCandidateToPipelineMapping(models.back().get(), mappingPathForModel);
        assert(!pm.segmentCandidates.empty());
        pipelineMappings.push_back(std::make_shared<PipelineMapping>(pm));
    }

    // 暂时将所有subbatch设置为1
    std::cout << "total spm page: " << config.getSPMTotalPages() << "\n";

    uint64 maxCycles = UINT64_MAX;
    uint totalBatch = ap.totalBatch;

    // Create one ThreadRunPipeline per loaded model and keep ownership of thread objects
    std::vector<std::vector<std::unique_ptr<ThreadRunPipeline>>> threadPtrs(models.size()); // model_idx -> idx -> thread
    std::vector<std::vector<uint64>> model_cycles(models.size()); // model_idx -> idx -> cycles
    uint total_threads = 0;
    for (size_t i = 0; i < models.size(); ++i) {
        threadPtrs[i].push_back(std::make_unique<ThreadRunPipeline>(total_threads, models[i].get(), pipelineMappings[i], maxCycles, totalBatch));
        std::cout << "create ThreadRunPipeline. tid=" << total_threads << ", model_idx=" << i << std::endl;
        ++ total_threads;
    }

    // Part6: Run

    printf("\nrunning...\n");
    {
        std::vector<uint> model_has_done(modelNames.size(), 0);
        std::vector<uint> model_run_cnt(modelNames.size(), 0);
        while (true) {
            if (RT.getCycles() % 10000000 == 0) {
                std::cout << "Cycles: " << RT.getCycles() << ", Models has Done: " << mudnac::toString(model_has_done) << ", Models run cnt: " << mudnac::toString(model_run_cnt) << std::endl;
            }

            for (uint model_idx = 0;model_idx < modelNames.size();++ model_idx) {
                uint j = model_run_cnt[model_idx];
                bool ok = threadPtrs[model_idx][j]->tick();
                if (ok) {
                    model_cycles[model_idx].push_back(threadPtrs[model_idx][j]->end_cycles - threadPtrs[model_idx][j]->start_cycles);
                    ++ model_run_cnt[model_idx];
                    model_has_done[model_idx] = 1;

                    std::cout << "Model " << modelNames[model_idx] << " run " << j << " done, cycles=" << (threadPtrs[model_idx][j]->end_cycles - threadPtrs[model_idx][j]->start_cycles) << "\n";

                    auto mptr = std::make_unique<Model>();
                    ModelLoader::load(*mptr, MODEL_ROOT + modelNames[model_idx], model_addr_local[model_idx], 0, 1);
                    models[model_idx] = nullptr;
                    models[model_idx] = std::move(mptr);


                    std::string mappingPathForModel = pipelineMappingNames[model_idx];
                    std::cout << "Loaded model " << modelNames[model_idx] << " with mapping " << mappingPathForModel << "\n";
                    pipelineMappings[model_idx] = std::make_shared<PipelineMapping>(PipelineMappingParser::pipelineCandidateToPipelineMapping(models[model_idx].get(), mappingPathForModel));

                    threadPtrs[model_idx].push_back(std::make_unique<ThreadRunPipeline>(total_threads, models[model_idx].get(), pipelineMappings[model_idx], maxCycles, totalBatch));
                    std::cout << "create ThreadRunPipeline. tid=" << total_threads << ", model_idx=" << model_idx << std::endl;
                    ++ total_threads;
                }
            }

            uint finished = 0;
            for (auto t: model_has_done) {
                finished += t;
            }
            if (finished == modelNames.size()) {
                std::cout << "the last model done\n";
                break;
            }
            
            system->tick();
        }
    }

    // Part7: Print and return

    // ss << "Model Latency: " << thread.modelCyclesList[0] << "\n";
    // ss << "Layer Latency: " << mudnac::toString(thread.layerCyclesList[0]).c_str() << "\n";
    ss << "Total Latency: " << RT.getCycles() << std::endl;
    ss << "Model Latencies:\n";
    for (size_t i = 0; i < modelNames.size(); ++i) {
        ss << "  Model " << modelNames[i] << ":\n";
        for (size_t j = 0; j < model_cycles[i].size(); ++j) {
            ss << "    Run " << j << ": " << model_cycles[i][j] << " cycles\n";
        }
    }
    ss << "Model Avg Latencies:\n";
    for (size_t i = 0; i < modelNames.size(); ++i) {
        uint64 total_model_cycles = 0;
        for (size_t j = 0; j < model_cycles[i].size(); ++j) {
            total_model_cycles += model_cycles[i][j];
        }
        uint64 avg_model_cycles = total_model_cycles / model_cycles[i].size();
        ss << "  Model " << modelNames[i] << ": " << avg_model_cycles << " cycles\n";
    }
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
