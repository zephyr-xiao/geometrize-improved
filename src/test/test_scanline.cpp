// 扫描线:相等运算符、trimScanlines 裁剪语义、凹多边形语义锁定、downscaleScanlines 投影
#include "doctest.h"

#include <cstdint>

#include "geometrize/rasterizer/scanline.h"
#include "geometrize/rasterizer/rasterizer.h"

TEST_CASE("Scanline 相等运算符")
{
    const geometrize::Scanline a{5, 10, 20};
    const geometrize::Scanline b{5, 10, 20};
    const geometrize::Scanline c{6, 10, 20};
    const geometrize::Scanline d{5, 11, 20};
    const geometrize::Scanline e{5, 10, 21};

    CHECK(a == b);
    CHECK(a != c);
    CHECK(a != d);
    CHECK(a != e);
}

TEST_CASE("trimScanlines y 半开区间 [minY, maxY)")
{
    const std::vector<geometrize::Scanline> lines{
        geometrize::Scanline{4, 0, 10},   // y == minY 保留
        geometrize::Scanline{5, 0, 10},   // 中间保留
        geometrize::Scanline{9, 0, 10},   // y == maxY-1 保留
        geometrize::Scanline{10, 0, 10},  // y == maxY 丢弃
        geometrize::Scanline{3, 0, 10},   // y < minY 丢弃
    };

    const auto trimmed = geometrize::trimScanlines(lines, 0, 4, 11, 10);
    REQUIRE(trimmed.size() == 3U);
    CHECK(trimmed[0].y == 4);
    CHECK(trimmed[1].y == 5);
    CHECK(trimmed[2].y == 9);
}

TEST_CASE("trimScanlines x 双端裁剪到 [minX, maxX-1] 与空行丢弃")
{
    const std::vector<geometrize::Scanline> lines{
        geometrize::Scanline{5, -5, 20},   // 两端越界 → clamp 到 [0, 10]
        geometrize::Scanline{6, 8, 3},     // x1 > x2 → 整行丢弃
        geometrize::Scanline{7, 100, 200}, // 完全越界 → clamp 到 [10, 10] 后仍合法,保留
    };

    const auto trimmed = geometrize::trimScanlines(lines, 0, 0, 11, 10);
    REQUIRE(trimmed.size() == 2U);
    CHECK(trimmed[0].y == 5);
    CHECK(trimmed[0].x1 == 0);
    CHECK(trimmed[0].x2 == 10);
    CHECK(trimmed[1].y == 7);
    CHECK(trimmed[1].x1 == 10);
    CHECK(trimmed[1].x2 == 10);
}

TEST_CASE("trimScanlines 保持原顺序")
{
    const std::vector<geometrize::Scanline> lines{
        geometrize::Scanline{8, 0, 10},
        geometrize::Scanline{4, 0, 10},
        geometrize::Scanline{6, 0, 10},
    };
    const auto trimmed = geometrize::trimScanlines(lines, 0, 0, 11, 10);
    REQUIRE(trimmed.size() == 3U);
    CHECK(trimmed[0].y == 8);
    CHECK(trimmed[1].y == 4);
    CHECK(trimmed[2].y == 6);
}

// 凹多边形语义锁定(ROADMAP Q4.1 点名):scanlinesForPolygon 每 y 行输出
// [行内最小 x, 行内最大 x] 的单一扫描线 —— 凹进的部分被"横向淹没"(凸包式逐行填充),
// 不是 even-odd 填充。锁定该行为防止未来被"顺手修复"改变输出。
TEST_CASE("scanlinesForPolygon 凹多边形:凹处被横向填满(语义锁定)")
{
    // C 形凹多边形(顶点顺时针,自动闭合):右中开一口
    //   (0,0) → (10,0) → (10,10) → (0,10) → (0,8) → (8,8) → (8,2) → (0,2) →(闭合)(0,0)
    // y=4 行的轮廓 x 集合 = {10(右外壁), 8(凹口右壁)},输出单一行 [8,10]:
    // 两壁之间(8..10,凹口内无轮廓点的区段)被横向淹没填满,不是 even-odd 剖析内外
    const std::vector<geometrize::Scanline> lines = geometrize::scanlinesForPolygon({
        {0.0f, 0.0f}, {10.0f, 0.0f}, {10.0f, 10.0f}, {0.0f, 10.0f},
        {0.0f, 8.0f}, {8.0f, 8.0f}, {8.0f, 2.0f}, {0.0f, 2.0f},
    });

    REQUIRE(!lines.empty());

    // 每 y 恰一条扫描线
    for(std::size_t i = 0; i < lines.size(); i++) {
        for(std::size_t j = i + 1; j < lines.size(); j++) {
            CHECK(lines[i].y != lines[j].y);
        }
    }

    // y=4(凹口区)行:[x1=8, x2=10] 全填 —— 两壁之间被淹没
    const geometrize::Scanline* midRow = nullptr;
    for(const auto& line : lines) {
        if(line.y == 4) {
            midRow = &line;
        }
    }
    REQUIRE(midRow != nullptr);
    CHECK(midRow->x1 == 8);
    CHECK(midRow->x2 == 10);
}

