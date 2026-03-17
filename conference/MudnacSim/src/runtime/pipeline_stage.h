#ifndef MUDNACSIM_PIPELINE_STAGE_H
#define MUDNACSIM_PIPELINE_STAGE_H

#include "pipeline_buffer.h"

namespace mudnac {
    /**
     * @brief 控制流水线一个阶段的执行。
     * @details 流水线阶段包含一个或多个层，这些层按照mapping中给定的顺序执行。当下一层的数据准备好且被分配的加速器
     * 核心空闲时，开始执行下一层，直到所有层执行结束，切换到下一个sub-batch。
     * 
     * 
     * 1. Stage的SPM Tensor管理：
     * 按照spm buffer管理，分为  入口/出口张量、固定张量、内部共享张量、内部独立张量
     * 
     * 固定张量（无依赖张量）：在Stage执行过程中始终不变的张量（如卷积层的权重、矩阵乘法的偏置等），这些张量在计算图中
     * 不依赖任何节点，仅在Stage第一次执行时从DRAM中读取一次，之后不再进行读取（要求层映射中该张量dram bypass=1, 
     * spm bypass=0）。需要手动在stage执行前，将数据通过fetch指令读入。
     * 固定张量也可以不提前读取，作为普通张量（dram bypass=0, spmbypass=0）处理。目前实现为LazyFetch模式，在mapping中设置。
     * 
     * 内部独立张量：作为普通张量处理。这些张量有两类来源：
     * （1）输入或输出张量，由于片上空间有限，层执行完后不在片上驻留
     * （2）类似固定张量，来自不会变化的权重，但dram bypass=1，无需提前读取，作为普通张量读取。
     * 处理方式：dram bypass=0, spm bypass=1时，直接从dram读取；
     * dram bypass=0, spm bypass=0时，每次从dram读一个tile，加速器从spm读该tile。
     * 
     * 内部共享张量：在同一个阶段内的层间共享存储的张量。执行时第一次需要（作为输出），进行分配；如果仍有后续层依赖该张量，
     * 则继续保留为其分配的内存空间，直到后续层不再需该张量时释放
     * 
     * 入口张量、出口张量：
     * 阶段入口和出口张量。其依赖层一端在阶段内，一端在阶段外。在Mapping中定义了Tensor的分配和访存方式。
     * 从spm分配上来看，有两种类型：single buffer和double buffer。sing buffer只有一块存储空间缓存数据，stage的计算和stage间
     * 访存不能重叠；double buffer有2块存储空间（inuse和nouse），stage计算和stage间访存能够重叠。
     * 
     * 从访存方式来看，有四种来源：DRAM（在SPM上有缓冲，但需要从DRAM读写，且没有依赖）、DRAM_DEPEN（在SPM上有缓冲，但需要从DRAM读写，有依赖）、ISOLATE_SPM（通过其它Stage获取，但在SPM上不共享）和SHARED_SPM（与其它
     * Stage共享），
     * 对于single-buffer张量，不可以是SHARED_SPM类型。因为如果是SHARED_SPM,两个Stage会进入，串行状态。此时二者应该被放在一个Stage中，不应该被划分为两个Stage。
     * 对于single-buffer的出口张量，不可以是DRAM类型，因为如果是DRAM，应该直接写到DRAM中。
     * 并且，写回不需要额外的指令。因此，其full data标记始终都是false。层执行结束后的任何时候都可以写回。】
     * 
     * 对于double-buffer张量，三种类型都可以。
     * 
     * 出口张量所需SPM全部由Stage管理（包括pipeline buffer），由Stage完成其物理页分配、数据写入，提供包括tensor物理页获取接口
     * 在内的若干接口，供Pipeline调用，以及时将信息通告给后续Stage。
     * 入口张量DRAM类型和ISOLATE_SPM类型由Stage分配物理页，SHARED_SPM类型由上层pipeline为其设置物理页
     * 
     * 约束：
     * 对于流水线的起始阶段，它们的入口Tensor均来自DRAM；对于流水线的结束阶段，它们的出口Tensor均写回到DRAM。对于其它
     * 流水线阶段，每个阶段至少有一个入口张量来自ISOLATE_SPM或SHARED_SPM。
     * 
     * 内部张量：没有Pipeline Buffer的张量。使用时分配，使用后释放。
     * 
     * 
     * 2. 张量地址空间管理：
     * Pipeline为每个Stage分配其所需的物理页，Stage管理这些物理页。
     * 目前实现两个版本：
     * FREE：哪个能用，用哪个
     * BANK_AWARE：优先分配平均距离近的物理页
     * Stage记录了为每个不同的tensor id分配了哪些物理页。根据Layer需要，将这些物理页传递给Layer，由Layer将其分配
     * 到各个张量。Layer执行结束后，对分配的物理页进行回收。
     *  
     * 
     * 3. tick执行过程
     * Stage执行包括以下阶段：
     * （1）STATE_CONFIG: 资源配置和初始化。配置网络层，配置mapping。确定接收到来自Pipeline的acc分配、pSpmPage分配。
     * （2）STATE_SPM_ALLOC: 完成pipline buffer创建，完成初始化阶段需要进行的物理页分配，之后不再改变该部分物理页分配。
     * 该阶段需要物理页分配的张量：
     * - DRAM类型、ISOLATE_SPM类型入口张量
     * - 全部出口张量
     * - 固定张量
     * 该阶段还需要确定src/dst dram地址的张量：
     * - 全部类型张量
     * 如果不够分配，会报错assert。需要上层保证分配spm足够。
     * pipeline buffer创建完毕后，置位pipeBufferCreateReady
     * （3）STATE_SPM_ALLOC_WAIT:等待上层分配Shared入口张量物理页分配、非DRAM类型出口张量目的地址分配【TODO：目的地址可能可以删除，不需要】，进入下一步。
     * （4）STATE_FETHCH_FIX_TENSOR:发送指令，进行固定张量读入
     * （5）STATE_WAIT_FETHCH：等待读入完成。读入完成后，为Stage设置stage ready标记，表示有输入后就能执行。
     * （6）开始流水线执行。
     * pipeline执行逻辑：
     * (.1) STATE_PIPELINE_WAITING：
     * 当出口buffer数据发送完（所有出口张量的两个buffer都空闲且可用）、入口buffer数据消耗完（所有入口张量的两个buffer都计算完），如果continueRun=false,不再执行，进入ENDING状态
     * 等待inuse入口buffer准备好（inuseready=true)，等待inuse出口buffer空闲
     * (.2) STATE_LAYER_EXECUTE: 按顺序执行层。
     * 按顺检查每个层：
     * - 该层执行过，或者执行完，跳过
     * - 加速器被占用，跳过
     * - 依赖的input TensorId（inner shared或inner isolate类型）还没有作为output计算过，跳过。
     * - 所需物理页计算。包括全部的inner isolate类型、所有输出的inner shared类型。【TODO】 POOL层特殊处理：无需加速器，输出tensor可以直接复用输入tensor的page
     * - 如果剩余物理页不足，跳过
     * 拿到即将执行的层
     * 标记为获取并占用加速器资源
     * 标记并获取spm资源，为其分配物理页。
     * - 为所有inner isolate类型分配物理页
     * - 对于输入的inner shared类型，查询其剩余使用次数记录，并-1
     * - 对于输出的inner shared类型，为其创建剩余使用次数记录
     * - 为所有tensor设置页表
     * tick所有执行中的层。
     * tick返回true的层：
     * - 执行结束，重置层状态，重置层页表
     * - 释放加速器资源
     * - 释放inner isolate类型tensor资源
     * - 更新输入inner shared类型tensor资源的计数。如果为0，则释放该部分物理页
     * - full data置位
     * 所有层执行结束，进入下一个阶段
     * 
     * (.3) STATE_WAIT_NEXT_ITER: 
     * 切换入口张量的inuse和nouse；
     * 切换出口张量的inuse和nouse。
     * 标记入口tensor fulldata=false
     * 回到STATE_PIPELINE_WAITING状态
     * 
     * (.4) STATE_ENDING: 始终返回true。此时，认为Stage执行结束。
     * 
     * 
     * 【TODO】
     * 1. 下一个stage必须在上一个stage完成后，才能开始读入口buffer，以避免某些DRAM类型的buffer数据计算结束前就提前完成读取
     * 2. 允许出口buffer，同时写回DRAM和发送SEND指令，需要对于ISOLATE_SPM_DRAM和SHARED_SPM_DRAM类型添加标记also dram，同时发送send和flush指令并等待
     * 3. 多层在同一stage时，可能导致同一tensor的shape strides不同引发buffer错误。需要重新梳理Stage和Pipeline执行过程，针对其中shape strides不同的地方，重新做处理，可能需要重建索引等工作
     */
    

