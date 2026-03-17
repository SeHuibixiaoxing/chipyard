#ifndef MUDNACSIM_RUNTIME_H
#define MUDNACSIM_RUNTIME_H


#include "cache_system.h"
#include "spm_system.h"
#include "global_variables.h"
#include "mapping.h"
#include "accelerator_manager.h"
#include "yaml-cpp/yaml.h"
#include <string>
#include <deque>
#include <tuple>
#include <climits>


namespace mudnac {


    class HardwareConfig;

    class Runtime;

    class PipelineRuntime;

    class Layer;

    class Model;

    class Thread;

    class ScheduleAction;


    extern PipelineRuntime RT;

    class HardwareConfig {
    public:
        uint numAccels;
        uint inputBufBytes, weightBufBytes, psumBufBytes;
        uint inputWordBytes, psumWordBytes;
        uint nBanks;
        uint pageBytes;
        uint nPages;
        uint nPagesPerBank;
        uint spmTotalBytes;
        double dramBytesPerCycles;
        uint dramChannel;

        void init(BaseSystem &system);
    };

    class SpmSource {
    public:
        SpmSource();
        uint num_spm_pages_;
        std::unordered_map<TensorId, PageSetShared> weight_spm_pages; // [tensorid] -> shared_ptr spm_ppages for weight
        std::vector<std::unordered_map<TensorId, std::vector<PageSetShared>>> in_stage_spm_pages_; // [stage_idx] -> map[tensorid, vec<shared_ptr spm_ppages for double buffer>];
        std::unordered_map<TensorId, std::vector<PageSetShared>> ring_buffer_spm_pages_; // [tensorid] -> vec[shared_ptr spm_ppages for each ring buffer];
        void clear();
    };

    class AccSource {
    public:
        AccSource();
        uint num_acc_;
        std::vector<std::vector<uint>> acc_ids_; // [stage_idx] -> vec[pacc id]
        std::vector<uint> all_acc_ids_; // all pacc id

        void clear();
    };
    class Runtime {
    public:

        uint64 PID;

        // system
        BaseSystem *system;
        HardwareConfig hw;
        GlobalVariables gv;

        std::deque<Thread *> threads;

        double runElapsedSeconds = 0;

        std::vector<uint> model_has_run_once;
        std::vector<uint> spm_pages_alloc_per_model;

        Runtime();

        void setSystem(BaseSystem &system_);
        void setSystem(BaseSystem *system_);

        void run();

        bool isSPMSystem();

        void addCmd(uint accId, AccelTiledCMD *cmd);

        AccelTiledCMD *getFinishedCmd(uint accId, bool is_pipeline);
        AccelTiledCMD *getFinishedCmd(const uint accId, const uint globalId, bool is_pipeline);

        uint64 getCycles();

        void setAccMemEpochRate(uint accId, uint64 epoch, uint64 maxReqs);

        void setAccPageForBypass(uint accId, uint pageForBypass);

        void setAccPageForBypass(uint accId);

        std::string getFormatedRunElapsedTime();

        /*
        * =========
        * CaMDN使用
        * =========
        */
        bool acquireAccel(uint tid, uint numAccels);

        void releaseAccel(uint tid, uint numAccels);

        uint getAvailableAccel();

        bool acquireSPM(uint tid, uint numVPages); 

        void releaseSPM(uint tid, uint numVPages);

        inline uint getAvailableSPM();

        void releaseAll(uint tid);

        void setMemEpochRate(uint tid, uint64 epoch, uint64 maxReqs);

        void setPageForBypass(uint tid, uint pageForBypass);

        void spmRepartition(uint tid, uint64 &threshold);

        void spmRepartitionReduce(uint tid, uint64 &threshold);

        void computeRepartition(uint tid);

        void memoryRepartition(uint tid);
        
        void setPageTable(uint accId, uint vPage, uint pPage);
        void invalidPageTable(uint accId, uint vPage);
    };

    class PipelineProfilingResult {
    public:
        // Target 结构体，用于作为检索 key
        struct Target {
            uint acc;
            uint spm_per_acc_kb;
            uint batch;
            uint dram_bw;
            uint noc_bw;
            uint partition_num;
            uint model_idx;

            // 比较运算符，用于 map
            bool operator==(const Target& other) const;

