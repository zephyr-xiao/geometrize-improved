// 核心能量函数与颜色计算:差分一致性、确定性
#include "doctest.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "geometrize/bitmap/bitmap.h"
#include "geometrize/bitmap/rgba.h"
#include "geometrize/commonutil.h"
#include "geometrize/core.h"
#include "geometrize/exporter/shapejsonexporter.h"
#include "geometrize/exporter/svgexporter.h"
#include "geometrize/shape/ellipse.h"
#include "geometrize/rasterizer/rasterizer.h"
#include "geometrize/rasterizer/scanline.h"
#include "geometrize/shape/shapefactory.h"
#include "geometrize/shaperesult.h"

namespace
{

geometrize::Bitmap makeGradientBitmap(std::uint32_t width, std::uint32_t height)
{
    std::vector<std::uint8_t> data(width * height * 4U);
    for(std::uint32_t y = 0; y < height; y++) {
        for(std::uint32_t x = 0; x < width; x++) {
            const std::size_t offset{(static_cast<std::size_t>(width) * y + x) * 4U};
            data[offset] = static_cast<std::uint8_t>(x * 255U / (width - 1U));
            data[offset + 1U] = static_cast<std::uint8_t>(y * 255U / (height - 1U));
            data[offset + 2U] = static_cast<std::uint8_t>((x + y) * 255U / (width + height - 2U));
            data[offset + 3U] = 255U;
        }
    }
    return geometrize::Bitmap{width, height, std::move(data)};
}

} // namespace

TEST_CASE("differenceFull:同图零分")
{
    const auto bitmap = makeGradientBitmap(16, 16);
    CHECK(geometrize::core::differenceFull(bitmap, bitmap) == 0.0);
}

TEST_CASE("differenceFull:全黑白图分数为 1")
{
    const geometrize::Bitmap black{8, 8, geometrize::rgba{0, 0, 0, 0}};
    const geometrize::Bitmap white{8, 8, geometrize::rgba{255, 255, 255, 255}};

    // 每通道差 255 → dr²+dg²+db²+da² = 4×255²,除以 (8·8·4) 开方 /255 = 1
    const double score = geometrize::core::differenceFull(black, white);
    CHECK(score == doctest::Approx(1.0).epsilon(1e-12));
}

TEST_CASE("differencePartial 与 differenceFull 全覆盖重算一致")
{
    const auto target = makeGradientBitmap(16, 16);
    const auto before = makeGradientBitmap(16, 16);
    auto after = before;

    // 全图覆盖的扫描线集
    std::vector<geometrize::Scanline> lines;
    for(std::int32_t y = 0; y < 16; y++) {
        lines.emplace_back(y, 0, 15);
    }

    geometrize::drawLines(after, geometrize::rgba{128, 64, 32, 255}, lines);

    const double baseScore = geometrize::core::differenceFull(target, before);
    const double partial = geometrize::core::differencePartial(target, before, after, baseScore, lines);
    const double full = geometrize::core::differenceFull(target, after);

    // 全覆盖时 partial 就是 full 的增量重算,二者应逐位一致
    CHECK(partial == full);
}

TEST_CASE("computeColor:纯色区域 alpha=255 得区域平均色")
{
    const geometrize::Bitmap target{8, 8, geometrize::rgba{200, 100, 50, 255}};
    const geometrize::Bitmap current{8, 8, geometrize::rgba{0, 0, 0, 255}};

    const std::vector<geometrize::Scanline> lines{geometrize::Scanline{0, 0, 7}};
    const geometrize::rgba color = geometrize::core::computeColor(target, current, lines, 255);

    // alpha=255 时 r/g/b 收敛到目标平均(全图纯色即 200/100/50)
    CHECK(color.r == 200);
    CHECK(color.g == 100);
    CHECK(color.b == 50);
}

