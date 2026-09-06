#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../bitmap/rgba.h"

namespace geometrize
{
class Shape;
struct ScanlineColor;
struct ShapeResult;
}

namespace geometrize
{

namespace exporter
{

/**
 * @brief SVG_STYLE_HOOK A hook that an SVG exporter should use to augment shape styling produced by the getSvgShapeData method.
 */
static const std::string SVG_STYLE_HOOK = "::svg_style_hook::";

enum class RotatedEllipseSVGExportMode
{
    ELLIPSE_ITEM = 0, // Export as a translated, rotated and scaled svg <ellipse>. OpenFL's SVG library can't handle this
    POLYGON = 1 // Export as a <polygon>, OpenFL's SVG library can handle this, but it looks quite ugly
};

/**
 * @brief The SVGExportOptions struct represents the options that can be set for the SVG export.
 */
struct SVGExportOptions
{
    RotatedEllipseSVGExportMode rotatedEllipseExportMode{ RotatedEllipseSVGExportMode::ELLIPSE_ITEM }; // Technique to use when exporting rotated ellipses
    std::size_t itemId{ 0 }; // Id to tag the exported SVG shapes with
};

/**
 * @brief getSvgShapeData Gets the SVG data for a single shape. This is just the <rect>/<path> etc block for the shape itself, not a complete SVG image.
 * @param color The color of the shape.
 * @param shape The shape to convert to SVG data.
 * @param options additional options used by the exporter.
 * @return The SVG shape data for the given shape.
 */
std::string getSingleShapeSVGData(const geometrize::rgba& color, const geometrize::Shape& shape, SVGExportOptions options = SVGExportOptions{});

/**
 * @brief getSingleShapeSVGData A2.4 分段颜色重载。segments 非空时输出行级色带组
 * (<g id=... fill-opacity=... shape-rendering="crispEdges"> 内含合并后的 <rect> 序列,
 * 全覆盖替代基元元素——叠加补丁会在基元混色之上再混一次,不忠实于位图)。
 * segments 为空时与单色重载逐字节一致(线型形状恒走此路径)。
 * @param color The average color of the shape (used when segments is empty).
 * @param shape The shape to convert to SVG data.
 * @param segments The per-scanline colors recorded when the shape was drawn.
 * @param options additional options used by the exporter.
 * @return The SVG shape data for the given shape.
 */
std::string getSingleShapeSVGData(const geometrize::rgba& color, const geometrize::Shape& shape, const std::vector<geometrize::ScanlineColor>& segments, SVGExportOptions options = SVGExportOptions{});

/**
 * @brief exportSVG Exports a single shape as a complete SVG image.
 * @param color The color of the shape to export.
 * @param shape The shape to export.
 * @param width The width of the SVG image.
 * @param height The height of the SVG image.
 * @param options additional options used by the exporter.
 * @return A string representing the SVG image.
 */
std::string exportSingleShapeSVG(const geometrize::rgba& color, const geometrize::Shape& shape, const std::uint32_t width, const std::uint32_t height, SVGExportOptions options = SVGExportOptions{});

/**
 * @brief exportSVG Exports shape data as a complete SVG image.
 * @param data The shape data to export.
 * @param width The width of the SVG image.
 * @param height The height of the SVG image.
 * @param options additional options used by the exporter.
 * @return A string representing the SVG image.
 */
std::string exportSVG(const std::vector<geometrize::ShapeResult>& data, const std::uint32_t width, const std::uint32_t height, SVGExportOptions options = SVGExportOptions{});

}

}
