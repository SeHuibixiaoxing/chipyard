#ifndef MUDNACSIM_PIPELINE_H
#define MUDNACSIM_PIPELINE_H

#include "pipeline_stage.h"

namespace mudnac {
    /**
     * @brief 控制一个Pipeline的执行
     * @details 流水线控制多个流水线阶段。负责流水线张量（各阶段入口张量、出口张量）的读写，以及相关完成信号的通告
     * 
     * 1. tick执行逻辑
     * （1）STATE_CONFIG：资源配置初始化。配置Model、pipeline mapping，接受被分配的物理acc、pSpmPage，用于flush/send/fetch指令收发的加速器pipeAcc
     * （2）STATE_STAGE_CONFIG：配置Stage。根据Stage需求，完成相关网络层、mapping、acc、pSpmPage配置。
     * （3）STATE_WAIT_PIPEBUFFER：tick所有stage，等待pipe buffer分配完成
     * （4）STATE_PIPEBUFFER_CONFIG：
     * - 获取第一个Stage的入口张量（入口pipe buffer）和最后一个Stage的出口张量（出口pipe buffer）
     * - 为SHARED类型入口pipe buffer分配spm page
     * - 配置stage的pipe buffer，包括shared入口张量物理页分配（与同一张量作为之前阶段的出口张量共享被分配的物理页面，从该出口张量获取物理页）
     * - 对于ISOLATE、SHARED类型，创建pipe buffer pair（一对一），将具有相同tensor id的pipe buffer映射在一起，约束：它们应该有相同的访存类型，但可能有不同的buffer数量
     * - 对于ISOLATE、SHARED SPM类型入张量，同时为其分配目的spm物理页面
     * （5）STATE_STAGE_EXECUTE：开始执行。
     * 
     * 收集fetch/flush/send指令
     * - 根据tensorid，更新指令计数
     * 
     * 入口pipebuffer的batch偏移表示正在准备处理的batch offset + subbatch size
     * 出口pipebuffer的batch偏移表示处理完的batch offset
     * 
     * （注：确保入口DRAM没有入度，出口DRAM没有出度）
     * 处理入口DRAM类型pipe buffer，检查inuse状态：
     * - 如果是empty，等待有足够batch后，发送fetch，更新指令计数，设置为fetching状态
     * - 如果是full，则跳过，等待使用完成
     * - 如果是fetching，则检查指令计数，是否完成。没完成不做处理。完成了，set full data标记， 切换到下一缓冲区，更新该buffer执行的batch数，还需要更新pipeline的batch数
     * 
     * 处理出口DRAM类型的pipe buffer，检查inuse状态：
     * - 如果是empty，则跳过，等待数据填充完成
     * - 如果是full，发送flush指令，更新指令计数，设置为flushing状态
     * - 如果是flushing，则检查指令计数，是否完成。没完成不做处理。完成了，clear full data标记，切换到下一缓冲区。更新该buffer执行的batch数，还需要更新pipeline的batch数
     * 
     * =====非环形buffer处理=====
     * （DRAM_DEPEN类型与DRAM类型差异仅在于是否等待依赖张量，可以归为一类处理）
     * 处理入口DRAM_DEPEN类型pipe buffer，检查inuse状态：
     * - 如果是empty，检查前序pipebuffer，是否少一个或两个sub batch。如果少，则发起fetch命令，设置为fetching状态，更新该buffer执行的batch数
     * - 如果是full，则跳过，等待使用完成
     * - 如果是fetching，则检查指令计数，是否完成。没完成不做处理。完成了，set full data标记， 切换到下一缓冲区
     * 
     * 处理出口DRAM_DEPEN类型pipe buffer，检查inuse状态：
     * - 如果是empty，则跳过，等待数据填充完成
     * - 如果是full，检查所有对应的入口tensor（必须是DRAM_DEPEN类型），inuse不相等、或inuse相等但处于（入口为full）或（入口empty且batch offset相同），发送flush指令，设置为flushing状态
     * - 如果是flushing，则检查指令计数，是否完成。没完成不做处理。完成了，clear full data标记，切换到下一缓冲区。更新该buffer执行的batch数
     * 
     * 遍历每一对ISOLATE pipe buffer pair（一对一）：
     * - 如果出口in process和入口in process都处于sending状态，则检查指令计数，为0则更新出口inprocess状态为empty而
     * 入口inprocess状态为full
     * - 如果出口inprocess full且入口inprocess empty，则发送send指令，并设置二者状态为sending
     * - 其余状态不做处理
     * 
     * 遍历每一对SHARED pipe buffer pair：
     * - 同步两侧buffer状态即可。两款块buffer始终一个在写入、一个在读出。当出口inproces full且入口inprocess empty时，设置出口
     * inproces empty，入口inprocess full，并切换到下一个inprocess(singbuffer切换后，是同一个buffer)
     * 
     * 转移：empty,empty ->(前一个stage执行完) full, empty ->(pipeline通知) full, full ->(后一个stage执行完) full, empty->(pipeline通知)empty, empty...
     * 关键：前一个状态是empty, empty还是full, full
     * 
     * =====环形buffer处理=====
     * 设ring buffer数量为k（直接在mapping中指定）。每个环形buffer都拥有自己的head和tail，[head, tail)表示full data的数据的subbatch区间
     * 多个pipebuffer共享同一个ringbuffer，且必须有一个入口、一个或多个出口，这些pipebuffer的类型是一样的。每个ring buffer的每个subbatch记录一个count，表示被读的次数，为0时，head+1
     * 每个pipbuffer维护一个subBatchoffset：
     * * 对于入口，表示下一个将要读的subbatch的起始offset，[0, offset）已经读入；
     * * 对于出口，表示已经send出去的subbatch数量，[0, offset）已经发送并完成发送
     *
     * 处理入口DRAM_DEPEN类型pipe buffer，检查inuse状态:
     * - 如果是empty，检查ring buffer。若head <= offset < tail。则向环形buffer的offset位置发起fetch命令，设置为fetching状态，更新该buffer执行的batch数
     * - 如果是full，则跳过，等待使用完成
     * - 如果是fetching，则检查指令计数，是否完成。没完成不做处理。完成了，set full data标记，并将offset + 1。更新ring buffer的subbatch count。head为0，则head += 1，直到不为0
     * 
     * 处理出口DRAM_DEPEN类型pipe buffer，检查inuse状态：
     * - 如果是empty，则跳过，等待数据填充完成
     * - 如果是full，检查ring buffer, 如果tail - head < k, 向tail发送flush指令，设置为flushing状态
     * - 如果是flushing，则检查指令计数，是否完成。没完成不做处理。完成了，clear full data标记，切换到下一缓冲区。更新该buffer执行的batch数，tail增加1
     * 
     * 
     * 遍历入口ISOLATE pipe buffer
     * - 如果是empty，检查ring buffer。若head <= offset < tail。则从环形buffer的offset位置发起send命令，并将offset + 1，设置为fetching状态，更新该buffer执行的batch数
     * - 如果是full，则跳过，等待使用完成
     * - 如果是sending，则检查指令计数，是否完成。没完成不做处理。完成了，set full data标记。增加ring buffer在该(offset - 1)下被使用的次数。更新ring buffer head，当head的被使用次数达到其出度时，head + 1
     * 
     * 遍历出口ISOLATE pipe buffer：
     * - 如果是empty，则跳过，等待数据填充完成
     * - 如果是full，检查ring buffer, 如果tail - head < k, 向tail发送send指令，设置为sending状态
     * - 如果是sending，则检查指令计数，是否完成。没完成不做处理。完成了，clear full data标记，切换到下一缓冲区。更新该buffer执行的batch数，tail增加1
     * TODO: 针对入口张量和出口张量大小不同的情况，在ring buffer创建时，在ring buffer中记录shape、strides、spmPPages，选择两边的最小值。发送时，按照小的那个进行发送。
     * 
     * 
     * SHARED pipe buffer：【暂不支持 TODO:支持SHARED模式】
     * 
     * =======================
     * 
     * 对各阶段tick（）
     * - 如果readyToEnd=true，且没有在处理中的batch（batchProcessing=0），对第一个阶段设置continueRun=false，如果其tick返回true，则接着对下一个阶段设置
     * continueRunfalse，直到全部阶段返回true，跳转到STATE_END状态
     * 
     */
    class Pipeline {
    public:   
        enum PipelineState {
            STATE_BEGIN,
            STATE_CONFIG,
            STATE_PIPELINE_BUFFER_CONFIG,
            STATE_STAGE_CONFIG,
            STATE_STAGE_EXECUTE,
            STATE_END,
        };

