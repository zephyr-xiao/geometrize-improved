// 9 形状光栅化:边界性质、退化输入、越界、宏分流锁定分叉点
#include "doctest.h"

#include <cmath>
#include <cstdint>
#include <memory>

#include "geometrize/bitmap/bitmap.h"
#include "geometrize/commonutil.h"
#include "geometrize/rasterizer/rasterizer.h"
#include "geometrize/rasterizer/scanline.h"
#include "geometrize/shape/circle.h"
#include "geometrize/shape/ellipse.h"
#include "geometrize/shape/line.h"
#include "geometrize/shape/polyline.h"
#include "geometrize/shape/quadraticbezier.h"
#include "geometrize/shape/rectangle.h"
#include "geometrize/shape/rotatedellipse.h"
#include "geometrize/shape/rotatedrectangle.h"
#include "geometrize/shape/triangle.h"

namespace
{

// 光栅化输出的边界性质:y ∈ [yMin, yMax),x ∈ [xMin, xMax-1],x1 ≤ x2
void checkBounds(const std::vector<geometrize::Scanline>& lines, std::int32_t xMin, std::int32_t yMin, std::int32_t xMax, std::int32_t yMax)
{
    for(const auto& line : lines) {
        CHECK(line.y >= yMin);
        CHECK(line.y < yMax);
        CHECK(line.x1 >= xMin);
        CHECK(line.x1 <= line.x2);
        CHECK(line.x2 < xMax);
    }
}

std::size_t countPixels(const std::vector<geometrize::Scanline>& lines)
{
    std::size_t total = 0;
    for(const auto& line : lines) {
        total += static_cast<std::size_t>(line.x2 - line.x1 + 1);
    }
    return total;
}

} // namespace

// Ellipse 非对称边界:改进版修复了上游 `y1 >= xMin` 笔误(应为 yMin)。
// 该笔误在 y1∈[xMin,yMax)∩[yMin,yMax) 才产生可观察差异,需椭圆顶部落在画布上半部
// 且 xMin 不为 0 的场景。用非对称视口(xMin=10)放大差异:
// 上游会因 y1 与 xMin 比较错位而丢行,改进版正确输出。
TEST_CASE("Ellipse 非对称视口分叉点(宏分流锁定)")
{
    const geometrize::Ellipse ellipse{30.0f, 12.0f, 6.0f, 10.0f};
    // 视口 x∈[10, 40):椭圆 y 范围 [2, 22),顶部 y=2 的行在上游被错误丢行
    // (y1=2 >= xMin=10 为假,但 y1 >= yMin=0 为真 → 上游丢弃,改进版保留)
    const auto lines = geometrize::rasterize(ellipse, 10, 0, 40, 24);
    REQUIRE(!lines.empty());
    checkBounds(lines, 10, 0, 40, 24);

#if defined(GEOTEST_FAST)
    // 改进版:顶部行(y = m_y - dy)正确保留 —— 最小 y 应为 ceil 方向可触及的顶部
    std::int32_t minY = INT32_MAX;
    for(const auto& line : lines) {
        minY = (line.y < minY) ? line.y : minY;
    }
    CHECK(minY <= 12); // 椭圆上半个(m_y=12 之上)必须有行存活
#elif defined(GEOTEST_BASE)
    // 上游怪癖:y1 与 xMin 错位比较导致顶部行系统性丢失
    std::int32_t minY = INT32_MAX;
    for(const auto& line : lines) {
        minY = (line.y < minY) ? line.y : minY;
    }
    CHECK(minY >= 10); // y < xMin(=10) 的顶部行全部丢失
#endif
}

TEST_CASE("Circle:居中、面积性质、单像素退化、越界")
{
    SUBCASE("居中圆:闭圆判定 + 面积在 πr² 邻域")
    {
        const geometrize::Circle circle{32.0f, 32.0f, 20.0f};
        const auto lines = geometrize::rasterize(circle, 0, 0, 64, 64);
        CHECK(!lines.empty());
        checkBounds(lines, 0, 0, 64, 64);

        const double area = static_cast<double>(countPixels(lines));
        const double expected = 3.141592653589793 * 20.0 * 20.0;
        CHECK(area > expected * 0.85);
        CHECK(area < expected * 1.15);
    }

    SUBCASE("r=0 单像素退化")
    {
        const geometrize::Circle circle{32.0f, 32.0f, 0.0f};
        const auto lines = geometrize::rasterize(circle, 0, 0, 64, 64);
        REQUIRE(lines.size() == 1U);
        CHECK(lines[0].y == 32);
        CHECK(lines[0].x1 == 32);
        CHECK(lines[0].x2 == 32);
    }

    SUBCASE("圆心完全越界 → 空输出")
    {
        const geometrize::Circle circle{-100, -100, 10.0f};
        CHECK(geometrize::rasterize(circle, 0, 0, 64, 64).empty());
    }

    SUBCASE("负半径 → 空输出(防御性)")
    {
        const geometrize::Circle circle{32.0f, 32.0f, -5.0f};
        CHECK(geometrize::rasterize(circle, 0, 0, 64, 64).empty());
    }
}