TEST_CASE("scanlinesForPolygon 基础性质:空顶点、单点、自动闭合")
{
    // 空顶点 → 空输出
    CHECK(geometrize::scanlinesForPolygon({}).empty());

    // 单点 → 自闭合 bresenham 单点 → 一条单像素扫描线
    const auto single = geometrize::scanlinesForPolygon({{3.0f, 4.0f}});
    REQUIRE(single.size() == 1U);
    CHECK(single[0].y == 4);
    CHECK(single[0].x1 == 3);
    CHECK(single[0].x2 == 3);

    // 三角形:输出按 y 升序
    const auto tri = geometrize::scanlinesForPolygon({{0.0f, 0.0f}, {10.0f, 0.0f}, {5.0f, 10.0f}});
    REQUIRE(!tri.empty());
    for(std::size_t i = 1; i < tri.size(); i++) {
        CHECK(tri[i - 1].y < tri[i].y);
    }
}

// 金字塔轨道核心纯函数(改进版专属,baseline 变体无此符号):全分辨率扫描线 →
// 半分辨率坐标投影(floor 映射、行对合并、x 取并、同行相交合并、trim)。
// 语义锁定防止实现被"顺手改"破坏保守超覆盖约定。
#if defined(GEOTEST_FAST)
TEST_CASE("downscaleScanlines 行对合并与 x 取并(floor 映射)")
{
    // 相邻行 4,5 → 半分辨率行 2;x [10,20]+[12,25] 取并 [10,25] → [5,12]
    const std::vector<geometrize::Scanline> pairRows{
        geometrize::Scanline{4, 10, 20},
        geometrize::Scanline{5, 12, 25},
    };
    const auto merged = geometrize::downscaleScanlines(pairRows, 32, 32);
    REQUIRE(merged.size() == 1U);
    CHECK(merged[0].y == 2);
    CHECK(merged[0].x1 == 5);
    CHECK(merged[0].x2 == 12);
}

TEST_CASE("downscaleScanlines 奇数尾行独立成行")
{
    // y=4 无 5 配对 → 独立行 2;y=6/7 → 行 3
    const std::vector<geometrize::Scanline> oddTail{
        geometrize::Scanline{4, 8, 9},
        geometrize::Scanline{6, 0, 3},
        geometrize::Scanline{7, 2, 7},
    };
    const auto merged = geometrize::downscaleScanlines(oddTail, 8, 8);
    REQUIRE(merged.size() == 2U);
    CHECK(merged[0].y == 2);
    CHECK(merged[0].x1 == 4);
    CHECK(merged[0].x2 == 4);
    CHECK(merged[1].y == 3);
    CHECK(merged[1].x1 == 0);
    CHECK(merged[1].x2 == 3);
}

TEST_CASE("downscaleScanlines 同行相交区间合并(防重复覆盖)与 trim")
{
    // Line 形状同一源行多条退化扫描线:0 与 1 折叠后 [0,1]+[1,2] 相交 → 合并 [0,2];
    // [3,4] 与之不相交独立成行。trim 到 halfWidth=2 后:[0,2]→[0,1]、[3,4]→[1,1]
    const std::vector<geometrize::Scanline> degenerate{
        geometrize::Scanline{0, 0, 2},
        geometrize::Scanline{0, 2, 4},
        geometrize::Scanline{0, 6, 9},
    };
    const auto merged = geometrize::downscaleScanlines(degenerate, 2, 4);
    REQUIRE(merged.size() == 2U);
    CHECK(merged[0].y == 0);
    CHECK(merged[0].x1 == 0);
    CHECK(merged[0].x2 == 1);
    CHECK(merged[1].x1 == 1);
    CHECK(merged[1].x2 == 1);
}

TEST_CASE("downscaleScanlines 空输入与空输出")
{
    CHECK(geometrize::downscaleScanlines({}, 8, 8).empty());

    // 全部行都在位图外(y >= 2*halfHeight)→ trim 后为空
    const std::vector<geometrize::Scanline> outOfRange{
        geometrize::Scanline{100, 0, 2},
    };
    CHECK(geometrize::downscaleScanlines(outOfRange, 4, 4).empty());
}
#endif