        Pipeline(uint pipeLogIdx_, std::shared_ptr<ScheduleAction> action):  pipeLogIdx(pipeLogIdx_) {
            resetAllState();
            action_ = action;
        }
        
        void resetAllState() {
            action_ = NULL;
            
            maxBatchOffset = 0;

            state = PipelineState::STATE_BEGIN;
            stages.clear();
            lastDisableStageIdx = -1;

            watingCmd.clear();
        }


        // 输入供给
        void addBatch(uint batch);

        // getter
        uint getNoProcessBatch();
        uint getBatchProcessing();
        uint getTotalBatchProcessed();
        uint getSubBatchSize();
        uint getMaxBatchOffset();

        std::string getPerformance();

        nlohmann::json pipebufferToJson() const;
        std::string pipebufferToString() const;

        nlohmann::json pipeBufferIndexToJson(bool with_pipebuffer) const;
        std::string pipebufferIndexToString(bool with_pipebuffer) const;

        nlohmann::json ringBufferToJson() const;
        std::string ringBufferToString() const;

        bool tick();

        // subbatch计时
        uint pipelineStartTime; // pipeline启动时间
        uint finishConfigTime; // 结束pipeline config时间
        uint finishStageConfigTime; // 结束所有stage config时间
        uint finishPipebufferCreateTime; // stage创建完成pipebuffer时间
        uint finishPipebufferConfigTime; // pipebuffer配置完成时间
        std::vector<uint64> startTime, endTime, duration;

