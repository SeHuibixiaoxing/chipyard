#ifndef MUDNACSIM_ACCELERATOR_MANAGER_H
#define MUDNACSIM_ACCELERATOR_MANAGER_H

#include "tools.h"
#include <vector>

namespace mudnac {

    class AcceleratorManager {
    public:
        enum class PipelineAccAllocStrategy {
            FREE,     // 单个加速器分配
            HILBERT   // 希尔伯特曲线顺序分配
        };

        AcceleratorManager();

        /**
         * @brief 初始化函数
         * @param noc_w 片上网络宽度
         * @param noc_h 片上网络长度
         * @param strategy 加速器分配策略
         */
        void init(uint noc_w, uint noc_h, PipelineAccAllocStrategy strategy = PipelineAccAllocStrategy::FREE);

        /**
         * @brief 获取空闲加速器数量
         * @return 空闲加速器数量
         */
        uint GetAvailableAccelCount() const;

        /**
         * @brief 获取被占用加速器数量
         * @return 被占用加速器数量
         */
        uint GetOccupiedAccelCount() const;

        /**
         * @brief 从空闲加速器中分配k个加速器
         * @param k 需要分配的加速器数量
         * @return 分配的加速器编号列表，如果无法分配则返回空列表
         */
        std::vector<uint> AllocateAccels(uint k);

        /**
         * @brief 释放一组加速器
         * @param accels 要释放的加速器编号列表
         */
        void ReleaseAccels(const std::vector<uint>& accels);

        /**
         * @brief 改变TANGRAM方法时使用的长条宽度
         * @param width 新的长条宽度
         */
        /**
         * @brief 改变不分配加速器数量的阈值p
         * @param p 新的阈值
         */
        void SetFragmentationThreshold(uint p);
        
        /**
         * @brief 将行列坐标转换为加速器ID
         * @param row 行号
         * @param col 列号
         * @return 加速器ID
         */
        uint CoordToId(uint row, uint col) const;
        
        /**
         * @brief 将加速器ID转换为行列坐标
         * @param id 加速器ID
         * @return 行列坐标pair
         */
        std::pair<uint, uint> IdToCoord(uint id) const;

    private:
        uint noc_w_;                      // 片上网络宽度
        uint noc_h_;                      // 片上网络长度
        PipelineAccAllocStrategy strategy_;        // 分配策略
        uint fragmentation_threshold_;             // 碎片化阈值
        
        std::vector<bool> accel_status_;           // 加速器状态，true表示被占用，false表示空闲
        std::vector<uint> hilbert_sequence_;       // 希尔伯特曲线顺序的加速器ID序列
        
        // 动态维护的计数器
        uint available_count_;                      // 空闲加速器数量
        uint occupied_count_;                       // 被占用加速器数量
        
        /**
         * @brief 生成希尔伯特曲线顺序的加速器序列
         */
        void GenerateHilbertSequence();
        
        /**
         * @brief 递归生成希尔伯特曲线
         * @param n 当前阶数
         * @param x 起始x坐标
         * @param y 起始y坐标
         * @param xi x方向参数
         * @param xj x方向参数
         * @param yi y方向参数
         * @param yj y方向参数
         * @param seq 序列容器
         */
        void GenerateHilbertRecursive(int n, int x, int y, int xi, int xj, int yi, int yj, std::vector<uint>& seq);
        
        /**
         * @brief 检查坐标是否在合法范围内
         * @param x x坐标
         * @param y y坐标
         * @return 是否合法
         */
        bool IsValidCoordinate(int x, int y) const;
        
        /**
         * @brief 按照希尔伯特曲线顺序分配k个加速器
         * @param k 需要分配的加速器数量
         * @return 分配的加速器编号列表
         */
        std::vector<uint> AllocateAccelsByHilbert(uint k);

        /**
         * @brief 按照空闲顺序分配k个加速器
         * @param k 需要分配的加速器数量
         * @return 分配的加速器编号列表
         */
        std::vector<uint> AllocateAccelsByFree(uint k);
    };

} // namespace mudnac

#endif // MUDNACSIM_ACCELERATOR_MANAGER_H