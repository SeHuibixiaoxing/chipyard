#ifndef MUDNACSIM_ARG_PARSER_H
#define MUDNACSIM_ARG_PARSER_H


#include "tools.h"
#include "cxxopts.hpp"


class ArgParser {
public:
    // NoC
    std::string noc;
    bool noc_shared;
    uint noc0_flit_size;
    uint noc0_vc_num;
    uint noc0_vc_size;
    uint noc1_flit_size;
    uint noc1_vc_num;
    uint noc1_vc_size;
    // Hardware
    std::string system;
    uint channels;
    uint cache;
    uint ways;
    uint accels;
    std::string spm_addr_type;
    int spmSizePerBank;
    int spmSizePerBankBytes;
    bool spm_multicast;
    // Workload
    std::string model;
    uint layer;
    uint threads;
    double time;
    uint inf_count;
    uint benchmark;

    uint totalBatch; // for pipeline
    // Output
    std::string output;
    std::string output_root;
    // Scheduling
    uint acc_alloc_idx;
    uint spm_alloc_idx;
    std::string spm_alloc_mode;
    std::string spm_page_strategy;

    // ext
    std::string extOption;

    // pipeline
    std::string pipeSpmStrategy;
    std::string pipeAccStrategy;

    // expr_single_pipeline
    std::string pipelineMappingPath;

    // debug
    std::vector<std::string> debugFlag;
    std::string debugFlagStr;

    // expr_mixed_models
    std::string acc_num_list_str;
    std::vector<uint> acc_num_list;

    std::string spm_alloc_list_str;
    std::vector<uint> spm_alloc_kb_list;

    ArgParser() {
        // NoC
        noc = "none";
        noc_shared = true;
        noc0_flit_size = 64;
        noc0_vc_num = 4;
        noc0_vc_size = 1;
        noc1_flit_size = noc0_flit_size;
        noc1_vc_num = noc0_vc_num;
        noc1_vc_size = noc0_vc_size;
        // Hardware
        system = "cache";
        channels = 4;
        cache = 4;
        ways = 12;
        accels = 16;
        spm_addr_type = "conti";
        spm_multicast = true;
        spmSizePerBank = -1;
        spmSizePerBankBytes = -1;
        // Workload
        model = "resnet50";
        layer = 0;
        threads = 16;
        time = 100;
        inf_count = 1;
        benchmark = 0;
        totalBatch = 1;
        // Output
        std::time_t tm = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        output = std::to_string(tm);
        output_root = "expr/output/default";
        // Scheduling
        acc_alloc_idx = 0;
        spm_alloc_idx = 0;
        spm_alloc_mode = "static";
        spm_page_strategy = "naive";

        // ext
        extOption = "";

        // pipeline
        pipeSpmStrategy = "allbank";
        pipeAccStrategy = "free";

        // expr_single_pipeline
        pipelineMappingPath = "";

        // debug
        debugFlag.clear();
    }

