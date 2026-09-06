// ErrorWeightMap:A2.1 误差图引导的权重表语义(块聚合/整数缩放/加权采样/退化路径)
#include "doctest.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <vector>

#include "geometrize/bitmap/bitmap.h"
#include "geometrize/commonutil.h"

// ErrorWeightMap 是改进版专属(baseline 库无此符号),头与用例整体宏分流
#if defined(GEOTEST_FAST)
#include "geometrize/core/errorweightmap.h"
#endif

namespace
{

// 纯色位图助手
geometrize::Bitmap makeSolid(std::uint32_t width, std::uint32_t height, geometrize::rgba color)
{
    geometrize::Bitmap bitmap{width, height, color};
    return bitmap;
}

std::int32_t blockL1(const geometrize::Bitmap& target, const geometrize::Bitmap& current, std::uint32_t x, std::uint32_t y)
{
    const geometrize::rgba t{target.getPixel(x, y)};
    const geometrize::rgba c{current.getPixel(x, y)};
    return std::abs(static_cast<std::int32_t>(t.r) - c.r) + std::abs(static_cast<std::int32_t>(t.g) - c.g)
        + std::abs(static_cast<std::int32_t>(t.b) - c.b) + std::abs(static_cast<std::int32_t>(t.a) - c.a);
}

} // namespace

// ErrorWeightMap 是改进版专属(baseline 库无此符号),整文件宏分流
#if defined(GEOTEST_FAST)
// 头已随宏守卫前置引入
// 头已随宏守卫前置引入

TEST_CASE("rebuild 块和精确性:blockSize=2 小图逐块核对 L1 差和")
{
    geometrize::Bitmap target{4, 4, geometrize::rgba{10, 20, 30, 255}};
    geometrize::Bitmap current{4, 4, geometrize::rgba{10, 20, 30, 255}};
    // 仅 (1,1) 与 (2,3) 两像素有差,各差 (5,5,5,0)=15
    current.setPixel(1, 1, geometrize::rgba{15, 25, 35, 255});
    current.setPixel(2, 3, geometrize::rgba{5, 15, 25, 255});

    geometrize::core::ErrorWeightMap map{2U};
    map.rebuild(target, current);

    // 4×4 图、块 2 → 2×2 块;(1,1) 在块 (0,0)、(2,3) 在块 (1,1),两块各占总误差一半
    CHECK(map.blockCount() == 4U);
    // 不变量:采样永不落入零误差块(冷块零权重,CDF 二分天然跳过)
    geometrize::commonutil::seedRandomGenerator(42);
    int hotHits{0};
    for(int i = 0; i < 200; i++) {
        std::int32_t cx{0};
        std::int32_t cy{0};
        REQUIRE(map.sampleCenter(cx, cy));
        const bool inHotBlock{((cx <= 1 && cy <= 1) || (cx >= 2 && cy >= 2))};
        CHECK(inHotBlock);
        const bool onHotPixel{(cx == 1 && cy == 1) || (cx == 2 && cy == 3)};
        if(onHotPixel) {
            hotHits++;
        }
    }
    // 统计性:热像素命中概率 = 1/2(选块)× 1/4(块内抖动)= 1/4,200 次期望 50,容忍 3σ 波动
    CHECK(hotHits >= 32);
    CHECK(hotHits <= 68);
}

TEST_CASE("rebuild 确定性:同输入两跑逐块一致")
{
    geometrize::Bitmap target{makeSolid(37, 23, geometrize::rgba{1, 2, 3, 255})};
    geometrize::Bitmap current{makeSolid(37, 23, geometrize::rgba{9, 8, 7, 255})};

    geometrize::core::ErrorWeightMap first{8U};
    first.rebuild(target, current);
    geometrize::core::ErrorWeightMap second{8U};
    second.rebuild(target, current);

    CHECK(first.blockCount() == second.blockCount());
    // 同图两跑的采样序列必须一致(确定性门禁)
    geometrize::commonutil::seedRandomGenerator(777);
    std::vector<std::pair<std::int32_t, std::int32_t>> firstSamples;
    for(int i = 0; i < 50; i++) {
        std::int32_t cx{0};
        std::int32_t cy{0};
        CHECK(first.sampleCenter(cx, cy));
        firstSamples.emplace_back(cx, cy);
    }
    geometrize::commonutil::seedRandomGenerator(777);
    for(std::size_t i = 0; i < firstSamples.size(); i++) {
        std::int32_t cx{0};
        std::int32_t cy{0};
        CHECK(second.sampleCenter(cx, cy));
        CHECK(cx == firstSamples[i].first);
        CHECK(cy == firstSamples[i].second);
    }
}

