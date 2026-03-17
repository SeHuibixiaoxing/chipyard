#include "pipeline_buffer.h"

namespace mudnac {
    RingBuffer::RingBuffer(int size, int out_degree_, const std::vector<uint64>& strides, const std::vector<uint64>& shape, 
                        const std::vector<PageSetShared>& p_spm_pages): size_(size), strides_(strides), shape_(shape), out_degree_(out_degree_), p_spm_pages_(p_spm_pages), head_(0), tail_(0) {

    }
    RingBuffer::RingBuffer(int size, int out_degree_, const std::vector<uint64>& strides, const std::vector<uint64>& shape, 
                        const std::vector<std::pair<uint, uint>>& dram_addr_range): 
                size_(size), strides_(strides), shape_(shape), out_degree_(out_degree_), dram_addr_range_(dram_addr_range), head_(0), tail_(0) {

    }

    void RingBuffer::setSpmPages(const std::vector<PageSetShared>& p_spm_pages) {
        p_spm_pages_ = p_spm_pages;
    }
    void RingBuffer::setDramBaseAddr(const std::vector<std::pair<uint, uint>>& dram_addr_range) {
        dram_addr_range_ = dram_addr_range;
    }

    bool RingBuffer::hasIdleBuffer() {
        return (tail_ - head_) < size_;
    }

    bool RingBuffer::hasReadyBuffer(uint subBatchOffset) {
        return head_ <= subBatchOffset && subBatchOffset < tail_;
    }

    bool RingBuffer::isTailOffset(uint subBatchOffset) {
        return subBatchOffset == tail_;
    }

    PageSetShared RingBuffer::getPSpmPages(uint sub_batch_offset) {
        return p_spm_pages_[sub_batch_offset % size_];
    }

    std::pair<uint, uint> RingBuffer::getDramAddrRange(uint sub_batch_offset) {
        return dram_addr_range_[sub_batch_offset % size_];
    }

    uint RingBuffer::getDramBaseAddr(uint sub_batch_offset) {
        return dram_addr_range_[sub_batch_offset % size_].first;
    }

    std::vector<uint64> RingBuffer::getStrides() {
        return strides_;
    }
    std::vector<uint64> RingBuffer::getShape() {
        return shape_;
    }

    void RingBuffer::use(uint sub_batch_offset) {
        auto it = use_count_.find(sub_batch_offset);
        assert(it != use_count_.end());
        -- (it->second);
        assert(it->second >= 0);
        while (it->second == 0 && head_ == sub_batch_offset) {
            ++ head_;
            use_count_.erase(it);
            it = use_count_.find(head_);
            if (it == use_count_.end()) break;
        }
    }

    void RingBuffer::fill(uint sub_batch_offset) {
        assert(sub_batch_offset == tail_);
        ++ tail_;
        use_count_[sub_batch_offset] = out_degree_;
    }

    nlohmann::json RingBuffer::toJson() const {
        nlohmann::json js;
#ifdef JSON_DBG
        {
            std::ostringstream ss;
            ss << this;
            js["ptr"] = ss.str();
        }
        js["size"] = size_;
        js["head"] = head_;
        js["tail"] = tail_;
        js["out_degree"] = out_degree_;
        {
            std::vector<uint> pages_size;
            for (int i = 0;i < p_spm_pages_.size();++ i) {
                pages_size.push_back(p_spm_pages_[i]->size());
            }
            js["p_spm_pages"] = pages_size;
        }
        js["use_count(subbatch_id: use_count)"] = use_count_;
#endif
        return js;
    }

    std::string RingBuffer::toString() const {
        return toJson().dump(JSON_DUMP_INDENTATION);
    }

