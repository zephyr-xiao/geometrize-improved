#include "scanline.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "../commonutil.h"

namespace geometrize
{

Scanline::Scanline(const std::int32_t y, const std::int32_t x1, const std::int32_t x2) : y{y}, x1{x1}, x2{x2} {}

bool operator==(const geometrize::Scanline& lhs, const geometrize::Scanline& rhs)
{
    return lhs.y == rhs.y && lhs.x1 == rhs.x1 && lhs.x2 == rhs.x2;
}

bool operator!=(const geometrize::Scanline& lhs, const geometrize::Scanline& rhs)
{
    return lhs.y != rhs.y || lhs.x1 != rhs.x1 || lhs.x2 != rhs.x2;
}

std::vector<geometrize::Scanline> trimScanlines(const std::vector<geometrize::Scanline>& scanlines, std::int32_t minX, std::int32_t minY, std::int32_t maxX, std::int32_t maxY)
{
    std::vector<geometrize::Scanline> trimmedScanlines;
    trimmedScanlines.reserve(scanlines.size());

    for(const geometrize::Scanline& line : scanlines) {
        if(line.y < minY || line.y >= maxY) {
            continue;
        }
        if(line.x1 > line.x2) {
            continue;
        }
        // int 直接 clamp(上游经 float 中转,float 可无损表示该域内整数,数值恒等)
        const std::int32_t x1 = geometrize::commonutil::clamp(line.x1, minX, maxX - 1);
        const std::int32_t x2 = geometrize::commonutil::clamp(line.x2, minX, maxX - 1);
        trimmedScanlines.emplace_back(Scanline(line.y, x1, x2));
    }
    return trimmedScanlines;
}

std::vector<geometrize::Scanline> downscaleScanlines(const std::vector<geometrize::Scanline>& lines, const std::int32_t halfWidth, const std::int32_t halfHeight)
{
    std::vector<geometrize::Scanline> downscaled;
    downscaled.reserve(lines.size());

    // 相邻行对 (2k, 2k+1) 折叠为半分辨率行 k;x 取并后 floor 除 2:
    // 覆盖区只放不放收,搜索阶段的评估偏保守,不影响最终全分辨率判定。
    // 细形状(Line/Polyline)同一源行可有多条退化扫描线,折叠后同一半分辨率
    // 行可能落出相交区间——能量函数按像素遍历,重复覆盖会被重复累加,
    // 故与上一条同行的相交区间就地合并,保证输出行内互不重叠。
    for(std::size_t i = 0; i < lines.size();) {
        const std::int32_t y{lines[i].y / 2};
        std::int32_t x1{lines[i].x1};
        std::int32_t x2{lines[i].x2};
        if(i + 1 < lines.size() && lines[i + 1].y / 2 == y) {
            x1 = (std::min)(x1, lines[i + 1].x1);
            x2 = (std::max)(x2, lines[i + 1].x2);
            i += 2;
        } else {
            i += 1;
        }
        x1 /= 2;
        x2 /= 2;
        if(!downscaled.empty() && downscaled.back().y == y && x1 <= downscaled.back().x2 && downscaled.back().x1 <= x2) {
            downscaled.back().x1 = (std::min)(downscaled.back().x1, x1);
            downscaled.back().x2 = (std::max)(downscaled.back().x2, x2);
        } else {
            downscaled.emplace_back(Scanline(y, x1, x2));
        }
    }

    return geometrize::trimScanlines(downscaled, 0, 0, halfWidth, halfHeight);
}

}