    struct StageConfig {
        Model* model_;
        const StageMapping* stage_mapping_;
        uint subbatch_size_;
        std::vector<uint> acc_;
        std::unordered_map<TensorId, std::vector<PageSetShared>> tensor_spm_ppages_;
        std::vector<PipelineBuffer*> pipeline_buffers_;
    };

    class Stage {
    public:
        Stage(uint stageLogIdx_, const StageConfig& stage_config);

        // stage终止
        void disableRun();

        // 辅助函数
        bool isEntryTensorInStage(TensorId tensorId);
        bool isExportTensorInStage(TensorId tensorId);
        bool isFixedTensorInStage(TensorId tensorId);
        bool isInnerIsolateTensorInStage(TensorId tensorId);
        bool isInnerSharedTensorInStage(TensorId tensorId);

        uint getBatchProcessing();
        uint getBatchProcessed();

        bool tick();

        // 计时
        uint64 launchTime; // stage启动的时间戳
        uint64 finishConfigTime; // 结束配置和资源分配的时间戳
        uint64 finishFixTensorFetchTime; // 结束固定张量fetch时间戳
        std::vector<uint64> startTime, endTime; // [subBatch]->time 开始执行/结束执行时间戳
        std::vector<uint64> preWait, sucWait, duration; // 执行前等待时间，执行后等待时间，执行时间