TEST_CASE("Rectangle:1x1 退化与越界")
{
    SUBCASE("1x1 矩形 → 2x2 含端点(行区间 [y1,y2] 双端闭合)")
    {
        const geometrize::Rectangle rectangle{10.0f, 10.0f, 11.0f, 11.0f};
        const auto lines = geometrize::rasterize(rectangle, 0, 0, 64, 64);
        REQUIRE(lines.size() == 2U);
        CHECK(countPixels(lines) == 4U);
        checkBounds(lines, 0, 0, 64, 64);
    }

    SUBCASE("完全越界 → 空输出")
    {
        const geometrize::Rectangle rectangle{100.0f, 100.0f, 200.0f, 200.0f};
        CHECK(geometrize::rasterize(rectangle, 0, 0, 64, 64).empty());
    }
}

TEST_CASE("Line:重合端点单像素、对角线")
{
    SUBCASE("重合端点 → 单像素")
    {
        const geometrize::Line line{5.0f, 5.0f, 5.0f, 5.0f};
        const auto lines = geometrize::rasterize(line, 0, 0, 64, 64);
        CHECK(countPixels(lines) == 1U);
    }

    SUBCASE("水平线 → bresenham 逐点单像素行(每 x 一条 [x,x])")
    {
        const geometrize::Line line{4.0f, 8.0f, 20.0f, 8.0f};
        const auto lines = geometrize::rasterize(line, 0, 0, 64, 64);
        REQUIRE(lines.size() == 17U); // x=4..20 逐点
        for(const auto& l : lines) {
            CHECK(l.y == 8);
            CHECK(l.x1 == l.x2);
        }
        CHECK(lines.front().x1 == 4);
        CHECK(lines.back().x1 == 20);
    }
}

TEST_CASE("Triangle:三顶点重合退化、越界顶点裁剪")
{
    SUBCASE("三顶点重合 → 单像素")
    {
        const geometrize::Triangle triangle{7.0f, 7.0f, 7.0f, 7.0f, 7.0f, 7.0f};
        const auto lines = geometrize::rasterize(triangle, 0, 0, 64, 64);
        CHECK(countPixels(lines) == 1U);
    }

    SUBCASE("部分越界:输出仍在视口内")
    {
        const geometrize::Triangle triangle{-10.0f, -10.0f, 80.0f, 30.0f, 30.0f, 80.0f};
        const auto lines = geometrize::rasterize(triangle, 0, 0, 64, 64);
        CHECK(!lines.empty());
        checkBounds(lines, 0, 0, 64, 64);
    }
}

TEST_CASE("RotatedRectangle 与 RotatedEllipse:边界性质")
{
    const geometrize::RotatedRectangle rotatedRectangle{32.0f, 32.0f, 24.0f, 12.0f, 45.0f};
    const auto rectangleLines = geometrize::rasterize(rotatedRectangle, 0, 0, 64, 64);
    CHECK(!rectangleLines.empty());
    checkBounds(rectangleLines, 0, 0, 64, 64);

    const geometrize::RotatedEllipse rotatedEllipse{32.0f, 32.0f, 18.0f, 8.0f, 30.0f};
    const auto ellipseLines = geometrize::rasterize(rotatedEllipse, 0, 0, 64, 64);
    CHECK(!ellipseLines.empty());
    checkBounds(ellipseLines, 0, 0, 64, 64);
}

TEST_CASE("Ellipse:极端长宽比")
{
    // rx=1, ry=40:细高椭圆,每行宽度 ≤ 3 像素但行数多
    const geometrize::Ellipse ellipse{32.0f, 32.0f, 1.0f, 40.0f};
    const auto lines = geometrize::rasterize(ellipse, 0, 0, 64, 64);
    CHECK(!lines.empty());
    checkBounds(lines, 0, 0, 64, 64);
    for(const auto& line : lines) {
        CHECK(line.x2 - line.x1 <= 2); // 半宽 ≤ 1 → 行宽 ≤ 3
    }
}

TEST_CASE("Polyline:两段去重(共享中点不重复输出)")
{
    const geometrize::Polyline polyline{{{4.0f, 4.0f}, {8.0f, 4.0f}, {8.0f, 8.0f}}};
    const auto lines = geometrize::rasterize(polyline, 0, 0, 64, 64);

    // 折线光栅化是不重叠单像素行(每行一条 [x,x]);共享点 (8,4) 只出现一次
    std::size_t pixelCount = 0;
    for(const auto& line : lines) {
        pixelCount += static_cast<std::size_t>(line.x2 - line.x1 + 1);
    }
    // L 形:4→8 水平 5 点 + 4→8 垂直 5 点 - 1 共享 = 9
    CHECK(pixelCount == 9U);
}

