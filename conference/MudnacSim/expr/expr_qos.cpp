#include "mudnacsim.h"
#include "cxxopts.hpp"


static std::deque<std::string> modelNameList = {
        "resnet50", "mobilenetv2", "effinetb0", "vitb16", "pointpillars", "w2v2base", "bertbase", "gnmt"
};

static std::deque<std::deque<uint>> benchmarkList = {
        {0, 1, 2, 3, 4, 5, 6, 7},
        {0, 1, 6, 7},
};

static std::deque<uint64> modelHasRequestPossibilityList = {
        100, 100, 100, 100, 100, 100, 100, 100
};

static std::deque<uint64> modelTaskArriveFpsList = {
//        200, 250, 250, 20, 10, 60, 20, 200  // backup
        150, 350, 350, 25, 10, 60, 25, 150  // backup2-5
//        150, 350, 350, 30, 10, 60, 30, 150  // backup6
//        150, 300, 300, 30, 10, 60, 30, 150  // backup7
};

static std::deque<uint64> modelTargetFpsList = {
//        200, 250, 250, 20, 10, 60, 20, 200
        150, 350, 350, 25, 10, 60, 25, 150
//        150, 350, 350, 30, 10, 60, 30, 150
//        150, 300, 300, 30, 10, 60, 30, 150
};

static std::deque<uint> modelPossibilityList = {
        16, 63, 48, 4, 1, 9, 4, 27
};


void createWorkload(uint benchmark, uint tasks);