        bool getContinueRun();
        void generatePerformance();
        std::string getPerformance();


    protected:
        // 状态控制
        enum StageState {
            STATE_BEGINING,
            STATE_CONFIG,
            STATE_FETHCH_FIX_TENSOR,
            STATE_WAIT_FETCH,
            STATE_PIPELINE_WAITING,
            STATE_LAYER_EXECUTE,
            STATE_PIPELINE_LOOP,
            STATE_ENDING
        };

        void resetALLStage();

        // Stage初始化配置
        void initStage(Model* model, const StageMapping* stageMapping_, uint subBatchSize_, const std::vector<uint>& acc, const std::unordered_map<TensorId, std::vector<PageSetShared>>& tensor_spm_ppages);
        // Pipeline buffer配置
        void configPipelineBuffer(PipelineBuffer* pipeline_buffer);

        // set方法，在init中调用
        void setLayers(const Model* model);
        void setPipeAcc(const std::vector<uint>& acc);

        void stateChange(StageState state_);

        // acc页表配置
        // accIdx: Stage中的虚拟acc编号，物理acc编号为accAllocSet[accIdx]
        void setAccSpmPage(uint accIdx, uint vPage, uint pPage);
        void setAllAccAllocatedSpmPage(uint vPage, uint pPage);
        void invalidAccSpmPage(uint accIdx, uint vPage);
        void invalidAllAccAllocatedSpmPage(uint vPage);

        // 向加速器发送指令，fetch张量
        void sendFetchCmd(uint accId, TensorId tensorId, uint vPageBase);
        void waitFetchCmd(); // 等待fetch指令结束，并释放页表

        const StageMapping* stageMapping;
        std::vector<Layer*> layers;
        std::vector<uint> accPool;
        std::unordered_map<TensorId, PipelineBuffer*> entryTensorPipelineBuffer;
        std::unordered_map<TensorId, PipelineBuffer*> exportTensorPipelineBuffer;
        std::unordered_map<TensorId, std::vector<PageSetShared>> tensor_spm_ppages_; //  tensorid -> ppages
        bool stage_initialized;

        std::vector<uint> layerRunCount; // 层执行次数

        // layer执行状况
        std::vector<uint> layerDone;   // 是否完成执行
        std::vector<uint> layerRunning; // 层是否执行中

        // 虚拟acc使用情况
        std::vector<uint> accIdxInUse;

        StageState state;

        // 每次执行的subBatchSize
        uint subBatchSize;

        // 当流水线开始运行时，continueRun自动设为true。流水线会一直等待足够的batch、
        // 完成的input buffer和空闲的output buffer执行流水。当conitnueRun被设置为false
        // 且有效入口张量input执行结束、有效出口buffer写出后，流水线结束，tick返回false。
        bool continueRun;

        // 记录inner张量计算和被使用情况。如果存在，说明已经算完，不存在则未算完；数值>1说明还需要被这个数量的层使用。数值=0说明可以被删除，不会被使用
        std::unordered_map<TensorId, uint> innerTensorUsage;

        // fetch cmd管理
        std::list<std::tuple<TensorId, uint, uint64>> fetchCmdWaiting; // 尚未返回的fetch cmd对应[tensor id, acc id, global id]
        std::unordered_map<TensorId, std::pair<uint, uint>> vPageRangeFetchCmd; // 被用于fetch cmd的vpage. global id->range

        // 是否生成计时
        bool hasGeneratePerf;

        // batch计数
        uint batchProcessing; // 计算中
        uint batchProcessed; // 计算完成

        uint stageLogIdx;
        
    };
}


#endif