TEST_CASE("QuadraticBezier:边界性质")
{
    const geometrize::QuadraticBezier bezier{8.0f, 32.0f, 32.0f, 0.0f, 56.0f, 32.0f};
    const auto lines = geometrize::rasterize(bezier, 0, 0, 64, 64);
    CHECK(!lines.empty());
    checkBounds(lines, 0, 0, 64, 64);
}

TEST_CASE("rasterize 分派:非法类型(Release 语义)返回空")
{
    class BadShape : public geometrize::Shape
    {
    public:
        std::shared_ptr<geometrize::Shape> clone() const override { return nullptr; }
        geometrize::ShapeTypes getType() const override { return static_cast<geometrize::ShapeTypes>(0xDEAD); }
    };

    const BadShape bad;
    // Release(NDEBUG)下 assert 失效,switch 走 default 返回空 vector
    CHECK(geometrize::rasterize(bad, 0, 0, 64, 64).empty());
}

// ---- A2.4 drawLinesSegmented(分段混合,改进版专属) ----
#if defined(GEOTEST_FAST)

TEST_CASE("drawLinesSegmented:两行异色混合,逐像素等于手算预乘公式")
{
    geometrize::Bitmap dst{4, 4, geometrize::rgba{0, 0, 0, 0}};
    const std::vector<geometrize::rgba> colors{geometrize::rgba{200, 100, 50, 128}, geometrize::rgba{10, 20, 30, 255}};
    const std::vector<geometrize::Scanline> lines{geometrize::Scanline{1, 0, 3}, geometrize::Scanline{2, 0, 3}};

    geometrize::drawLinesSegmented(dst, colors, lines);

    // 手算预乘:drawLines 公式,行 1 用 colors[0]、行 2 用 colors[1]
    auto expectBlend = [](const geometrize::rgba d, const geometrize::rgba c) {
        std::uint32_t sr{c.r}; sr |= sr << 8; sr = sr * c.a / 255U;
        std::uint32_t sg{c.g}; sg |= sg << 8; sg = sg * c.a / 255U;
        std::uint32_t sb{c.b}; sb |= sb << 8; sb = sb * c.a / 255U;
        std::uint32_t sa{c.a}; sa |= sa << 8;
        const std::uint32_t m{0xFFFF};
        const std::uint32_t aa{(m - sa) * 257U};
        return geometrize::rgba{
            static_cast<std::uint8_t>(((d.r * aa + sr * m) / m) >> 8),
            static_cast<std::uint8_t>(((d.g * aa + sg * m) / m) >> 8),
            static_cast<std::uint8_t>(((d.b * aa + sb * m) / m) >> 8),
            static_cast<std::uint8_t>(((d.a * aa + sa * m) / m) >> 8)};
    };

    for(std::int32_t x = 0; x < 4; x++) {
        CHECK(dst.getPixel(static_cast<std::uint32_t>(x), 1).r == expectBlend(geometrize::rgba{0, 0, 0, 0}, colors[0]).r);
        CHECK(dst.getPixel(static_cast<std::uint32_t>(x), 1).g == expectBlend(geometrize::rgba{0, 0, 0, 0}, colors[0]).g);
        CHECK(dst.getPixel(static_cast<std::uint32_t>(x), 2).b == expectBlend(geometrize::rgba{0, 0, 0, 0}, colors[1]).b);
        CHECK(dst.getPixel(static_cast<std::uint32_t>(x), 2).a == 255U);
    }
    // 未覆盖行保持原样
    CHECK(dst.getPixel(1, 0).a == 0U);
}

TEST_CASE("drawLinesSegmented:单行逐像素与 drawLines 等价")
{
    const geometrize::Bitmap src{8, 8, geometrize::rgba{90, 120, 150, 200}};
    const geometrize::rgba color{37, 200, 88, 160};
    const std::vector<geometrize::Scanline> lines{geometrize::Scanline{3, 1, 6}};

    geometrize::Bitmap a{src};
    geometrize::drawLines(a, color, lines);

    geometrize::Bitmap b{src};
    geometrize::drawLinesSegmented(b, {color}, lines);

    for(std::int32_t y = 0; y < 8; y++) {
        for(std::int32_t x = 0; x < 8; x++) {
            const auto pa = a.getPixel(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
            const auto pb = b.getPixel(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
            CHECK(pa.r == pb.r);
            CHECK(pa.g == pb.g);
            CHECK(pa.b == pb.b);
            CHECK(pa.a == pb.a);
        }
    }
}

#endif

