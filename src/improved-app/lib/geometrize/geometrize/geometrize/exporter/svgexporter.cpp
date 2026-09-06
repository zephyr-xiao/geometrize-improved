#include "svgexporter.h"

#include <cassert>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <regex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "../rasterizer/rasterizer.h"
#include "../bitmap/rgba.h"
#include "../shape/shape.h"
#include "../shape/circle.h"
#include "../shape/ellipse.h"
#include "../shape/line.h"
#include "../shape/polyline.h"
#include "../shape/quadraticbezier.h"
#include "../shape/rectangle.h"
#include "../shape/rotatedellipse.h"
#include "../shape/rotatedrectangle.h"
#include "../shape/triangle.h"
#include "../shaperesult.h"
#include "../commonutil.h"

namespace
{

std::string getSvgShapeData(const geometrize::Circle& s)
{
    std::stringstream strm;
    strm << "<circle cx=\"" << s.m_x << "\" cy=\"" << s.m_y << "\" r=\"" << s.m_r << "\" " << geometrize::exporter::SVG_STYLE_HOOK << " />";
    return strm.str();
}

std::string getSvgShapeData(const geometrize::Ellipse& s)
{
    std::stringstream strm;
    strm << "<ellipse cx=\"" << s.m_x << "\" cy=\"" << s.m_y << "\" rx=\"" << s.m_rx << "\" ry=\"" << s.m_ry << "\" " << geometrize::exporter::SVG_STYLE_HOOK << " />";
    return strm.str();
}

std::string getSvgShapeData(const geometrize::Line& s)
{
    std::stringstream strm;
    strm << "<line x1=\"" << s.m_x1 << "\" y1=\"" << s.m_y1 << "\" x2=\"" << s.m_x2 << "\" y2=\"" << s.m_y2 << "\" " << geometrize::exporter::SVG_STYLE_HOOK << " />";
    return strm.str();
}

std::string getSvgShapeData(const geometrize::Polyline& s)
{
    std::stringstream strm;
    strm << "<polyline points=\"";
    for(std::size_t i = 0; i < s.m_points.size(); i++) {
        strm << s.m_points[i].first << "," << s.m_points[i].second;
        if(i != s.m_points.size() - 1) {
            strm << " ";
        }
    }
    strm << "\" ";

    strm << geometrize::exporter::SVG_STYLE_HOOK << " ";
    strm << "/>";

    return strm.str();
}

std::string getSvgShapeData(const geometrize::QuadraticBezier& s)
{
    std::stringstream strm;
    strm << "<path d=\"M" << s.m_x1 << " " << s.m_y1 << " Q " << s.m_cx << " " << s.m_cy << " " << s.m_x2 << " " << s.m_y2 << "\" " << geometrize::exporter::SVG_STYLE_HOOK << " />";
    return strm.str();
}

std::string getSvgShapeData(const geometrize::Rectangle& s)
{
    std::stringstream strm;
    strm << "<rect x=\"" << (std::fmin)(s.m_x1, s.m_x2) << "\" y=\"" << (std::fmin)(s.m_y1, s.m_y2) << "\" width=\"" << (std::fmax)(s.m_x1, s.m_x2) - (std::fmin)(s.m_x1, s.m_x2) << "\" height=\"" << (std::fmax)(s.m_y1, s.m_y2) - (std::fmin)(s.m_y1, s.m_y2) << "\" " << geometrize::exporter::SVG_STYLE_HOOK << " />";
    return strm.str();
}

std::string getSvgShapeData(const geometrize::RotatedEllipse& s, const geometrize::exporter::RotatedEllipseSVGExportMode mode)
{
    std::stringstream strm;

    switch(mode) {
    case geometrize::exporter::RotatedEllipseSVGExportMode::ELLIPSE_ITEM:
        {
            strm << "<g transform=\"translate(" << s.m_x << " " << s.m_y << ") rotate(" << s.m_angle << ") scale(" << s.m_rx << " " << s.m_ry << ")\">"
              << "<ellipse cx=\"" << 0 << "\" cy=\"" << 0 << "\" rx=\"" << 1 << "\" ry=\"" << 1 << "\" " << geometrize::exporter::SVG_STYLE_HOOK << " />"
              << "</g>";
        }
        break;
    case geometrize::exporter::RotatedEllipseSVGExportMode::POLYGON:
        {
            const std::size_t pointCount = 20;
            std::vector<std::pair<float, float>> points{geometrize::getPointsOnRotatedEllipse(s, pointCount)};

            strm << "<polygon points=\"";
            for(std::size_t i = 0; i < points.size(); i++) {
                strm << points[i].first << "," << points[i].second;
                if(i != points.size() - 1) {
                    strm << " ";
                }
            }
            strm << "\" " << geometrize::exporter::SVG_STYLE_HOOK << "/>";
        }
        break;
    }

    return strm.str();
}

std::string getSvgShapeData(const geometrize::RotatedRectangle& s)
{
    const std::vector<std::pair<float, float>> points{geometrize::getCornerPoints(s)};
    std::stringstream strm;
    strm << "<polygon points=\"";
    for(std::size_t i = 0; i < points.size(); i++) {
        strm << points[i].first << "," << points[i].second;
        if(i != points.size() - 1) {
            strm << " ";
        }
    }
    strm << "\" " << geometrize::exporter::SVG_STYLE_HOOK << "/>";
    return strm.str();
}

std::string getSvgShapeData(const geometrize::Triangle& s)
{
    std::stringstream strm;
    strm << "<polygon points=\"" << s.m_x1 << "," << s.m_y1 << " " << s.m_x2 << "," << s.m_y2 << " " << s.m_x3 << "," << s.m_y3 << "\" " << geometrize::exporter::SVG_STYLE_HOOK << " " << "/>";
    return strm.str();
}

std::string getSvgShapeData(const geometrize::Shape& s, const geometrize::exporter::SVGExportOptions& options)
{
    switch(s.getType()) {
    case geometrize::ShapeTypes::RECTANGLE:
        return getSvgShapeData(static_cast<const geometrize::Rectangle&>(s));
    case geometrize::ShapeTypes::ROTATED_RECTANGLE:
        return getSvgShapeData(static_cast<const geometrize::RotatedRectangle&>(s));
    case geometrize::ShapeTypes::TRIANGLE:
        return getSvgShapeData(static_cast<const geometrize::Triangle&>(s));
    case geometrize::ShapeTypes::ELLIPSE:
        return getSvgShapeData(static_cast<const geometrize::Ellipse&>(s));
    case geometrize::ShapeTypes::ROTATED_ELLIPSE:
        return getSvgShapeData(static_cast<const geometrize::RotatedEllipse&>(s), options.rotatedEllipseExportMode);
    case geometrize::ShapeTypes::CIRCLE:
        return getSvgShapeData(static_cast<const geometrize::Circle&>(s));
    case geometrize::ShapeTypes::LINE:
        return getSvgShapeData(static_cast<const geometrize::Line&>(s));
    case geometrize::ShapeTypes::QUADRATIC_BEZIER:
        return getSvgShapeData(static_cast<const geometrize::QuadraticBezier&>(s));
    case geometrize::ShapeTypes::POLYLINE:
        return getSvgShapeData(static_cast<const geometrize::Polyline&>(s));
    default:
        assert(0 && "Bad shape type");
        return "";
    }
}

std::string getSVGRgbColorAttrib(const geometrize::rgba color)
{
    std::ostringstream stream;
    stream << "rgb("
           << static_cast<std::int32_t>(color.r) << ","
           << static_cast<std::int32_t>(color.g) << ","
           << static_cast<std::int32_t>(color.b) << ")";
    return stream.str();
}

std::string getSVGStrokeAttrib(const geometrize::rgba color)
{
    std::stringstream stream;
    stream << "stroke=\"" << getSVGRgbColorAttrib(color) << "\"";
    return stream.str();
}

std::string getSVGFillAttrib(const geometrize::rgba color)
{
    std::stringstream stream;
    stream << "fill=\"" << getSVGRgbColorAttrib(color) << "\"";
    return stream.str();
}

std::string getSVGFillOpacityAttrib(const geometrize::rgba color)
{
    std::stringstream stream;
    stream << "fill-opacity=\"" << static_cast<float>(color.a) / 255.0f << "\"";
    return stream.str();
}

std::string getSVGStrokeOpacityAttrib(const geometrize::rgba color)
{
    std::stringstream stream;
    stream << "stroke-opacity=\"" << static_cast<float>(color.a) / 255.0f << "\"";
    return stream.str();
}

std::string getSingleShapeSVGData(const geometrize::rgba& color, const geometrize::Shape& shape, const geometrize::exporter::SVGExportOptions& options)
{
    std::stringstream stream;

    std::string shapeData{getSvgShapeData(shape, options)};
    const geometrize::ShapeTypes shapeType{shape.getType()};

    std::string styles{""};

    styles.append("id=\"" + std::to_string(options.itemId) + "\" ");

    if(shapeType == geometrize::ShapeTypes::LINE
            || shapeType == geometrize::ShapeTypes::POLYLINE
            || shapeType == geometrize::ShapeTypes::QUADRATIC_BEZIER) {
        styles.append(getSVGStrokeAttrib(color));
        styles.append(" stroke-width=\"1\" fill=\"none\" ");
        styles.append(getSVGStrokeOpacityAttrib(color));
    } else {
        styles.append(getSVGFillAttrib(color));
        styles.append(" ");
        styles.append(getSVGFillOpacityAttrib(color));
    }

    shapeData = std::regex_replace(shapeData, std::regex(geometrize::exporter::SVG_STYLE_HOOK), styles);

    stream << shapeData << "\n";

    return stream.str();
}

// ---- A2.4 分段颜色:行级色带组 ----

// 段合并容差:每通道 ≤2(整数平均的 1-bit 噪声级)。对运行中段做增量均值判定而非逐行比首行,
// 防链式漂移;判定与冲刷全整数,零浮点。
// 判定语义:新行 c 加入 n 行段(和为 sum)后,|c·n − sum| ≤ tol·(n+1),即 c 与"含新行的段均值"之差有界。
constexpr std::int32_t SEGMENT_MERGE_TOLERANCE{2};

std::string getSegmentedShapeSVGData(const geometrize::rgba& color, const geometrize::Shape& shape,
                                     const std::vector<geometrize::ScanlineColor>& segments,
                                     const geometrize::exporter::SVGExportOptions& options)
{
    std::stringstream stream;

    // 色带组:alpha 各行相同(逐行独立取色共享同一混合系数),提升到组级;id 挂在 g 上。
    // shape-rendering="crispEdges" 声明整数对齐意图;整数倍缩放下整数坐标本就无缝(QtSvg 已验证)。
    stream << "<g id=\"" << options.itemId << "\" " << getSVGFillOpacityAttrib(color)
           << " shape-rendering=\"crispEdges\">\n";

    // 每行 rect 覆盖该行实际区间 [x1,x2](面状形状的边缘行区间随轮廓渐变,宽度不一)。
    // 合并条件:y 连续 + 区间一致 + 增量均值在容差内(保守合并,边缘行天然不并入)。
    // 注意:光栅化的行输出顺序是形状实现的自由(如椭圆按 dy 从中心向两边),导出侧按 y 排序
    // 后再合并——排序只影响导出聚类的起点,行内容与位图无耦合。
    std::vector<const geometrize::ScanlineColor*> ordered;
    ordered.reserve(segments.size());
    for(const geometrize::ScanlineColor& seg : segments) {
        ordered.push_back(&seg);
    }
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const geometrize::ScanlineColor* a, const geometrize::ScanlineColor* b) { return a->y < b->y; });

    std::int32_t runStartY{0};
    std::int32_t runCount{0};
    std::int32_t runX1{0};
    std::int32_t runX2{0};
    std::int64_t sumR{0};
    std::int64_t sumG{0};
    std::int64_t sumB{0};
    std::int32_t runRectCount{0};
    bool hasRun{false};

    auto flushRun = [&]() {
        if(!hasRun || runCount <= 0) {
            return;
        }
        const std::int32_t r{static_cast<std::int32_t>(sumR / runRectCount)};
        const std::int32_t g{static_cast<std::int32_t>(sumG / runRectCount)};
        const std::int32_t b{static_cast<std::int32_t>(sumB / runRectCount)};
        stream << "<rect x=\"" << runX1 << "\" y=\"" << runStartY << "\" width=\""
               << (runX2 - runX1 + 1) << "\" height=\"" << runCount
               << "\" fill=\"rgb(" << r << "," << g << "," << b << ")\" />\n";
    };

    for(const geometrize::ScanlineColor* segPtr : ordered) {
        const geometrize::ScanlineColor& seg{*segPtr};
        const bool canMerge = hasRun
            && seg.y == runStartY + runCount // y 连续(防段间空洞)
            && seg.x1 == runX1 && seg.x2 == runX2 // 区间一致(边缘行区间渐变,不合并)
            && std::abs(static_cast<std::int32_t>(seg.color.r) * runRectCount - sumR) <= SEGMENT_MERGE_TOLERANCE * (runRectCount + 1)
            && std::abs(static_cast<std::int32_t>(seg.color.g) * runRectCount - sumG) <= SEGMENT_MERGE_TOLERANCE * (runRectCount + 1)
            && std::abs(static_cast<std::int32_t>(seg.color.b) * runRectCount - sumB) <= SEGMENT_MERGE_TOLERANCE * (runRectCount + 1);

        if(canMerge) {
            runCount++;
            runRectCount++;
            sumR += seg.color.r;
            sumG += seg.color.g;
            sumB += seg.color.b;
        } else {
            flushRun();
            runStartY = seg.y;
            runCount = 1;
            runX1 = seg.x1;
            runX2 = seg.x2;
            sumR = seg.color.r;
            sumG = seg.color.g;
            sumB = seg.color.b;
            runRectCount = 1;
            hasRun = true;
        }
    }
    flushRun();

    stream << "</g>\n";
    return stream.str();
}

}