    void parse(int argc, char *argv[]) {
        cxxopts::Options opt("");

        opt.add_options()
                // NoC
                ("noc", "network-on-chip type", cxxopts::value<std::string>())  // none/HomoTileMesh
                ("noc-shared", "if it's a shared NoC", cxxopts::value<uint>())  // 0/1
                ("noc0-flit-size", "flit size(Bytes) for NoC 0", cxxopts::value<uint>())
                ("noc0-vc-num", "number of virtual channels for NoC 0", cxxopts::value<uint>())
                ("noc0-vc-size", "size of virtual channels(flits) for NoC 0", cxxopts::value<uint>())
                ("noc1-flit-size", "flit size(Bytes) for NoC 1", cxxopts::value<uint>())
                ("noc1-vc-num", "number of virtual channels for NoC 1", cxxopts::value<uint>())
                ("noc1-vc-size", "size of virtual channels(flits) for NoC 1", cxxopts::value<uint>())
                // Hardware
                ("system", "system type", cxxopts::value<std::string>())  // cache/spm
                ("channels", "num of memory channels", cxxopts::value<uint>())
                ("cache", "size(MB) of cache", cxxopts::value<uint>())
                ("ways", "num of ways for accels", cxxopts::value<uint>())
                ("accels", "num of accels", cxxopts::value<uint>())
                ("spm-addr-type", "spm addressing type", cxxopts::value<std::string>())  // block/page/conti
                ("spm-multicast", "enable spm multicast", cxxopts::value<uint>())  // 0/1
                ("spm-size-per-bank", "spm size per bank", cxxopts::value<uint>())
                ("spm-size-per-bank-bytes", "spm size per bank in bytes", cxxopts::value<uint>())
                // Workload
                ("model", "model name", cxxopts::value<std::string>())
                ("layer", "layer index", cxxopts::value<uint>())
                ("threads", "num of threads", cxxopts::value<uint>())
                ("time", "simulation time(s)", cxxopts::value<double>())
                ("inf-count", "inference count", cxxopts::value<uint>())
                ("benchmark", "benchmark index", cxxopts::value<uint>())
                ("total-batch", "total batch for pipeline", cxxopts::value<uint>())
                // Output
                ("output", "output name", cxxopts::value<std::string>())
                ("output-root", "output root", cxxopts::value<std::string>())
                // Scheduling
                ("acc-alloc-idx", "accel allocation index", cxxopts::value<uint>())
                ("spm-alloc-idx", "SPM allocation index", cxxopts::value<uint>())
                ("spm-alloc-mode", "SPM allocation mode", cxxopts::value<std::string>())  // static/dynamic/fix
                ("spm-page-strategy", "SPM page allocation strategy", cxxopts::value<std::string>())  // naive/ordered
                // ext
                ("ext", "extent options", cxxopts::value<std::string>())
                // expr_single_pipeline
                ("expr-pipeline-layer-list", "layer ids list for expr_single_pipeline", cxxopts::value<std::string>())
                ("use-double-buffer", "enable double buffer for expr_single_pipeline", cxxopts::value<uint>())
                ("use-lazy-fetch", "enable lazy fetch for expr_single_pipeline", cxxopts::value<uint>())
                // pipeline
                ("pipe-spm-strategy", "allbank or mindis", cxxopts::value<std::string>())
                ("pipe-acc-strategy", "free or hilbert", cxxopts::value<std::string>())
                // expr_single_pipeline
                ("pipeline-mapping-path", "path to pipeline mapping yaml file", cxxopts::value<std::string>())
                // debug
                ("debug-flags", "debug flags. e.g. PIPELINE,PIPELINE_DETAIL", cxxopts::value<std::string>())
                // expr_midxed_models3
                ("acc-num-list", "accel num list for mixed models", cxxopts::value<std::string>())
                ("spm-alloc-kb-list", "spm alloc kb list for mixed models", cxxopts::value<std::string>());

        auto arg = opt.parse(argc, argv);

        // NoC
        if (arg.count("noc")) noc = arg["noc"].as<std::string>();
        if (arg.count("noc-shared")) noc_shared = arg["noc-shared"].as<uint>();
        if (arg.count("noc0-flit-size")) noc0_flit_size = arg["noc0-flit-size"].as<uint>();
        if (arg.count("noc0-vc-num")) noc0_vc_num = arg["noc0-vc-num"].as<uint>();
        if (arg.count("noc0-vc-size")) noc0_vc_size = arg["noc0-vc-size"].as<uint>();
        if (arg.count("noc1-flit-size")) noc1_flit_size = arg["noc1-flit-size"].as<uint>();
        if (arg.count("noc1-vc-num")) noc1_vc_num = arg["noc1-vc-num"].as<uint>();
        if (arg.count("noc1-vc-size")) noc1_vc_size = arg["noc1-vc-size"].as<uint>();
        // Hardware
        if (arg.count("system")) system = arg["system"].as<std::string>();
        if (arg.count("channels")) channels = arg["channels"].as<uint>();
        if (arg.count("cache")) cache = arg["cache"].as<uint>();
        if (arg.count("ways")) ways = arg["ways"].as<uint>();
        if (arg.count("accels")) accels = arg["accels"].as<uint>();
        if (arg.count("spm-addr-type")) spm_addr_type = arg["spm-addr-type"].as<std::string>();
        if (arg.count("spm-multicast")) spm_multicast = arg["spm-multicast"].as<uint>();
        if (arg.count("spm-size-per-bank")) spmSizePerBank = arg["spm-size-per-bank"].as<uint>();
        if (arg.count("spm-size-per-bank-bytes")) spmSizePerBankBytes = arg["spm-size-per-bank-bytes"].as<uint>();
        // Workload
        if (arg.count("model")) model = arg["model"].as<std::string>();
        if (arg.count("layer")) layer = arg["layer"].as<uint>();
        if (arg.count("threads")) threads = arg["threads"].as<uint>();
        if (arg.count("time")) time = arg["time"].as<double>();
        if (arg.count("inf-count")) inf_count = arg["inf-count"].as<uint>();
        if (arg.count("benchmark")) benchmark = arg["benchmark"].as<uint>();
        if (arg.count("total-batch")) totalBatch = arg["total-batch"].as<uint>();
        // Output
        if (arg.count("output")) output = arg["output"].as<std::string>();
        if (arg.count("output-root")) output_root = arg["output-root"].as<std::string>();
        // Scheduling
        if (arg.count("acc-alloc-idx")) acc_alloc_idx = arg["acc-alloc-idx"].as<uint>();
        if (arg.count("spm-alloc-idx")) spm_alloc_idx = arg["spm-alloc-idx"].as<uint>();
        if (arg.count("spm-alloc-mode")) spm_alloc_mode = arg["spm-alloc-mode"].as<std::string>();
        if (arg.count("spm-page-strategy")) spm_page_strategy = arg["spm-page-strategy"].as<std::string>();

        // ext
        if (arg.count("ext")) extOption = arg["ext"].as<std::string>();

        // pipeline
        if (arg.count("pipe-spm-strategy")) pipeSpmStrategy = arg["pipe-spm-strategy"].as<std::string>();
        if (arg.count("pipe-acc-strategy")) pipeAccStrategy = arg["pipe-acc-strategy"].as<std::string>();

        // expr_single_pipeline
        if (arg.count("pipeline-mapping-path")) pipelineMappingPath = arg["pipeline-mapping-path"].as<std::string>();

        // NoC
        assert(noc == "none" or noc == "HomoTileMesh");
        assert(noc0_flit_size > 0);
        assert(noc0_vc_num > 0);
        assert(noc1_flit_size > 0);
        assert(noc1_vc_num > 0);
        // Hardware
        assert(system == "cache" or system == "spm");
        assert(channels == 1 or channels == 2 or channels == 4 or channels == 8 or channels == 16);
        assert(cache > 0);
        assert(ways == 4 or ways == 8 or ways == 12 or ways == 16);
        assert(accels > 0);
        assert(spm_addr_type == "block" or spm_addr_type == "page" or spm_addr_type == "conti");
        // Workload
        assert(threads > 0);
        assert(time > 0);
        assert(inf_count > 0);
        // Scheduling
        assert(acc_alloc_idx < 5);
        // assert(spm_alloc_idx < 3);
        assert(spm_alloc_mode == "static" or spm_alloc_mode == "dynamic" or spm_alloc_mode == "fix");
        assert(spm_page_strategy == "naive" or spm_page_strategy == "ordered");

        if (noc0_vc_size < std::ceil(64.0 / noc0_flit_size)) {
            noc0_vc_size = std::ceil(64.0 / noc0_flit_size);
        }
        if (noc1_vc_size < std::ceil(64.0 / noc1_flit_size)) {
            noc1_vc_size = std::ceil(64.0 / noc1_flit_size);
        }

        // pipeline
        assert(pipeSpmStrategy == "allbank" or pipeSpmStrategy == "mindis");
        assert(pipeAccStrategy == "free" or pipeAccStrategy == "hilbert");

        // debug
        if (arg.count("debug-flags")) debugFlagStr = arg["debug-flags"].as<std::string>();
        if (debugFlagStr != "NONE") {
            debugFlag = mudnac::splitStrBy(debugFlagStr, ',');
            for (const auto& flag: debugFlag) {
                DebugLogger::enable(flag);
            }
        }

        // expr_mixed_models3
        if (arg.count("acc-num-list")) acc_num_list_str = arg["acc-num-list"].as<std::string>();
        if (!acc_num_list_str.empty()) {
            auto acc_strs = mudnac::splitStrBy(acc_num_list_str, ',');
            for (const auto& s: acc_strs) {
                acc_num_list.push_back(std::stoul(s));
            }
        }

        if (arg.count("spm-alloc-kb-list")) spm_alloc_list_str = arg["spm-alloc-kb-list"].as<std::string>();
        if (!spm_alloc_list_str.empty()) {
            auto spm_strs = mudnac::splitStrBy(spm_alloc_list_str, ',');
            for (const auto& s: spm_strs) {
                spm_alloc_kb_list.push_back(std::stoul(s));
            }
        }
        
    }

