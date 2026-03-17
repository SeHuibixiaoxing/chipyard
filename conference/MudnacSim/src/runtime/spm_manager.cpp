#include "spm_manager.h"

namespace mudnac {
    SPMManager::SPMManager() {

    }
    void SPMManager::init(uint num_banks, uint pages_per_bank, uint page_size) {
        num_banks_ =num_banks;
        pages_per_bank_ = pages_per_bank;
        page_size_ = page_size;
        total_pages_ = num_banks * pages_per_bank;
        remain_pages_ = total_pages_;
            
        if (num_banks == 0 || pages_per_bank == 0 || page_size == 0) {
            assert(false);
        }

        // Create a distinct PageSetShared for each bank (avoid sharing the same pointer)
        free_pages_per_bank_.clear();
        free_pages_per_bank_.reserve(num_banks_);
        for (uint i = 0; i < num_banks_; ++i) {
            free_pages_per_bank_.push_back(std::make_shared<PageSet>());
        }

        // 初始化每个bank的剩余页号
        for (uint bank_id = 0; bank_id < num_banks; bank_id++) {
            free_pages_per_bank_[bank_id]->reserve(pages_per_bank);
            for (uint page_offset = 0; page_offset < pages_per_bank; page_offset++) {
                uint global_page = bank_id * pages_per_bank_ + page_offset;
                free_pages_per_bank_[bank_id]->push_back(global_page);
            }
        }
    }

    uint SPMManager::getFreePagesInBank(uint bank_id) const {
            validateBankId(bank_id);
            return free_pages_per_bank_[bank_id]->size();
    }

    uint SPMManager::getAllocatedPagesInBank(uint bank_id) const {
        validateBankId(bank_id);
        return pages_per_bank_ - getFreePagesInBank(bank_id);
    }

    uint SPMManager::getTotalRemainPages() const {
        return remain_pages_;
    }

    uint SPMManager::getTotalAllocatedPages() const {
        return total_pages_ - getTotalRemainPages();
    }

    std::pair<PageSetShared, uint> SPMManager::allocateFromBank(uint bank_id, uint num_pages) {
        validateBankId(bank_id);
        
        PageSetShared allocated_pages = std::make_shared<PageSet>();
        uint free_in_bank = free_pages_per_bank_[bank_id]->size();
        uint pages_to_allocate = std::min(num_pages, free_in_bank);
        uint failed_to_allocate = num_pages - pages_to_allocate;
        
        allocated_pages->reserve(pages_to_allocate);
        
        // 从剩余页号列表末尾取出页号
        for (uint i = 0; i < pages_to_allocate; i++) {
            allocated_pages->push_back(free_pages_per_bank_[bank_id]->back());
            free_pages_per_bank_[bank_id]->pop_back();
        }

        remain_pages_ -= pages_to_allocate;
        
        return std::make_pair(allocated_pages, failed_to_allocate);
    }

    std::pair<PageSetShared, uint> SPMManager::allocateFixedBankInterlace(const std::vector<uint> bank_ids, uint num_pages, float init_factor, float decrease_factor) {
        PageSetShared allocated_pages = std::make_shared<PageSet>();
        allocated_pages->reserve(num_pages);
        
        uint failed_to_allocate = num_pages;
        uint total_allocated = 0;

        bool has_page = true;
        float current_factor = init_factor;
        
        // 循环直到分配完所有请求的页数或无法再分配
        while (true) {
            while (has_page && total_allocated < num_pages) {
                has_page = false;
                for (uint current_bank_id_idx = 0;current_bank_id_idx < bank_ids.size() && total_allocated < num_pages;current_bank_id_idx ++) {
                    uint current_bank_id = bank_ids[current_bank_id_idx];
                    // 如果当前bank有可用页，且剩余页数比例大于当前因子
                    if (free_pages_per_bank_[current_bank_id]->size() < static_cast<uint>(pages_per_bank_ * current_factor)) continue;
                    if (free_pages_per_bank_[current_bank_id]->empty()) continue;

                    // 从当前bank取一页
                    allocated_pages->push_back(free_pages_per_bank_[current_bank_id]->back());
                    free_pages_per_bank_[current_bank_id]->pop_back();
                    total_allocated++;
                    failed_to_allocate--;
                    has_page = true;
                }
            }
            if (failed_to_allocate == 0)  break;
            if (current_factor <= 0.0000001) break;
            current_factor *= decrease_factor;
        }

        remain_pages_ -= (num_pages - failed_to_allocate);
        return std::make_pair(allocated_pages, failed_to_allocate);   
    }

    std::pair<PageSetShared, uint> SPMManager::allocateAllBankInterlace(uint num_pages, float init_factor, float decrease_factor) {
        std::vector<uint> bank_ids(num_banks_);
        std::iota(bank_ids.begin(), bank_ids.end(), 0);
        return allocateFixedBankInterlace(bank_ids, num_pages, init_factor, decrease_factor);
    }

    void SPMManager::freePages(PageSetShared pages_to_free) {
        for (uint page : (*pages_to_free)) {
            assert(total_pages_ >= page);
            
            uint bank_id = page / pages_per_bank_;
            validateBankId(bank_id);
            
            // 将页号添加到对应bank的剩余页号列表末尾
            free_pages_per_bank_[bank_id]->push_back(page);
        }
        remain_pages_ += pages_to_free->size();
        pages_to_free->clear();
    }

    nlohmann::json SPMManager::toJson() const {
        nlohmann::json js;
#ifdef JSON_DBG
        js["num_banks"] = num_banks_;
        js["pages_per_bank"] = pages_per_bank_;
        js["page_size"] = page_size_;
        js["total_pages"] = total_pages_;
        js["remain_pages"] = remain_pages_;
        js["total_capacity(Bytes)"] = total_pages_ * page_size_;
        {
            nlohmann::json free_pages_per_bank_js;
            for (uint i = 0; i < num_banks_; i++) {
                free_pages_per_bank_js[std::to_string(i)] = free_pages_per_bank_[i]->size();
            }
            js["free_pages_per_bank"] = free_pages_per_bank_js;
        }
#endif
        return js;
    }

    std::string SPMManager::toString() const {
        return toJson().dump(JSON_DUMP_INDENTATION);
    }

    void SPMManager::validateBankId(uint bank_id) const {
        assert(bank_id < num_banks_);
    }
}