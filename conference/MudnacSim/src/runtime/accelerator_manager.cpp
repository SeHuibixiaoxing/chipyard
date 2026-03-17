#include "accelerator_manager.h"
#include <cmath>
#include <algorithm>

namespace mudnac {
    // Helper function to check if a number is a power of 2
    bool is_power_of_two(uint n) {
        return (n > 0) && ((n & (n - 1)) == 0);
    }

    AcceleratorManager::AcceleratorManager() {
    }

    void AcceleratorManager::init(uint noc_w, uint noc_h, PipelineAccAllocStrategy strategy) { 
        noc_w_ = noc_w;
        noc_h_ = noc_h;
        strategy_ = strategy;
        fragmentation_threshold_ = 0;
        available_count_ = noc_w * noc_h;
        occupied_count_ = 0;

        // 验证NoC维度是2的幂次
        assert(is_power_of_two(noc_w_));
        assert(is_power_of_two(noc_h_));
        
        // 初始化所有加速器为空闲状态
        accel_status_.resize(noc_w * noc_h, false);
        
        // 生成希尔伯特曲线序列
        GenerateHilbertSequence();

        std::cout << "hilbert_sequence_=" << mudnac::toString(hilbert_sequence_) << std::endl;
    }

    void AcceleratorManager::GenerateHilbertSequence() {
        hilbert_sequence_.clear();

        // 找到能够覆盖整个NoC区域的最小2的幂次
        uint max_dim = std::max(noc_w_, noc_h_);
        uint size = 1;
        while (size < max_dim) size <<= 1;

        // Use iterative d->(x,y) Hilbert mapping to avoid duplicate coordinates
        auto rot = [](int n, int &x, int &y, int rx, int ry) {
            if (ry == 0) {
                if (rx == 1) {
                    x = n - 1 - x;
                    y = n - 1 - y;
                }
                // swap x and y
                int t = x; x = y; y = t;
            }
        };

        auto d2xy = [&](uint d) {
            int x = 0, y = 0;
            uint t = d;
            for (uint s = 1; s < size; s <<= 1) {
                int rx = (t / 2) & 1;
                int ry = (t ^ rx) & 1;
                rot(s, x, y, rx, ry);
                x += s * rx;
                y += s * ry;
                t /= 4;
            }
            return std::make_pair(x, y);
        };

        const uint total = size * size;
        hilbert_sequence_.reserve(std::min<uint>(total, noc_w_ * noc_h_));
        for (uint d = 0; d < total; ++d) {
            auto p = d2xy(d);
            uint row = static_cast<uint>(p.second); // y -> column? keep consistent with IdToCoord
            uint col = static_cast<uint>(p.first);
            // Note: our CoordToId expects (row, col)
            // p.first is x (col), p.second is y (row)
            if (row < noc_h_ && col < noc_w_) {
                hilbert_sequence_.push_back(CoordToId(row, col));
            }
        }
    }

    void AcceleratorManager::GenerateHilbertRecursive(int n, int x, int y, int xi, int xj, int yi, int yj, std::vector<uint>& seq) {
        if (n <= 0) {
            // Compute the center point for this cell according to the standard Hilbert recursion
            int cx = x + (xi + yi) / 2;
            int cy = y + (xj + yj) / 2;
            if (IsValidCoordinate(cx, cy)) {
                seq.push_back(CoordToId(static_cast<uint>(cx), static_cast<uint>(cy)));
            }
        } else {
            // 递归生成希尔伯特曲线的四个象限
            GenerateHilbertRecursive(n-1, x, y, yi/2, yj/2, xi/2, xj/2, seq);
            GenerateHilbertRecursive(n-1, x+xi/2, y+xj/2, xi/2, xj/2, yi/2, yj/2, seq);
            GenerateHilbertRecursive(n-1, x+xi/2+yi/2, y+xj/2+yj/2, xi/2, xj/2, yi/2, yj/2, seq);
            GenerateHilbertRecursive(n-1, x+xi/2+yi, y+xj/2+yj, -yi/2, -yj/2, -xi/2, -xj/2, seq);
        }
    }

    bool AcceleratorManager::IsValidCoordinate(int x, int y) const {
        return x >= 0 && static_cast<uint>(x) < noc_h_ && 
               y >= 0 && static_cast<uint>(y) < noc_w_;
    }

    uint AcceleratorManager::GetAvailableAccelCount() const {
        return available_count_;
    }