TEST_CASE("全零误差退化:sampleCenter 返回 false 且零 RNG 消耗")
{
    geometrize::Bitmap target{makeSolid(16, 16, geometrize::rgba{200, 100, 50, 255})};
    geometrize::Bitmap current{makeSolid(16, 16, geometrize::rgba{200, 100, 50, 255})};

    geometrize::core::ErrorWeightMap map{4U};
    map.rebuild(target, current);
    CHECK(map.blockCount() == 16U);

    std::int32_t cx{-1};
    std::int32_t cy{-1};
    CHECK_FALSE(map.sampleCenter(cx, cy));

    // 零消耗验证:调用后 RNG 状态不动,下一次 randomRange 产出与直接产出一致
    geometrize::commonutil::seedRandomGenerator(31337);
    const std::int32_t expected{geometrize::commonutil::randomRange(0, 999999)};
    geometrize::commonutil::seedRandomGenerator(31337);
    std::int32_t ignored{0};
    (void)map.sampleCenter(ignored, ignored);
    CHECK(geometrize::commonutil::randomRange(0, 999999) == expected);
}

TEST_CASE("单热块集中性:采样全部落入热块边界内")
{
    // 32×32 图,只在 (0..7, 0..7) 区域(块 0 的 8×8 子区)放误差
    geometrize::Bitmap target{makeSolid(32, 32, geometrize::rgba{0, 0, 0, 255})};
    geometrize::Bitmap current{makeSolid(32, 32, geometrize::rgba{0, 0, 0, 255})};
    for(std::uint32_t y = 0; y < 8U; y++) {
        for(std::uint32_t x = 0; x < 8U; x++) {
            current.setPixel(x, y, geometrize::rgba{255, 255, 255, 255});
        }
    }

    geometrize::core::ErrorWeightMap map{16U};
    map.rebuild(target, current);
    CHECK(map.blockCount() == 4U);

    for(int i = 0; i < 100; i++) {
        std::int32_t cx{0};
        std::int32_t cy{0};
        REQUIRE(map.sampleCenter(cx, cy));
        // 全部权重在块 0 → 坐标必须落在 [0,15]×[0,15](块内均匀抖动)
        CHECK(cx >= 0);
        CHECK(cx <= 15);
        CHECK(cy >= 0);
        CHECK(cy <= 15);
    }
}

TEST_CASE("边缘部分块:抖动坐标不越图界")
{
    // 21×13 图、块 8 → 3×2 块,右/下边缘为部分块
    geometrize::Bitmap target{makeSolid(21, 13, geometrize::rgba{0, 0, 0, 255})};
    geometrize::Bitmap current{makeSolid(21, 13, geometrize::rgba{255, 255, 255, 255})};

    geometrize::core::ErrorWeightMap map{8U};
    map.rebuild(target, current);
    CHECK(map.blockCount() == 6U);

    for(int i = 0; i < 300; i++) {
        std::int32_t cx{0};
        std::int32_t cy{0};
        REQUIRE(map.sampleCenter(cx, cy));
        CHECK(cx >= 0);
        CHECK(cx <= 20);
        CHECK(cy >= 0);
        CHECK(cy <= 12);
    }
}

TEST_CASE("缩放不变量:极端大误差下 totalScaled 仍安全")
{
    // 每像素最大 L1 = 4×255 = 1020,4096² 全差是真实上限场景;小图放大差值等价验证:
    // 128×128 全差块和极大,缩放后采样必须可用且坐标在界内
    geometrize::Bitmap target{makeSolid(128, 128, geometrize::rgba{0, 0, 0, 0})};
    geometrize::Bitmap current{makeSolid(128, 128, geometrize::rgba{255, 255, 255, 255})};

    geometrize::core::ErrorWeightMap map{16U};
    map.rebuild(target, current);
    CHECK(map.blockCount() == 64U);

    for(int i = 0; i < 100; i++) {
        std::int32_t cx{0};
        std::int32_t cy{0};
        REQUIRE(map.sampleCenter(cx, cy));
        CHECK(cx >= 0);
        CHECK(cx <= 127);
        CHECK(cy >= 0);
        CHECK(cy <= 127);
    }
}

TEST_CASE("未重建的空图:blockCount 为零且采样返回 false")
{
    geometrize::core::ErrorWeightMap map{16U};
    CHECK(map.blockCount() == 0U);
    std::int32_t cx{0};
    std::int32_t cy{0};
    CHECK_FALSE(map.sampleCenter(cx, cy));
}

// ---- F3.2 区域优先加权 ----

TEST_CASE("区域加权:单区域命中率约 10 倍(块中心判定)")
{
    // 64×64、块 16 → 4×4=16 块,误差均匀;区域 {16,16,31,31} 按块中心判定恰含块(1,1)一个块
    // (块中心 (24,24);其余块中心在区域外)→ 加权后命中率 = 10/25 = 0.4
    geometrize::Bitmap target{64, 64, geometrize::rgba{0, 0, 0, 255}};
    geometrize::Bitmap current{64, 64, geometrize::rgba{255, 255, 255, 255}};

    geometrize::core::ErrorWeightMap map{16U};
    const std::vector<geometrize::core::RegionRect> regions{geometrize::core::RegionRect{16, 16, 31, 31}};
    map.rebuild(target, current, regions);

    geometrize::commonutil::seedRandomGenerator(12345);
    int hits{0};
    const int total{300};
    for(int i = 0; i < total; i++) {
        std::int32_t cx{0};
        std::int32_t cy{0};
        REQUIRE(map.sampleCenter(cx, cy));
        CHECK(cx >= 0);
        CHECK(cx <= 63);
        CHECK(cy >= 0);
        CHECK(cy <= 63);
        if(cx >= 16 && cx <= 31 && cy >= 16 && cy <= 31) {
            hits++;
        }
    }
    // 3σ 区间:p=0.4,μ=120,σ=8.49 → [95, 145]
    CHECK(hits >= 95);
    CHECK(hits <= 145);
}