    private:        
        // mapping
        std::shared_ptr<ScheduleAction> action_ = NULL;

        // batch
        uint maxBatchOffset;

        // acc
        std::vector<std::vector<uint>> pacc_ids_each_stage_; // stageidx -> vec[pacc]

        // spm
        std::vector<std::unordered_map<TensorId, std::vector<uint>>> ppages_each_stage_each_tensor_; // stageidx -> map[tensorId, vec[ppages]]
        std::vector<std::unordered_map<TensorId, std::vector<std::vector<uint>>>> ppages_each_stage_ring_buffer_; // stageidx -> map[tensorId, vec[vec[ppages] for each ring buffer]]
        
        // Pipeline Buffer
        std::vector<std::unordered_map<TensorId, PipelineBuffer>> pipebuffer_set; // stage_idx -> tensor_id -> pipeline buffer

        // pipeline buffer索引index
        std::unordered_map<TensorId, std::unordered_map<uint, std::pair<PipelineBuffer*, uint>>> tensorId2EntryDramOrDramDepenPipeBuffer; // [tensorid, stageIdx]->[pipeBuffer, nowIdx]
        std::unordered_map<TensorId, std::unordered_map<uint, std::pair<PipelineBuffer*, uint>>> tensorId2ExportDramOrDramDepenPipeBuffer; // [tensorid, stageId]->[pipeBuffer, nowIdx]
        std::unordered_map<TensorId, std::unordered_map<uint, std::pair<PipelineBuffer*, uint>>> tensorId2EntryIsolatePipeBufferWithRingbuffer; // [tensorid, stageId]->[pipeBuffer, nowIdx]
        std::unordered_map<TensorId, std::unordered_map<uint, std::pair<PipelineBuffer*, uint>>> tensorId2ExportIsolatePipeBufferWithRingbuffer; // [tensorid, stageId]->[pipeBuffer, nowIdx]

