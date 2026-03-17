#include "mudnacsim.h"
#include "cxxopts.hpp"


static std::deque<std::string> modelNameList = {
        "resnet50", // 0
        "mobilenetv2", // 1
        "effinetb0",  // 2
        "vitb16", // 3
        "pointpillars", // 4
        "w2v2base", // 5
        "bertbase", // 6
        "gnmt", // 7
        
        // "berttiny", // 8
        // "bertmini", // 9
        // "bertsmall", // 10
        // "bertmedium", // 11
};

static std::deque<std::deque<uint>> benchmarkList = {
        {0, 1, 2, 3, 4, 5, 6, 7},
        {0, 1, 6, 7},
};

static std::deque<uint64> modelHasRequestPossibilityList = {
        100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100
};

static std::deque<uint64> modelTaskArriveFpsList = {
//        200, 250, 250, 20, 10, 60, 20, 200  // backup
        // 150, 350, 350, 25, 10, 60, 25, 150  // backup2-5
        160, 352, 352, 32, 16, 80, 32, 160, 160, 96, 64, 48  // backup2-5
//        150, 350, 350, 30, 10, 60, 30, 150  // backup6
//        150, 300, 300, 30, 10, 60, 30, 150  // backup7
};

static std::deque<uint64> modelTargetFpsList = {
//        200, 250, 250, 20, 10, 60, 20, 200
        // 150, 350, 350, 25, 10, 60, 25, 150
        160, 352, 352, 32, 16, 80, 32, 160, 160, 96, 64, 48  // backup2-5
//        150, 350, 350, 30, 10, 60, 30, 150
//        150, 300, 300, 30, 10, 60, 30, 150
};

static std::deque<uint> modelPossibilityList = {
        16, 63, 48, 4, 1, 9, 4, 27, 0, 0, 0
};

uint acc_origin = 4;
float fps_scale = 1.0;

void createWorkload(uint benchmark, uint tasks);


