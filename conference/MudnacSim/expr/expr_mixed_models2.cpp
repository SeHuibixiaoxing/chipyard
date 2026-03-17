#include <mudnacsim.h>
#include "cxxopts.hpp"


static std::deque<std::deque<std::string>> benchmarkList = {
        {"resnet50", "mobilenetv2", "effinetb0", "vitb16", "pointpillars", "w2v2base", "bertbase", "gnmt"},  // 0
        {"resnet50", "mobilenetv2", "bertbase", "gnmt"},  // 1
        {"resnet50", "bertbase"},  // 2
};

static std::deque<std::deque<uint64>> runTimesList = {
        {16, 63, 48, 4, 1, 9, 4, 27},  // 0
        {5, 10, 1, 5},  // 1
        {4, 1},  // 2
};


int main(int argc, char **argv) {
    const std::string MODEL_ROOT = "expr/input/models/";
    const std::string PROFILING_ROOT = "expr/input/profiling/";
    const std::string OUTPUT_ROOT = "expr/output/expr_mixed_models2/default/";
    const std::string DRAMSIM3_CONFIG_FOLDER = "DRAMsim3/configs/";

    // Part1: Parse input options

    cxxopts::Options opt("expr_mixed_models2");
    opt.add_options()
            ("system", "system type", cxxopts::value<std::string>())  // cache/spm
            ("channels", "num of memory channels", cxxopts::value<uint>())
            ("cache", "size(MB) of cache", cxxopts::value<uint>())
            ("ways", "num of ways for accels", cxxopts::value<uint>())
            ("accels", "num of accels", cxxopts::value<uint>())
            ("threads", "num of threads", cxxopts::value<uint>())
            ("benchmark", "benchmark index", cxxopts::value<uint>())
            ("output", "output name", cxxopts::value<std::string>())
            ("output-root", "output root path", cxxopts::value<std::string>())
            ("acc-alloc-idx", "accel allocation index", cxxopts::value<uint>())
            ("spm-alloc-idx", "SPM allocation index", cxxopts::value<uint>())
            ("spm-alloc-mode", "SPM allocation mode", cxxopts::value<std::string>());  // static/dynamic/fix
    auto arg = opt.parse(argc, argv);

    std::string systemType = "cache";
    uint memChannels = 4;
    uint cacheSizeMB = 16;
    uint spmWays = 12;
    uint numAccels = 16;
    uint numThreads = 16;
    uint benchmark = 0;
    std::string outputName = "";
    std::string outputRoot = OUTPUT_ROOT;
    uint staticAccAllocIdx = 0;
    uint staticSpmAllocIdx = 0;
    std::string spmAllocMode = "static";  // static/dynamic/fix

    if (arg.count("system")) systemType = arg["system"].as<std::string>();
    if (arg.count("channels")) memChannels = arg["channels"].as<uint>();
    if (arg.count("cache")) cacheSizeMB = arg["cache"].as<uint>();
    if (arg.count("ways")) spmWays = arg["ways"].as<uint>();
    if (arg.count("accels")) numAccels = arg["accels"].as<uint>();
    if (arg.count("threads")) numThreads = arg["threads"].as<uint>();
    if (arg.count("benchmark")) benchmark = arg["benchmark"].as<uint>();
    if (arg.count("output")) outputName = arg["output"].as<std::string>();
    if (arg.count("output-root")) outputRoot = arg["output-root"].as<std::string>();
    if (arg.count("acc-alloc-idx")) staticAccAllocIdx = arg["acc-alloc-idx"].as<uint>();
    if (arg.count("spm-alloc-idx")) staticSpmAllocIdx = arg["spm-alloc-idx"].as<uint>();
    if (arg.count("spm-alloc-mode")) spmAllocMode = arg["spm-alloc-mode"].as<std::string>();

    std::deque<std::string> &modelNameList = benchmarkList[benchmark];

    assert(systemType == "cache" or systemType == "spm");
    assert(memChannels == 1 or memChannels == 2 or memChannels == 4 or memChannels == 8);
    assert(cacheSizeMB > 0);
    assert(spmWays == 4 or spmWays == 8 or spmWays == 12 or spmWays == 16);
    assert(numAccels > 0);
    assert(numThreads > 0 and numThreads <= numAccels);
    assert(benchmark < benchmarkList.size());
    assert(staticAccAllocIdx < RT.gv.NUM_ACCEL_ALLOCS);
    assert(staticSpmAllocIdx < RT.gv.NUM_SPM_ALLOCS);
    assert(spmAllocMode == "static" or spmAllocMode == "dynamic" or spmAllocMode == "fix");

    std::stringstream ss;
    ss << "\n";
    ss << "expr_mixed_models2\n";
    ss << "\tsystem type: " << systemType << "\n";
    ss << "\tmemory channels: " << memChannels << "\n";
    ss << "\tcache size(MB): " << cacheSizeMB << "\n";
    ss << "\tnum of SPM ways: " << spmWays << "\n";
    ss << "\tnum of accels: " << numAccels << "\n";
    ss << "\tnum of threads: " << numThreads << "\n";
    ss << "\tbenchmark index: " << benchmark << "\n";
    ss << "\toutput name: " << outputName << "\n";
    ss << "\toutput root: " << outputRoot << "\n";
    ss << "\taccel allocation index: " << staticAccAllocIdx << "\n";
    ss << "\tspm allocation index: " << staticSpmAllocIdx << "\n";
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
//    config.cacheWriteThrough = true;
//    config.cacheWriteAllocation = true;
    config.cacheTotalBytes = cacheSizeMB * 1024 * 1024;
    config.cacheWays = 16;
    config.cacheBanks = 8;
//    config.cacheBanks = 1;
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
    RT.gv.memoryAccessMode = RT.gv.MEMORY_ACCESS_MODE_UNLIMITED;
    RT.gv.spmAllocMode = RT.gv.SPM_ALLOC_MODE_STATIC;
    if (spmAllocMode == "dynamic") {
        RT.gv.spmAllocMode = RT.gv.SPM_ALLOC_MODE_DYNAMIC;
    } else if (spmAllocMode == "fix") {
        RT.gv.spmAllocMode = RT.gv.SPM_ALLOC_MODE_FIX;
    }

    uint profileScaleIndex = ceil(log2f((float) numThreads));
    std::string scaleFolder = "scaling_" + systemType;
    RT.gv.loadProfilingResults(modelNameList, profileScaleIndex, scaleFolder);

    // Part4: Load models

    assert(numThreads <= 64);
    std::deque<std::deque<Model *>> models(numThreads);
    for (uint tid = 0; tid < numThreads; tid++) {
        uint64 addrOffset = (uint64) 0x40000000 * (uint64) tid;  // NOTE: 1GB for each threads
        for (auto modelName: modelNameList) {
            Model *m = new Model;
            ModelLoader::load(*m, MODEL_ROOT + modelName, addrOffset, tid);
            models[tid].push_back(m);
            printf("model: tid=%u name=%s addr=[%lx,%lx)\n", tid, modelName.c_str(), m->addrLo, m->addrHi);
            addrOffset = m->addrHi;
        }
    }

    // Part5: Create threads

    std::deque<std::deque<uint>> tasks(numThreads);
    std::mt19937 re;
    for (uint tid = 0; tid < numThreads; tid++) {
        for (uint modelIdx = 0; modelIdx < modelNameList.size(); modelIdx++) {
            for (uint cnt = 0; cnt < runTimesList[benchmark][modelIdx]; cnt++) {
                tasks[tid].push_back(modelIdx);
            }
        }
        std::shuffle(tasks[tid].begin(), tasks[tid].end(), re);
        printf("tid=%u, tasks=%s\n", tid, toString(tasks[tid]).c_str());
    }
    std::deque<ThreadTaskQueue> threads;
    for (uint tid = 0; tid < numThreads; tid++) {
        threads.emplace_back(tid, models[tid], tasks[tid],
                             staticAccAllocIdx, staticSpmAllocIdx);
        RT.threads.push_back(&threads.back());
    }

    // Part6: Run

    printf("start running\n");
    RT.run();

    // Part7: Print and return

    ss << "\n########## Thread Performance ##########\n";
    auto memCtrlCounter = system->memCtrl.getSummaryCounter();
    ss << "Total DRAM Access: " << memCtrlCounter.getTotalBytes() << "\n";
    ss << "DRAM Access(Bytes per Thread): " << memCtrlCounter.getTotalBytes() / numThreads << "\n";
    uint64 cacheAccessBytes = 0;
    if (RT.isSPMSystem()) {
        cacheAccessBytes = (dynamic_cast<SPMSystem *>(RT.system))->spm.getTotalCounter().totalAccessBytes();
    } else {
        cacheAccessBytes = (dynamic_cast<CacheSystem *>(RT.system))->cache.getTotalCounter().totalAccessBytes();
    }
    ss << "Cache/SPM Access(Bytes per Thread): " << cacheAccessBytes / numThreads << "\n";
    for (uint modelIdx = 0; modelIdx < modelNameList.size(); modelIdx++) {
        ss << "Model: " << modelNameList[modelIdx] << "\n";
        std::deque<uint64> modelCyclesList;
        for (auto &thread: threads) {
            thread.getModelCyclesListByModelIdx(modelIdx, modelCyclesList);
        }
        uint64 modelCyclesSum = 0;
        for (uint64 cycles: modelCyclesList) {
            modelCyclesSum += cycles;
        }
        ss << "\tAvg Model Cycles: " << modelCyclesSum / modelCyclesList.size() << "\n";
        ss << "\tModel Cycles: " << toString(modelCyclesList) << "\n";
    }
    for (auto &thread: threads) {
        ss << "Thread: " << thread.tid << "\n";
        ss << "\tModel Index Order: " << toString(thread.taskModelIdxQueue) << "\n";
        ss << "\tThread Cycles: " << thread.threadEndCycles - thread.threadStartCycles << "\n";
        ss << "\tCycles Waiting for Accel: " << RT.gv.threadWaitingAccCycles[thread.tid] << "\n";
        ss << "\tCycles Waiting for SPM: " << RT.gv.threadWaitingSpmCycles[thread.tid] << "\n";
    }

    ss << "\n########## System Performance ##########\n";
    ss << RT.system->getPerformanceInfo().c_str() << "\n";

    std::cout << ss.str();

    std::ofstream ofs(mudnacsimOutputRoot + outputName);
    assert(ofs.is_open());
    ofs << ss.str();
    ofs.close();

    delete system;
    return 0;
}
