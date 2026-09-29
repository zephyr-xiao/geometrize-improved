#include "gifexporter.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <functional>
#include <thread>

#include <QImage>
#include <QRgb>
#include <QSet>
#include <QVector>

#include "BurstLinker.h"

#include "geometrize/bitmap/bitmap.h"
#include "geometrize/core.h"
#include "geometrize/rasterizer/rasterizer.h"
#include "geometrize/rasterizer/scanline.h"
#include "geometrize/shape/shape.h"
#include "geometrize/shaperesult.h"

#include "image/imageloader.h"
#include "imageexporter.h"

namespace {

std::uint32_t* makeImageData(const QImage& image)
{
    // This is deleted by the library after the image is added
    const std::size_t imageSizePixels = image.width() * image.height();
    auto bgrs = new std::uint32_t[imageSizePixels];
    const uchar* imageBytes = image.constBits();

    // 输入钉死为 Format_ARGB32(调用侧保证):小端内存序 [B,G,R,A],
    // 打包成小端 [r,g,b,0](BurstLinker reinterpret_cast RGB{r,g,b} 所需)。
    // 按旧链路"假设 RGBA 内存"的读法处理 ARGB 内存会把红蓝互换(GIF 泛蓝实测根因)。
    int byteIndex = 0;
    for(std::size_t pixelIdx = 0; pixelIdx < imageSizePixels; pixelIdx++) {
        const std::int32_t b = imageBytes[byteIndex];
        const std::int32_t g = imageBytes[byteIndex + 1];
        const std::int32_t r = imageBytes[byteIndex + 2];

        const std::uint32_t packed = static_cast<std::uint32_t>(b << 16 | g << 8 | r);
        bgrs[pixelIdx] = packed;

        byteIndex += 4;
    }

    return bgrs;
}

/// 把单个形状按其存档颜色画上位图画布:分段色存档非空时逐行取色(与任务落画同一路径),
/// 否则整形状单色。alpha 预乘混合公式与 drawLines 一致,帧画面与运行时位图同源。
void drawShapeOnCanvas(geometrize::Bitmap& canvas, const geometrize::ShapeResult& shape)
{
    std::vector<geometrize::Scanline> lines{shape.shape->rasterize(*shape.shape)};
    if(!shape.segments.empty()) {
        std::vector<geometrize::rgba> colors;
        colors.reserve(shape.segments.size());
        for(const geometrize::ScanlineColor& segment : shape.segments) {
            colors.push_back(segment.color);
        }
        geometrize::drawLinesSegmented(canvas, colors, lines);
    } else {
        geometrize::drawLines(canvas, shape.color, lines);
    }
}

/// 增量帧渲染:画布逐帧累积新形状的光栅化混合。相比"每帧序列化 SVG 整帧重放"的
/// O(N²) 老路线(百万形状 = 10^11 级绘制指令,不可完成),总成本 O(N)。
class IncrementalFrameRenderer
{
public:
    IncrementalFrameRenderer(const std::uint32_t width, const std::uint32_t height) :
        m_canvas{width, height, geometrize::rgba{0, 0, 0, 0}}
    {
    }

    /// 把 [first, last) 区间的形状画上画布(区间来自调用侧的帧切片)
    void drawRange(const geometrize::ShapeResult* first, const geometrize::ShapeResult* const last)
    {
        for(; first != last; ++first) {
            drawShapeOnCanvas(m_canvas, *first);
        }
    }

    QImage snapshot() const
    {
        return geometrize::image::createImage(m_canvas);
    }

private:
    geometrize::Bitmap m_canvas;
};

bool addFrame(
    QImage image,
    const std::uint32_t outputWidth,
    const std::uint32_t outputHeight,
    const std::uint32_t delayMs,
    blk::BurstLinker& gif)
{
    // 深拷贝钉死为 Format_ARGB32:makeImageData 按 ARGB 内存序读通道(见其注释);
    // scaled 放大后同样保持该格式,通道序全程确定
    image = image.convertToFormat(QImage::Format_ARGB32);
    if(image.isNull() || image.width() == 0 || image.height() == 0) {
        assert(0 && "Attempted to add invalid image");
        return false;
    }
    if(static_cast<std::uint32_t>(image.width()) != outputWidth || static_cast<std::uint32_t>(image.height()) != outputHeight) {
        image = image.scaled(static_cast<int>(outputWidth), static_cast<int>(outputHeight),
                             Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_ARGB32);
    }

    gif.connect(makeImageData(image), delayMs, blk::QuantizerType::Octree, blk::DitherType::NO, 0, 0);

    return true;
}

}