int main(int argc, char **argv) {
    const std::string MODEL_ROOT = "models/";
    const std::string PROFILING_ROOT = "models/";
    const std::string OUTPUT_ROOT = "expr/output/expr_qos2e/default/";
    const std::string DRAMSIM3_CONFIG_FOLDER = "DRAMsim3/configs/";

    // Part1: Parse input options

    cxxopts::Options opt("expr_qos");
    opt.add_options()
            ("channels", "num of memory channels", cxxopts::value<uint>())
            ("accels", "num of accels", cxxopts::value<uint>())
            ("threads", "num of threads", cxxopts::value<uint>())
            ("benchmark", "benchmark index", cxxopts::value<uint>())
            ("tasks", "number of tasks", cxxopts::value<uint>())
            ("target-scale", "target scale", cxxopts::value<double>())
            ("output", "output name", cxxopts::value<std::string>())
            ("spm-size-per-bank", "SPM size per bank (KB)", cxxopts::value<uint>())
            ("noc-flit-size", "NoC flit size (bytes)", cxxopts::value<uint>())
            ("batch-size", "batch size", cxxopts::value<uint>())
            ("qps-per-model", "QPS per model (comma separated)", cxxopts::value<std::string>())
            ("output-root", "output root path", cxxopts::value<std::string>());

    auto arg = opt.parse(argc, argv);

    uint memChannels = 4;
    uint numAccels = 16;
    uint numThreads = 8;
    uint benchmark = 0;
    uint tasks = 20;
    uint spm_size_per_bank_kb = 1024;
    uint noc_flit_size = 64;
    double targetScale = 1;
    uint batch_size_per_task = 16;
    uint acc_target_origin = 16;
    std::string outputName = "";
    std::string outputRoot = OUTPUT_ROOT;
    std::string qpsStr;


    if (arg.count("channels")) memChannels = arg["channels"].as<uint>();
    if (arg.count("accels")) numAccels = arg["accels"].as<uint>();
    if (arg.count("threads")) numThreads = arg["threads"].as<uint>();
    if (arg.count("benchmark")) benchmark = arg["benchmark"].as<uint>();
    if (arg.count("tasks")) tasks = arg["tasks"].as<uint>();
    if (arg.count("target-scale")) targetScale = arg["target-scale"].as<double>();
    if (arg.count("output")) outputName = arg["output"].as<std::string>();
    if (arg.count("spm_size_per_bank_kb")) spm_size_per_bank_kb = arg["spm-size-per-bank"].as<uint>();
    if (arg.count("noc-flit-size")) noc_flit_size = arg["noc-flit-size"].as<uint>();
    if (arg.count("output-root")) outputRoot = arg["output-root"].as<std::string>();
    if (arg.count("batch-size")) batch_size_per_task = arg["batch-size"].as<uint>();
    if (arg.count("qps-per-model")) {
        qpsStr = arg["qps-per-model"].as<std::string>();
        std::stringstream ss(qpsStr);
        std::string token;
        uint i = 0;
        while (std::getline(ss, token, ',')) {
            modelTaskArriveFpsList[i] = std::stoull(token);
            ++ i;
        }
    }

    assert(memChannels == 1 or memChannels == 2 or memChannels == 4 or memChannels == 8);
    assert(numAccels > 0);
    assert(numThreads > 0 and numThreads <= numAccels);
    assert(benchmark < benchmarkList.size());
    assert(tasks <= RT.gv.MAX_TASKS);

    std::stringstream ss;
    ss << "\n";
    ss << "expr_qos\n";
    ss << "\tmemory channels: " << memChannels << "\n";
    ss << "\tnum of accels: " << numAccels << "\n";
    ss << "\tnum of threads: " << numThreads << "\n";
    ss << "\tbenchmark index: " << benchmark << "\n";
    ss << "\tnum of tasks: " << tasks << "\n";
    ss << "\ttarget scale: " << targetScale << "\n";
    ss << "\toutput name: " << outputName << "\n";
    ss << "\toutput root: " << outputRoot << "\n";
    ss << "\n";
    std::cout << ss.str();

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

    if (outputName == "") {
        std::time_t tm = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        outputName = std::to_string(tm);
    }
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

    // RT.gv.accelAllocMode = RT.gv.ACCEL_ALLOC_MODE_STATIC;
    // if (accAllocMode == "dynamic") {
    RT.gv.accelAllocMode = RT.gv.ACCEL_ALLOC_MODE_STATIC;
    // }
    RT.gv.memoryAccessMode = RT.gv.MEMORY_ACCESS_MODE_UNLIMITED;
    // if (memoryMode == "limited") {
    //     RT.gv.memoryAccessMode = RT.gv.MEMORY_ACCESS_MODE_LIMITED;
    // }
    // RT.gv.spmAllocMode = RT.gv.SPM_ALLOC_MODE_STATIC;
    // if (spmAllocMode == "dynamic") {
    RT.gv.spmAllocMode = RT.gv.SPM_ALLOC_MODE_FIX;
    // }

    uint scaleIndex = 0;
    std::string scaleFolder = "scaling_qos_spm";
    RT.gv.loadProfilingResults(modelNameList, scaleIndex, scaleFolder, "profiling/");

    for (uint modelIdx = 0; modelIdx < modelNameList.size(); modelIdx++) {
        RT.gv.qos_modelTargetCycles[modelIdx] = 1e9 / (modelTaskArriveFpsList[modelIdx] / batch_size_per_task) * targetScale;
    }

    // Part4: Load models
    fps_scale = (numAccels/ static_cast<float>(acc_origin) / static_cast<float>(batch_size_per_task));
    for (uint modelIdx = 0; modelIdx < modelNameList.size(); modelIdx++) {
        RT.gv.qos_modelTargetCycles[modelIdx] = 1e9 / (modelTaskArriveFpsList[modelIdx] / batch_size_per_task) * targetScale;
    }
    std::string modelBatchRoot = MODEL_ROOT + "batch" + std::to_string(batch_size_per_task) + "/";

    createWorkload(benchmark, tasks);

    std::deque<Model *> models;
    uint64 addrOffset = 0;
    for (auto &modelName: modelNameList) {
        Model *m = new Model;
        ModelLoader::load(*m, modelBatchRoot + modelName, addrOffset);
        models.push_back(m);
        printf("model: name=%s addr=[%lx,%lx)\n", modelName.c_str(), m->addrLo, m->addrHi);
        addrOffset = m->addrHi;
    }

    std::deque<Model *> taskModelQueue;
    for (uint taskIdx = 0; taskIdx < tasks; taskIdx++) {
        uint modelIdx = RT.gv.qos_taskModelTypeQueue[taskIdx];
        auto &modelName = modelNameList[modelIdx];
        Model *m = new Model;
        ModelLoader::load(*m, modelBatchRoot + modelName, models[modelIdx]->addrLo);
        taskModelQueue.push_back(m);
        printf("task: taskIdx=%u, modelIdx=%u, name=%s addr=[%lx,%lx)\n",
               taskIdx, modelIdx, modelName.c_str(), m->addrLo, m->addrHi);
    }

    // Part5: Create threads

    std::deque<ThreadQoS> threads;
    for (uint tid = 0; tid < numThreads; tid++) {
        threads.emplace_back(tid, taskModelQueue, tasks, 4, 1);
        RT.threads.push_back(&threads.back());
    }

    // Part6: Run

    RT.run();

    // Part7: Print and return

    // read single model cycles from profiling
    // std::deque<uint64> singleModelCycles(modelNameList.size(), 0);
    // for (uint modelIdx = 0; modelIdx < modelNameList.size(); modelIdx++) {
    //     std::string modelName = modelNameList[modelIdx];
    //     std::string path = "profiling/" + modelName;
    //     std::ifstream ifs(path);
    //     assert(ifs.is_open());
    //     while (not ifs.eof()) {
    //         std::string line;
    //         std::getline(ifs, line);
    //         if (line.empty()) {
    //             continue;
    //         }
    //         size_t pos = 0;
    //         for (int accelAllocIdx = 0; accelAllocIdx < RT.gv.NUM_ACCEL_ALLOCS; accelAllocIdx++) {
    //             size_t end = line.find(',', pos);
    //             assert(end > pos and end != line.npos);
    //             uint64 cycles = (double) std::stoull(line.substr(pos, end - pos));
    //             if (accelAllocIdx == 1) {
    //                 singleModelCycles[modelIdx] += cycles;
    //                 break;
    //             }
    //             pos = end + 1;
    //         }
    //     }
    //     ifs.close();
    // }

    ss << "\n######## Task Performance ########\n";
    uint satisfied = 0;
    // double stp = 0;
    for (uint taskIdx = 0; taskIdx < tasks; taskIdx++) {
        uint modelIdx = RT.gv.qos_taskModelTypeQueue[taskIdx];
        uint64 dispatch = RT.gv.qos_taskDispatchTime[taskIdx];
        uint64 target = RT.gv.qos_modelTargetCycles[modelIdx];
        uint64 waiting = RT.gv.qos_taskStartTime[taskIdx] - RT.gv.qos_taskDispatchTime[taskIdx];
        uint64 startToFinish = RT.gv.qos_taskEndTime[taskIdx] - RT.gv.qos_taskStartTime[taskIdx];
        uint64 dispatchToFinish = RT.gv.qos_taskEndTime[taskIdx] - RT.gv.qos_taskDispatchTime[taskIdx];
        int64 slack = (int64) target - (int64) dispatchToFinish;
        double slackRate = (double) slack / (double) target;
        ss << "Task: " << taskIdx << "\n";
        ss << "\tWorkload Type: " << modelIdx << "\n";
        ss << "\tDispatch: " << dispatch << "\n";
        ss << "\tTarget: " << target << "\n";
        ss << "\tWaiting: " << waiting << "\n";
        ss << "\tStart to Finish: " << startToFinish << "\n";
        ss << "\tDispatch to Finish: " << dispatchToFinish << "\n";
        // ss << "\tSingle Model Latency: " << singleModelCycles[modelIdx] << "\n";
        ss << "\tSlack: " << slack << "(" << slackRate << ")" << "\n";
        if (slack > 0) {
            satisfied++;
        }
        // stp += ((double) (singleModelCycles[modelIdx] / 10000)) / ((double) (startToFinish / 10000));
    }
    ss << "SLA: " << (double) satisfied / (double) tasks << "\n";
    // ss << "STP: " << (double) stp / (double) tasks << "\n";

    ss << "\n######## Model Performance ########\n";
    for (auto modelIdx: benchmarkList[benchmark]) {
        std::deque<uint64> modelCyclesList;
        uint64 avgModelCycles = 0;
        double sla = 0;
        for (uint taskIdx = 0; taskIdx < tasks; taskIdx++) {
            if (modelIdx == RT.gv.qos_taskModelTypeQueue[taskIdx]) {
                uint64 startToFinish = RT.gv.qos_taskEndTime[taskIdx] - RT.gv.qos_taskStartTime[taskIdx];
                uint64 dispatchToFinish = RT.gv.qos_taskEndTime[taskIdx] - RT.gv.qos_taskDispatchTime[taskIdx];
                uint64 target = RT.gv.qos_modelTargetCycles[modelIdx];
                modelCyclesList.push_back(startToFinish);
                avgModelCycles += startToFinish;
                if (dispatchToFinish < target) {
                    sla += 1;
                }
            }
        }
        double sd = 0;
        if (modelCyclesList.size() > 0) {
            avgModelCycles /= modelCyclesList.size();
            for (int64 cycles: modelCyclesList) {
                sd += (double) (cycles - (int64) avgModelCycles) * (cycles - (int64) avgModelCycles) /
                      modelCyclesList.size();
            }
            sd = std::sqrt(sd);
            sla = sla / modelCyclesList.size();
        }
        ss << "Model: " << modelNameList[modelIdx] << "\n";
        ss << "\tTask Count: " << modelCyclesList.size() << "\n";
        ss << "\tTarget Model Cycles: " << RT.gv.qos_modelTargetCycles[modelIdx] << "\n";
        ss << "\tProfile Model Cycles: " << RT.gv.pf_modelCycles[0][modelIdx] << ","
           << RT.gv.pf_modelCycles[1][modelIdx] << "," << RT.gv.pf_modelCycles[2][modelIdx] << "\n";
        ss << "\tAvg Model Cycles: " << avgModelCycles << "\n";
        ss << "\tStandard Deviation: " << (uint64) sd << "\n";
        ss << "\tSLA: " << sla << "\n";
    }

    ss << "\n######## Thread Performance ########\n";
    for (auto &thread: threads) {
        ss << "Thread: " << thread.tid << "\n";
        ss << "\tCycles: " << thread.threadEndCycles - thread.threadStartCycles << "\n";
        ss << "\tCycles Waiting for tasks: " << thread.waitingCycles << "\n";
        ss << "\tCycles Waiting for Accel: " << RT.gv.threadWaitingAccCycles[thread.tid] << "\n";
        ss << "\tCycles Waiting for SPM: " << RT.gv.threadWaitingSpmCycles[thread.tid] << "\n";
        ss << "\tNum of Task: " << thread.runTaskList.size() << "\n";
    }

    ss << "\n######## System Performance ########\n";
    ss << RT.system->getPerformanceInfo().c_str() << "\n";

    std::cout << ss.str();

    std::ofstream ofs(mudnacsimOutputRoot + outputName);
    assert(ofs.is_open());
    ofs << ss.str();
    ofs.close();

    for (auto *model: models) {
        delete model;
    }
    for (auto *model: taskModelQueue) {
        delete model;
    }
    delete system;
    return 0;
}