        std::unordered_map<TensorId, std::unordered_map<uint, std::tuple<PipelineBuffer*, PipelineBuffer*, uint, uint>>> tensorId2IsolatePipeBufferNoRingbuffer; // [出口tensorId]（一个tensorId只能在一个stage作为出口）->[nxtStageIdx,指向的入口tensorId所在的stage，可能有多个]->[出口preExportPipeBuffer, 入口nxtEntryPipeBuffer, preBufferIdx, nxtBufferIdx] 其中nxt/preBufferIdx为出口正在/等待处理的bufferIdx 
        std::unordered_map<TensorId, std::unordered_map<uint, std::unordered_map<uint, std::tuple<PipelineBuffer*, PipelineBuffer*, bool>>>> tensorId2SharedPipeBuffer; // 多一个buffer idx作为key，[tensor_id][buffer_idx][stage_idx]->value,value上多一个bool记录前一个状态是empty, empty(false)还是full, full(true)
        
        std::unordered_map<TensorId, std::unordered_map<uint, std::pair<PipelineBuffer*, bool>>> tensorId2EntryAllRingbufferPipebuffer; // [tensorid, stageid]->[pipeBuffer, tag]
        std::unordered_map<TensorId, std::unordered_map<uint, std::pair<PipelineBuffer*, bool>>> tensorId2ExportAllRingbufferPipebuffer; // [tensorid, stageid]->[pipeBuffer, tag]

        // ring buffer
        std::unordered_map<TensorId, RingBuffer> ring_buffer_; // [tensorId] -> [ringBuffer]

        // 尚未回收的指令,[accId, globalCmdId]
        std::list<std::pair<uint, uint64>> watingCmd;

        // state
        PipelineState state;
        uint lastDisableStageIdx; // 上一个返回true的流水线阶段

        // stage
        std::vector<Stage> stages;

        // debug lpg
        uint pipeLogIdx; // =schedule action id

        // pipebuffer创建
        void createPipebuffer();

        // ringbuffer创建
        void createRingBuffer();

        // stage创建
        void createStages();

        // pipebuffer监控
        void processEntryDramPipebuffer(TensorId tensor_id, uint stage_idx, PipelineBuffer* pipe_buffer, uint& buffer_idx);
        void processExportDramPipebuffer(TensorId tensor_id, uint stage_idx, PipelineBuffer* pipe_buffer, uint& buffer_idx);
        void processIsolateSpmPipebufferWithoutRingbuffer(TensorId tensor_id, PipelineBuffer* pre_pipe_buffer, uint& pre_buffer_idx, uint nxt_stage_idx, PipelineBuffer* nxt_pipe_buffer, uint& nxt_buffer_idx);
        void processIsolateSpmPipebufferWithoutRingbufferSyn(TensorId tensor_id, std::unordered_map<uint, std::tuple<PipelineBuffer*, PipelineBuffer*, uint, uint>>& p);
        void processSharedSpmPipebufferWithoutRingbuffer(TensorId tensor_id, PipelineBuffer* pre_pipe_buffer, uint buffer_idx, bool pre_tag);
        void processEntryIsolateSpmPipebufferWithRingbuffer(TensorId tensor_id, uint stage_idx, PipelineBuffer* pipe_buffer, uint& buffer_idx);
        void processExportIsolateSpmPipebufferWithRingbuffer(TensorId tensor_id, uint stage_idx, PipelineBuffer* pipe_buffer, uint& buffer_idx);

        void processEntryAllRingbuffer(TensorId tensor_id, uint stage_idx, PipelineBuffer* pipe_buffer, bool& tag); // 入口，全部直接从ring buffer读。
        void processExportAllRingbuffer(TensorId tensor_id, uint stage_idx, PipelineBuffer* pipe_buffer, bool& tag); // 出口，全部直接写到ring buffer。

        // state
        void stateChange(PipelineState state_);

        // cmd
        void sendFetchCmd(uint accId, uint tensorId, uint stageIdx, uint64 dramBaseAddr, const std::vector<uint64>& strides, const std::vector<uint64>& shape, uint vPageBase);
        void sendFlushCmd(uint accId, uint tensorId, uint stageIdx, uint64 dramBaseAddr, const std::vector<uint64>& strides, const std::vector<uint64>& shape, uint vPageBase);
        void sendSendCmd(uint accId, uint tensorId, uint stageIdx, const std::vector<uint64>& strides, const std::vector<uint64>& shape, uint srcVPageBase, uint dstVPageBase);
        void waitCmd();

    };
} // mudnac

#endif // MUDNACSIM_PIPELINE_H

