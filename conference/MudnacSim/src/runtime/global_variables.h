#ifndef MUDNACSIM_GLOBAL_VARIABLES_H
#define MUDNACSIM_GLOBAL_VARIABLES_H


#include "tools.h"
#include "spm_manager.h"


namespace mudnac {


    class GlobalVariables {
    public:
        GlobalVariables() { 

        }

        // for normal
        // const  // note: should equal to mapping candidates
        const static uint NUM_ACCEL_ALLOCS = 6;
        const static uint NUM_SPM_ALLOCS = 11;

        const static uint MAX_THREADS = 100;
        const static uint MAX_MODELS = 100;
        const static uint MAX_TASKS = 1000;

        const static uint ACCEL_ALLOC_MODE_STATIC = 0;
        const static uint ACCEL_ALLOC_MODE_DYNAMIC = 1;

        const static uint SPM_ALLOC_MODE_STATIC = 0;
        const static uint SPM_ALLOC_MODE_DYNAMIC = 1;
        const static uint SPM_ALLOC_MODE_FIX = 2;

        const static uint SPM_PAGE_ALLOC_STRATEGY_NAIVE = 0;
        const static uint SPM_PAGE_ALLOC_STRATEGY_ORDERED = 1;
        const static uint SPM_PAGE_ALLOC_STRATEGY_AFFILIATION = 2;

        const static uint MEMORY_ACCESS_MODE_UNLIMITED = 0;
        const static uint MEMORY_ACCESS_MODE_LIMITED = 1;

        // hardware related
        uint numAccels, numPages, numBanks;

        // experiment setting
        uint accelAllocMode;
        uint spmAllocMode;
        uint memoryAccessMode;

        // Accel resource
        std::vector<std::vector<uint>> threadAccelAlloc; // threadAccelAlloc[threadId]=accel list
        std::set<uint> availableAccelSet; 
        std::deque<uint> accelAcquireQueue;  // for FCFS implementation
        std::map<uint, uint> accelAcquireDict;  // for FCFS implementation. map: threadId -> accelCount
        bool enableThreadAccelAffiliation; // if true, use threadAccelAffiliation. thread tid only can be allocated accel ids in threadAccelAffiliation[tid]
        std::vector<std::vector<uint>> threadAccelAffiliation;  // available accel IDs for each thread. In expr_mixed_models, each accelerator is bound to one thread.
        
        // SPM resource
        std::vector<std::deque<uint>> threadSpmPageAlloc; // threadSpmPageAlloc[threadId]=spm_page list
        std::set<uint> availableSpmPageSet;
        std::deque<uint> spmAcquireQueue;  // for FCFS implementation
        std::map<uint, uint> spmAcquireDict;  // for FCFS implementation. map: threadId -> pageCount
        uint spmPageAllocStrategy;
        std::vector<std::vector<uint>> accelSpmBankOrder;  // for NoC-aware SPM mapping. accelId -> spmBankOrder list. Sort all the optional banks by hops from the accelId accelerator
        std::vector<std::vector<uint>> threadSPMAffiliation;  // available spm page IDs for each thread. use for pipeline.
        std::vector<std::vector<std::pair<uint, uint>>> threadPipelineAccIdxVPage; // (acc, vpage) for each thread for pipeline. it is one-to-one correspondence with threadSPMAffiliation.


        // runtime updating info
        std::deque<uint64> threadWaitingAccCycles;  // cycles waiting for accel resource
        std::deque<uint64> threadWaitingSpmCycles;  // cycles waiting for spm resource
        std::vector<uint> threadCurrentModelType;  // current model type of each thread
        std::vector<uint> threadCurrentLayerIdx;  // current layer index of each thread
        std::vector<uint> threadAccelAllocIdx;  // accel allocation index of each thread
        std::vector<uint> threadSpmAllocIdx;  // spm allocation index of each thread

        // Memory Bandwidth repartition
        std::vector<uint64> bw_threadEpoch;
        std::vector<uint64> bw_threadMaxReq;

        // SPM repartition
        std::vector<bool> spm_threadInterLayerReuse; //tid -> true/flase. If true, inter-layer reuse is enabled in this thread. It will be set true after the head layer in the layer group is repartitioned seccessfully. It will be set false after the tail layer finishes or the repartition is failed.
        std::vector<uint64> spm_threadEstimatedReleasingTime;
        std::vector<uint> spm_threadEstimatedAcquiredPages;
        double spm_alpha; // The coefficient of pages reserved for the next layer of the thread if the thread is able to execute the current layer and release spm before the wait time threshold
        double spm_beta; // The coefficient of cycles allowed to wait for page release

        // Profiling results
        bool pf_doneProfiling;
        std::vector<std::vector<uint64>> pf_modelCycles;  // [MAX_ACCEL_ALLOCS, MAX_MODELS]
        std::vector<std::vector<std::deque<uint64>>> pf_modelLayerCycles;  // [MAX_ACCEL_ALLOCS, MAX_MODELS, layers]
        std::vector<std::vector<std::deque<uint64>>> pf_modelRemainingCycles;  // [MAX_ACCEL_ALLOCS, MAX_MODELS, layers]

        // QoS Scenario related vars are named with prefix "qos_"
        std::vector<uint64> qos_modelTargetCycles;  // target (expected) cycles for each model
        std::vector<uint> qos_taskModelTypeQueue;  // model type of each task
        uint qos_nextTaskIdx;
        std::vector<uint64> qos_taskDispatchTime;  // dispatch time of each task
        std::vector<uint64> qos_taskStartTime;  // start time of each task
        std::vector<uint64> qos_taskEndTime;  // end time of each task
        std::vector<uint> qos_threadCurrentTaskIdx;  // current task index of each thread
        std::vector<int64> qos_threadCurrentScore;  // for computeRepartition
        std::vector<int64> qos_threadCurrentMemScore;  // for memoryRepartition
        std::vector<uint64> qos_threadDramUtil;  // for memoryRepartition

        void init(uint numAccels_, uint numPages_, uint numBanks_, uint pages_per_bank, uint page_siz);

        void loadProfilingResults(const std::deque<std::string> &modelNameList,
                                  uint scaleIndex = 0,
                                  std::string scaleFolder = "scaling",
                                  std::string profileRoot = "expr/input/profiling/");
    };

} // mudnac


#endif //MUDNACSIM_GLOBAL_VARIABLES_H