            // 哈希函数，用于 unordered_map
            struct Hash {
                size_t operator()(const Target& t) const;
            };

            nlohmann::json toJson() const;
            std::string toString() const;
        };

        // 预处理条目：同一个 target 下的 mapping 顺序执行
        // prefix_cost: 包含当前 mapping 在内的累计执行时间
        // suffix_cost: 当前 mapping 之后还需要的执行时间（不含当前）
        struct PreprocessedEntry {
            std::vector<std::string> mapping_paths;
            std::vector<uint> cost;

            std::vector<uint> pre_cost; // 第i个sub-graph及其之前的sub-graph的cost之和
            std::vector<uint> remain_cost; // 第i个sub-graph及其之后（含i）的sub-graph的cost之和，剩余时间
        };

        // 从 YAML 文件加载数据并预处理
        void loadFromFile(const std::string& filepath);

        // 通过 target 检索预处理后的条目序列
        const PreprocessedEntry& get(const Target& target) const;

        // 获取所有可用的参数集合（升序唯一）
        const std::vector<uint>& getAvailableAccs() const;
        const std::vector<uint>& getAvailableSpmPerAccKb() const;
        const std::vector<uint>& getAvailableBatches() const;
        const std::vector<uint>& getAvailableDramBw() const;
        const std::vector<uint>& getAvailableNocBw() const;
        const std::vector<uint>& getAvailablePartitionNums() const;

    private:
        std::unordered_map<Target, PreprocessedEntry, Target::Hash> preprocessed_data_;
        // available parameter sets discovered from profiling file
        std::vector<uint> available_accs_;
        std::vector<uint> available_spm_per_acc_kb_;
        std::vector<uint> available_batches_;
        std::vector<uint> available_dram_bw_;
        std::vector<uint> available_noc_bw_;
        std::vector<uint> available_partition_nums_;
    };

    class PipelineRuntime: public Runtime {
    public:
        enum class PipelineSpmAllocStrategy {
            ALL_BANK_PAGE_INTER, // 所有bank上交错
            MIN_DIS, // 最小距离优先
        };

        enum class ScheduleAccAllocStrategy {
            STATIC_FIXED,
            DYNAMIC_BASE
        };

        PipelineRuntime();

        void PipelineRuntimeInit(uint noc_w, uint noc_h, uint noc_bytes_per_cycle, AcceleratorManager::PipelineAccAllocStrategy acc_alloc_strategy, PipelineSpmAllocStrategy spm_alloc_strategy);

        void ScheduleRuntimeInit(const std::string profiling_filepath, uint batch_size_per_task, uint num_tasks, uint num_threads, float time_out_threshold, uint schedule_partition_num, uint model_type_num, const std::string& method, ScheduleAccAllocStrategy schedule_acc_alloc_strategy, float acc_increase_factor, float spm_mindis_init_factor, float spm_mindis_decrease_factor);

        // 硬件属性
        uint num_accels_, pages_per_bank_, page_bytes_size_;
        uint noc_h_, noc_w_, noc_bytes_per_cycle_;

        // mapping属性
        std::string method_;
        ScheduleAccAllocStrategy schedule_acc_alloc_strategy_;
        float spm_mindis_init_factor_, spm_mindis_decrease_factor_;

        // fetch/send/flush指令管理
        std::vector<uint> accelFetchCmdCount, accelSendCmdCount, accelFlushCmdCount; // 加速器fetch/send/flush指令计数
        std::vector<IntervalManager> spm_vpage_manager_; // 加速器spm页管理，哪些虚拟页区间被占用.[accid]->vpage manager
        const int PIPELINE_VPAGE_OFFSET = (1 << 17);
        
        // dram地址管理
        IntervalManager dram_addr_manager_; // dram地址管理。保留低位2G用于模型执行，剩余用于pipeline管理
        const long long EXT_DRAM_ADDR_OFFSET = (1 << 30);

        // SPM资源管理
        SPMManager spm_ppage_manager_;
        PipelineSpmAllocStrategy spm_alloc_strategy_;

        // 加速器资源管理
        AcceleratorManager accel_manager_;

        // profiling数据
        PipelineProfilingResult profiling_result_;

        // ---------------- Advanced QoS scheduler (new) ----------------
        bool enable_advanced_qos_ = false; // feature gate

