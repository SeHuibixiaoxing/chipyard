#include "mudnacsim.h"
#include "cxxopts.hpp"


int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);


    const std::string MODEL_ROOT = "models/";
    const std::string OUTPUT_ROOT = "expr/output/expr_single_model/default/";

    // Part1: Parse input options

    ArgParser ap;
    ap.accels = 4;
    ap.channels = 1;
    ap.cache = 4;
    ap.output_root = OUTPUT_ROOT;
    ap.parse(argc, argv);
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
    config.cacheBanks = 4;
    config.spmWays = ap.ways;
    config.spmPageBytes = 32 * 1024;
    config.spmMulticast = ap.spm_multicast;
    config.accelNum = ap.accels;
    config.accelArrayH = config.accelArrayW = 32;
    config.accelSpmAddrType = AccelConfig::SPM_ADDR_TYPE_CONTINUOUS;
    // config.accelSpmAddrType = AccelConfig::SPM_ADDR_TYPE_PAGE_INTERLEAVED;

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

    // SPMNoCSystemHomoTileMesh *system = new SPMNoCSystemHomoTileMesh(config, noc0RouterConfig, noc1RouterConfig, noc0NiConfig, noc1NiConfig, ap.noc_shared);  
    auto system = new SPMSystem(config);
    //SPMSystem *system = new SPMSystem(config);

    bool isParallel = ap.extOption == "1" ? true : false;

    std::vector<uint64> fetchShape, fetchStrides;
    std::vector<uint64> flushShape, flushStrides;
    std::vector<uint64> sendShape, sendStrides;

    fetchShape = {54,54,1,1,64};
    flushShape = {54,54,1,1,64};
    sendShape = {54,54,1,1,64};

    // fetchShape = {16,16,32};
    // flushShape = {16,16,32};
    // sendShape = {16,16,32};

    std::cout << "fetchShape=" << mudnac::toString(fetchShape) << std::endl;
    std::cout << "flushShape=" << mudnac::toString(flushShape) << std::endl;
    std::cout << "sendShape=" << mudnac::toString(sendShape) << std::endl;

    fetchStrides.resize(fetchShape.size());
    flushStrides.resize(flushShape.size());
    sendStrides.resize(sendShape.size());

    fetchStrides.back() = 1;
    flushStrides.back() = 1;
    sendStrides.back() = 1;

    for(int i = fetchShape.size() - 2;i >= 0;-- i) {
        fetchStrides[i] = fetchShape[i + 1] * fetchStrides[i + 1];
    }
    for(int i = flushShape.size() - 2;i >= 0;-- i) {
        flushStrides[i] = flushShape[i + 1] * flushStrides[i + 1];
    }
    for(int i = sendShape.size() - 2;i >= 0;-- i) {
        sendStrides[i] = sendShape[i + 1] * sendStrides[i + 1];
    }

    std::cout << "fetchStrides=" << mudnac::toString(fetchStrides) << std::endl;
    std::cout << "flushStrides=" << mudnac::toString(flushStrides) << std::endl;
    std::cout << "sendStrides=" << mudnac::toString(sendStrides) << std::endl;

    // Part6: Run
    auto fetchCMD = std::make_unique<AccelSPMFetch>();
    auto flushCMD = std::make_unique<AccelSPMFlush>();
    auto sendCMD = std::make_unique<AccelSPMSend>();
    uint pageIdxOffset = log2Uint(config.spmPageBytes);
    uint dramChannelIdxOffset = MemCtrlConfig::DRAM_CHANNEL_IDX_OFFSET;

    uint64 fetchDramAddr = 0;
    uint64 fetchSpmAddr = 0;
    Tensor tensorFetch(fetchDramAddr, fetchStrides);
    Tensor spmTensorFetchLayout(fetchSpmAddr, fetchStrides);


    // uint64 flushDramADdr = (static_cast<uint>(log2(fetchStrides[0]*fetchShape[0])) / dramChannelIdxOffset + 1) << dramChannelIdxOffset;
    uint64 flushDramADdr = fetchStrides[0]*fetchShape[0];
    // uint64 flushSpmADdr = (config.getSPMTotalPages() / config.cacheBanks) << pageIdxOffset + fetchStrides[0] * fetchShape[0];

    // uint64 flushDramADdr = 0;
    uint64 flushSpmADdr = 0;

    Tensor tensorFlush(flushDramADdr, flushStrides);
    Tensor spmTensorFlushLayout(flushSpmADdr, flushStrides);


    for(uint i = 0;i < config.getSPMTotalPages();++ i) {
        system->accels[0].pageTable.set(i, i + 1, true);
    }

    //fetchCMD
    SPMTensor spmFetchTensor;

    spmFetchTensor.set(spmTensorFetchLayout, false, false);
    fetchCMD->shape = fetchShape;
    fetchCMD->inputTensors[0] = tensorFetch;
    fetchCMD->inputSPMTensors[0] = spmFetchTensor;
    

    //flushCMD
    SPMTensor spmFlushTensor;

    spmFlushTensor.set(spmTensorFlushLayout, false, false);
    flushCMD->shape = flushShape;
    flushCMD->outputTensors[0] = tensorFlush;
    flushCMD->outputSPMTensors[0] = spmFlushTensor;

    //sendCMD
    SPMTensor spmSendSrcTensor;
    SPMTensor spmSendDstTensor;

    spmSendSrcTensor.set(spmTensorFetchLayout, false, false);
    spmSendDstTensor.set(spmTensorFlushLayout, false, false);
    sendCMD->shape = sendShape;
    sendCMD->inputSPMTensors[0] = spmSendSrcTensor;
    sendCMD->outputSPMTensors[0] = spmSendDstTensor;
    
    

    printf("\nrunning...\n");   
  
    system->addCmd(0, fetchCMD.get());
    std::cout << "add fetchCMD:" << fetchCMD->toString() << std::endl;
    if(isParallel) {
        system->addCmd(0, flushCMD.get());
        std::cout << "add flushCMD:" << flushCMD->toString() << std::endl;
        system->addCmd(0, sendCMD.get());
        std::cout << "add sendCMD" << sendCMD->toString() << std::endl;
    }

    // Part7: Print and return
    uint maxCycles = 1e6, nowCycles = 0;
    bool fetchOk = false, flushOk = false, sendOk = false;
    for(;nowCycles < maxCycles && !(fetchOk && flushOk && sendOk);++ nowCycles) {
        system->tick();
        if(nowCycles % 10 == 0) {
            printf("system cycle: %u\n", nowCycles);
        }
        auto finishCmd = system->getFinishedCmd(0);
        if(finishCmd != NULL) {
            if(typeid(*finishCmd) == typeid(AccelSPMFetch)) {
                fetchOk = true;
                printf("%u finish fetchCMD\n", nowCycles);
                if(!isParallel) {
                    system->addCmd(0, flushCMD.get());
                    std::cout << "add flushCMD" << flushCMD->toString() << std::endl;
                }
            } else if(typeid(*finishCmd) == typeid(AccelSPMFlush)) {
                flushOk = true;
                printf("%u finish flushCMD\n", nowCycles);
                if(!isParallel) {
                    system->addCmd(0, sendCMD.get());
                    std::cout << "add sendCMD" << sendCMD->toString() << std::endl;
                }
            } else if(typeid(*finishCmd) == typeid(AccelSPMSend)) {
                sendOk = true;
                printf("%u finish sendCMD\n", nowCycles);
            }
            else {
                assert(false);
            }
        }
    }

    if(fetchOk) {
        printf("pass Fetch\n");
    } else {
        printf("Fetch fail\n");
    }

    if(flushOk) {
        printf("pass Flush\n");
    } else {
        printf("Flush fail\n");
    }

    if(sendOk) {
        printf("pass Send\n");
    } else {
        printf("Send fail\n");
    }

    ss << "\n########## Performance Info ##########\n\n";
    ss << system->getPerformanceInfo().c_str();

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

    delete system;
    return 0;
}
