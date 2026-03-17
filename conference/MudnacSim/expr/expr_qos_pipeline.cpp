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
        100, 
        100, 
        100, 
        100, 
        100, 
        100, 
        100, 
        100, 
        
        // 100, 
        // 100, 
        // 100, 
        // 100
};

static std::deque<uint64> modelTaskArriveFpsList = {
//        200, 250, 250, 20, 10, 60, 20, 200  // backup
        // 150, 350, 350, 25, 10, 60, 25, 150  // backup2-5

        // 10: 1 0.8 0.8
        // 20: 0.8 0.6 0.6
        // 160, // 0
        // 352, // 1
        // 352, // 2
        // 32, // 3
        // 32, // 4
        // 64, // 5
        // 32, // 6
        // 160, // 7
        // // 160, // 8
        // // 96, // 9
        // // 64, // 10
        // // 48  // 11

        160, // 0
        160, // 1
        160, // 2
        64, // 3
        64, // 4
        64, // 5
        64, // 6
        80, // 7
        // 160, // 8
        // 96, // 9
        // 64, // 10
        // 48  // 11

//        150, 350, 350, 30, 10, 60, 30, 150  // backup6
//        150, 300, 300, 30, 10, 60, 30, 150  // backup7
};

// static std::deque<uint64> modelTargetFpsList = {
// //        200, 250, 250, 20, 10, 60, 20, 200
//         // 150, 350, 350, 25, 10, 60, 25, 150
//         160, 352, 352, 32, 16, 80, 32, 160, 160, 96, 64, 48  // backup2-5
// //        150, 350, 350, 30, 10, 60, 30, 150
// //        150, 300, 300, 30, 10, 60, 30, 150
// };

static std::deque<uint> modelPossibilityList = {
        16, 63, 48, 4, 1, 9, 4, 27, 0, 0, 0
};

uint acc_origin = 4;
float fps_scale = 1.0;

void createWorkload(uint benchmark, uint tasks, uint batch_per_task);


