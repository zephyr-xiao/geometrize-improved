#pragma once

#include <cstdint>
#include <vector>

namespace geometrize
{
class Bitmap;
}

namespace geometrize
{

namespace core
{

/**
 * @brief The RegionRect struct 封闭区间矩形(含端点),全分辨率像素坐标。
 * 用于区域优先绘制(F3.2):落在区域内的误差块获得加权。
 */
struct RegionRect
{
    std::int32_t xMin{0};
    std::int32_t yMin{0};
    std::int32_t xMax{0};
    std::int32_t yMax{0};
};

inline bool operator==(const RegionRect& a, const RegionRect& b)
{
    return a.xMin == b.xMin && a.yMin == b.yMin && a.xMax == b.xMax && a.yMax == b.yMax;
}

/**
 * @brief ErrorWeightMap 误差加权放置的权重表(A2.1 误差图引导,opt-in 增强轨道)。
 * 把 |target−current| 的四通道 L1 差按 blockSize×blockSize 块聚合,构建整数缩放的
 * inclusive CDF;sampleCenter 按块误差加权采样一个全分辨率坐标,供候选形状放置引导。
 * 重建是 (target, current) 的纯整数函数;主线程提交任务前重建,任务期间只读。
 */
class ErrorWeightMap
{
public:
    explicit ErrorWeightMap(std::uint32_t blockSize = 16U);

    /**
     * @brief rebuild Rebuilds the weight map from the target and current bitmaps. O(W×H) single pass.
     * @param target The target bitmap.
     * @param current The current bitmap (must match the target dimensions).
     * @param priorityRegions 优先区域矩形列表(全分辨率像素坐标,闭区间),区域内的块误差乘 priorityFactor。
     * 空列表与不传逐位一致;命中判定 = 块中心点落在矩形内,多矩形重叠不叠加(命中即乘)。
     * @param priorityFactor 区域内块误差的整数倍率(≥1)。
     */
    void rebuild(const geometrize::Bitmap& target, const geometrize::Bitmap& current,
                 const std::vector<RegionRect>& priorityRegions = {}, std::uint32_t priorityFactor = 10U);

    /**
     * @brief sampleCenter Samples a center point, weighted by per-block error.
     * Consumes randomRange draws only when a sample is produced (3 draws); zero when no error remains.
     * @param cx Receives the sampled x-coordinate.
     * @param cy Receives the sampled y-coordinate.
     * @return False when the map holds no error (uniform fallback: the caller keeps the original setup position).
     */
    bool sampleCenter(std::int32_t& cx, std::int32_t& cy) const;

    /**
     * @brief blockCount Gets the number of blocks. Zero means the map has never been built.
     */
    std::size_t blockCount() const
    {
        return m_blockSums.size();
    }

private:
    std::uint32_t m_blockSize;
    std::uint32_t m_blocksX{0U};
    std::uint32_t m_blocksY{0U};
    std::int32_t m_mapWidth{0};
    std::int32_t m_mapHeight{0};
    std::vector<std::uint32_t> m_blockSums; ///< 重建后存放缩放前块和,缩放计算就地覆盖为缩放值。
    std::vector<std::uint64_t> m_prefix; ///< 缩放后权重的 inclusive 前缀和,零权重块无增量、二分天然跳过。
    std::int32_t m_totalScaled{0}; ///< 缩放后总权重,恒 < INT32_MAX(缩放除数保证)。
};

}

}