        // scheduler knobs
        float sched_alpha_ = 0.5f;
        float sched_beta_wait_ = 1.2f;
        uint sched_k_cap_ = 32;
        uint64 sched_wait_end_time_ns_ = 50ULL * 1000ULL * 1000ULL; // 50ms default
        float sched_eps_ = 1e-6f;
        float sched_deadline_severe_factor_ = 2.0f;
        uint sched_min_split_batches_ = 10;
        float sched_omega_wait_ = 0.5f; // aging weight for anti-starvation

        // fragmentation hint
        float spm_fragmentation_hint_ = 0.0f;

        inline uint free_acc() const {
            return (num_accels_ > acc_occcupied_num_) ? (num_accels_ - acc_occcupied_num_) : 0;
        }
        inline uint free_spm_pages() const {
            uint total_pages = num_accels_ * pages_per_bank_;
            return (total_pages > spm_pages_occupied_num_) ? (total_pages - spm_pages_occupied_num_) : 0;
        }

        struct QoSBatch {
            uint model_idx = 0;
            uint subgraph_idx = 0;
            uint64 arrival_ts = 0;
            uint64 deadline_ts = 0;
        };

        struct QoSTask {
            uint task_id = 0;
            uint model_idx = 0;
            uint subgraph_idx = 0;
            uint batch_count = 1;
            uint64 earliest_arrival_ts = 0;
            uint64 deadline_ts = 0;
            uint64 dispatch_ts = 0;
            uint64 next_preempt_boundary_ts = 0;
            uint acc_target_idx = 0;
            uint spm_target_idx = 0;
            std::string mapping_path;
            uint64 time_pred_ns = 0;
            bool finalized = false;
            uint legacy_task_idx = UINT_MAX;
        };

        struct ModelQpsHist {
            uint64 window_ns = 1'000'000'000ULL; // 1s
            std::deque<uint64> arrivals_ts;
            double qps() const;
            void record(uint64 ts);
        };

        std::vector<ModelQpsHist> model_qps_hist_;

        std::deque<QoSTask> sched_pending_;
        std::deque<QoSTask> sched_ready_;
        std::deque<QoSTask> sched_running_;

        // qos实验
        uint batch_size_per_task_; // 每个任务的batch数。用于固定batch数的动态调度实验
        uint schedule_partition_num_; // 调度点数量
        uint qos_next_task_idx_; // 下一个分发任务的idx
        uint num_threads_;
        std::vector<uint64> qos_model_target_cycles_;  // 每个模型的截止时间 task_idx -> target
        std::vector<uint> qos_task_model_type_queue_;  // task idx -> model type idx
        std::vector<uint64> qos_task_dispatch_time_;  // task_idx -> dispatch time of each task 分发时间
        std::vector<uint64> qos_task_start_time_;  // task_idx -> start time of each task 开始调度时间
        std::vector<std::vector<uint64>> qos_task_subgraph_start_alloc_time_; // task_idx -> sub graph idx -> start alloc time of each task。开始资源分配时间
        std::vector<std::vector<uint64>> qos_task_subgraph_start_run_time_; // task_idx -> sub graph idx -> start run time of each task。开始执行时间（排除等待资源分配时间）
        std::vector<std::vector<uint64>> qos_task_subgraph_end_run_time_; // task_idx -> sub graph idx -> end run time of each task。sub-graph结束时间
        std::vector<std::vector<uint>> qos_task_subgraph_acc_num_; // task_idx -> sub graph idx -> acc num list of each subgraph 
        std::vector<std::vector<int>> qos_task_subgraph_slack_time_; // task_idx -> subgraph -> slack time of each task  每个子图执行前的slack时间
        std::vector<std::vector<uint64>> qos_task_subgraph_est_time_subgraph_;  // task_idx -> subgraph -> estimated time of each task 每个子图的预计执行时间
        std::vector<std::vector<uint64>> qos_task_subgraph_est_time_task_;  // task_idx -> subgraph -> remaining time of each task 每个子图的调度节点，后续整个人物预计剩余执行时间
        std::vector<uint64> qos_task_end_time_;  // task_idx -> end time of each task
        std::vector<uint> qos_thread_current_task_idx_;  // current task index of each thread