int main(int argc, char **argv) {
    const std::string MODEL_ROOT = "expr/input/models/";
    const std::string PROFILING_ROOT = "expr/input/profiling/";
    const std::string OUTPUT_ROOT = "expr/output/expr_qos/default/";
    const std::string DRAMSIM3_CONFIG_FOLDER = "DRAMsim3/configs/";

    // Part1: Parse input options

    cxxopts::Options opt("expr_qos");
    opt.add_options()
            ("system", "system type", cxxopts::value<std::string>())  // cache/spm
            ("channels", "num of memory channels", cxxopts::value<uint>())
            ("cache", "size(MB) of cache", cxxopts::value<uint>())
            ("ways", "num of ways for accels", cxxopts::value<uint>())
            ("accels", "num of accels", cxxopts::value<uint>())
            ("threads", "num of threads", cxxopts::value<uint>())
            ("benchmark", "benchmark index", cxxopts::value<uint>())
            ("tasks", "number of tasks", cxxopts::value<uint>())
            ("target-scale", "target scale", cxxopts::value<double>())
            ("output", "output name", cxxopts::value<std::string>())
            ("output-root", "output root path", cxxopts::value<std::string>())
            ("acc-alloc-idx", "accel allocation index", cxxopts::value<uint>())
            ("spm-alloc-idx", "SPM allocation index", cxxopts::value<uint>())
            ("memory-mode", "memory access mode", cxxopts::value<std::string>())  // unlimited/limited
            ("acc-alloc-mode", "accel allocation mode", cxxopts::value<std::string>())  // static/dynamic
            ("spm-alloc-mode", "SPM allocation mode", cxxopts::value<std::string>());  // static/dynamic
    auto arg = opt.parse(argc, argv);

    std::string systemType = "cache";
    uint memChannels = 4;
    uint cacheSizeMB = 16;
    uint spmWays = 12;
    uint numAccels = 16;
    uint numThreads = 8;
    uint benchmark = 0;
    uint tasks = 20;
    double targetScale = 1;
    std::string outputName = "";
    std::string outputRoot = OUTPUT_ROOT;
    uint staticAccAllocIdx = 1;
    uint staticSpmAllocIdx = 0;
    std::string memoryMode = "unlimited";
    std::string accAllocMode = "static";
    std::string spmAllocMode = "static";  // static/dynamic

    if (arg.count("system")) systemType = arg["system"].as<std::string>();
    if (arg.count("channels")) memChannels = arg["channels"].as<uint>();
    if (arg.count("cache")) cacheSizeMB = arg["cache"].as<uint>();
    if (arg.count("ways")) spmWays = arg["ways"].as<uint>();
    if (arg.count("accels")) numAccels = arg["accels"].as<uint>();
    if (arg.count("threads")) numThreads = arg["threads"].as<uint>();
    if (arg.count("benchmark")) benchmark = arg["benchmark"].as<uint>();
    if (arg.count("tasks")) tasks = arg["tasks"].as<uint>();
    if (arg.count("target-scale")) targetScale = arg["target-scale"].as<double>();
    if (arg.count("output")) outputName = arg["output"].as<std::string>();
    if (arg.count("output-root")) outputRoot = arg["output-root"].as<std::string>();
    if (arg.count("acc-alloc-idx")) staticAccAllocIdx = arg["acc-alloc-idx"].as<uint>();
    if (arg.count("spm-alloc-idx")) staticSpmAllocIdx = arg["spm-alloc-idx"].as<uint>();
    if (arg.count("memory-mode")) memoryMode = arg["memory-mode"].as<std::string>();
    if (arg.count("acc-alloc-mode")) accAllocMode = arg["acc-alloc-mode"].as<std::string>();
    if (arg.count("spm-alloc-mode")) spmAllocMode = arg["spm-alloc-mode"].as<std::string>();

    assert(systemType == "cache" or systemType == "spm");
    assert(memChannels == 1 or memChannels == 2 or memChannels == 4 or memChannels == 8);
    assert(cacheSizeMB > 0);
    assert(spmWays == 4 or spmWays == 8 or spmWays == 12);
    assert(numAccels > 0);
    assert(numThreads > 0 and numThreads <= numAccels);
    assert(benchmark < benchmarkList.size());
    assert(tasks <= RT.gv.MAX_TASKS);
    assert(staticAccAllocIdx < RT.gv.NUM_ACCEL_ALLOCS);
    assert(staticSpmAllocIdx < RT.gv.NUM_SPM_ALLOCS);
    assert(memoryMode == "limited" or memoryMode == "unlimited");
    assert(accAllocMode == "static" or accAllocMode == "dynamic");
    assert(spmAllocMode == "static" or spmAllocMode == "dynamic");

    std::stringstream ss;
    ss << "\n";
    ss << "expr_qos\n";
    ss << "\tsystem type: " << systemType << "\n";
    ss << "\tmemory channels: " << memChannels << "\n";
    ss << "\tcache size(MB): " << cacheSizeMB << "\n";
    ss << "\tnum of SPM ways: " << spmWays << "\n";
    ss << "\tnum of accels: " << numAccels << "\n";
    ss << "\tnum of threads: " << numThreads << "\n";
    ss << "\tbenchmark index: " << benchmark << "\n";
    ss << "\tnum of tasks: " << tasks << "\n";
    ss << "\ttarget scale: " << targetScale << "\n";
    ss << "\toutput name: " << outputName << "\n";
    ss << "\toutput root: " << outputRoot << "\n";
    ss << "\taccel allocation index: " << staticAccAllocIdx << "\n";
    ss << "\tspm allocation index: " << staticSpmAllocIdx << "\n";
    ss << "\tmemory access mode: " << memoryMode << "\n";
    ss << "\taccel allocation mode: " << accAllocMode << "\n";
    ss << "\tspm allocation mode: " << spmAllocMode << "\n";
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
    config.dramsim3ConfigPath = DRAMSIM3_CONFIG_FOLDER + "DDR4_8Gb_x16_3200_am1.ini";
    config.dramsim3OutputPath = dramsimOutputPath;
    config.memCtrlChannels = memChannels;
    config.cacheTotalBytes = cacheSizeMB * 1024 * 1024;
    config.cacheWays = 16;
    config.cacheBanks = 8;
    config.spmWays = spmWays;
    config.spmPageBytes = 32 * 1024;
    config.accelNum = numAccels;
    config.accelArrayH = config.accelArrayW = 32;

    BaseSystem *system = NULL;
    if (systemType == "cache") {
        system = new CacheSystem(config);
    } else {
        system = new SPMSystem(config);
    }

    // Part3: Initiate runtime

    RT.setSystem(system);
    RT.gv.accelAllocMode = RT.gv.ACCEL_ALLOC_MODE_STATIC;
    if (accAllocMode == "dynamic") {
        RT.gv.accelAllocMode = RT.gv.ACCEL_ALLOC_MODE_DYNAMIC;
    }
    RT.gv.memoryAccessMode = RT.gv.MEMORY_ACCESS_MODE_UNLIMITED;
    if (memoryMode == "limited") {
        RT.gv.memoryAccessMode = RT.gv.MEMORY_ACCESS_MODE_LIMITED;
    }
    RT.gv.spmAllocMode = RT.gv.SPM_ALLOC_MODE_STATIC;
    if (spmAllocMode == "dynamic") {
        RT.gv.spmAllocMode = RT.gv.SPM_ALLOC_MODE_DYNAMIC;
    }

    uint scaleIndex = 1;
    std::string scaleFolder = "scaling_qos_" + systemType;
    RT.gv.loadProfilingResults(modelNameList, scaleIndex, scaleFolder);

    for (uint modelIdx = 0; modelIdx < modelNameList.size(); modelIdx++) {
        RT.gv.qos_modelTargetCycles[modelIdx] = 1e9 / modelTargetFpsList[modelIdx] * targetScale;
    }

    // Part4: Load models

    createWorkload(benchmark, tasks);

    std::deque<Model *> models;
    uint64 addrOffset = 0;
    for (auto &modelName: modelNameList) {
        Model *m = new Model;
        ModelLoader::load(*m, MODEL_ROOT + modelName, addrOffset);
        models.push_back(m);
        printf("model: name=%s addr=[%lx,%lx)\n", modelName.c_str(), m->addrLo, m->addrHi);
        addrOffset = m->addrHi;
    }

    std::deque<Model *> taskModelQueue;
    for (uint taskIdx = 0; taskIdx < tasks; taskIdx++) {
        uint modelIdx = RT.gv.qos_taskModelTypeQueue[taskIdx];
        auto &modelName = modelNameList[modelIdx];
        Model *m = new Model;
        ModelLoader::load(*m, MODEL_ROOT + modelName, models[modelIdx]->addrLo);
        taskModelQueue.push_back(m);
        printf("task: taskIdx=%u, modelIdx=%u, name=%s addr=[%lx,%lx)\n",
               taskIdx, modelIdx, modelName.c_str(), m->addrLo, m->addrHi);
    }

    // Part5: Create threads

    std::deque<ThreadQoS> threads;
    for (uint tid = 0; tid < numThreads; tid++) {
        threads.emplace_back(tid, taskModelQueue, tasks, staticAccAllocIdx, staticSpmAllocIdx);
        RT.threads.push_back(&threads.back());
    }

    // Part6: Run

    RT.run();

    // Part7: Print and return

    // read single model cycles from profiling
    std::deque<uint64> singleModelCycles(modelNameList.size(), 0);
    for (uint modelIdx = 0; modelIdx < modelNameList.size(); modelIdx++) {
        std::string modelName = modelNameList[modelIdx];
        std::string path = "expr/input/profiling/" + modelName;
        std::ifstream ifs(path);
        assert(ifs.is_open());
        while (not ifs.eof()) {
            std::string line;
            std::getline(ifs, line);
            if (line.empty()) {
                continue;
            }
            size_t pos = 0;
            for (int accelAllocIdx = 0; accelAllocIdx < RT.gv.NUM_ACCEL_ALLOCS; accelAllocIdx++) {
                size_t end = line.find(',', pos);
                assert(end > pos and end != line.npos);
                uint64 cycles = (double) std::stoull(line.substr(pos, end - pos));
                if (accelAllocIdx == 1) {
                    singleModelCycles[modelIdx] += cycles;
                    break;
                }
                pos = end + 1;
            }
        }
        ifs.close();
    }

    ss << "\n######## Task Performance ########\n";
    uint satisfied = 0;
    double stp = 0;
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
        ss << "\tSingle Model Latency: " << singleModelCycles[modelIdx] << "\n";
        ss << "\tSlack: " << slack << "(" << slackRate << ")" << "\n";
        if (slack > 0) {
            satisfied++;
        }
        stp += ((double) (singleModelCycles[modelIdx] / 10000)) / ((double) (startToFinish / 10000));
    }
    ss << "SLA: " << (double) satisfied / (double) tasks << "\n";
    ss << "STP: " << (double) stp / (double) tasks << "\n";

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
        uint64 inter = (uint64) 1e9 / modelTaskArriveFpsList[modelIdx];
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
