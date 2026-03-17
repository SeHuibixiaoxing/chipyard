#include "mudnacsim.h"
#include "cxxopts.hpp"


static std::deque<std::deque<std::string>> benchmarkList = {
        {"resnet50", "mobilenetv2", "effinetb0", "vitb16", "pointpillars", "w2v2base", "bertbase", "gnmt"},  // 0
        {"resnet50", "mobilenetv2", "effinetb0", "pointpillars"},  // 1
        {"vitb16", "vitb16", "w2v2base", "bertbase"},  // 2
        {"resnet50", "mobilenetv2", "vitb16", "bertbase"},  // 3
};


int main(int argc, char *argv[]) {
    const std::string MODEL_ROOT = "models/";
    const std::string OUTPUT_ROOT = "expr/output/expr_mixed_models/default/";

    // Part1: Parse input options

    ArgParser ap;
    ap.accels = 16;
    ap.threads = ap.accels;
    ap.channels = 8;
    ap.cache = 16;
    ap.time = 0.1;
    ap.inf_count = 1000;
    ap.output_root = OUTPUT_ROOT;
    ap.spm_addr_type = "conti";
    ap.parse(argc, argv);
    std::cout << ap.getString() << "\n\n";

    assert(ap.accels == 4 or ap.accels == 8 or ap.accels == 16 or ap.accels == 32 or ap.accels == 64);
    assert((ap.acc_alloc_idx == 0 and ap.threads <= ap.accels) or
           (ap.acc_alloc_idx == 1 and ap.threads * 2 <= ap.accels) or
           (ap.acc_alloc_idx == 2 and ap.threads * 4 <= ap.accels));

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
    config.dramsim3ConfigPath = "DRAMsim3/configs/DDR4_8Gb_x16_3200_am1.ini";
    config.dramsim3OutputPath = dramsimOutputPath;
    config.memCtrlChannels = ap.channels;
    config.cacheTotalBytes = ap.cache * 1024 * 1024;
    config.cacheWays = 16;
    config.cacheBanks = ap.accels;
    config.spmWays = ap.ways;
    config.spmPageBytes = 32 * 1024;
    config.spmMulticast = ap.spm_multicast;
    config.accelNum = ap.accels;
    config.accelArrayH = config.accelArrayW = 32;
    config.accelSpmAddrType = AccelConfig::SPM_ADDR_TYPE_CONTINUOUS;
    if (ap.spm_addr_type == "block") config.accelSpmAddrType = AccelConfig::SPM_ADDR_TYPE_BLOCK_INTERLEAVED;
    if (ap.spm_addr_type == "page") config.accelSpmAddrType = AccelConfig::SPM_ADDR_TYPE_PAGE_INTERLEAVED;

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
    RT.gv.spmPageAllocStrategy = RT.gv.SPM_PAGE_ALLOC_STRATEGY_NAIVE;
    if (ap.spm_page_strategy == "ordered") {
        RT.gv.spmPageAllocStrategy = RT.gv.SPM_PAGE_ALLOC_STRATEGY_ORDERED;
    }

    // uint profileScaleIndex = ceil(log2f((float) ap.threads));
    // std::string scaleFolder = "scaling_" + ap.system;
    // RT.gv.loadProfilingResults(benchmarkList[ap.benchmark], profileScaleIndex, scaleFolder);

    // write thread-accel affiliation
//    RT.gv.enableThreadAccelAffiliation = false;
    RT.gv.enableThreadAccelAffiliation = true;
    RT.gv.threadAccelAffiliation.resize(ap.threads);
    if (ap.acc_alloc_idx == 0) {
        assert(ap.threads <= ap.accels);
        for (uint tid = 0; tid < ap.threads; tid++) {
            RT.gv.threadAccelAffiliation[tid].push_back(tid);
        }
    } else if (ap.acc_alloc_idx == 1) {
        assert(ap.threads * 2 <= ap.accels);
        for (uint tid = 0; tid < ap.threads; tid++) {
            RT.gv.threadAccelAffiliation[tid].push_back(tid * 2);
            RT.gv.threadAccelAffiliation[tid].push_back(tid * 2 + 1);
        }
    } else if (ap.acc_alloc_idx == 2) {
        assert(ap.threads * 4 <= ap.accels);
        uint64 accelsPerRow = std::sqrt(ap.accels);
        uint64 threadsPerRow = accelsPerRow / 2;
        for (uint tid = 0; tid < ap.threads; tid++) {
            uint accIdOffset = (tid / threadsPerRow) * accelsPerRow * 2;
            RT.gv.threadAccelAffiliation[tid].push_back(accIdOffset + (tid % threadsPerRow) * 2);
            RT.gv.threadAccelAffiliation[tid].push_back(accIdOffset + (tid % threadsPerRow) * 2 + 1);
            RT.gv.threadAccelAffiliation[tid].push_back(accIdOffset + (tid % threadsPerRow) * 2 + accelsPerRow);
            RT.gv.threadAccelAffiliation[tid].push_back(accIdOffset + (tid % threadsPerRow) * 2 + 1 + accelsPerRow);
        }
    } else {
        assert(false);
    }
    std::cout << "Thread-Accel Affiliation: " << "\n";
    std::cout << toString(RT.gv.threadAccelAffiliation) << "\n";

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
    std::cout << "SPM Allocation Order: " << "\n";
    std::cout << toString(RT.gv.accelSpmBankOrder) << "\n";

//    delete system;
//    exit(0);

    // Part4: Load models

    uint64 addrOffset = 0;
    std::deque<Model> modelList(ap.threads);
    for (uint tid = 0; tid < ap.threads; tid++) {
        uint modelTypeIdx = tid % benchmarkList[ap.benchmark].size();
        auto &model = modelList[tid];
        ModelLoader::load(model, MODEL_ROOT + benchmarkList[ap.benchmark][modelTypeIdx], addrOffset, tid);
        printf("model: name=%s addr=[%lx,%lx)\n",
               benchmarkList[ap.benchmark][modelTypeIdx].c_str(), model.addrLo, model.addrHi);
        addrOffset = model.addrHi;
    }

    // Part5: Create threads

    const uint64 maxCycles = ap.time * config.freqMHz * 1000000;
    std::deque<ThreadRunModel> threadList;
    for (uint tid = 0; tid < ap.threads; tid++) {
        uint modelTypeIdx = tid % benchmarkList[ap.benchmark].size();
        threadList.emplace_back(tid, &modelList[tid], modelTypeIdx, ap.acc_alloc_idx, ap.spm_alloc_idx,
                                0, ap.inf_count, maxCycles);
        RT.threads.push_back(&threadList.back());
    }

    // Part6: Run

    RT.run();

    // Part7: Print and return

    // generate output string

    ss << "\n########## Model Info ##########\n\n";
    for (uint modelId = 0; modelId < benchmarkList[ap.benchmark].size(); modelId++) {
        uint64 avgModelCycles = 0;
        uint64 maxModelCycles = 0;
        uint64 numRuns = 0;
        for (uint tid = 0; tid < ap.threads; tid++) {
            if ((tid % benchmarkList[ap.benchmark].size()) == modelId) {
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
        ss << "\tModel Name: " << benchmarkList[ap.benchmark][modelId] << "\n";
        ss << "\tNum of Runs: " << numRuns << "\n";
        ss << "\tAvg Model Latency: " << avgModelCycles << "\n";
        ss << "\tMax Model Latency: " << maxModelCycles << "\n";
    }

    ss << "\n########## Thread Info ##########\n\n";
    for (uint tid = 0; tid < ap.threads; tid++) {
        uint modelTypeIdx = tid % benchmarkList[ap.benchmark].size();
        auto &t = threadList[tid];
        ss << "Thread " << t.tid << "\n";
        ss << "\tModel Name: " << benchmarkList[ap.benchmark][modelTypeIdx] << "\n";
        ss << "\tNum of Runs: " << t.getNumRuns() << "\n";
        ss << "\tAvg Model Latency: " << t.getAvgModelCycles() << "\n";
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
