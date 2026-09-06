// 通用工具:clamp、randomRange、getAverageImageColor、mapShapeBoundsToImage、透明像素检测
#include "doctest.h"

#include <cstdint>
#include <tuple>
#include <vector>

#include "geometrize/bitmap/bitmap.h"
#include "geometrize/commonutil.h"
#include "geometrize/rasterizer/scanline.h"
#include "geometrize/runner/imagerunneroptions.h"

TEST_CASE("clamp:三段边界")
{
    CHECK(geometrize::commonutil::clamp(5, 0, 10) == 5);
    CHECK(geometrize::commonutil::clamp(-1, 0, 10) == 0);
    CHECK(geometrize::commonutil::clamp(11, 0, 10) == 10);
    CHECK(geometrize::commonutil::clamp(0, 0, 10) == 0);
    CHECK(geometrize::commonutil::clamp(10, 0, 10) == 10);
}

TEST_CASE("randomRange:min==max 恒值、范围内、固定种子复现")
{
    geometrize::commonutil::seedRandomGenerator(42);
    CHECK(geometrize::commonutil::randomRange(7, 7) == 7);

    for(int i = 0; i < 100; i++) {
        const std::int32_t value = geometrize::commonutil::randomRange(-10, 10);
        CHECK(value >= -10);
        CHECK(value <= 10);
    }

    geometrize::commonutil::seedRandomGenerator(4242);
    const std::int32_t first = geometrize::commonutil::randomRange(0, 100);
    geometrize::commonutil::seedRandomGenerator(4242);
    CHECK(geometrize::commonutil::randomRange(0, 100) == first);
}

TEST_CASE("getAverageImageColor:手算均值、空图零值")
{
    // 2x2:左上红、右上绿、左下蓝、右下白
    // r 通道 (255+0+0+255)/4 = 127(整数截断 127.5)、g (0+255+0+255)/4 = 127、b 同
    const std::vector<std::uint8_t> data{
        255, 0, 0, 255,     0, 255, 0, 255,
        0, 0, 255, 255,     255, 255, 255, 255,
    };
    const geometrize::Bitmap bitmap{2, 2, data};

    const geometrize::rgba average = geometrize::commonutil::getAverageImageColor(bitmap);
    CHECK(average.r == 127);
    CHECK(average.g == 127);
    CHECK(average.b == 127);
    CHECK(average.a == 255);

    const geometrize::Bitmap empty{0, 0, geometrize::rgba{0, 0, 0, 0}};
    const geometrize::rgba zero = geometrize::commonutil::getAverageImageColor(empty);
    CHECK(zero.r == 0);
    CHECK(zero.a == 0);
}

TEST_CASE("mapShapeBoundsToImage:百分比→像素换算")
{
    const geometrize::Bitmap bitmap{100, 50, geometrize::rgba{0, 0, 0, 0}};

    // 实现语义:返回闭区间坐标 [min, max],max = percent/100 * (size-1)
    const geometrize::ImageRunnerShapeBoundsOptions full{true, 0.0, 0.0, 100.0, 100.0};
    const auto [fxMin, fyMin, fxMax, fyMax] = geometrize::commonutil::mapShapeBoundsToImage(full, bitmap);
    CHECK(fxMin == 0);
    CHECK(fyMin == 0);
    CHECK(fxMax == 99);
    CHECK(fyMax == 49);

    const geometrize::ImageRunnerShapeBoundsOptions quarter{true, 50.0, 50.0, 100.0, 100.0};
    const auto [qxMin, qyMin, qxMax, qyMax] = geometrize::commonutil::mapShapeBoundsToImage(quarter, bitmap);
    CHECK(qxMin == 50);
    CHECK(qyMin == 25);
    CHECK(qxMax == 99);
    CHECK(qyMax == 49);

    // 未启用 → 全图闭区间
    const geometrize::ImageRunnerShapeBoundsOptions disabled{false, 10.0, 10.0, 90.0, 90.0};
    const auto [dxMin, dyMin, dxMax, dyMax] = geometrize::commonutil::mapShapeBoundsToImage(disabled, bitmap);
    CHECK(dxMin == 0);
    CHECK(dyMin == 0);
    CHECK(dxMax == 99);
    CHECK(dyMax == 49);
}

TEST_CASE("scanlinesContainTransparentPixels:alpha 阈值语义")
{
    // 4x1 位图:前两个像素不透明,第三个半透明(alpha=100),第四个全透明
    const std::vector<std::uint8_t> data{
        255, 0, 0, 255,   0, 255, 0, 255,   0, 0, 255, 100,   255, 255, 255, 0,
    };
    const geometrize::Bitmap bitmap{4, 1, data};

    const std::vector<geometrize::Scanline> fullLine{geometrize::Scanline{0, 0, 3}};
    CHECK(geometrize::commonutil::scanlinesContainTransparentPixels(fullLine, bitmap, 255));

    // 阈值 100:alpha=100 算不透明;第 4 像素(alpha=0)是否被检测到存在变体分叉
#if defined(GEOTEST_FAST)
    // 改进版闭区间遍历(x <= x2):末像素 alpha=0 < 100 → 检测到
    CHECK(geometrize::commonutil::scanlinesContainTransparentPixels(fullLine, bitmap, 100));
#else
    // 上游半开区间遍历(x < x2):末像素被漏检 → 不检测到
    CHECK_FALSE(geometrize::commonutil::scanlinesContainTransparentPixels(fullLine, bitmap, 100));
#endif

    // 仅覆盖不透明区:不检测到透明
    const std::vector<geometrize::Scanline> opaquePart{geometrize::Scanline{0, 0, 1}};
    CHECK_FALSE(geometrize::commonutil::scanlinesContainTransparentPixels(opaquePart, bitmap, 255));
}