namespace geometrize
{

namespace exporter
{

std::string getSingleShapeSVGData(const geometrize::rgba& color, const geometrize::Shape& shape, SVGExportOptions options)
{
    return ::getSingleShapeSVGData(color, shape, options);
}

std::string getSingleShapeSVGData(const geometrize::rgba& color, const geometrize::Shape& shape, const std::vector<geometrize::ScanlineColor>& segments, SVGExportOptions options)
{
    if(segments.empty()) {
        return ::getSingleShapeSVGData(color, shape, options);
    }
    return ::getSegmentedShapeSVGData(color, shape, segments, options);
}

std::string exportSingleShapeSVG(const geometrize::rgba& color, const geometrize::Shape& shape, const std::uint32_t width, const std::uint32_t height, SVGExportOptions options)
{
    std::stringstream stream;

    stream << "<?xml version=\"1.0\" standalone=\"no\"?>" << "\n";
    stream << "<svg xmlns=\"http://www.w3.org/2000/svg\" version=\"1.2\" baseProfile=\"tiny\" " <<
              "width=\"" << width << "\" " << "height=\"" << height << "\" " <<
              "viewBox=\"" << 0 << " " << 0 << " " << width << " " << height << "\">" << "\n";

    stream << ::getSingleShapeSVGData(color, shape, options);

    stream << "</svg>";

    return stream.str();
}

std::string exportSVG(const std::vector<geometrize::ShapeResult>& data, const std::uint32_t width, const std::uint32_t height, SVGExportOptions options)
{
    std::stringstream stream;

    stream << "<?xml version=\"1.0\" standalone=\"no\"?>" << "\n";
    stream << "<svg xmlns=\"http://www.w3.org/2000/svg\" version=\"1.2\" baseProfile=\"tiny\" " <<
              "width=\"" << width << "\" " << "height=\"" << height << "\" " <<
              "viewBox=\"" << 0 << " " << 0 << " " << width << " " << height << "\">" << "\n";

    for(std::size_t i = 0; i < data.size(); i++) {
        options.itemId = i;

        const geometrize::ShapeResult& s(data[i]);

        // A2.4:segments 非空走色带组重载,空走基元单色(逐字节一致)
        stream << geometrize::exporter::getSingleShapeSVGData(s.color, *(s.shape), s.segments, options);
    }

    stream << "</svg>";

    return stream.str();
}

}

}