    PipelineBuffer::PipelineBuffer(TensorId tensorId_, StageMapping::TensorStayType tensorStayType_, bool withDoubleBuffer_, const std::vector<uint64>& strides_, const std::vector<uint64>& shape_, uint stage_idx): 
                tensorId(tensorId_), tensorStayType(tensorStayType_), withDoubleBuffer(withDoubleBuffer_),
                strides(strides_), shape(shape_), stage_idx_(stage_idx) {
        if(withDoubleBuffer) {
            inUseIdx = 0; 
            noUseIdx = 1;
        } else {
            inUseIdx = 0; 
            noUseIdx = 0;
        }
        bufferHasFullData[0] = bufferHasFullData[1] = false;
        cmdRunning[0] = cmdRunning[1] = false;
        cmdAcc[0] = cmdAcc[1] = false;
        cmdVPageRange[0] = cmdVPageRange[1] = std::make_pair(0, 0);
        fetchFlushSendCmdCounts[0] = fetchFlushSendCmdCounts[1] = 0;

        dramBaseAddr[0] = dramBaseAddr[1] = -1;

        subBatchOffset = 0;

        pSpmPages[0] = pSpmPages[1] = nullptr;

        ring_buffer_ = nullptr;
        use_ring_buffer_ = false;
        ringBufferCmdVPageRange[0] = ringBufferCmdVPageRange[1] = std::make_pair(0, 0);
    }
    PageSetShared PipelineBuffer::getPSpmPages(uint idx) {
        return pSpmPages[idx];
    }
    PageSetShared PipelineBuffer::getPSpmPagesInUse() {
        return getPSpmPages(inUseIdx);
    }

    bool PipelineBuffer::noAllocatedSpmPage() const {
        return pSpmPages[inUseIdx]->empty() && pSpmPages[noUseIdx]->empty();
    }

    bool PipelineBuffer::noAllocatedExportDstDram() const {
        return dramBaseAddr[inUseIdx] < 0 && dramBaseAddr[noUseIdx] < 0;
    }
    bool PipelineBuffer::noAllocatedExportDst() const {
        return noAllocatedExportDstDram();
    }

    bool PipelineBuffer::bufferFull(uint bufferIdx) const {
        assert(bufferIdx == inUseIdx || bufferIdx == noUseIdx);
        return bufferHasFullData[bufferIdx];
    }
    bool PipelineBuffer::inUseBufferFull() const {
        return bufferFull(inUseIdx);
    }
    bool PipelineBuffer::noUseBufferFull() const {
        return bufferFull(noUseIdx);
    }
    bool PipelineBuffer::allBufferFull() const {
        return inUseBufferFull() && noUseBufferFull();
    }
    bool PipelineBuffer::allBufferEmpty() const {
        return !inUseBufferFull() && !noUseBufferFull();
    }

    bool PipelineBuffer::ringBufferHasCreated() const {
        return use_ring_buffer_ && ring_buffer_ != nullptr;
    }

    nlohmann::json PipelineBuffer::toJson() const {
        nlohmann::json js;
#ifdef JSON_DBG
        {
            std::ostringstream ss;
            ss << this;
            js["ptr"] = ss.str();
        }
        js["stage_idx"] = stage_idx_;
        js["tensor_id"] = tensorId;
        js["tensorStayType"] = StageMapping::tensorStayTypeToString(tensorStayType);
        js["withDoubleBuffer"] = withDoubleBuffer;
        js["inUseIdx"] = inUseIdx;
        js["noUseIdx"] = noUseIdx;
        js["subBatchOffset"] = subBatchOffset;
        js["strides"] = strides;
        js["shape"] = shape;
        
        for (uint i = 0;i < pSpmPages.size();++ i) {
            js["buffer-" + std::to_string(i)] = {
                {"pSpmPages.size()", pSpmPages[i]->size()},
                {"addr(pSpmPages)", reinterpret_cast<uint64>(pSpmPages[i].get())},
                {"dramBaseAddr", dramBaseAddr[i]},
                {"bufferHasFullData", bufferHasFullData[i]},
                {"cmdRunning", cmdRunning[i]},
                {"cmdVPageRange", {cmdVPageRange[i].first, cmdVPageRange[i].second}},
                {"cmdAcc", cmdAcc[i]},
                {"fetchFlushSendCmdCounts", fetchFlushSendCmdCounts[i]}
            };
        }

        {
            std::ostringstream ss;
            ss << ring_buffer_;
            js["ring_buffer_ptr"] = ss.str();
        }

        if (ring_buffer_ != nullptr) {
            js["ring_buffer"] = ring_buffer_->toJson();
        } else {
            js["ring_buffer"] = nlohmann::json::object();
        }
#endif
        return js;
    }

     std::string PipelineBuffer::toString() const {
        return toJson().dump(JSON_DUMP_INDENTATION);
     }
}