        std::vector<float> qos_thread_current_score_;  // task_idx -> score for computeRepartition
        std::vector<int64> qos_thread_nxt_schedule_time_; // task_idx -> next schedule time for computeRepartition

        // 运行时资源分配
        uint acc_occcupied_num_;
        std::vector<uint> thread_acc_occupied_num_; // [thread id] -> occupied acc num

        uint spm_pages_occupied_num_;
        std::vector<uint> thread_spm_occupied_num_; // [thread id] -> occupied spm num

        std::vector<uint> thread_acc_target_idx_; // [thread id] -> target acc num
        std::vector<uint> thread_acc_wait_target_idx_; // [thread id] -> wait acc idx
        std::vector<uint> thread_spm_pages_per_acc_kb_target_idx_; // [thread id] -> target spm ppage num

        std::vector<uint> thread_acc_alloc_timeout_;    // [thread id] -> alloc timeout time
        float time_out_threshold_; // alloc timeout threshold
        float acc_increase_factor_;

        // thread运行管理
        std::vector<uint> thread_current_model_type_;  // current model type of each thread
        std::vector<uint> thread_current_subgraph_idx_;  // current layer index of each thread

        // scheduler entry points (new)
        void SchedOnBatchArrive(const QoSBatch& b);
        void SchedFinalizePending(uint64 now);
        bool SchedBuildCandidates(const QoSTask& task,
                      std::vector<std::tuple<uint,uint,uint64,std::string>>& out,
                      uint batch_count);
        bool SchedPickBestFeasible(const QoSTask& task, uint64 now, QoSTask& chosen);
        bool SchedScheduleNextForThread(uint tid, uint64 now, QoSTask& assigned);
        bool SchedTryPreemptOrDrop(const QoSTask& task, uint64 now);
        uint64 PredictTimeNs(uint acc_idx, uint spm_idx, uint model_idx, uint subgraph_idx, uint batch);
        float ScoreCandidate(uint req_acc, uint req_spm_pages, uint64 pred_ns, uint64 deadline_ns, uint64 now, uint64 earliest_arrival_ts) const;
        uint ComputeWaitBatchMax(uint model_idx, uint subgraph_idx, uint64 earliest_arrival_ts, uint64 deadline_ts, uint64 now);
        void UpdateSpmFragmentationHint();

        uint GetMinimumAccIdx(const std::vector<uint>& accSet, const std::vector<uint> cmdCount);

        bool AllocAcc(std::shared_ptr<ScheduleAction> schedule_action);
        bool AllocSpm(std::shared_ptr<ScheduleAction> schedule_action);

        std::shared_ptr<ScheduleAction> GenerateAction(std::shared_ptr<SegmentMapping> segment_mapping, Model* model);
        void ReleaseAction(std::shared_ptr<ScheduleAction> schedule_action);

        std::shared_ptr<PipelineMapping> GetPipelineMapping(Model* model, const std::string& mapping_path);

        PipelineProfilingResult::Target GenerateTarget(uint target_acc, uint target_spm_per_acc_kb, uint model_idx);
        PipelineProfilingResult::Target GenerateTargetFromIdx(uint target_acc_idx, uint target_spm_per_acc_kb_idx, uint model_idx);

    private:
        bool AllocSpmAllBankPageInter(std::shared_ptr<ScheduleAction> schedule_action);
        bool AllocSpmMinDis(std::shared_ptr<ScheduleAction> schedule_action);
    };

    class ScheduleAction {
    public:
        ScheduleAction();
        uint action_id_; // 构造函数自动生成

        std::shared_ptr<SegmentMapping> segment_mapping_; // 手动赋值

        AccSource acc_source_; // Runtime中的方法生成
        SpmSource spm_source_; // Runtime中的方法生成

        Model* model; // 手动赋值

        nlohmann::json toJson() const;
        std::string toString() const;
    private:
        inline static uint max_action_id_ = 0;
    };

    class Layer {
    public:
        /**
         * Common Part
         */
        enum class LayerRuntimeType {
            SINGLE_LAYER,
            PIPELINE_LAYER,
        };
        LayerRuntimeType runtimeType;

        std::string type;
        std::vector<uint> paramList;
        std::vector<Tensor> tensorList;
        std::vector<Tensor> tensorList2; // for pipeline double buffer
        std::vector<TensorId> tensorIds;

