#include "rasterizer.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "../commonutil.h"
#include "../bitmap/bitmap.h"
#include "../bitmap/rgba.h"
#include "../shape/circle.h"
#include "../shape/ellipse.h"
#include "../shape/line.h"
#include "../shape/polyline.h"
#include "../shape/quadraticbezier.h"
#include "../shape/rectangle.h"
#include "../shape/rotatedellipse.h"
#include "../shape/rotatedrectangle.h"
#include "../shape/triangle.h"
#include "scanline.h"

namespace geometrize
{

std::vector<std::pair<float, float>> getCornerPoints(const geometrize::RotatedRectangle& r)
{
    const float x1{(std::fmin)(r.m_x1, r.m_x2)};
    const float x2{(std::fmax)(r.m_x1, r.m_x2)};
    const float y1{(std::fmin)(r.m_y1, r.m_y2)};
    const float y2{(std::fmax)(r.m_y1, r.m_y2)};

    const float cx{(x2 + x1) / 2.0f};
    const float cy{(y2 + y1) / 2.0f};

    const float ox1{x1 - cx};
    const float ox2{x2 - cx};
    const float oy1{y1 - cy};
    const float oy2{y2 - cy};

    const float rads{r.m_angle * 3.141f / 180.0f};
    const float c{std::cos(rads)};
    const float s{std::sin(rads)};

    const std::pair<float, float> ul{ox1 * c - oy1 * s + cx, ox1 * s + oy1 * c + cy};
    const std::pair<float, float> bl{ox1 * c - oy2 * s + cx, ox1 * s + oy2 * c + cy};
    const std::pair<float, float> ur{ox2 * c - oy1 * s + cx, ox2 * s + oy1 * c + cy};
    const std::pair<float, float> br{ox2 * c - oy2 * s + cx, ox2 * s + oy2 * c + cy};

    return {ul, ur, br, bl};
}

std::vector<std::pair<float, float>> getPointsOnRotatedEllipse(const geometrize::RotatedEllipse& e, const std::size_t numPoints)
{    
    std::vector<std::pair<float, float>> points;
    const float rads{e.m_angle * (3.141f / 180.0f)};
    const float co{std::cos(rads)};
    const float si{std::sin(rads)};

    for(std::uint32_t i = 0; i < numPoints; i++) {
        const float angle{((360.0f / numPoints) * i) * (3.141f / 180.0f)};
        const float crx{e.m_rx * std::cos(angle)};
        const float cry{e.m_ry * std::sin(angle)};
        points.push_back(std::make_pair(crx * co - cry * si + e.m_x, crx * si + cry * co + e.m_y));
    }

    return points;
}

void drawLines(geometrize::Bitmap& image, const geometrize::rgba color, const std::vector<geometrize::Scanline>& lines)
{
    // Convert the non-premultiplied color to alpha-premultiplied 16-bits per channel RGBA
    // In other words, scale the rgb color components by the alpha component
    std::uint32_t sr{color.r};
    sr |= sr << 8;
    sr *= color.a;
    sr /= UINT8_MAX;
    std::uint32_t sg{color.g};
    sg |= sg << 8;
    sg *= color.a;
    sg /= UINT8_MAX;
    std::uint32_t sb{color.b};
    sb |= sb << 8;
    sb *= color.a;
    sb /= UINT8_MAX;
    std::uint32_t sa{color.a};
    sa |= sa << 8;

    const std::uint32_t m{UINT16_MAX};
    const std::uint32_t aa{(m - sa) * 257U};

    if(image.getDataRef().empty()) {
        return;
    }

    // 裸指针行游标:公式与舍入次序逐像素保持,仅消除逐像素函数调用与索引重算
    auto* data = image.getDataRefMut().data();
    const std::size_t rowStride{static_cast<std::size_t>(image.getWidth()) * 4U};

    for(const geometrize::Scanline& line : lines) {
        if(line.y < 0) {
            continue;
        }
        auto* row = data + rowStride * static_cast<std::size_t>(line.y);

        for(std::int32_t x = line.x1; x <= line.x2; x++) {
            const std::size_t offset{static_cast<std::size_t>(x) * 4U};

            const std::uint32_t d_r{row[offset]};
            const std::uint32_t d_g{row[offset + 1U]};
            const std::uint32_t d_b{row[offset + 2U]};
            const std::uint32_t d_a{row[offset + 3U]};

            row[offset]     = static_cast<std::uint8_t>(((d_r * aa + sr * m) / m) >> 8);
            row[offset + 1U] = static_cast<std::uint8_t>(((d_g * aa + sg * m) / m) >> 8);
            row[offset + 2U] = static_cast<std::uint8_t>(((d_b * aa + sb * m) / m) >> 8);
            row[offset + 3U] = static_cast<std::uint8_t>(((d_a * aa + sa * m) / m) >> 8);
        }
    }
}

void drawLinesSegmented(geometrize::Bitmap& image, const std::vector<geometrize::rgba>& colors, const std::vector<geometrize::Scanline>& lines)
{
    // 契约:colors 与 lines 等长一一对应;扫描线互不重叠是库内光栅化不变量,逐像素至多混合一次。
    if(image.getDataRef().empty() || colors.size() < lines.size()) {
        return;
    }

    // 裸指针行游标:公式与舍入次序逐像素与 drawLines 一致,仅预乘提到行循环(每行一次)
    auto* data = image.getDataRefMut().data();
    const std::size_t rowStride{static_cast<std::size_t>(image.getWidth()) * 4U};
    const std::uint32_t m{UINT16_MAX};

    for(std::size_t i = 0; i < lines.size(); i++) {
        const geometrize::Scanline& line = lines[i];
        if(line.y < 0) {
            continue;
        }
        const geometrize::rgba color = colors[i];

        // Convert the non-premultiplied color to alpha-premultiplied 16-bits per channel RGBA
        std::uint32_t sr{color.r};
        sr |= sr << 8;
        sr *= color.a;
        sr /= UINT8_MAX;
        std::uint32_t sg{color.g};
        sg |= sg << 8;
        sg *= color.a;
        sg /= UINT8_MAX;
        std::uint32_t sb{color.b};
        sb |= sb << 8;
        sb *= color.a;
        sb /= UINT8_MAX;
        std::uint32_t sa{color.a};
        sa |= sa << 8;
        const std::uint32_t aa{(m - sa) * 257U};

        auto* row = data + rowStride * static_cast<std::size_t>(line.y);

        for(std::int32_t x = line.x1; x <= line.x2; x++) {
            const std::size_t offset{static_cast<std::size_t>(x) * 4U};

            const std::uint32_t d_r{row[offset]};
            const std::uint32_t d_g{row[offset + 1U]};
            const std::uint32_t d_b{row[offset + 2U]};
            const std::uint32_t d_a{row[offset + 3U]};

            row[offset]     = static_cast<std::uint8_t>(((d_r * aa + sr * m) / m) >> 8);
            row[offset + 1U] = static_cast<std::uint8_t>(((d_g * aa + sg * m) / m) >> 8);
            row[offset + 2U] = static_cast<std::uint8_t>(((d_b * aa + sb * m) / m) >> 8);
            row[offset + 3U] = static_cast<std::uint8_t>(((d_a * aa + sa * m) / m) >> 8);
        }
    }
}

void copyLines(geometrize::Bitmap& destination, const geometrize::Bitmap& source, const std::vector<geometrize::Scanline>& lines)
{
    if(destination.getDataRef().empty() || source.getDataRef().empty()) {
        return;
    }

    // 行段连续区直接 memcpy;数据布局行宽恰为 width*4 无 padding,行内 [x1,x2] 对应连续 4*(x2-x1+1) 字节。
    auto* dstData = destination.getDataRefMut().data();
    const auto* srcData = source.getDataRef().data();
    const std::size_t rowStride{static_cast<std::size_t>(destination.getWidth()) * 4U};

    for(const geometrize::Scanline& line : lines) {
        if(line.y < 0 || line.x1 > line.x2) {
            continue;
        }
        const std::size_t startOffset{static_cast<std::size_t>(line.x1) * 4U};
        const std::size_t lengthBytes{static_cast<std::size_t>(line.x2 - line.x1 + 1) * 4U};
        std::memcpy(dstData + rowStride * static_cast<std::size_t>(line.y) + startOffset,
                    srcData + rowStride * static_cast<std::size_t>(line.y) + startOffset,
                    lengthBytes);
    }
}

std::vector<std::pair<std::int32_t, std::int32_t>> bresenham(std::int32_t x1, std::int32_t y1, const std::int32_t x2, const std::int32_t y2)
{
    std::int32_t dx{x2 - x1};
    const std::int8_t ix{static_cast<std::int8_t>((dx > 0) - (dx < 0))};
    dx = std::abs(dx) << 1;

    std::int32_t dy{y2 - y1};
    const std::int8_t iy{static_cast<std::int8_t>((dy > 0) - (dy < 0))};
    dy = std::abs(dy) << 1;

    std::vector<std::pair<std::int32_t, std::int32_t>> points;
    points.push_back(std::make_pair(x1, y1));

    if (dx >= dy) {
        std::int32_t error(dy - (dx >> 1));
        while (x1 != x2) {
            if (error >= 0 && (error || (ix > 0))) {
                error -= dx;
                y1 += iy;
            }

            error += dy;
            x1 += ix;

            points.push_back(std::make_pair(x1, y1));
        }
    } else {
        std::int32_t error(dx - (dy >> 1));
        while (y1 != y2) {
            if (error >= 0 && (error || (iy > 0))) {
                error -= dy;
                x1 += ix;
            }

            error += dx;
            y1 += iy;

            points.push_back(std::make_pair(x1, y1));
        }
    }

    return points;
}

std::vector<geometrize::Scanline> scanlinesForPolygon(const std::vector<std::pair<float, float>>& points)
{
    std::vector<geometrize::Scanline> lines;

    // Get the pixel outline of the polygon
    std::vector<std::pair<std::int32_t, std::int32_t>> edges;
    for(std::size_t i = 0; i < points.size(); i++) {
        const std::pair<std::int32_t, std::int32_t> p1{static_cast<std::int32_t>(points[i].first), static_cast<std::int32_t>(points[i].second)};
        const std::pair<std::int32_t, std::int32_t> p2{(i == (points.size() - 1)) ? std::make_pair(static_cast<std::int32_t>(points[0U].first), static_cast<std::int32_t>(points[0U].second)) : std::make_pair(static_cast<std::int32_t>(points[i + 1U].first), static_cast<std::int32_t>(points[i + 1U].second))};
        const std::vector<std::pair<std::int32_t, std::int32_t>> p1p2{geometrize::bresenham(p1.first, p1.second, p2.first, p2.second)};
        edges.insert(edges.end(), p1p2.begin(), p1p2.end());
    }

    // Convert outline to scanlines.
    // 扁平化等价实现:map<,set<>> 的键升序 ↔ sort 后 y 升序;set 的 min/max ↔ 每 y 组内线性扫描。
    // sort 是值排序且 (y,x) 全序确定,输出 Scanline 序列与原 map/set 路径一一对应。
    std::sort(edges.begin(), edges.end(), [](const std::pair<std::int32_t, std::int32_t>& lhs, const std::pair<std::int32_t, std::int32_t>& rhs) {
        return (lhs.second < rhs.second) || (lhs.second == rhs.second && lhs.first < rhs.first);
    });

    for(std::size_t i = 0; i < edges.size();) {
        const std::int32_t y{edges[i].second};
        const std::int32_t xMinOfRow{edges[i].first};
        std::size_t j{i};
        std::int32_t xMaxOfRow{xMinOfRow};
        while(j < edges.size() && edges[j].second == y) {
            xMaxOfRow = edges[j].first; // 同 y 组内 x 升序,末个即 max
            j++;
        }
        lines.push_back(geometrize::Scanline(y, xMinOfRow, xMaxOfRow));
        i = j;
    }

    return lines;
}

std::vector<geometrize::Scanline> rasterize(const geometrize::Shape& s, std::int32_t xMin, std::int32_t yMin, std::int32_t xMax, std::int32_t yMax)
{
    switch(s.getType()) {
    case geometrize::ShapeTypes::RECTANGLE:
        return rasterize(static_cast<const geometrize::Rectangle&>(s), xMin, yMin, xMax, yMax);
    case geometrize::ShapeTypes::ROTATED_RECTANGLE:
        return rasterize(static_cast<const geometrize::RotatedRectangle&>(s), xMin, yMin, xMax, yMax);
    case geometrize::ShapeTypes::TRIANGLE:
        return rasterize(static_cast<const geometrize::Triangle&>(s), xMin, yMin, xMax, yMax);
    case geometrize::ShapeTypes::ELLIPSE:
        return rasterize(static_cast<const geometrize::Ellipse&>(s), xMin, yMin, xMax, yMax);
    case geometrize::ShapeTypes::ROTATED_ELLIPSE:
        return rasterize(static_cast<const geometrize::RotatedEllipse&>(s), xMin, yMin, xMax, yMax);
    case geometrize::ShapeTypes::CIRCLE:
        return rasterize(static_cast<const geometrize::Circle&>(s), xMin, yMin, xMax, yMax);
    case geometrize::ShapeTypes::LINE:
        return rasterize(static_cast<const geometrize::Line&>(s), xMin, yMin, xMax, yMax);
    case geometrize::ShapeTypes::QUADRATIC_BEZIER:
        return rasterize(static_cast<const geometrize::QuadraticBezier&>(s), xMin, yMin, xMax, yMax);
    case geometrize::ShapeTypes::POLYLINE:
        return rasterize(static_cast<const geometrize::Polyline&>(s), xMin, yMin, xMax, yMax);
    default:
        assert(0 && "Bad shape type");
        return std::vector<geometrize::Scanline>{};
    }
}

std::vector<geometrize::Scanline> rasterize(const geometrize::Circle& s, const std::int32_t xMin, const std::int32_t yMin, const std::int32_t xMax, const std::int32_t yMax)
{
    std::vector<geometrize::Scanline> lines;

    const std::int32_t r{static_cast<std::int32_t>(s.m_r)};

    // O(r) 化:整数 x 满足 x²+y²≤r² ⟺ |x| ≤ ⌊√(r²−y²)⌋,
    // 判定域关于 x=0 对称故原实现的 front/back 恒为 [−half,+half],与逐点扫描取端点等价。
    // y∈[−r,r] 时 r²−y²≥0 恒成立(含 r=0 的单像素行),与原版"空行分支不可达"的行为一致。
    const std::int64_t rr2{static_cast<std::int64_t>(r) * r};
    for(std::int32_t y = -r; y <= r; y++) {
        const std::int64_t rem{rr2 - static_cast<std::int64_t>(y) * y};
        if(rem < 0) {
            continue; // 不可达分支,防御性保留以对齐原版空行语义
        }
        // isqrt:浮点初值 + 校正循环,uint64 全程防溢出且精确
        std::int64_t half{static_cast<std::int64_t>(std::sqrt(static_cast<double>(rem)))};
        while(half > 0 && half * half > rem) {
            half--;
        }
        while((half + 1) * (half + 1) <= rem) {
            half++;
        }

        const std::int32_t fy{static_cast<std::int32_t>(s.m_y) + y};
        const std::int32_t x1{commonutil::clamp(static_cast<std::int32_t>(s.m_x) - static_cast<std::int32_t>(half), xMin, xMax - 1)};
        const std::int32_t x2{commonutil::clamp(static_cast<std::int32_t>(s.m_x) + static_cast<std::int32_t>(half), xMin, xMax - 1)};
        lines.push_back(geometrize::Scanline(fy, x1, x2));
    }

    return geometrize::trimScanlines(lines, xMin, yMin, xMax, yMax);
}

std::vector<geometrize::Scanline> rasterize(const geometrize::Ellipse& s, const std::int32_t xMin, const std::int32_t yMin, const std::int32_t xMax, const std::int32_t yMax)
{
    std::vector<geometrize::Scanline> lines;

    const float aspect{static_cast<float>(s.m_rx) / static_cast<float>(s.m_ry)};

    for (std::int32_t dy = 0; dy < s.m_ry; dy++) {
        const std::int32_t y1{static_cast<std::int32_t>(s.m_y) - dy};
        const std::int32_t y2{static_cast<std::int32_t>(s.m_y) + dy};

        if ((y1 < yMin || y1 >= yMax) && (y2 < yMin || y2 >= yMax)) {
            continue;
        }

        const std::int32_t v{static_cast<std::int32_t>(std::sqrt(s.m_ry * s.m_ry - dy * dy) * aspect)};
        std::int32_t x1{static_cast<std::int32_t>(s.m_x) - v};
        std::int32_t x2{static_cast<std::int32_t>(s.m_x) + v};
        if (x1 < xMin) {
            x1 = xMin;
        }
        if (x2 >= xMax) {
            x2 = xMax - 1;
        }

        if (y1 >= yMin && y1 < yMax) {
            lines.push_back(Scanline(y1, x1, x2));
        }
        if (y2 >= yMin && y2 < yMax && dy > 0) {
            lines.push_back(Scanline(y2, x1, x2));
        }
    }

    return geometrize::trimScanlines(lines, xMin, yMin, xMax, yMax);
}

std::vector<geometrize::Scanline> rasterize(const geometrize::Line& s, const std::int32_t xMin, const std::int32_t yMin, const std::int32_t xMax, const std::int32_t yMax)
{
    std::vector<geometrize::Scanline> lines;

    const std::vector<std::pair<std::int32_t, std::int32_t>> points{geometrize::bresenham(static_cast<std::int32_t>(s.m_x1), static_cast<std::int32_t>(s.m_y1), static_cast<std::int32_t>(s.m_x2), static_cast<std::int32_t>(s.m_y2))};
    for(const auto& point : points) {
       lines.push_back(geometrize::Scanline(point.second, point.first, point.first));
    }

    return geometrize::trimScanlines(lines, xMin, yMin, xMax, yMax);
}

std::vector<geometrize::Scanline> rasterize(const geometrize::Polyline& s, const std::int32_t xMin, const std::int32_t yMin, const std::int32_t xMax, const std::int32_t yMax)
{
    std::vector<geometrize::Scanline> lines;

    // Prevent scanline overlap, it messes up the energy functions that rely on the scanlines not intersecting themselves
    std::set<std::pair<std::int32_t, std::int32_t>> duplicates;

    for(std::size_t i = 0; i < s.m_points.size(); i++) {
        const std::pair<std::int32_t, std::int32_t> p0{s.m_points[i].first, s.m_points[i].second};
        const std::pair<std::int32_t, std::int32_t> p1{i < (s.m_points.size() - 1) ? std::make_pair(static_cast<std::int32_t>(s.m_points[i + 1].first), static_cast<std::int32_t>(s.m_points[i + 1].second)) : p0};

        const std::vector<std::pair<std::int32_t, std::int32_t>> points{geometrize::bresenham(p0.first, p0.second, p1.first, p1.second)};
        for(const auto& point : points) {
            if(duplicates.insert(point).second) {
                lines.push_back(geometrize::Scanline(point.second, point.first, point.first));
            }
        }
    }

    return geometrize::trimScanlines(lines, xMin, yMin, xMax, yMax);
}

std::vector<geometrize::Scanline> rasterize(const geometrize::QuadraticBezier& s, const std::int32_t xMin, const std::int32_t yMin, const std::int32_t xMax, const std::int32_t yMax)
{
    std::vector<geometrize::Scanline> scanlines;

    std::vector<std::pair<std::int32_t, std::int32_t>> points;
    const std::uint32_t pointCount{20};
    for(std::uint32_t i = 0; i <= pointCount; i++) {
        const float t{static_cast<float>(i) / static_cast<float>(pointCount)};
        const float tp{1 - t};
        const std::int32_t x{static_cast<std::int32_t>(tp * (tp * s.m_x1 + (t * s.m_cx)) + t * ((tp * s.m_cx) + (t * s.m_x2)))};
        const std::int32_t y{static_cast<std::int32_t>(tp * (tp * s.m_y1 + (t * s.m_cy)) + t * ((tp * s.m_cy) + (t * s.m_y2)))};
        points.push_back(std::make_pair(x, y));
    }

    // Prevent scanline overlap, it messes up the energy functions that rely on the scanlines not intersecting themselves
    std::set<std::pair<std::int32_t, std::int32_t>> duplicates;

    for(std::uint32_t i = 0; i < points.size() - 1; i++) {
        const std::pair<std::int32_t, std::int32_t> p0{points[i]};
        const std::pair<std::int32_t, std::int32_t> p1{points[i + 1]};

        const std::vector<std::pair<std::int32_t, std::int32_t>> points{geometrize::bresenham(static_cast<std::int32_t>(p0.first), static_cast<std::int32_t>(p0.second), static_cast<std::int32_t>(p1.first), static_cast<std::int32_t>(p1.second))};
        for(const std::pair<std::int32_t, std::int32_t>& point : points) {
            if(duplicates.insert(point).second) {
                scanlines.push_back(geometrize::Scanline(point.second, point.first, point.first));
            }
        }
    }

    return geometrize::trimScanlines(scanlines, xMin, yMin, xMax, yMax);
}

std::vector<geometrize::Scanline> rasterize(const geometrize::Rectangle& s, const std::int32_t xMin, const std::int32_t yMin, const std::int32_t xMax, const std::int32_t yMax)
{
    const std::int32_t x1{static_cast<std::int32_t>((std::fmin)(s.m_x1, s.m_x2))};
    const std::int32_t x2{static_cast<std::int32_t>((std::fmax)(s.m_x1, s.m_x2))};
    const std::int32_t y1{static_cast<std::int32_t>((std::fmin)(s.m_y1, s.m_y2))};
    const std::int32_t y2{static_cast<std::int32_t>((std::fmax)(s.m_y1, s.m_y2))};

    std::vector<geometrize::Scanline> lines;
    for(std::int32_t y = y1; y <= y2; y++) {
        lines.push_back(geometrize::Scanline(y, x1, x2));
    }
    return geometrize::trimScanlines(lines, xMin, yMin, xMax, yMax);
}

std::vector<geometrize::Scanline> rasterize(const geometrize::RotatedEllipse& s, const std::int32_t xMin, const std::int32_t yMin, const std::int32_t xMax, const std::int32_t yMax)
{
    const std::uint32_t pointCount{20};
    std::vector<std::pair<float, float>> points = getPointsOnRotatedEllipse(s, pointCount);

    std::vector<geometrize::Scanline> scanlines{geometrize::scanlinesForPolygon(points)};
    return geometrize::trimScanlines(scanlines, xMin, yMin, xMax, yMax);
}

std::vector<geometrize::Scanline> rasterize(const geometrize::RotatedRectangle& s, const std::int32_t xMin, const std::int32_t yMin, const std::int32_t xMax, const std::int32_t yMax)
{
    std::vector<geometrize::Scanline> scanlines{geometrize::scanlinesForPolygon(getCornerPoints(s))};
    return geometrize::trimScanlines(scanlines, xMin, yMin, xMax, yMax);
}

std::vector<geometrize::Scanline> rasterize(const geometrize::Triangle& s, const std::int32_t xMin, const std::int32_t yMin, const std::int32_t xMax, const std::int32_t yMax)
{
     std::vector<geometrize::Scanline> scanlines = geometrize::scanlinesForPolygon({
         {static_cast<std::int32_t>(s.m_x1), static_cast<std::int32_t>(s.m_y1)},
         {static_cast<std::int32_t>(s.m_x2), static_cast<std::int32_t>(s.m_y2)},
         {static_cast<std::int32_t>(s.m_x3), static_cast<std::int32_t>(s.m_y3)}});

    return geometrize::trimScanlines(scanlines, xMin, yMin, xMax, yMax);
}

bool scanlinesOverlap(const std::vector<geometrize::Scanline>& first, const std::vector<geometrize::Scanline>& second)
{
    for(const auto& f : first) {
        for(const auto& s : second) {
            if(f.y == s.y) {
                if(f.x1 <= s.x2 && f.x2 >= s.x1) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool scanlinesContain(const std::vector<geometrize::Scanline>& first, const std::vector<geometrize::Scanline>& second)
{
    for(const auto& s : second) {
        bool contained = false;
        for(const auto& f : first) {
            if(f.y == s.y) {
                if(f.x1 <= s.x1 && f.x2 >= s.x2) {
                    contained = true;
                    break;
                }
            }
        }

        if(!contained) {
            return false;
        }
    }

    return true;
}

bool shapesOverlap(const geometrize::Shape& a, const geometrize::Shape& b, const std::int32_t xMin, const std::int32_t yMin, const std::int32_t xMax, const std::int32_t yMax)
{
    return geometrize::scanlinesOverlap(geometrize::rasterize(a, xMin, yMin, xMax, yMax), geometrize::rasterize(b, xMin, yMin, xMax, yMax));
}

bool shapeContains(const geometrize::Shape& container, const geometrize::Shape& containee, const std::int32_t xMin, const std::int32_t yMin, const std::int32_t xMax, const std::int32_t yMax)
{
    return geometrize::scanlinesContain(rasterize(container, xMin, yMin, xMax, yMax), rasterize(containee, xMin, yMin, xMax, yMax));
}

std::vector<std::pair<std::int32_t, std::int32_t>> shapeToPixels(const geometrize::Shape& shape, const std::int32_t xMin, const std::int32_t yMin, const std::int32_t xMax, const std::int32_t yMax)
{
    const auto scanlines = geometrize::rasterize(shape, xMin, yMin, xMax, yMax);
    std::vector<std::pair<std::int32_t, std::int32_t>> points = {};
    for(const auto& scanline : scanlines) {
        for(std::int32_t x = scanline.x1; x <= scanline.x2; x++) {
            points.push_back({x, scanline.y});
        }
    }
    return points;
}

}