TEST_CASE("copyLines 与 drawLines:字节级效果")
{
    geometrize::Bitmap dst{4, 4, geometrize::rgba{0, 0, 0, 0}};
    const geometrize::Bitmap src{4, 4, geometrize::rgba{11, 22, 33, 44}};

    const std::vector<geometrize::Scanline> lines{geometrize::Scanline{1, 0, 3}};

    geometrize::copyLines(dst, src, lines);
    for(std::int32_t x = 0; x < 4; x++) {
        const geometrize::rgba pixel = dst.getPixel(static_cast<std::uint32_t>(x), 1);
        CHECK(pixel.r == 11);
        CHECK(pixel.g == 22);
        CHECK(pixel.b == 33);
        CHECK(pixel.a == 44);
    }

    // drawLines 不透明色直接覆盖(RGB 预计算公式在 alpha=255 时收敛到源色)
    geometrize::drawLines(dst, geometrize::rgba{200, 200, 200, 255}, lines);
    const geometrize::rgba drawn = dst.getPixel(2, 1);
    CHECK(drawn.r == 200);
    CHECK(drawn.g == 200);
    CHECK(drawn.b == 200);
    CHECK(drawn.a == 255);
}

TEST_CASE("固定种子:RNG 序列确定(randomRange)")
{
    geometrize::commonutil::seedRandomGenerator(12345);
    const std::int32_t first = geometrize::commonutil::randomRange(0, 1000);
    const std::int32_t second = geometrize::commonutil::randomRange(0, 1000);

    geometrize::commonutil::seedRandomGenerator(12345);
    CHECK(geometrize::commonutil::randomRange(0, 1000) == first);
    CHECK(geometrize::commonutil::randomRange(0, 1000) == second);
    (void)second;
}

// bestHillClimbStateEnhanced 是改进版专属(算法增强轨道),宏分流
#if defined(GEOTEST_FAST)
namespace
{

std::string stateShapeJson(const geometrize::State& state)
{
    std::vector<geometrize::ShapeResult> results;
    results.emplace_back(geometrize::ShapeResult{state.m_score, geometrize::rgba{0, 0, 0, state.m_alpha}, state.m_shape});
    return geometrize::exporter::exportShapeJson(results);
}

} // namespace

TEST_CASE("T14 bestHillClimbStateEnhanced 确定性:同 seed 两调用输出全等")
{
    const auto target = makeGradientBitmap(48, 48);
    const auto runOnce = [target](const geometrize::core::HillClimbEnhancements& enhancements) {
        geometrize::commonutil::seedRandomGenerator(606);
        geometrize::Bitmap current{target.getWidth(), target.getHeight(), geometrize::commonutil::getAverageImageColor(target)};
        geometrize::Bitmap buffer{current};
        const auto creator = geometrize::createDefaultShapeCreator(geometrize::ShapeTypes::ELLIPSE, 0, 0, 47, 47);
        return geometrize::core::bestHillClimbStateEnhanced(creator, 128, 4, 20, target, current, buffer, 0.5, 0, 0, enhancements);
    };

    // shift 机制(仅自适应步长)
    geometrize::core::HillClimbEnhancements adaptiveOnly;
    adaptiveOnly.adaptiveStep = true;
    const auto s1 = runOnce(adaptiveOnly);
    const auto s2 = runOnce(adaptiveOnly);
    CHECK(stateShapeJson(s1) == stateShapeJson(s2));
    CHECK(s1.m_score == s2.m_score);

    // tier 机制(仅 alpha 搜索)
    geometrize::core::HillClimbEnhancements alphaOnly;
    alphaOnly.alphaSearch = true;
    const auto a1 = runOnce(alphaOnly);
    const auto a2 = runOnce(alphaOnly);
    CHECK(stateShapeJson(a1) == stateShapeJson(a2));
    CHECK(a1.m_score == a2.m_score);
}