int main(int argc, char **argv) {
    const std::string MODEL_ROOT = "models/pipeline/";
    const std::string PROFILING_ROOT = "models/pipeline/";
    const std::string OUTPUT_ROOT = "expr/output/expr_qos_pipeline/default/";
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
            ("pipe-spm-strategy", "pipeline SPM allocation strategy", cxxopts::value<std::string>())
            ("pipe-acc-strategy", "pipeline accelerator allocation strategy", cxxopts::value<std::string>())
            ("noc-flit-size", "NoC flit size (bytes)", cxxopts::value<uint>())
            ("batch-size", "batch size", cxxopts::value<uint>())
            ("timeout-threshold", "allocation timeout threshold (seconds)", cxxopts::value<float>())
            ("partition-num", "number of scheduling partitions", cxxopts::value<uint>())
            ("debug-flags", "debug flag string (comma separated)", cxxopts::value<std::string>())
            ("method", "mapping method. ours, gemini or tangram.", cxxopts::value<std::string>())
            ("schedule-acc-alloc-method", "accelerator allocation method in schedule. static_fixed or dynamic_base", cxxopts::value<std::string>())
            ("acc-increase-factor", "accelerator increase factor when deadline missed", cxxopts::value<float>())
            ("qps-per-model", "QPS per model (comma separated)", cxxopts::value<std::string>())
            ("output-root", "output root path", cxxopts::value<std::string>());

    auto arg = opt.parse(argc, argv);

    uint memChannels = 4;
    uint numAccels = 16;
    uint numThreads = 8;
    uint benchmark = 0;
    uint tasks = 20;
    uint spm_size_per_bank_kb = 1024;
    std::string pipeSpmStrategy = "allbank";
    std::string pipeAccStrategy = "hilbert";
    uint noc_flit_size = 64;
    double targetScale = 1;
    double timeout_threshold = 0.1;
    uint partition_num = 3;
    uint batch_size_per_task = 16;
    uint acc_target_origin = 16;
    std::string outputName = "";
    std::string outputRoot = OUTPUT_ROOT;
    std::string debugFlagStr;
    std::vector<std::string> debugFlag;
    std::string method;
    std::string schedule_acc_alloc_method_str;
    PipelineRuntime::ScheduleAccAllocStrategy schedule_acc_alloc_strategy = PipelineRuntime::ScheduleAccAllocStrategy::DYNAMIC_BASE;
    float acc_increase_factor = 1.0;
    std::string qpsStr;

    if (arg.count("channels")) memChannels = arg["channels"].as<uint>();
    if (arg.count("accels")) numAccels = arg["accels"].as<uint>();
    if (arg.count("threads")) numThreads = arg["threads"].as<uint>();
    if (arg.count("benchmark")) benchmark = arg["benchmark"].as<uint>();
    if (arg.count("tasks")) tasks = arg["tasks"].as<uint>();
    if (arg.count("target-scale")) targetScale = arg["target-scale"].as<double>();
    if (arg.count("output")) outputName = arg["output"].as<std::string>();
    if (arg.count("spm_size_per_bank_kb")) spm_size_per_bank_kb = arg["spm-size-per-bank"].as<uint>();
    if (arg.count("pipe-spm-strategy")) pipeSpmStrategy = arg["pipe-spm-strategy"].as<std::string>();
    if (arg.count("pipe-acc-strategy")) pipeAccStrategy = arg["pipe-acc-strategy"].as<std::string>();
    if (arg.count("noc-flit-size")) noc_flit_size = arg["noc-flit-size"].as<uint>();
    if (arg.count("output-root")) outputRoot = arg["output-root"].as<std::string>();
    if (arg.count("batch-size")) batch_size_per_task = arg["batch-size"].as<uint>();
    if (arg.count("timeout-threshold")) timeout_threshold = arg["timeout-threshold"].as<float>();
    if (arg.count("partition-num")) partition_num = arg["partition-num"].as<uint>();
    if (arg.count("method")) method = arg["method"].as<std::string>();
    if (arg.count("schedule-acc-alloc-method")) schedule_acc_alloc_method_str = arg["schedule-acc-alloc-method"].as<std::string>();
    if (arg.count("acc-increase-factor")) acc_increase_factor = arg["acc-increase-factor"].as<float>();
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
    if (arg.count("debug-flags")) {
        debugFlagStr = arg["debug-flags"].as<std::string>();
        std::stringstream ss(debugFlagStr);
        std::string flag;
        while (std::getline(ss, flag, ',')) {
            debugFlag.push_back(flag);
            DebugLogger::enable(flag);
        }
    }
    if (!schedule_acc_alloc_method_str.empty()) {
        if (schedule_acc_alloc_method_str == "static_fixed") {
            schedule_acc_alloc_strategy = PipelineRuntime::ScheduleAccAllocStrategy::STATIC_FIXED;
        } else if (schedule_acc_alloc_method_str == "dynamic_base") {
            schedule_acc_alloc_strategy = PipelineRuntime::ScheduleAccAllocStrategy::DYNAMIC_BASE;
        } else {
            assert(false && "Unknown schedule acc alloc method");
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
    config.accelSpmAddrType = AccelConfig::SPM_ADDR_TYPE_CONTINUOUS;

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
    AcceleratorManager::PipelineAccAllocStrategy acc_manager_strategy;
    if (pipeAccStrategy == "free") {
        acc_manager_strategy = AcceleratorManager::PipelineAccAllocStrategy::FREE;
    } else if (pipeAccStrategy == "hilbert") {
        acc_manager_strategy = AcceleratorManager::PipelineAccAllocStrategy::HILBERT;
    } else {
        assert(false && "Unknown pipeAccStrategy");
    }
    
    PipelineRuntime::PipelineSpmAllocStrategy spm_manager_strategy;
    if (pipeSpmStrategy == "allbank") {
        spm_manager_strategy = PipelineRuntime::PipelineSpmAllocStrategy::ALL_BANK_PAGE_INTER;
    } else if (pipeSpmStrategy == "mindis") {
        spm_manager_strategy = PipelineRuntime::PipelineSpmAllocStrategy::MIN_DIS;
    } else {
        assert(false && "Unknown pipeSpmStrategy");
    }

    RT.setSystem(system);
    uint scaleIndex = 1;
    RT.PipelineRuntimeInit(system->getAccW(), system->getAccH(),
                              noc_flit_size,
                              acc_manager_strategy,
                              spm_manager_strategy);
    
    // load profiline
    std::string profiling_path = PROFILING_ROOT + "schedule_point_profiling_" + method + ".yaml";
    RT.ScheduleRuntimeInit(profiling_path, batch_size_per_task, tasks, numThreads, timeout_threshold, partition_num, modelNameList.size(), method, schedule_acc_alloc_strategy, acc_increase_factor);

    fps_scale = (RT.num_accels_ / static_cast<float>(acc_origin) / static_cast<float>(batch_size_per_task));
    for (uint modelIdx = 0; modelIdx < modelNameList.size(); modelIdx++) {
        RT.qos_model_target_cycles_[modelIdx] = 1e9 / (modelTaskArriveFpsList[modelIdx] / batch_size_per_task) * targetScale;
    }

    // Part4: Create workloads and Load models
    createWorkload(benchmark, tasks, batch_size_per_task);

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
        uint modelIdx = RT.qos_task_model_type_queue_[taskIdx];
        auto &modelName = modelNameList[modelIdx];
        Model *m = new Model;
        ModelLoader::load(*m, MODEL_ROOT + modelName, models[modelIdx]->addrLo);
        taskModelQueue.push_back(m);
        printf("task: taskIdx=%u, modelIdx=%u, name=%s addr=[%lx,%lx)\n",
               taskIdx, modelIdx, modelName.c_str(), m->addrLo, m->addrHi);
    }

    // Part5: Create threads

    std::deque<ThreadQoSPipeline> threads;
    for (uint tid = 0; tid < numThreads; tid++) {
        threads.emplace_back(tid, taskModelQueue, tasks);
        RT.threads.push_back(&threads.back());
    }

    // Part6: Run

    RT.run();

    // Part7: Print and return

    ss << "\n######## Task Performance ########\n";
    uint satisfied = 0;
    // TODO: stp
    // double stp = 0;
    for (uint taskIdx = 0; taskIdx < tasks; taskIdx++) {
        uint modelIdx = RT.qos_task_model_type_queue_[taskIdx];
        uint64 dispatch = RT.qos_task_dispatch_time_[taskIdx];
        uint64 target = RT.qos_model_target_cycles_[modelIdx];
        uint64 waiting = RT.qos_task_start_time_[taskIdx] - RT.qos_task_dispatch_time_[taskIdx];
        uint64 startToFinish = RT.qos_task_end_time_[taskIdx] - RT.qos_task_start_time_[taskIdx];
        uint64 dispatchToFinish = RT.qos_task_end_time_[taskIdx] - RT.qos_task_dispatch_time_[taskIdx];
        int64 slack = (int64) target - (int64) dispatchToFinish;
        double slackRate = (double) slack / (double) target;
        ss << "Task: " << taskIdx << "\n";
        ss << "\tWorkload Type: " << modelIdx << ", " << modelNameList[modelIdx] << "\n";
        ss << "\tDispatch: " << dispatch << "\n";
        ss << "\tTarget: " << target << "\n";
        ss << "\tWaiting: " << waiting << "\n";
        ss << "\tStart to Finish: " << startToFinish << "\n";
        ss << "\tDispatch to Finish: " << dispatchToFinish << "\n";
        ss << "\tAlloc Start Time: " << mudnac::toString(RT.qos_task_subgraph_start_alloc_time_[taskIdx]) << "\n";
        ss << "\tRun Start Time: " << mudnac::toString(RT.qos_task_subgraph_start_run_time_[taskIdx]) << "\n";
        ss << "\tRun End Time: " << mudnac::toString(RT.qos_task_subgraph_end_run_time_[taskIdx]) << "\n";

        ss << "\tAlloc Waiting Time: [";
        for (uint i = 0;i < RT.qos_task_subgraph_start_run_time_[taskIdx].size();++ i) {
            ss << RT.qos_task_subgraph_start_run_time_[taskIdx][i] - RT.qos_task_subgraph_start_alloc_time_[taskIdx][i] << ",";
        }
        ss << "]" << "\n";

        ss << "\tExecute Time: [";
        for (uint i = 0;i < RT.qos_task_subgraph_start_run_time_[taskIdx].size();++ i) {
            ss << RT.qos_task_subgraph_end_run_time_[taskIdx][i] - RT.qos_task_subgraph_start_alloc_time_[taskIdx][i] << ",";
        }
        ss << "]" << "\n";

        ss << "\tEst Time per subgraph (subgraph): " << mudnac::toString(RT.qos_task_subgraph_est_time_subgraph_[taskIdx]) << "\n";
        ss << "\tEst Time per subgraph (task): " << mudnac::toString(RT.qos_task_subgraph_est_time_task_[taskIdx]) << "\n";
        ss << "\tAcc per subgraph: " << mudnac::toString(RT.qos_task_subgraph_acc_num_[taskIdx]);
        ss << "\tASlack per subgraph: " << mudnac::toString(RT.qos_task_subgraph_slack_time_[taskIdx]);
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

        float total_acc_num = 0;
        uint total_wait_alloc_time = 0, total_slack_time = 0, total_wait_time = 0;
        for (uint taskIdx = 0; taskIdx < tasks; taskIdx++) {
            if (modelIdx == RT.qos_task_model_type_queue_[taskIdx]) {
                uint64 startToFinish = RT.qos_task_end_time_[taskIdx] - RT.qos_task_start_time_[taskIdx];
                uint64 dispatchToFinish = RT.qos_task_end_time_[taskIdx] - RT.qos_task_dispatch_time_[taskIdx];
                uint64 target = RT.qos_model_target_cycles_[modelIdx];
                modelCyclesList.push_back(startToFinish);
                avgModelCycles += startToFinish;
                if (dispatchToFinish < target) {
                    sla += 1;
                }
                
                uint tmp_acc_num = 0, tmp_wait_alloc_time = 0;
                for (uint i = 0;i < RT.qos_task_subgraph_start_run_time_[taskIdx].size();++ i) {
                    tmp_acc_num += RT.qos_task_subgraph_acc_num_[taskIdx][i];
                    total_wait_alloc_time += RT.qos_task_subgraph_start_run_time_[taskIdx][i] - RT.qos_task_subgraph_start_alloc_time_[taskIdx][i];
                }

                total_acc_num += 1.0 * tmp_acc_num / RT.qos_task_subgraph_start_run_time_[taskIdx].size();
                total_slack_time += target - dispatchToFinish;
                total_wait_time += RT.qos_task_start_time_[taskIdx] - RT.qos_task_dispatch_time_[taskIdx];
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
        ss << "\tTarget Model Cycles: " << RT.qos_model_target_cycles_[modelIdx] << "\n";
        ss << "\tProfile Model Cycles: ";
        for (uint i = 0;i < RT.profiling_result_.getAvailableAccs().size(); i++) {
            uint acc = RT.profiling_result_.getAvailableAccs()[i];
            uint spm = RT.profiling_result_.getAvailableSpmPerAccKb()[0];
            ss << "profiling[acc=" << acc << ", spm=" << spm << "]=" << RT.profiling_result_.get(RT.GenerateTarget(acc, spm, modelIdx)).remain_cost[0] << ";";
        }
        ss << "\n";
        ss << "\tAvg Acc Num: " << 1.0 * total_acc_num / modelCyclesList.size() << "\n";
        ss << "\tAvg wait alloc time per task: " << 1.0 * total_wait_alloc_time / modelCyclesList.size() << "\n";
        ss << "\tAvg wait schedule time per task: " << 1.0 * total_wait_time / modelCyclesList.size() << "\n";
        ss << "\tAvg Model Cycles: " << avgModelCycles << "\n";
        ss << "\tStandard Deviation: " << (uint64) sd << "\n";
        ss << "\tSLA: " << sla << "\n";
    }

    ss << "\n######## Thread Performance ########\n";
    for (auto &thread: threads) {
        ss << "Thread: " << thread.tid << "\n";
        ss << "\tCycles: " << thread.threadEndCycles - thread.threadStartCycles << "\n";
        ss << "\tCycles Waiting for tasks: " << thread.waitingCycles << "\n";
        // ss << "\tCycles Waiting for SPM: " << RT.thread_waiting_spm_cycles_[thread.tid] << "\n";
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

static void createWorkload2(uint benchmark, uint tasks, uint batch_size_per_task = 1) {
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

        RT.qos_task_model_type_queue_[taskIdx] = modelIdx;
        RT.qos_task_dispatch_time_[taskIdx] = dispatchTime;
        printf("createWorkload2: taskIdx=%u, modelIdx=%u, dispatch=%lu\n", taskIdx, modelIdx, dispatchTime);

        // compute the next dispatch time of this model
        modelNextDispatchTimeList[modelIdx] += modelAvgInterList[modelIdx];
        while (possDistri(re) > modelHasRequestPossibilityList[modelIdx]) {
            printf("\tno request, modelIdx=%u, dispatch=%lu\n", modelIdx, modelNextDispatchTimeList[modelIdx]);
            modelNextDispatchTimeList[modelIdx] += modelAvgInterList[modelIdx];
        }
    }
}

void createWorkload(uint benchmark, uint tasks, uint batch_size_per_task = 1) {
    createWorkload2(benchmark, tasks, batch_size_per_task);
}
