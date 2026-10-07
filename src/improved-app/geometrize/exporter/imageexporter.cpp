#include "imageexporter.h"

#include <cassert>
#include <string>

#include <QByteArray>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>

#include "geometrize/bitmap/bitmap.h"
#include "geometrize/commonutil.h"
#include "geometrize/exporter/svgexporter.h"
#include "geometrize/rasterizer/rasterizer.h"
#include "geometrize/rasterizer/scanline.h"
#include "geometrize/shape/shape.h"
#include "geometrize/shaperesult.h"

#include "image/imageloader.h"

namespace geometrize
{

namespace exporter
{

bool exportBitmap(const geometrize::Bitmap& bitmap, const std::string& filePath)
{
    const QImage image{geometrize::image::createImage(bitmap)};
    return exportImage(image, filePath);
}

bool exportImage(const QImage& image, const std::string& filePath)
{
    return image.save(QString::fromStdString(filePath));
}

bool exportImages(const std::vector<QImage>& images, const std::string& targetDir, const std::string& baseFilename, const std::string& fileExtension)
{
    for(std::size_t i = 0; i < images.size(); i++) {
        const std::string path{targetDir + baseFilename + "_" + std::to_string(i) + fileExtension};
        if(!images[i].save(QString::fromStdString(path))) {
            return false;
        }
    }
    return true;
}

QImage renderSvgShapeDataToImage(
        const std::vector<geometrize::ShapeResult>& shapes,
        const std::uint32_t inputWidth,
        const std::uint32_t inputHeight,
        const std::uint32_t outputWidth,
        const std::uint32_t outputHeight)
{
    const std::string data{geometrize::exporter::exportSVG(shapes, inputWidth, inputHeight)};
    const QByteArray arrayData(data.c_str(), static_cast<int>(data.length()));
    QSvgRenderer renderer;
    renderer.load(arrayData);

    if(!renderer.isValid()) {
        assert(0 && "SVG renderer is in an invalid state");
        return QImage();
    }

    QPainter painter;
    QImage image(outputWidth, outputHeight, QImage::Format_RGBA8888);
    image.fill(0);

    painter.begin(&image);
    renderer.render(&painter);
    painter.end();

    return image;
}

bool exportRasterizedSvg(
        const std::vector<geometrize::ShapeResult>& shapes,
        const std::uint32_t inputWidth,
        const std::uint32_t inputHeight,
        const std::uint32_t outputWidth,
        const std::uint32_t outputHeight,
        const std::string& filePath)
{
    const QImage image{geometrize::exporter::renderSvgShapeDataToImage(shapes, inputWidth, inputHeight, outputWidth, outputHeight)};
    return geometrize::exporter::exportImage(image, filePath);
}

namespace
{

/// 序列导出每批最多写盘的图片数(百万形状会产生百万张 PNG,超出时均匀抽稀到上限)。
/// 唯一真源:写盘循环与 exportedFrameCount 共用,防止"告知用户的张数"与"实际写盘张数"漂移。
constexpr std::size_t maxSequenceFrames{1000};

}

std::size_t exportedFrameCount(const std::size_t shapeCount)
{
    if(shapeCount == 0) {
        return 0;
    }

    // 与 exportRasterizedSvgs 的取帧规则一致:每 frameStride 个形状取一帧,且末帧必写
    const std::size_t frameStride{(shapeCount + maxSequenceFrames - 1) / maxSequenceFrames};
    return (shapeCount / frameStride) + ((shapeCount % frameStride != 0) ? 1 : 0);
}

bool exportRasterizedSvgs(
        const std::vector<geometrize::ShapeResult>& shapes,
        std::uint32_t inputWidth,
        std::uint32_t inputHeight,
        std::uint32_t outputWidth,
        std::uint32_t outputHeight,
        const std::string& targetDir,
        const std::string& baseFilename,
        const std::string& fileExtension)
{
    if(shapes.empty()) {
        return false;
    }

    // 增量帧:画布逐帧累积新形状,每帧存一张 PNG。老路线"每帧序列化 SVG 前缀整帧重放"
    // 是 O(N²)(百万形状 = 10^11 级绘制指令,不可完成),增量路线总成本 O(N)。
    const std::size_t frameStride{(shapes.size() + maxSequenceFrames - 1) / maxSequenceFrames};

    geometrize::Bitmap canvas{inputWidth, inputHeight, geometrize::rgba{0, 0, 0, 0}};
    for(std::size_t i = 0; i < shapes.size(); i++) {
        std::vector<geometrize::Scanline> lines{shapes[i].shape->rasterize(*shapes[i].shape)};
        if(!shapes[i].segments.empty()) {
            std::vector<geometrize::rgba> colors;
            colors.reserve(shapes[i].segments.size());
            for(const geometrize::ScanlineColor& segment : shapes[i].segments) {
                colors.push_back(segment.color);
            }
            geometrize::drawLinesSegmented(canvas, colors, lines);
        } else {
            geometrize::drawLines(canvas, shapes[i].color, lines);
        }

        if((i + 1) % frameStride != 0 && i != shapes.size() - 1U) {
            continue;
        }
        const std::string path{targetDir + "/" + baseFilename + "_" + std::to_string(i) + fileExtension};
        QImage image{geometrize::image::createImage(canvas)};
        if(static_cast<std::uint32_t>(image.width()) != outputWidth || static_cast<std::uint32_t>(image.height()) != outputHeight) {
            image = image.scaled(static_cast<int>(outputWidth), static_cast<int>(outputHeight),
                                 Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }
        if(!exportImage(image, path)) {
            return false;
        }
    }
    return true;
}

}

}
