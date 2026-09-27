// SVG 导出:文档包装与片段累积的一致性(应用矢量预览按片段累积重建文档,必须与 exportSVG 逐字节一致)
#include "doctest.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "geometrize/bitmap/rgba.h"
#include "geometrize/exporter/svgexporter.h"
#include "geometrize/shape/circle.h"
#include "geometrize/shape/line.h"
#include "geometrize/shape/rectangle.h"
#include "geometrize/shaperesult.h"

#if defined(GEOTEST_FAST)
namespace
{

geometrize::ShapeResult makeResult(const std::shared_ptr<geometrize::Shape>& shape, const geometrize::rgba color)
{
    return geometrize::ShapeResult{0.0, color, shape, {}};
}

}

TEST_CASE("exportSVGDocument:片段累积与 exportSVG 逐字节一致")
{
    const std::uint32_t width = 64;
    const std::uint32_t height = 32;

    const std::vector<geometrize::ShapeResult> shapes{
        makeResult(std::make_shared<geometrize::Circle>(10.0f, 12.0f, 6.0f), geometrize::rgba{10, 20, 30, 128}),
        makeResult(std::make_shared<geometrize::Rectangle>(2.0f, 3.0f, 40.0f, 20.0f), geometrize::rgba{200, 100, 50, 255}),
        makeResult(std::make_shared<geometrize::Line>(0.0f, 0.0f, 63.0f, 31.0f), geometrize::rgba{1, 2, 3, 200}),
    };

    // 应用侧累积路径:逐形状序列化(编号与 exportSVG 的逐形状下标一致)后整体拼装文档
    std::string accumulated;
    for(std::size_t i = 0; i < shapes.size(); i++) {
        geometrize::exporter::SVGExportOptions options;
        options.itemId = i;
        accumulated.append(geometrize::exporter::getSingleShapeSVGData(shapes[i].color, *(shapes[i].shape), shapes[i].segments, options));
    }

    const std::string fromResults{geometrize::exporter::exportSVG(shapes, width, height)};
    const std::string fromFragments{geometrize::exporter::exportSVGDocument(accumulated, width, height)};

    CHECK(fromResults == fromFragments);

    // 文档包装本身:XML 声明开头、</svg> 收尾、viewBox 与画布尺寸一致
    CHECK(fromFragments.rfind("<?xml version=\"1.0\" standalone=\"no\"?>\n", 0) == 0);
    CHECK(fromFragments.find("viewBox=\"0 0 64 32\"") != std::string::npos);
    CHECK(fromFragments.size() >= 6);
    CHECK(fromFragments.compare(fromFragments.size() - 6, 6, "</svg>") == 0);

    // 空片段:包装仍然自洽(清空后重建文档的边界情形)
    const std::string emptyDocument{geometrize::exporter::exportSVGDocument("", width, height)};
    CHECK(emptyDocument.compare(emptyDocument.size() - 6, 6, "</svg>") == 0);
    CHECK(emptyDocument == geometrize::exporter::exportSVG(std::vector<geometrize::ShapeResult>{}, width, height));
}
#endif