uint getRandomModelIndex(uint benchmark,
                         std::uniform_int_distribution<uint> &distri,
                         std::default_random_engine &engine) {
    uint r = distri(engine);
    uint lower = 0;
    for (uint modelIdx: benchmarkList[benchmark]) {
        if (r < lower + modelPossibilityList[modelIdx]) {
            return modelIdx;
        }
        lower += modelPossibilityList[modelIdx];
    }
    assert(false);
}


static void createWorkload1(uint benchmark, uint tasks) {
    // NOTE: closely follow create_workload in gemmini
    const uint GROUP_SIZE = 8;

    uint possSum = 0;
    for (uint modelIdx: benchmarkList[benchmark]) {
        possSum += modelPossibilityList[modelIdx];
    }
    std::default_random_engine re;
    std::uniform_int_distribution<uint> modelDistri(0, possSum - 1);
    std::uniform_real_distribution<double> interDistri(0.8, 1.2);

    uint64 cyclesSum = 0;
    for (uint modelIdx: benchmarkList[benchmark]) {
        cyclesSum += RT.gv.pf_modelCycles[1][modelIdx] * 4 * modelPossibilityList[modelIdx];
    }
    uint64 interval = cyclesSum / (uint64) possSum;

    for (uint taskIdx = 0; taskIdx < tasks; taskIdx++) {
        uint modelIdx = getRandomModelIndex(benchmark, modelDistri, re);
        uint64 dispatchTime = 0;
        if (taskIdx < GROUP_SIZE) {
            dispatchTime = taskIdx * (interval / GROUP_SIZE);
        } else {
            dispatchTime = RT.gv.qos_taskDispatchTime[taskIdx - GROUP_SIZE] + interval * interDistri(re);
        }
        RT.gv.qos_taskModelTypeQueue[taskIdx] = modelIdx;
        RT.gv.qos_taskDispatchTime[taskIdx] = dispatchTime;
    }

    // sort
    for (int i = 0; i < tasks; i++) {
        for (int j = i + 1; j < tasks; j++) {
            if (RT.gv.qos_taskDispatchTime[i] > RT.gv.qos_taskDispatchTime[j]) {
                std::swap(RT.gv.qos_taskDispatchTime[i], RT.gv.qos_taskDispatchTime[j]);
                std::swap(RT.gv.qos_taskModelTypeQueue[i], RT.gv.qos_taskModelTypeQueue[j]);
            }
        }
    }
    for (int i = 1; i < tasks; i++) {
        assert(RT.gv.qos_taskDispatchTime[i] > RT.gv.qos_taskDispatchTime[i - 1]);
    }

    // make sure adjacent tasks are of different model
    for (int i = 1; i < tasks; i++) {
        uint modelIdx = RT.gv.qos_taskModelTypeQueue[i];
        while (modelIdx == RT.gv.qos_taskModelTypeQueue[i - 1]) {
            modelIdx = getRandomModelIndex(benchmark, modelDistri, re);
        }
        RT.gv.qos_taskModelTypeQueue[i] = modelIdx;
    }

    for (uint taskIdx = 0; taskIdx < tasks; taskIdx++) {
        printf("createWorkload1: taskIdx=%u, modelIdx=%u, dispatch=%lu\n",
               taskIdx, RT.gv.qos_taskModelTypeQueue[taskIdx], RT.gv.qos_taskDispatchTime[taskIdx]);
    }
}