        std::vector<std::vector<LayerMapping>> mappingTable;  // 仅非pipeline使用，[NUM_ACCEL_ALLOC, NUM_SPM_ALLOC] [num of acc idx(2^x), num of spm idx(the spm size allocated is stored in LayerMapping)]
        
        Layer(const std::string &type_ = "null", uint numTensors = 0, uint tid_ = 0);
        virtual ~Layer() = default;

        static uint getNxtGroupIdx();

        virtual void setParam(const std::vector<uint> &v) = 0;
        void setTid(uint tid_);

        void resetAllState();

        void setSingleMode();

        void setPipelineMode();

        virtual bool tick(uint batch = 1);

        nlohmann::json toJson() const;
        std::string toString() const;

        void loop(const std::vector<uint>& accId);

        virtual void loop(const std::vector<uint>& accId, bool isPipeline) = 0;

        /**
         * Single Layer Runtime
         */
        std::string lgType;  // none/head/inter/tail

        bool tickSingle(uint batch);

        bool lgTypeIsNone();

        bool lgTypeIsHead();

        bool lgTypeIsTail();

        bool lgTypeIsInter();
        struct PipeLayerMapperKey {
            std::vector<uint64> dramBypass;
            std::vector<uint64> spmBypass;
            uint64 accNum;

            bool operator==(const PipeLayerMapperKey& other) const;
            bool operator<(const PipeLayerMapperKey& rhs) const;
        };

        std::map<PipeLayerMapperKey, std::vector<LayerMapping>> pipeLayerMappingTable; // [dramBypass, spmBypass, accNum]->mapping列表(不同spm占用)

        LayerMapping* getLayerMapping(const PipeLayerMapperKey& key);

        bool tickPipeline();

        void resetStateForPipelineReuse();
        void setPipeLayerMapping(LayerMapping *mapping_);
        void setPipeLayerMappingForLazyFetch(LayerMapping *mapping_);
        void swapLayerMappingForLazyFetch();
        void setPipeAcc(const std::vector<uint> acc);
        void releasePipeAcc();

        uint getTensorPageCountUseIdx(uint tensorIdxInMapping);
        uint getTensorPageCountUseId(TensorId tensorId);
        uint getTensorIdxInLayer(uint tensorId);
        uint getTensorId(uint tensorIdxInLayer);

        void setPipeSpmForAcc(uint tensorIdxInMapping, const std::vector<uint>& spmPPageSet);
        void invalidSpmVPageForAcc(uint tensorIdxInMapping);
        bool getTensorPageMapValid(uint tensorIdxInMapping);

        virtual std::vector<uint64> getTensorShape(TensorId tensorId) = 0;
        virtual std::vector<uint64> getTensorStrides(TensorId tensorId) = 0;

        LayerMapping *mapping;

        LayerMapping *mappingForLazyFetch;
    protected:
        /**
         * Common
         */
        enum class SingleLayerRuntimeState {
            STATE_BEGINNING = 0,
            STATE_RESOURCE_REPARTITION = 10,
            STATE_ACC_ALLOC = 20,
            STATE_SPM_ALLOC = 30,
            STATE_BW_ALLOC = 40,
            STATE_SENDING_CMD = 90,
            STATE_JOINING = 100,
            STATE_RELEASE_RESOURCE = 150,
            STATE_ENDING = 200, 
        };
        SingleLayerRuntimeState singleLayerState;

        enum class PipelineLayerRuntimeState {
            STATE_BEGINNING,
            STATE_ACC_ALLOC,
            STATE_SPM_ALLOC,
            STATE_BW_ALLOC,
            STATE_SENDING_CMD,
            STATE_JOINING,
            STATE_RELEASING_ACC,
            STATE_ENDING,
        };
        void pipelineLayerStateChange(PipelineLayerRuntimeState state);

        uint tid;
        uint cmdCounter;

        // Single Layer
        uint64 spmWaitingThreshold;

        std::vector<bool> tensorPageTableReady; // [tensorIdxInLayer]

        std::vector<uint> pipeAccAllocSet; 
        bool pipeAccAllocated;
        PipelineLayerRuntimeState pipelineLayerState;

    private:
        inline static uint groupIdOffsetCounter = 1;
    };

