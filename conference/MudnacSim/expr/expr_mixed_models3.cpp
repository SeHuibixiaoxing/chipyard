#include "mudnacsim.h"
#include "cxxopts.hpp"

int main(int argc, char *argv[]) {
    const std::string MODEL_ROOT = "models/";
    const std::string OUTPUT_ROOT = "expr/output/expr_mixed_models3/default/";

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
    RT.gv.accelAllocMode = RT.gv.ACCEL_ALLOC_MODE_STATIC;
    RT.gv.memoryAccessMode = RT.gv.MEMORY_ACCESS_MODE_UNLIMITED;
    RT.gv.spmAllocMode = RT.gv.SPM_ALLOC_MODE_FIX;
    RT.gv.spmPageAllocStrategy = RT.gv.SPM_PAGE_ALLOC_STRATEGY_ORDERED;

    // write thread-accel affiliation
//    RT.gv.enableThreadAccelAffiliation = false;
    RT.gv.enableThreadAccelAffiliation = false;
    // RT.gv.threadAccelAffiliation.resize(4); // 4
    // // 4 acc, 4 thread, 64 acc, 16 per thread
    // for (uint tid = 0; tid < 4; tid++) {
    //     uint accIdOffset = (tid / 2) * 32 + (tid %2) * 4;
    //     for (uint idx = 0;idx < 4;++ idx) {
    //         for (uint row_idx = 0;row_idx < 4;++ row_idx) {
    //             RT.gv.threadAccelAffiliation[tid].push_back(accIdOffset + 8 * idx + row_idx);
    //         }
    //     }
    // }

    std::cout << "Thread-Accel Affiliation: " << "\n";
    std::cout << toString(RT.gv.threadAccelAffiliation) << "\n";

    
//    delete system;
//    exit(0);

    // Part4: Load models
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

    ap.threads = modelNames.size();
    assert(ap.threads <= 4);

    // write page allocation order
    {
        // This should be the same as in "spm_noc_system.h"
        uint64 accelNumPerTile = std::ceil((double) config.accelNum / config.cacheBanks);
        uint64 H = 1, W = 1;
        while (H * W < config.cacheBanks) {
            if (H == W) {
                W *= 2;
            } else {
                H *= 2;
            }
        }
        RT.gv.accelSpmBankOrder.resize(ap.accels);
        for (uint accId = 0; accId < ap.accels; accId++) {
            RT.gv.accelSpmBankOrder[accId].resize(config.cacheBanks, 0);
            int h = (accId / accelNumPerTile) / W;
            int w = (accId / accelNumPerTile) % W;
            uint orderIdx = 0;
            for (uint hops = 0; hops <= (H - 1) + (W - 1); hops++) {
                for (uint bankId = 0; bankId < config.cacheBanks; bankId++) {
                    int h_ = bankId / W;
                    int w_ = bankId % W;
                    if ((std::abs(h - h_) + std::abs(w - w_)) == hops) {
//                        printf("%u %u %u %u\n", accId, orderIdx, bankId, hops);
                        assert(orderIdx < RT.gv.accelSpmBankOrder[accId].size());
                        RT.gv.accelSpmBankOrder[accId][orderIdx] = bankId;
                        orderIdx++;
                    }
                }
            }
            assert(orderIdx == config.cacheBanks);
        }
    }


    std::string BATCH_MODEL_ROOT = MODEL_ROOT + "batch" + std::to_string(ap.totalBatch) + "/";

    uint64 addrOffset = 0;
    std::deque<Model> modelList(ap.threads);
    for (uint tid = 0; tid < ap.threads; tid++) {
        uint modelTypeIdx = tid % modelNames.size();
        auto &model = modelList[tid];
        ModelLoader::load(model, BATCH_MODEL_ROOT + modelNames[modelTypeIdx], addrOffset, tid);
        printf("model: name=%s addr=[%lx,%lx)\n",
               modelNames[modelTypeIdx].c_str(), model.addrLo, model.addrHi);
        addrOffset = model.addrHi;
    }

    // Part5: Create threads

    std::deque<ThreadRunModel> threadList;
    for (uint tid = 0; tid < ap.threads; tid++) {
        uint modelTypeIdx = tid % modelNames.size();
        uint acc_idx = 0;
        while ((1 << (acc_idx + 1)) <= ap.acc_num_list[tid]) {
            acc_idx ++;
        }
        threadList.emplace_back(tid, &modelList[tid], modelTypeIdx, acc_idx, 0,
                                0, 10000);
        RT.spm_pages_alloc_per_model.push_back(ap.spm_alloc_kb_list[tid] * 1024 / config.spmPageBytes);
        RT.threads.push_back(&threadList.back());
    }

    RT.model_has_run_once.resize(modelNames.size(), 0);

    // Part6: Run

    RT.run();

    // Part7: Print and return

    // generate output string

    ss << "\n########## Model Info ##########\n\n";
    for (uint modelId = 0; modelId < modelNames.size(); modelId++) {
        uint64 avgModelCycles = 0;
        uint64 maxModelCycles = 0;
        uint64 numRuns = 0;
        for (uint tid = 0; tid < ap.threads; tid++) {
            if ((tid % modelNames.size()) == modelId) {
                auto &t = threadList[tid];
                avgModelCycles += t.getTotalModelCycles();
                maxModelCycles = std::max(maxModelCycles, t.getMaxModelCycles());
                numRuns += t.getNumRuns();
            }
        }
        if (numRuns > 0) {
            avgModelCycles /= numRuns;
        }
        ss << "Model " << modelId << "\n";
        ss << "\tModel Name: " << modelNames[modelId] << "\n";
        ss << "\tNum of Runs: " << numRuns << "\n";
        ss << "\tAvg Model Latency: " << avgModelCycles << "\n";
        ss << "\tMax Model Latency: " << maxModelCycles << "\n";
    }

    ss << "\n########## Thread Info ##########\n\n";
    for (uint tid = 0; tid < ap.threads; tid++) {
        uint modelTypeIdx = tid % modelNames.size();
        auto &t = threadList[tid];
        ss << "Thread " << t.tid << "\n";
        ss << "\tModel Name: " << modelNames[modelTypeIdx] << "\n";
        ss << "\tNum of Runs: " << t.getNumRuns() << "\n";
        ss << "\tAvg Model Latency: " << t.getAvgModelCycles() << "\n";

        
        uint64 total_cycles = 0, avg_cycles = 0;
        for (uint i = 0;i < t.modelCyclesList.size() - 1;++ i) {
            total_cycles += t.modelCyclesList[i];
        }
        if (t.modelCyclesList.size() == 1) {
            avg_cycles = t.modelCyclesList[0];
        } else {
            avg_cycles = total_cycles / (t.modelCyclesList.size() - 1);
        }
        ss << "\tAvg Model Latency except last one: " << avg_cycles << "\n";


        ss << "\tCycles Waiting for Accel: " << RT.gv.threadWaitingAccCycles[t.tid]
           << "(" << (double) RT.gv.threadWaitingAccCycles[t.tid] / RT.getCycles() << ")" << "\n";
        ss << "\tCycles Waiting for SPM: " << RT.gv.threadWaitingSpmCycles[t.tid]
           << "(" << (double) RT.gv.threadWaitingSpmCycles[t.tid] / RT.getCycles() << ")" << "\n";
        ss << "\tModel Latency: " << mudnac::toString(t.modelCyclesList).c_str() << "\n";
        ss << "\tLayer Latency: " << mudnac::toString(t.layerCyclesList).c_str() << "\n";
    }

    ss << "\n########## Performance Info ##########\n\n";
    ss << RT.system->getPerformanceInfo().c_str() << "\n";

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

    printf("\nmain: ending\n");
    
    delete RT.system;
    return 0;
}