static void createWorkload2(uint benchmark, uint tasks) {
    std::default_random_engine re;
    std::uniform_int_distribution<> possDistri(1, 100);

    std::deque<uint64> modelAvgInterList;
    for (uint modelIdx = 0; modelIdx < modelNameList.size(); modelIdx++) {
        uint64 inter = (uint64) 1e9 / (modelTaskArriveFpsList[modelIdx] * fps_scale);
        modelAvgInterList.emplace_back(inter);
        printf("createWorkload2: modelIdx=%u, avgInter=%lu\n", modelIdx, inter);
    }

    std::vector<uint64> modelNextDispatchTimeList(modelNameList.size(), 0);
    for (uint modelIdx = 0; modelIdx < modelNameList.size(); modelIdx++) {
        while (possDistri(re) > modelHasRequestPossibilityList[modelIdx]) {
            printf("\tno request, modelIdx=%u, dispatch=%lu\n", modelIdx, modelNextDispatchTimeList[modelIdx]);
            modelNextDispatchTimeList[modelIdx] += modelAvgInterList[modelIdx];
        }
    }

    for (uint taskIdx = 0; taskIdx < tasks; taskIdx++) {
        // get next dispatched model
        uint64 dispatchTime = UINT64_MAX;
        uint modelIdx = UINT32_MAX;
        for (uint i: benchmarkList[benchmark]) {
            if (modelNextDispatchTimeList[i] < dispatchTime) {
                dispatchTime = modelNextDispatchTimeList[i];
                modelIdx = i;
            }
        }
        assert(dispatchTime < UINT64_MAX and modelIdx < UINT32_MAX);

        // write RT.gv
        RT.gv.qos_taskModelTypeQueue[taskIdx] = modelIdx;
        RT.gv.qos_taskDispatchTime[taskIdx] = dispatchTime;
        printf("createWorkload2: taskIdx=%u, modelIdx=%u, dispatch=%lu\n", taskIdx, modelIdx, dispatchTime);

        // compute the next dispatch time of this model
        modelNextDispatchTimeList[modelIdx] += modelAvgInterList[modelIdx];
        while (possDistri(re) > modelHasRequestPossibilityList[modelIdx]) {
            printf("\tno request, modelIdx=%u, dispatch=%lu\n", modelIdx, modelNextDispatchTimeList[modelIdx]);
            modelNextDispatchTimeList[modelIdx] += modelAvgInterList[modelIdx];
        }
    }
}


void createWorkload(uint benchmark, uint tasks) {
//    createWorkload1(benchmark, tasks);
    createWorkload2(benchmark, tasks);
}
