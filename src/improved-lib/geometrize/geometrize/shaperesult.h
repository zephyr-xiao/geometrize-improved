#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "bitmap/rgba.h"

namespace geometrize
{
class Shape;
}

namespace geometrize
{

/**
 * @brief The ScanlineColor struct holds the per-scanline color of a segmented shape.
 */
struct ScanlineColor
{
    std::int32_t y; ///< The y-coordinate of the scanline.
    std::int32_t x1; ///< The leftmost x-coordinate of the scanline.
    std::int32_t x2; ///< The rightmost x-coordinate of the scanline.
    rgba color; ///< The color of the scanline.
};

/**
 * @brief The ShapeResult struct is a container for info about a shape added to the model.
 * @author Sam Twidale (https://samcodes.co.uk/)
 */
struct ShapeResult
{
    const double score;
    const geometrize::rgba color;
    const std::shared_ptr<geometrize::Shape> shape;
    // A2.4 分段颜色:非空时是各扫描线的行级取色存档(导出/重放/重做的唯一权威来源——
    // 分段色依赖落画时刻的位图,事后重算是错的)。空 = 单色语义(开关关/线型形状),全链向后兼容。
    const std::vector<ScanlineColor> segments;
};

}