TEST_CASE("T15 alpha 搜索:胜者档必须来自候选档位集")
{
    const auto target = makeGradientBitmap(48, 48);
    geometrize::commonutil::seedRandomGenerator(909);
    geometrize::Bitmap current{target.getWidth(), target.getHeight(), geometrize::commonutil::getAverageImageColor(target)};
    geometrize::Bitmap buffer{current};
    const auto creator = geometrize::createDefaultShapeCreator(geometrize::ShapeTypes::ELLIPSE, 0, 0, 47, 47);

    geometrize::core::HillClimbEnhancements enhancements;
    enhancements.alphaSearch = true;
    enhancements.alphaCandidates = {64U, 128U, 192U};
    // 用户 alpha 取 1(近乎全透明):在高对比 target 上不可能是最优档。
    // 旧断言把 userAlpha 也算作"合格档",于是档位穷举完全失效(原样回吐用户 alpha)时照样通过。
    const std::uint8_t userAlpha = 1U;

    const auto state = geometrize::core::bestHillClimbStateEnhanced(creator, userAlpha, 4, 20, target, current, buffer, 0.5, 0, 0, enhancements);
    const bool inCandidates = (state.m_alpha == 64U) || (state.m_alpha == 128U) || (state.m_alpha == 192U);
    CHECK(inCandidates);
    CHECK(state.m_alpha != userAlpha);
}

// ---- A2.4 分段颜色纯函数 ----

TEST_CASE("computeSegmentColors:横跨明暗边界逐行收敛,单色则发灰")
{
    // target 上半黑下半白,current 纯灰;整宽扫描线若干行
    const std::uint32_t width{16U};
    const std::uint32_t height{8U};
    geometrize::Bitmap target{width, height, geometrize::rgba{0, 0, 0, 255}};
    for(std::uint32_t y = height / 2U; y < height; y++) {
        for(std::uint32_t x = 0U; x < width; x++) {
            target.setPixel(x, y, geometrize::rgba{255, 255, 255, 255});
        }
    }
    const geometrize::Bitmap current{width, height, geometrize::rgba{128, 128, 128, 255}};

    std::vector<geometrize::Scanline> lines;
    for(std::int32_t y = 0; y < 8; y++) {
        lines.emplace_back(y, 0, 15);
    }

    const auto segmented = geometrize::core::computeSegmentColors(target, current, lines, 255);
    REQUIRE(segmented.size() == lines.size());

    // 每行独立取色收敛到该行目标色(alpha=255);整形状平均则得中间灰——锁定动机缺陷与修复
    for(std::size_t i = 0; i < segmented.size(); i++) {
        const std::uint8_t expected = (i < segmented.size() / 2U) ? 0U : 255U;
        CHECK(segmented[i].r == expected);
        CHECK(segmented[i].g == expected);
        CHECK(segmented[i].b == expected);
        CHECK(segmented[i].a == 255U);
    }
    const geometrize::rgba single = geometrize::core::computeColor(target, current, lines, 255);
    // 整形状平均被迫取中间灰(上半黑下半白各 64 像素,整数均值 127)
    CHECK(single.r == 127);
}

TEST_CASE("computeSegmentColors:单行输入与 computeColor 一致")
{
    const auto target = makeGradientBitmap(16, 16);
    geometrize::Bitmap current = makeGradientBitmap(16, 16);
    for(std::uint32_t y = 0U; y < 16U; y++) {
        for(std::uint32_t x = 0U; x < 16U; x++) {
            const std::uint8_t v = static_cast<std::uint8_t>((x * 7U + y * 3U) % 256U);
            current.setPixel(x, y, geometrize::rgba{v, v, v, 255});
        }
    }

    const std::vector<geometrize::Scanline> lines{geometrize::Scanline{5, 2, 13}};
    const auto segmented = geometrize::core::computeSegmentColors(target, current, lines, 128);
    REQUIRE(segmented.size() == 1U);
    const geometrize::rgba single = geometrize::core::computeColor(target, current, lines, 128);
    CHECK(segmented[0].r == single.r);
    CHECK(segmented[0].g == single.g);
    CHECK(segmented[0].b == single.b);
    CHECK(segmented[0].a == single.a);
}

TEST_CASE("computeSegmentColors:线型退化返回空、空输入返回空")
{
    const geometrize::Bitmap target{8, 8, geometrize::rgba{200, 100, 50, 255}};
    const geometrize::Bitmap current{8, 8, geometrize::rgba{0, 0, 0, 255}};

    // 全单像素行 = 线型形状(Line/Polyline/Bezier 的光栅化形态)
    const std::vector<geometrize::Scanline> lineLike{geometrize::Scanline{1, 3, 3}, geometrize::Scanline{2, 4, 4}};
    CHECK(geometrize::core::computeSegmentColors(target, current, lineLike, 128).empty());

    const std::vector<geometrize::Scanline> empty;
    CHECK(geometrize::core::computeSegmentColors(target, current, empty, 128).empty());
}