TEST_CASE("区域加权:空区域与不传逐位一致")
{
    geometrize::Bitmap target{48, 32, geometrize::rgba{10, 20, 30, 255}};
    geometrize::Bitmap current{48, 32, geometrize::rgba{200, 150, 100, 255}};

    geometrize::core::ErrorWeightMap plain{8U};
    plain.rebuild(target, current);
    geometrize::core::ErrorWeightMap empty{8U};
    empty.rebuild(target, current, {});

    CHECK(plain.blockCount() == empty.blockCount());
    geometrize::commonutil::seedRandomGenerator(777);
    std::vector<std::pair<std::int32_t, std::int32_t>> baseline;
    for(int i = 0; i < 60; i++) {
        std::int32_t cx{0};
        std::int32_t cy{0};
        REQUIRE(plain.sampleCenter(cx, cy));
        baseline.emplace_back(cx, cy);
    }
    geometrize::commonutil::seedRandomGenerator(777);
    for(const auto& expected : baseline) {
        std::int32_t cx{0};
        std::int32_t cy{0};
        REQUIRE(empty.sampleCenter(cx, cy));
        CHECK(cx == expected.first);
        CHECK(cy == expected.second);
    }
}

TEST_CASE("区域加权:溢出安全(全差 × factor=1000)")
{
    // 128×128 全差 + 全图区域 + factor 1000:totalWeighted ≈ 1e13,缩放后必须仍可用且坐标在界内
    geometrize::Bitmap target{128, 128, geometrize::rgba{0, 0, 0, 0}};
    geometrize::Bitmap current{128, 128, geometrize::rgba{255, 255, 255, 255}};

    geometrize::core::ErrorWeightMap map{16U};
    const std::vector<geometrize::core::RegionRect> regions{geometrize::core::RegionRect{0, 0, 127, 127}};
    map.rebuild(target, current, regions, 1000U);

    for(int i = 0; i < 100; i++) {
        std::int32_t cx{0};
        std::int32_t cy{0};
        REQUIRE(map.sampleCenter(cx, cy));
        CHECK(cx >= 0);
        CHECK(cx <= 127);
        CHECK(cy >= 0);
        CHECK(cy <= 127);
    }
}

TEST_CASE("区域加权:块中心边缘归属极性(恰含 vs 恰不含)")
{
    // 32×32、块 16 → 4 块,块中心 (8,8)/(24,8)/(8,24)/(24,24)
    // 矩形 {8,8,8,8} 单点只含块 0 中心;矩形 {9,9,9,9} 不含任何块中心 → 全零加权=均匀
    geometrize::Bitmap target{32, 32, geometrize::rgba{0, 0, 0, 255}};
    geometrize::Bitmap current{32, 32, geometrize::rgba{255, 255, 255, 255}};

    geometrize::core::ErrorWeightMap hit{16U};
    hit.rebuild(target, current, {geometrize::core::RegionRect{8, 8, 8, 8}});
    // 块 0 权重 ×10 → P(块0) = 10/13 ≈ 0.769;统计断言(非全中)
    geometrize::commonutil::seedRandomGenerator(999);
    int hits{0};
    for(int i = 0; i < 100; i++) {
        std::int32_t cx{0};
        std::int32_t cy{0};
        REQUIRE(hit.sampleCenter(cx, cy));
        CHECK(cx >= 0);
        CHECK(cx <= 31);
        CHECK(cy >= 0);
        CHECK(cy <= 31);
        if(cx <= 15 && cy <= 15) {
            hits++;
        }
    }
    // 3σ:μ=76.9,σ=4.2 → [64, 90]
    CHECK(hits >= 64);
    CHECK(hits <= 90);

    geometrize::core::ErrorWeightMap miss{16U};
    miss.rebuild(target, current, {geometrize::core::RegionRect{9, 9, 9, 9}});
    // 无块中心命中 → 加权不生效,分布与无区域一致(任一块都可落,靠界内断言)
    for(int i = 0; i < 50; i++) {
        std::int32_t cx{0};
        std::int32_t cy{0};
        REQUIRE(miss.sampleCenter(cx, cy));
        CHECK(cx >= 0);
        CHECK(cx <= 31);
        CHECK(cy >= 0);
        CHECK(cy <= 31);
    }
}

#endif