    class Model {
    public:
        static const uint STATE_IDLE = 0;
        static const uint STATE_RUNNING = 10;

        std::string path;
        std::deque<Layer*> layers;
        std::vector<PipelineMapping> pipelineMapping;
        uint numLayers;
        uint64 addrLo, addrHi;

        uint state;

        Model();

        ~Model();

        void setTid(uint tid);

        nlohmann::json toJson() const;
        std::string toString() const;
    };

    class Thread {
    public:
        static const uint STATE_INIT = 0;
        static const uint STATE_RUN = 50;
        static const uint STATE_DONE = 100;

        uint tid;
        uint state;
        Model *model;
        Layer *layer;
        uint progress;

        Thread(uint tid_);

        bool isDone();

        virtual ~Thread() = default;

        virtual bool tick() = 0;
    };


    class StageMappingParser {
    public:
        class StageCandidate {
        public:
            uint globalStageId;

            std::vector<uint64> layerIdList;
            
            std::vector<uint64> tensorIdList;
            std::vector<uint64> entryTensorIdList;
            std::vector<uint64> exportTensorIdList;

            std::vector<std::vector<uint64>> dramBypassList;
            std::vector<std::vector<uint64>> spmBypassList;

            std::vector<std::string> entryTensorTypeList;
            std::vector<uint> entryTensorDoubleBufferList;
            std::vector<std::string> exportTensorTypeList;
            std::vector<uint> exportTensorDoubleBufferList;

            std::vector<uint64> fixTensorDramBypassIdList;
            std::vector<uint64> innerIsolateTensorId;
            std::vector<uint64> innerSharedTensorId;

            std::vector<std::vector<uint>> tensorUsageCountList;
            std::vector<std::vector<uint64>> tensorUseLazyFetch;
            
            std::vector<std::vector<uint>> vAccelIdxList;
            std::vector<std::vector<uint>> pAccelIdxList;
            uint accUtil;
            std::unordered_map<TensorId, uint> tensorIdToSpmPageNums;

            void load(YAML::Node &node);
        };

        std::vector<StageCandidate> candidates;

        static StageMapping stageCandidateToStageMapping(Model* model, const StageCandidate& candidates);
    };
    
    // Segment Mapping Parser
    class SegmentMappingParser {
    public:
        class SegmentCandidate {
        public:
            uint segmentIdx;
            uint startLayerIdx;
            uint endLayerIdx;
            
            // ring buffer config
            std::unordered_map<TensorId, SegmentMapping::RingBufferConfig> ringBufferConfig;
            std::unordered_map<TensorId, uint> ring_buffer_count;
            std::unordered_map<TensorId, uint> ring_buffer_size_per;
            std::unordered_map<TensorId, uint> ring_buffer_use_count;
            
            // tensor info
            std::vector<std::unordered_map<TensorId, uint64>> tensorSpmUtilInStage;
            std::unordered_map<TensorId, uint64> tensorSpmUtilShared;
            std::unordered_map<TensorId, uint64> tensorSpmUtilInRingbuffer;
            std::unordered_map<TensorId, uint64> tensorSpmUtilWeight;
            
            uint64 totalSpmUtil;
            uint accUtil;
            uint64 cost;
            uint subBatchSize;
            
            std::vector<StageMappingParser::StageCandidate> stageCandidates;
            
            void load(YAML::Node &node);
        };

        std::vector<SegmentCandidate> candidates;

        static SegmentMapping segmentCandidateToSegmentMapping(Model* model, const SegmentCandidate& candidate);
    };
    
    // Pipeline Mapping Parser
    class PipelineMappingParser {
    public:
        class TargetConfig {
        public:
            uint numArrays;
            uint spmKB;
            uint spmKBPerArray;
            uint numMacsPerArray;
            double dramBwPerCycle;
            uint nocBwPerCycle;
            uint defaultBatch;
            
            void load(const YAML::Node &node);
        };
        
        TargetConfig target;
        std::vector<SegmentMappingParser::SegmentCandidate> segments;
        uint64 pipelineCost;
        
        void load(const std::string &path);
        static PipelineMapping pipelineCandidateToPipelineMapping(Model* model, const std::string& path);
    };

} // mudnac


#endif //MUDNACSIM_RUNTIME_H