TEST_CASE("isLineLikeShape:单像素行组为真、含宽行为假")
{
    const std::vector<geometrize::Scanline> lineLike{geometrize::Scanline{1, 3, 3}, geometrize::Scanline{2, 4, 4}};
    CHECK(geometrize::core::isLineLikeShape(lineLike));

    const std::vector<geometrize::Scanline> areaLike{geometrize::Scanline{1, 3, 3}, geometrize::Scanline{2, 4, 9}};
    CHECK_FALSE(geometrize::core::isLineLikeShape(areaLike));

    const std::vector<geometrize::Scanline> empty;
    CHECK_FALSE(geometrize::core::isLineLikeShape(empty));
}

TEST_CASE("defaultEnergyFunctionSegmented:线型退化与 defaultEnergyFunction 逐位一致")
{
    const auto target = makeGradientBitmap(24, 24);
    const geometrize::Bitmap current{24, 24, geometrize::rgba{60, 60, 60, 255}};
    geometrize::Bitmap bufferA{current};
    geometrize::Bitmap bufferB{current};

    const std::vector<geometrize::Scanline> lineLike{geometrize::Scanline{3, 5, 5}, geometrize::Scanline{4, 6, 6}, geometrize::Scanline{5, 7, 7}};
    const double classic = geometrize::core::defaultEnergyFunction(lineLike, 128, target, current, bufferA, 0.4);
    const double segmented = geometrize::core::defaultEnergyFunctionSegmented(lineLike, 128, target, current, bufferB, 0.4);
    CHECK(classic == segmented);
    // buffer 混合结果也应逐位一致(线型走同一条单色路径)
    for(std::int32_t y = 0; y < 24; y++) {
        for(std::int32_t x = 0; x < 24; x++) {
            CHECK(bufferA.getPixel(x, y).r == bufferB.getPixel(x, y).r);
        }
    }
}

#if defined(GEOTEST_FAST)

// 融合实现(defaultEnergyFunctionFused)与逐遍实现(computeColor+copyLines+drawLines+differencePartial)
// 在位精确口径下等价,且完全不写 scratch buffer(库内热路径据此传空位图)。
// 契约:扫描线落在图像范围内——库内光栅化保证(第十五批已实证 clipScanlinesToBitmap 对内置形状是恒等);
// 越界行/列在逐遍实现里是未定义读取,不属对照范围。
TEST_CASE("defaultEnergyFunctionFused 与逐遍实现逐位一致且不写 buffer")
{
    const auto target = makeGradientBitmap(32, 24);

    // current 用逐像素花纹(通道值互相纠缠),让混合公式的整数运算与回绕语义充分暴露
    std::vector<std::uint8_t> currentData(32U * 24U * 4U);
    for(std::uint32_t y = 0; y < 24; y++) {
        for(std::uint32_t x = 0; x < 32; x++) {
            const std::size_t offset{(static_cast<std::size_t>(32U) * y + x) * 4U};
            currentData[offset] = static_cast<std::uint8_t>((x * 7U + y * 13U) & 0xFFU);
            currentData[offset + 1U] = static_cast<std::uint8_t>((x * 29U + y * 3U) & 0xFFU);
            currentData[offset + 2U] = static_cast<std::uint8_t>((x * y * 5U) & 0xFFU);
            currentData[offset + 3U] = static_cast<std::uint8_t>(200U - ((x + y) % 60U));
        }
    }
    const geometrize::Bitmap current{32, 24, currentData};

    const std::vector<std::vector<geometrize::Scanline>> cases{
        {geometrize::Scanline{2, 4, 9}, geometrize::Scanline{3, 0, 31}, geometrize::Scanline{20, 7, 12}}, // 面状
        {geometrize::Scanline{5, 6, 6}, geometrize::Scanline{6, 7, 7}, geometrize::Scanline{7, 8, 8}},    // 线型
        {geometrize::Scanline{-3, 4, 9}, geometrize::Scanline{4, 3, 5}},                                 // 含 y<0 行(两侧同样跳过)
        {}                                                                                               // 空扫描线
    };
    const std::vector<std::uint32_t> alphas{1U, 128U, 255U};
    const std::vector<double> scores{0.0, 0.37, 0.9};

    for(const auto& lines : cases) {
        for(const std::uint32_t alpha : alphas) {
            for(const double score : scores) {
                geometrize::Bitmap buffer{current};
                const double classic = geometrize::core::defaultEnergyFunction(lines, alpha, target, current, buffer, score);

                geometrize::Bitmap sentinel{current};
                const double fused = geometrize::core::defaultEnergyFunctionFused(lines, alpha, target, current, sentinel, score);

                CHECK(fused == classic); // double 逐位相等(非 Approx)
                CHECK(sentinel.getDataRef() == current.getDataRef()); // 融合实现不写 buffer
            }
        }
    }
}

