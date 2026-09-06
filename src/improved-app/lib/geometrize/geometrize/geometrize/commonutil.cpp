#include "commonutil.h"

#include "core/errorweightmap.h"
#include "runner/imagerunneroptions.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <random>
#include <tuple>
#include <vector>

#include "bitmap/bitmap.h"
#include "bitmap/rgba.h"
#include "rasterizer/scanline.h"
#include "runner/imagerunneroptions.h"

namespace geometrize
{

namespace commonutil
{

thread_local static std::mt19937 mt(std::random_device{}());
thread_local static std::uniform_int_distribution<std::int32_t> pick;

void seedRandomGenerator(const std::uint32_t seed)
{
    mt.seed(seed);
}

std::int32_t randomRange(const std::int32_t min, const std::int32_t max)
{
    assert(min <= max);
    return pick(mt, std::uniform_int_distribution<std::int32_t>::param_type{min, max});
}

geometrize::rgba getAverageImageColor(const geometrize::Bitmap& image)
{
    const std::vector<std::uint8_t>& data{image.getDataRef()};
    const std::size_t size{data.size()};
    if(size == 0) {
        return geometrize::rgba{0, 0, 0, 0};
    }

    const std::size_t numPixels{size / 4U};

    std::uint32_t totalRed{0};
    std::uint32_t totalGreen{0};
    std::uint32_t totalBlue{0};
    for(std::size_t i = 0; i < size; i += 4U) {
        totalRed += data[i];
        totalGreen += data[i + 1U];
        totalBlue += data[i + 2U];
    }

    return geometrize::rgba{
        static_cast<std::uint8_t>(totalRed / numPixels),
        static_cast<std::uint8_t>(totalGreen / numPixels),
        static_cast<std::uint8_t>(totalBlue / numPixels),
        static_cast<std::uint8_t>(UINT8_MAX)
    };
}

bool scanlinesContainTransparentPixels(const std::vector<geometrize::Scanline>& scanlines, const geometrize::Bitmap& image, int minAlpha)
{
    const auto& trimmedScanlines = geometrize::trimScanlines(scanlines, 0, 0, image.getWidth(), image.getHeight());
    for(const geometrize::Scanline& scanline : trimmedScanlines) {
        // 闭区间遍历与库内其余扫描线遍历一致(上游 x < x2 漏检末像素)
        for(int x = scanline.x1; x <= scanline.x2; x++) {
            if(image.getPixel(x, scanline.y).a < minAlpha) {
                return true;
            }
        }
    }
    return false;
}

std::tuple<std::int32_t, std::int32_t, std::int32_t, std::int32_t> mapShapeBoundsToImage(const geometrize::ImageRunnerShapeBoundsOptions& options, const geometrize::Bitmap& image)
{
    if(!options.enabled) {
        return { 0, 0, image.getWidth() - 1, image.getHeight() - 1 };
    }

    const double xMinPx = options.xMinPercent / 100.0 * static_cast<double>(image.getWidth() - 1);
    const double yMinPx = options.yMinPercent / 100.0 * static_cast<double>(image.getHeight() - 1);
    const double xMaxPx = options.xMaxPercent / 100.0 * static_cast<double>(image.getWidth() - 1);
    const double yMaxPx = options.yMaxPercent / 100.0 * static_cast<double>(image.getHeight() - 1);

    std::int32_t xMin = static_cast<std::int32_t>(std::round(std::min(std::min(xMinPx, xMaxPx), image.getWidth() - 1.0)));
    std::int32_t yMin = static_cast<std::int32_t>(std::round(std::min(std::min(yMinPx, yMaxPx), image.getHeight() - 1.0)));
    std::int32_t xMax = static_cast<std::int32_t>(std::round(std::min(std::max(xMinPx, xMaxPx), image.getWidth() - 1.0)));
    std::int32_t yMax = static_cast<std::int32_t>(std::round(std::min(std::max(yMinPx, yMaxPx), image.getHeight() - 1.0)));

    // If we have a bad width or height, which is bound to cause problems - use the whole image
    if(xMax - xMin <= 1) {
        xMin = 0;
        xMax = image.getWidth() - 1;
    }
    if(yMax - yMin <= 1) {
        yMin = 0;
        yMax = image.getHeight() - 1;
    }

    return std::make_tuple(xMin, yMin, xMax, yMax);
}

std::vector<geometrize::core::RegionRect> mapPriorityRegionsToImage(const std::vector<geometrize::ImageRunnerPriorityRegionOptions>& regions, const geometrize::Bitmap& image)
{
    std::vector<geometrize::core::RegionRect> mapped;
    mapped.reserve(regions.size());
    for(const geometrize::ImageRunnerPriorityRegionOptions& region : regions) {
        const double xMinPx = region.xMinPercent / 100.0 * static_cast<double>(image.getWidth() - 1);
        const double yMinPx = region.yMinPercent / 100.0 * static_cast<double>(image.getHeight() - 1);
        const double xMaxPx = region.xMaxPercent / 100.0 * static_cast<double>(image.getWidth() - 1);
        const double yMaxPx = region.yMaxPercent / 100.0 * static_cast<double>(image.getHeight() - 1);

        // min/max 归一化(用户可能反着拖)+ clamp 图界,公式与 mapShapeBoundsToImage 同款
        const std::int32_t xMin = commonutil::clamp(static_cast<std::int32_t>(std::round((std::min)(xMinPx, xMaxPx))), 0, static_cast<std::int32_t>(image.getWidth() - 1));
        const std::int32_t yMin = commonutil::clamp(static_cast<std::int32_t>(std::round((std::min)(yMinPx, yMaxPx))), 0, static_cast<std::int32_t>(image.getHeight() - 1));
        const std::int32_t xMax = commonutil::clamp(static_cast<std::int32_t>(std::round((std::max)(xMinPx, xMaxPx))), 0, static_cast<std::int32_t>(image.getWidth() - 1));
        const std::int32_t yMax = commonutil::clamp(static_cast<std::int32_t>(std::round((std::max)(yMinPx, yMaxPx))), 0, static_cast<std::int32_t>(image.getHeight() - 1));

        // 退化矩形(映射后不足 2px)跳过:无引导价值还占配额
        if(xMax - xMin < 1 || yMax - yMin < 1) {
            continue;
        }
        mapped.push_back(geometrize::core::RegionRect{ xMin, yMin, xMax, yMax });
    }
    return mapped;
}

}

}