namespace geometrize
{

namespace exporter
{

bool exportGIF(
        const std::vector<geometrize::ShapeResult>& data,
        std::uint32_t inputWidth,
        std::uint32_t inputHeight,
        std::uint32_t outputWidth,
        std::uint32_t outputHeight,
        const std::function<bool(std::size_t)>& frameSkipPredicate,
        const std::string& filePath,
        const std::uint32_t frameDelayMs,
        const std::uint32_t loopCount,
        const std::uint32_t endPauseMs)
{
    if(data.empty()) {
        return false;
    }

    // loopCount=0 为 GIF 语义的无限循环,BurstLinker 直接透传
    blk::BurstLinker gif;
    if(!gif.init(filePath.c_str(), outputWidth, outputHeight, loopCount, std::max(1U, std::thread::hardware_concurrency()))) {
        assert(0 && "Failed to initialize BurstLinker GIF export");
        return false;
    }

    // 增量帧:画布按帧切片逐段累积混合,帧画面与任务运行时位图同源(同一套光栅化+混合函数)。
    // 画布透明底:上游 GUI 落画前有背景矩形(index 0)兜底,老路线的 SVG 渲染也是透明底。
    IncrementalFrameRenderer renderer{inputWidth, inputHeight};

    // 帧数封顶:百万形状按默认步长会产生 50 万+ 帧(数 GB 体积,播放器也扛不住)。
    // 超限时自动加大取帧间隔,保证帧数不超过上限(均匀采样,不偏头尾)。
    constexpr std::size_t maxFrames{1000};
    std::size_t keptFrames = 0;
    for(std::size_t i = 0; i < data.size(); i++) {
        if(!frameSkipPredicate(i)) {
            keptFrames++;
        }
    }
    std::size_t frameStride = 1; // 保留帧之间的采样步进(在 frameSkipPredicate 已筛过的基础上再均匀抽稀)
    if(keptFrames > maxFrames) {
        frameStride = (keptFrames + maxFrames - 1) / maxFrames;
    }

    std::size_t frameBegin = 0;
    std::size_t keptIndex = 0;
    for(std::size_t i = 0; i < data.size(); i++) {
        const bool isLast{(i == data.size() - 1U)};
        // 末帧必取:末索引若被 frameSkipPredicate 提前 continue 掉,末帧与结尾停顿会一起丢失
        // (默认 frameStep=20 时"形状数不是 20 的倍数"就是常态,等于该分支基本不可达)
        if(frameSkipPredicate(i) && !isLast) {
            continue;
        }
        const bool takeFrame = (keptIndex % frameStride == 0) || isLast; // 末帧必取
        keptIndex++;

        // 固定帧率模式:统一延迟(下限 20ms,GIF 播放器对更短延迟的兼容性差);
        // frameDelayMs=0 保持动态曲线旧行为(帧序号越大延迟越短)
        std::uint32_t delayMs = (frameDelayMs > 0)
            ? std::max(20U, frameDelayMs)
            : (i == 0 ? 0U : std::max(20, static_cast<std::int32_t>(1000 / (i + 1))));
        if(endPauseMs > 0 && isLast) {
            delayMs = endPauseMs; // Extra delay at end of animation
        }

        if(takeFrame) {
            // 帧切片 [frameBegin, i+1):本帧把新增形状画上画布后取快照
            renderer.drawRange(data.data() + frameBegin, data.data() + i + 1);
            frameBegin = i + 1;
            addFrame(renderer.snapshot(), outputWidth, outputHeight, delayMs, gif);
        }
    }

    gif.release();
    return true;
}

}

}