#endif

// ---- A2.4 SVG/JSON 分段导出 ----

TEST_CASE("SVG 分段导出:色带组替代基元,segments 空时与单色逐字节一致")
{
    const geometrize::Ellipse ellipse{24.0f, 24.0f, 10.0f, 8.0f};
    const geometrize::rgba color{100, 120, 140, 128};
    geometrize::exporter::SVGExportOptions options;
    options.itemId = 7;

    const std::string plain = geometrize::exporter::getSingleShapeSVGData(color, ellipse, options);

    // 空 segments:与单色重载逐字节一致(线型/关闭开关路径)
    const std::string empty = geometrize::exporter::getSingleShapeSVGData(color, ellipse, {}, options);
    CHECK(empty == plain);

    // 非空 segments:色带组输出
    std::vector<geometrize::ScanlineColor> segments;
    for(std::int32_t y = 16; y < 32; y++) {
        const std::uint8_t v = static_cast<std::uint8_t>(y * 8);
        segments.push_back(geometrize::ScanlineColor{y, 14, 34, geometrize::rgba{v, v, v, 128}});
    }
    const std::string banded = geometrize::exporter::getSingleShapeSVGData(color, ellipse, segments, options);
    CHECK(banded.find("<g id=\"7\"") != std::string::npos); // id 挂到组
    CHECK(banded.find("shape-rendering=\"crispEdges\"") != std::string::npos);
    CHECK(banded.find("fill-opacity=\"0.501961\"") != std::string::npos); // 128/255 组级
    CHECK(banded.find("<rect") != std::string::npos);
    CHECK(banded.find("<ellipse") == std::string::npos); // 基元被全覆盖替代

    // 相邻行颜色差 > 容差(每行 +8)→ 全部独立成 rect,行数 = 段数
    std::size_t rectCount = 0;
    for(std::size_t pos = banded.find("<rect"); pos != std::string::npos; pos = banded.find("<rect", pos + 1)) {
        rectCount++;
    }
    CHECK(rectCount == segments.size());
}

TEST_CASE("SVG 分段导出:同色相邻行合并、容差内合并、y 断裂不合并")
{
    const geometrize::Ellipse ellipse{24.0f, 24.0f, 10.0f, 8.0f};
    const geometrize::rgba color{100, 120, 140, 128};
    geometrize::exporter::SVGExportOptions options;

    std::vector<geometrize::ScanlineColor> segments;
    // 10 行完全同色、区间一致 → 1 个 rect
    for(std::int32_t y = 10; y < 20; y++) {
        segments.push_back(geometrize::ScanlineColor{y, 14, 34, geometrize::rgba{50, 60, 70, 128}});
    }
    // y 断裂(跳 1 行)+ 同色 → 新段
    segments.push_back(geometrize::ScanlineColor{21, 14, 34, geometrize::rgba{50, 60, 70, 128}});

    const std::string banded = geometrize::exporter::getSingleShapeSVGData(color, ellipse, segments, options);
    std::size_t rectCount = 0;
    for(std::size_t pos = banded.find("<rect"); pos != std::string::npos; pos = banded.find("<rect", pos + 1)) {
        rectCount++;
    }
    CHECK(rectCount == 2U);

    // 合并段行均色 = 50/60/70(全同色,均值精确)
    CHECK(banded.find("rgb(50,60,70)") != std::string::npos);
}

