#pragma once

#include <cstdint>
#include <vector>

namespace geometrize
{

/**
 * @brief The Scanline class represents a scanline, a row of pixels running across a bitmap.
 * @author Sam Twidale (https://samcodes.co.uk/)
 */
class Scanline
{
public:
    /**
     * @brief Scanline Creates a new scanline (members are uninitialized)
     */
    Scanline() = default;

    /**
     * @brief Scanline Creates a new scanline.
     * @param y The y-coordinate.
     * @param x1 The leftmost x-coordinate.
     * @param x2 The rightmost x-coordinate.
     */
    Scanline(std::int32_t y, std::int32_t x1, std::int32_t x2);

    ~Scanline() = default;
    Scanline& operator=(const Scanline&) = default;
    Scanline(const Scanline&) = default;

    std::int32_t y; ///< The y-coordinate of the scanline.
    std::int32_t x1; ///< The leftmost x-coordinate of the scanline.
    std::int32_t x2; ///< The rightmost x-coordinate of the scanline.
};

bool operator==(const geometrize::Scanline& lhs, const geometrize::Scanline& rhs);
bool operator!=(const geometrize::Scanline& lhs, const geometrize::Scanline& rhs);

/**
 * @brief trimScanlines Crops the scanning width of an array of scanlines so they do not scan outside of the given area.
 * @param scanlines The scanlines to crop.
 * @param minX The minimum x value to crop to.
 * @param minY The minimum y value to crop to.
 * @param maxX The maximum x value to crop to.
 * @param maxY The maximum y value to crop to.
 * @return A new vector of cropped scanlines.
 */
std::vector<geometrize::Scanline> trimScanlines(const std::vector<geometrize::Scanline>& scanlines, std::int32_t minX, std::int32_t minY, std::int32_t maxX, std::int32_t maxY);

/**
 * @brief downscaleScanlines 将全分辨率扫描线组映射到半分辨率坐标空间(坐标约定 floor:x → x/2)。
 * 金字塔搜索轨道专用:形状坐标留在全分辨率空间,仅评估时把覆盖区投影到半分辨率图上。
 * 相邻行对 (2k, 2k+1) 合并为半分辨率行 k,x 范围取两行的并(除以 2 向下取整,
 * 保守超覆盖——搜索启发式允许,最终接受判定仍在全分辨率进行)。
 * 输入按 (y, x1, x2) 升序时单趟 O(n);线型形状(Line/Polyline/QuadraticBezier)
 * 按路径顺序输出,函数内部先检测顺序、乱序才拷贝排序归一(多边形主路径零拷贝)。
 * @param lines The full-resolution scanlines to project.
 * @param halfWidth The width of the half-resolution bitmap (ceil(fullWidth / 2)).
 * @param halfHeight The height of the half-resolution bitmap (ceil(fullHeight / 2)).
 * @return The scanlines mapped into the half-resolution space, trimmed to the half-resolution bounds.
 */
std::vector<geometrize::Scanline> downscaleScanlines(const std::vector<geometrize::Scanline>& lines, std::int32_t halfWidth, std::int32_t halfHeight);

}