    std::string getString() {
        std::stringstream ss;
        // NoC
        ss << "\tNoC: " << noc << "\n";
        ss << "\tNoC shared: " << noc_shared << "\n";
        ss << "\tNoC 0 flit size(Bytes): " << noc0_flit_size << "\n";
        ss << "\tNoC 0 virtual channels: " << noc0_vc_num << "\n";
        ss << "\tNoC 0 virtual channel size(flits): " << noc0_vc_size << "\n";
        ss << "\tNoC 1 flit size(Bytes): " << noc1_flit_size << "\n";
        ss << "\tNoC 1 virtual channels: " << noc1_vc_num << "\n";
        ss << "\tNoC 1 virtual channel size(flits): " << noc1_vc_size << "\n";
        // Hardware
        ss << "\tsystem type: " << system << "\n";
        ss << "\tmemory channels: " << channels << "\n";
        ss << "\tcache size(MB): " << cache << "\n";
        ss << "\tnum of SPM ways: " << ways << "\n";
        ss << "\tnum of accels: " << accels << "\n";
        ss << "\tspm addressing type: " << spm_addr_type << "\n";
        ss << "\tspm multicast: " << spm_multicast << "\n";
        // Workload
        ss << "\tmodel name: " << model << "\n";
        ss << "\tlayer index: " << layer << "\n";
        ss << "\tnum of threads: " << threads << "\n";
        ss << "\tsimulation time(s): " << time << "\n";
        ss << "\tinference count: " << inf_count << "\n";
        ss << "\ttotalBatch: " << totalBatch << "\n";
        // Output
        ss << "\toutput name: " << output << "\n";
        ss << "\toutput root: " << output_root << "\n";
        // Scheduling
        ss << "\taccel allocation index: " << acc_alloc_idx << "\n";
        ss << "\tspm allocation index: " << spm_alloc_idx << "\n";
        ss << "\tspm allocation mode: " << spm_alloc_mode << "\n";
        ss << "\tspm page allocation strategy: " << spm_page_strategy << "\n";
        // ext
        ss << "\text option: " << extOption << "\n";
        // pipeline
        ss << "\tpipeSpmStrategy: " << pipeSpmStrategy << "\n";
        ss << "\tpipeAccStrategy: " << pipeAccStrategy<< "\n";
        // expr_single_pipeline
        ss << "\tpipelineMappingPath: " << pipelineMappingPath << "\n";
        // debug
        ss << "\tdebugFlag: " << debugFlagStr << "\n";
        // expr_mixed_models3
        ss << "\tacc_num_list: " << acc_num_list_str << "\n";
        ss << "\tspm_alloc_kb_list: " << spm_alloc_list_str << "\n";

        return ss.str();
    }
};


#endif //MUDNACSIM_ARG_PARSER_H
