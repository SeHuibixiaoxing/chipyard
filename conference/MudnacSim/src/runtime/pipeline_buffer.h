#ifndef MUDNACSIM_PIPELINE_BUFFER_H
#define MUDNACSIM_PIPELINE_BUFFER_H

#include "runtime.h"

namespace mudnac {
    class RingBuffer {
    public:
        RingBuffer(int size, int out_degree_, const std::vector<uint64>& strides, const std::vector<uint64>& shape, 
                    const std::vector<PageSetShared>& p_spm_pages);

        RingBuffer(int size, int out_degree_, const std::vector<uint64>& strides, const std::vector<uint64>& shape, 
                    const std::vector<std::pair<uint, uint>>& dram_addr_range);

        void setSpmPages(const std::vector<PageSetShared>& p_spm_pages);
        void setDramBaseAddr(const std::vector<std::pair<uint, uint>>& dram_addr_range);

        bool hasIdleBuffer();
        bool hasReadyBuffer(uint subBatchOffset);

        bool isTailOffset(uint subBatchOffset);

        PageSetShared getPSpmPages(uint sub_batch_offset);
        std::pair<uint, uint> getDramAddrRange(uint sub_batch_offset);
        uint getDramBaseAddr(uint sub_batch_offset);

        std::vector<uint64> getStrides();
        std::vector<uint64> getShape();

        void use(uint sub_batch_offset);
        void fill(uint sub_batch_offset);
        
        nlohmann::json toJson() const;
        std::string toString() const;
    private:
        int size_, head_, tail_;
        std::unordered_map<uint, uint> use_count_; // 被使用次数，subbatch->count
        int out_degree_; // 出度

        // shape和strides取其对应入口、出口张量的最小或最大值. all ring buffer类型是最大值，其余类型是最小值
        std::vector<uint64> strides_;
        std::vector<uint64> shape_;

        std::vector<PageSetShared> p_spm_pages_; // 每个ring buffer的spm物理页号
        std::vector<std::pair<uint, uint>> dram_addr_range_; // dram地址.[start, end]
    };

    class PipelineBuffer {
    public:
        PipelineBuffer() = delete;
        PipelineBuffer(TensorId tensorId_, StageMapping::TensorStayType tensorStayType_, bool withDoubleBuffer_, const std::vector<uint64>& strides_, const std::vector<uint64>& shape_, uint stage_idx);
        
        // spm物理页获取
        PageSetShared getPSpmPages(uint idx);
        PageSetShared getPSpmPagesInUse();

        // spm分配情况
        bool noAllocatedSpmPage() const;

        // dst赋值情况
        bool noAllocatedExportDstDram() const;
        bool noAllocatedExportDst() const;
        // spm 完成情况
        bool bufferFull(uint bufferIdx) const;
        bool inUseBufferFull() const;
        bool noUseBufferFull() const;
        bool allBufferFull() const;
        bool allBufferEmpty() const;
        bool ringBufferHasCreated() const;
    
        nlohmann::json toJson() const;
        std::string toString() const;


        // 基础属性
        TensorId tensorId;
        uint stage_idx_;
        StageMapping::TensorStayType tensorStayType;
        std::vector<uint64> strides;
        std::vector<uint64> shape;

        // ring buffer
        RingBuffer* ring_buffer_; // 共享ring buffer结构
        bool use_ring_buffer_;
        std::array<std::pair<uint, uint>, 2> ringBufferCmdVPageRange; // 记录运行中指令的ring buffer相关vpage，左闭由闭；对于入口，为src vpage；对于出口，为dst vpage

        // pipeline buffer 存储空间
        std::array<PageSetShared, 2> pSpmPages;

        // pipeline buffer指令监控和地址生成
        std::array<bool, 2> bufferHasFullData;
        std::array<bool, 2> cmdRunning; // 是否有flush/send/fetch指令执行中；对于send指令，由send接收方记录
        std::array<uint, 2> cmdAcc;  // 记录运行中的指令，在哪个物理加速器上，pAccId；对于send指令，双方记录同一个acc
        std::array<std::pair<uint, uint>, 2> cmdVPageRange; // 记录运行中指令的vpage，左闭右闭；对于send指令，send接收方记录src vpage、send接收方记录dst vpage
        std::array<uint, 2> fetchFlushSendCmdCounts; // 发送的fetch/flush/send指令计数。对于send指令，发送方和接收方都要统计
        
        // double buffer
        bool withDoubleBuffer;
        uint inUseIdx, noUseIdx; // 如果使用了double buffer, XOR=0, 否则，XOR=1

        // pipeline buffer 对应的dram地址. spm目的地址无需在此处存储，使用时获取即可
        std::array<uint64, 2> dramBaseAddr; // 仅被DRAM类型张量使用,对应dram起始地址（入口向量是fetch起始dram地址，出口向量是flush起始dram地址)

        // 用于记录pipebuffer处理完成的subbatch offset
        // 对于入口，统计的是下一个将要读的subbatch的起始offset, [0, offset)已经读入
        // 对于出口，统计的是已经send/flush完的subbatch数量，[0,offset)已经发送完并完成
        uint subBatchOffset;
    };
}

#endif