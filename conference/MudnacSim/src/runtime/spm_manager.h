#ifndef MUDNACSIM_SPM_MANAGER_H
#define MUDNACSIM_SPM_MANAGER_H

#include "tools.h"
namespace mudnac {
    class SPMManager {
    public:
        SPMManager();

        /**​
         * 构造函数
         * @param bank数量
         * @param pages_per_bank 每个bank的页数
         * @param page_size 每页的大小（字节）
         */
        void init(uint num_banks, uint pages_per_bank, uint page_size);
    

        /**​
         * 获取指定bank的剩余页数
         * @param bank_id bank ID
         * @return 剩余页数
         */
        uint getFreePagesInBank(uint bank_id) const;

        /**
         * 获取指定bank的已分配页数
         * @param bank_id bank ID
         * @return 已分配页数
         */
        uint getAllocatedPagesInBank(uint bank_id) const;

        /**
         * 获取总剩余页数
         * @return 总剩余页数
         */
        uint getTotalRemainPages() const;

        /**
         * 获取总已分配页数
         * @return 总已分配页数
         */
        uint getTotalAllocatedPages() const;

        /**
         * 从指定bank申请页
         * @param bank_id bank ID
         * @param num_pages 申请的页数
         * @return pair(分配页号集合,未能分配的页数)
         */
        std::pair<PageSetShared, uint> allocateFromBank(uint bank_id, uint num_pages);

        /**
         * 从指定bank集合中申请页（bank交错）
         * @param num_pages 申请的页数
         * @param bank_ids 指定bank集合
         * @param init_factor 初始分配因子，控制初始bank剩余页比例，小于该比例的bank不参与分配
         * @param decrease_factor 递减因子，控制剩余页比例系数的下降提督。每当分配后仍有剩余时，剩余页比例乘以该因子，继续分配
         * @return pair(分配页号集合,未能分配的页数)
         */
        std::pair<PageSetShared, uint> allocateFixedBankInterlace(const std::vector<uint> bank_ids, uint num_pages, float init_factor = 0.0, float decrease_factor = 0.0);

        /**
         * 从所有bank中申请页（bank交错）
         * @param num_pages 申请的页数
         * @return pair(分配页号集合,未能分配的页数)
         */
        std::pair<PageSetShared, uint> allocateAllBankInterlace(uint num_pages, float init_factor = 0.0, float decrease_factor = 0.0);


        /**
         * 释放物理页
         * @param pages_to_free 要释放的页号集合
         */
        void freePages(PageSetShared pages_to_free);

        nlohmann::json toJson() const;
        std::string toString() const;

    private:
        uint num_banks_;      // 核心数量（bank数量）
        uint pages_per_bank_; // 每个bank的页数
        uint page_size_;      // 每页的大小（字节）
        uint total_pages_;    // 总页数
        uint remain_pages_;   // 剩余页数
        
        // 每个bank的剩余页号列表（使用LIFO策略）
        std::vector<PageSetShared> free_pages_per_bank_;

        /**
         * 验证bank ID是否有效
         * @param bank_id bank ID
         */
        void validateBankId(uint bank_id) const;
    };

}


#endif // MUDNACSIM_SPM_MANAGER_H