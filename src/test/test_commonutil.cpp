// 通用工具:clamp、randomRange、getAverageImageColor、mapShapeBoundsToImage、透明像素检测
#include "doctest.h"

#include <cstdint>
#include <tuple>
#include <vector>

#include "geometrize/bitmap/bitmap.h"
#include "geometrize/commonutil.h"
#include "geometrize/rasterizer/rasterizer.h"
#include "geometrize/rasterizer/scanline.h"
#include "geometrize/runner/imagerunneroptions.h"
#include "geometrize/shape/rectangle.h"

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

#if defined(GEOTEST_FAST)
TEST_CASE("mapShapeBoundsToImage:off-by-one 修复路径(C.1.4)")
{
    // 上游语义(默认参数)返回闭区间上界 size-1,而 setup/mutate/rasterize 全程按排他上界消费
    // 该元组(内部一律 max-1 采样、clamp 到 max-1),错配导致画布最右列/最下行永不落画。
    // 修复路径按文档契约返回排他上界:整图 = (0, 0, width, height),百分比按整幅像素跨度换算。
    const geometrize::Bitmap bitmap{100, 50, geometrize::rgba{0, 0, 0, 0}};

    const geometrize::ImageRunnerShapeBoundsOptions full{true, 0.0, 0.0, 100.0, 100.0};
    const auto [fxMin, fyMin, fxMax, fyMax] = geometrize::commonutil::mapShapeBoundsToImage(full, bitmap, true);
    CHECK(fxMin == 0);
    CHECK(fyMin == 0);
    CHECK(fxMax == 100); // 排他上界:像素 x ∈ [0, 99] 全部可落画
    CHECK(fyMax == 50);

    const geometrize::ImageRunnerShapeBoundsOptions quarter{true, 50.0, 50.0, 100.0, 100.0};
    const auto [qxMin, qyMin, qxMax, qyMax] = geometrize::commonutil::mapShapeBoundsToImage(quarter, bitmap, true);
    CHECK(qxMin == 50);
    CHECK(qyMin == 25);
    CHECK(qxMax == 100);
    CHECK(qyMax == 50);

    // 未启用 → 整图排他上界
    const geometrize::ImageRunnerShapeBoundsOptions disabled{false, 10.0, 10.0, 90.0, 90.0};
    const auto [dxMin, dyMin, dxMax, dyMax] = geometrize::commonutil::mapShapeBoundsToImage(disabled, bitmap, true);
    CHECK(dxMin == 0);
    CHECK(dyMin == 0);
    CHECK(dxMax == 100);
    CHECK(dyMax == 50);

    // 退化区域(映射后不足 2px)仍回落到整图,且是排他上界
    const geometrize::ImageRunnerShapeBoundsOptions degenerate{true, 40.0, 40.0, 41.0, 41.0};
    const auto [gxMin, gyMin, gxMax, gyMax] = geometrize::commonutil::mapShapeBoundsToImage(degenerate, bitmap, true);
    CHECK(gxMin == 0);
    CHECK(gyMin == 0);
    CHECK(gxMax == 100);
    CHECK(gyMax == 50);

    // 关键性质:修复后的边界作为排他上界喂给形状工厂时,最右列/最下行确实落在光栅化范围内
    const std::vector<geometrize::Scanline> lines{geometrize::rasterize(
        geometrize::Rectangle{0.0f, 0.0f, static_cast<float>(bitmap.getWidth() - 1), static_cast<float>(bitmap.getHeight() - 1)},
        fxMin, fyMin, fxMax, fyMax)};
    bool reachesLastColumn = false;
    bool reachesLastRow = false;
    for(const geometrize::Scanline& line : lines) {
        reachesLastColumn = reachesLastColumn || (line.x2 == bitmap.getWidth() - 1);
        reachesLastRow = reachesLastRow || (line.y == bitmap.getHeight() - 1);
    }
    CHECK(reachesLastColumn);
    CHECK(reachesLastRow);
}
#endif