TEST_CASE("JSON 导出:segments 可选字段与结构")
{
    // 关闭路径:无 segments 字段
    std::vector<geometrize::ShapeResult> plainResults;
    auto circleA = std::make_shared<geometrize::Ellipse>(8.0f, 8.0f, 4.0f, 4.0f);
    plainResults.emplace_back(geometrize::ShapeResult{0.5, geometrize::rgba{1, 2, 3, 255}, circleA});
    const std::string plain = geometrize::exporter::exportShapeJson(plainResults);
    CHECK(plain.find("segments") == std::string::npos);

    // 开启路径:segments 数组输出
    std::vector<geometrize::ShapeResult> segResults;
    std::vector<geometrize::ScanlineColor> segments;
    segments.push_back(geometrize::ScanlineColor{5, 2, 9, geometrize::rgba{10, 20, 30, 128}});
    segments.push_back(geometrize::ScanlineColor{6, 3, 8, geometrize::rgba{40, 50, 60, 128}});
    segResults.emplace_back(geometrize::ShapeResult{0.5, geometrize::rgba{1, 2, 3, 128}, circleA, segments});
    const std::string json = geometrize::exporter::exportShapeJson(segResults);
    CHECK(json.find("\"segments\":[") != std::string::npos);
    CHECK(json.find("{\"y\":5,\"x1\":2,\"x2\":9,\"color\":[10,20,30,128]}") != std::string::npos);
    CHECK(json.find("{\"y\":6,\"x1\":3,\"x2\":8,\"color\":[40,50,60,128]}") != std::string::npos);
}

TEST_CASE("JSON 导出:良构性(单元素数组不得出现尾逗号)")
{
    // 回归点:外层循环曾用 `i <= size() - 2`,size()==1 时 size_t 下溢使条件恒真,
    // 单形状导出输出 "[{...},\n\n]}" —— 严格 JSON 解析器一律拒绝。子串断言抓不到,必须查良构性。
    const auto malformed = [](const std::string& json) {
        if(json.find(",]") != std::string::npos || json.find(",}") != std::string::npos) {
            return true;
        }
        int depth = 0;
        for(const char c : json) {
            if(c == '[' || c == '{') {
                depth++;
            } else if(c == ']' || c == '}') {
                depth--;
            }
            if(depth < 0) {
                return true;
            }
        }
        return depth != 0;
    };

    auto ellipse = std::make_shared<geometrize::Ellipse>(8.0f, 8.0f, 4.0f, 4.0f);

    // 单形状、无 segments:曾经唯一可触发的形态
    std::vector<geometrize::ShapeResult> single;
    single.emplace_back(geometrize::ShapeResult{0.5, geometrize::rgba{1, 2, 3, 255}, ellipse});
    const std::string singleJson = geometrize::exporter::exportShapeJson(single);
    CHECK(!malformed(singleJson));
    CHECK(singleJson.find(",\n\n]") == std::string::npos);

    // 单形状 + 单段 segments:外层与内层两处循环都要守住
    std::vector<geometrize::ScanlineColor> oneSegment;
    oneSegment.push_back(geometrize::ScanlineColor{5, 2, 9, geometrize::rgba{10, 20, 30, 128}});
    std::vector<geometrize::ShapeResult> singleSeg;
    singleSeg.emplace_back(geometrize::ShapeResult{0.5, geometrize::rgba{1, 2, 3, 128}, ellipse, oneSegment});
    CHECK(!malformed(geometrize::exporter::exportShapeJson(singleSeg)));

    // 多形状:分隔逗号必须仍在(防"修过头"把合法分隔符也删掉)
    std::vector<geometrize::ShapeResult> pair;
    pair.emplace_back(geometrize::ShapeResult{0.5, geometrize::rgba{1, 2, 3, 255}, ellipse});
    pair.emplace_back(geometrize::ShapeResult{0.4, geometrize::rgba{4, 5, 6, 255}, ellipse});
    const std::string pairJson = geometrize::exporter::exportShapeJson(pair);
    CHECK(!malformed(pairJson));
    CHECK(pairJson.find("},\n{") != std::string::npos);
}
#endif