    uint AcceleratorManager::GetOccupiedAccelCount() const {
        return occupied_count_;
    }

    std::vector<uint> AcceleratorManager::AllocateAccels(uint k) {
        if (k == 0) {
            return {};
        }
        // 如果请求的数量超过可用数量，直接返回空
        if (k > available_count_) {
            return {};
        }
        
        // 根据策略选择分配方式
        if (strategy_ == PipelineAccAllocStrategy::FREE) {
            // FREE策略：按单个加速器分配
            return AllocateAccelsByFree(k);
        } else {
            // HILBERT策略：按希尔伯特曲线顺序分配
            return AllocateAccelsByHilbert(k);
        }
    }

    std::vector<uint> AcceleratorManager::AllocateAccelsByHilbert(uint k) {
        std::vector<uint> allocated_accels;
        allocated_accels.reserve(k);
        
        uint allocated_count = 0;
        
        // 使用first-fit方式在希尔伯特曲线序列中寻找连续的空闲加速器
        for (size_t i = 0; i < hilbert_sequence_.size() && allocated_count < k; ) {
            uint needed = k - allocated_count;
            // 查找连续的空闲加速器
            size_t start_pos = i;
            size_t consecutive_free = 0;
            
            // 统计连续的空闲加速器数量
            while (i < hilbert_sequence_.size() && !accel_status_[hilbert_sequence_[i]]) {
                consecutive_free++;
                i++;
            }

            // 如果找到了连续的空闲加速器
            if (consecutive_free > 0) {
                // 检查这段序列长度是否大于阈值
                if (fragmentation_threshold_ == k || consecutive_free > fragmentation_threshold_) {
                    // 分配这段序列中足够数量的加速器
                    uint to_allocate = static_cast<uint>(std::min(static_cast<size_t>(consecutive_free), static_cast<size_t>(needed)));
                    
                    for (size_t j = start_pos; j < start_pos + to_allocate; ++j) {
                        uint accel_id = hilbert_sequence_[j];
                        accel_status_[accel_id] = true;
                        allocated_accels.push_back(accel_id);
                    }
                    
                    allocated_count += to_allocate;
                }
                // 如果连续空闲加速器数量小于等于阈值，则跳过这段（不分配）
            }
            
            // 跳过当前占用的加速器
            while (i < hilbert_sequence_.size() && accel_status_[hilbert_sequence_[i]]) {
                i++;
            }
        }
        
        // 检查是否成功分配了足够数量的加速器
        if (allocated_count < k) {
            // 回滚已分配的加速器
            for (uint id : allocated_accels) {
                accel_status_[id] = false;
            }
            return {};
        }
        
        // 更新计数器
        available_count_ -= k;
        occupied_count_ += k;
        
        return allocated_accels;
    }

    std::vector<uint> AcceleratorManager::AllocateAccelsByFree(uint k) {
        std::vector<uint> allocated_accels;
        allocated_accels.reserve(k);
        
        uint allocated_count = 0;
        for (uint i = 0; i < accel_status_.size() && allocated_count < k; ++i) {
            if (!accel_status_[i]) {
                accel_status_[i] = true;
                allocated_accels.push_back(i);
                allocated_count++;
            }
        }
        
        // 检查是否成功分配了足够数量的加速器
        if (allocated_count < k) {
            // 回滚已分配的加速器
            for (uint id : allocated_accels) {
                accel_status_[id] = false;
            }
            return {};
        }
        
        // 更新计数器
        available_count_ -= k;
        occupied_count_ += k;
        
        return allocated_accels;
    }

    void AcceleratorManager::ReleaseAccels(const std::vector<uint>& accels) {
        // 将这些加速器标记为空闲
        uint released_count = 0;
        for (uint id : accels) {
            if (id < accel_status_.size() && accel_status_[id]) {
                accel_status_[id] = false;
                released_count++;
            }
        }
        
        // 更新计数器
        available_count_ += released_count;
        occupied_count_ -= released_count;
    }

    void AcceleratorManager::SetFragmentationThreshold(uint p) {
        fragmentation_threshold_ = p;
    }

    uint AcceleratorManager::CoordToId(uint row, uint col) const {
        return row * noc_w_ + col;
    }

    std::pair<uint, uint> AcceleratorManager::IdToCoord(uint id) const {
        return std::make_pair(id / noc_w_, id % noc_w_);
    }

} // namespace mudnac