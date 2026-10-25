#include "imagetasksvgscene.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <QByteArray>
#include <QGraphicsScene>
#include <QGraphicsSvgItem>
#include <QGraphicsView>
#include <QPixmapCache>
#include <QSize>
#include <QSvgRenderer>

#include "geometrize/shaperesult.h"
#include "geometrize/exporter/svgexporter.h"

#include "scene/imagetaskpixmapgraphicsitem.h"
#include "scene/svgitem.h"

namespace geometrize
{

namespace scene
{

namespace
{

// 分块封板阈值:一个 item 承载的形状数上限。
// 块越大 → item 越少(每帧合成层数少、缓存内存小),但每次刷新要重解析的文档越大;
// 块越小 → 刷新越轻,但 item 数随形状数线性增长(退回上游的老问题)。
// 256 在两侧都留足余量:刷新只重解析当前块,帧开销只跟"形状数/256"成正比。
constexpr std::size_t SVG_CHUNK_SHAPE_LIMIT{256};

// 单个 item 的设备坐标缓存上限。**必须给足但不能给太大**,这是实测出来的:
// Qt 的 DeviceCoordinateCache 有一条"设备矩形超过该上限就完全旁路缓存、每帧直接 paint"的判定
// (qgraphicsscene.cpp 的 DeviceCoordinateCache 分支,比较的是**整幅 item 的设备矩形**,未裁视口),
// 而本场景每个分块 item 的 boundingRect 都是整幅画布,设备矩形随缩放线性增长。
//   · 上限太小(如 Qt 默认 1024×768):低倍率就旁路,丢掉拖动/生成时的缓存收益;
//   · 上限给足(32767):高倍率下 Qt 会按**整幅设备矩形**去建/维护缓存位图,1024 画布 ×16 缩放
//     = 16384×12288 的设备矩形,实测单次缩放 tick 从 11 ms 恶化到 15769 ms(离屏基准,见
//     tools/svgscene-bench 的交互口径),拖动/生成同步崩掉;
//   · 4096 是实测的甜点:低倍率(<4x)缓存生效(拖动 tick 1.9 ms、生成帧 11 ms),
//     高倍率自动旁路退化成直接渲染(缩放 tick 10~30 ms),两头都不炸。
// 真正让高倍率交互变顺滑的是"交互期切 item 坐标缓存"(见 setInteractiveRendering),
// 它把每 tick 的重光栅化彻底消掉(实测 ~1 ms/tick),不依赖这里的取值。
constexpr int SVG_ITEM_MAX_DEVICE_CACHE_SIZE{4096};

// 交互期 item 坐标缓存的总内存预算(按块数摊到每块,见 interactiveCacheSize)
constexpr int INTERACTIVE_CACHE_BUDGET_MB{192};

// 设备坐标缓存的全局预算上下限(按块数动态抬高 Qt 全局 pixmap 缓存上限)
constexpr int PIXMAP_CACHE_FLOOR_MB{128};
constexpr int PIXMAP_CACHE_CEILING_MB{768};

}

class ImageTaskSvgScene::ImageTaskSvgSceneImpl
{
public:
    ImageTaskSvgSceneImpl(ImageTaskSvgScene* pQ) : q{pQ}, m_width{0}, m_height{0}, m_liveItem{nullptr}, m_liveShapeCount{0}, m_nextShapeId{0}, m_interactive{false}
    {
    }
    ImageTaskSvgSceneImpl operator=(const ImageTaskSvgSceneImpl&) = delete;
    ImageTaskSvgSceneImpl(const ImageTaskSvgSceneImpl&) = delete;
    ~ImageTaskSvgSceneImpl() = default;

    void addShapes(const std::vector<geometrize::ShapeResult>& shapes, const std::uint32_t width, const std::uint32_t height)
    {
        if(shapes.empty()) {
            return;
        }

        // 形状按块累积。上游实现是每批新形状新建一个 item,而每份 SVG 文档的 viewBox 都是整幅画布,
        // 于是每个 item 的 boundingRect 都是整幅画布、在设备坐标缓存下各持一张全画布位图:
        // 形状到几百后每帧要合成几百层全画布图层,内存与帧耗时随形状数线性增长(越多越卡)。
        // 这里改为:当前块 append-only 累积片段,刷新只重拼/重解析当前块;块满即封板
        // (封板后不再重解析,也不改动),图层数因此是 形状数/256 量级而非 形状数/批 量级。
        if(m_items.empty() || m_width != width || m_height != height) {
            removeShapes();
            m_width = width;
            m_height = height;
        }

        // 一次调用可能带一整批形状(撤销/重做重放会把全部形状一次送来),故按块余量切分循环,
        // 保证单块不超过阈值——否则一次重放就生成一个巨型块,该块每次刷新都要整体重解析。
        std::size_t index{0};
        while(index < shapes.size()) {
            if(m_liveItem != nullptr && m_liveShapeCount >= SVG_CHUNK_SHAPE_LIMIT) {
                m_liveItem = nullptr; // 封板:该 item 留在场景中,不再重建
                m_liveData.clear();
                m_liveShapeCount = 0;
            }

            const std::size_t room{SVG_CHUNK_SHAPE_LIMIT - m_liveShapeCount};
            const std::size_t count{(std::min)(room, shapes.size() - index)};
            for(std::size_t i = 0; i < count; i++) {
                const geometrize::ShapeResult& result{shapes[index + i]};
                geometrize::exporter::SVGExportOptions options;
                options.itemId = m_nextShapeId++;
                // 与 exportSVG 逐形状同一调用:segments 非空走 A2.4 色带组,空走基元单色
                m_liveData.append(geometrize::exporter::getSingleShapeSVGData(result.color, *(result.shape), result.segments, options));
            }
            m_liveShapeCount += count;
            index += count;

            const std::string document{geometrize::exporter::exportSVGDocument(m_liveData, width, height)};
            const QByteArray documentData{document.data(), static_cast<int>(document.size())};

            if(m_liveItem == nullptr) {
                m_liveItem = new SvgItem(documentData);
                m_liveItem->setFlags(QGraphicsItem::ItemClipsToShape);
                // 静态 SVG 用设备坐标位图缓存,命中后免去每帧对 item 重过 QSvgRenderer
                m_liveItem->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
                // 取值理由与实测见 SVG_ITEM_MAX_DEVICE_CACHE_SIZE 处的说明(不是越大越好)。
                m_liveItem->setMaximumCacheSize(QSize(SVG_ITEM_MAX_DEVICE_CACHE_SIZE, SVG_ITEM_MAX_DEVICE_CACHE_SIZE));
                q->addItem(m_liveItem);
                m_liveItem->setZValue(0);
                m_items.push_back(m_liveItem);
                if(m_interactive) {
                    // 交互期新建的块直接按降级模式起(例如生成过程中正在缩放/拖动)
                    m_liveItem->setCacheMode(QGraphicsItem::ItemCoordinateCache, interactiveCacheSize());
                }
            } else {
                m_liveItem->setDocument(documentData);
            }
        }

        tunePixmapCacheForChunkCount();
    }

    // Qt 全局 pixmap 缓存装的是"每个分块 item 各自的缓存位图",总量随块数增长:
    //   · 设备坐标缓存(常态):单块受视口约束(见 maximumCacheSize 处的说明);
    //   · item 坐标缓存(交互期):单块 = interactiveCacheSize(),由 192MB 预算按块数摊分。
    // 块数一多就会超出默认上限、开始抖动重渲染,故按块数抬高上限;封顶防止失控。
    // 实测(离屏基准,≤5000 形状/20 块):128MB 与 512MB 无差别 ⇒ 这里在常见规模下是空操作,
    // 只在上万形状时兜底。
    void tunePixmapCacheForChunkCount()
    {
        QSize viewport{1024, 768};
        const QList<QGraphicsView*> sceneViews{q->views()};
        if(!sceneViews.empty() && sceneViews.front() != nullptr) {
            viewport = sceneViews.front()->viewport()->size();
        }

        const QSize interactiveSize{interactiveCacheSize()};
        const double deviceBytes{static_cast<double>(viewport.width()) * static_cast<double>(viewport.height()) * 1.44 * 4.0};
        const double interactiveBytes{static_cast<double>(interactiveSize.width()) * static_cast<double>(interactiveSize.height()) * 4.0};
        const double neededMb{static_cast<double>(m_items.size()) * (deviceBytes + interactiveBytes) / (1024.0 * 1024.0)};

        int mb{PIXMAP_CACHE_FLOOR_MB};
        if(neededMb > static_cast<double>(PIXMAP_CACHE_FLOOR_MB)) {
            mb = static_cast<int>(neededMb) + 32;
        }
        mb = (std::min)(mb, PIXMAP_CACHE_CEILING_MB);

        if(QPixmapCache::cacheLimit() != mb * 1024) {
            QPixmapCache::setCacheLimit(mb * 1024);
        }
    }

    // 交互期 item 坐标缓存的逻辑尺寸(即缓存位图的分辨率):以画布尺寸为上限(此时 1x 下不失真),
    // 再按块数与内存预算收缩——大画布 × 多块时不能让每块都按画布尺寸分配。
    QSize interactiveCacheSize() const
    {
        const int width{static_cast<int>(m_width)};
        const int height{static_cast<int>(m_height)};
        if(width <= 0 || height <= 0) {
            return QSize((std::max)(width, 1), (std::max)(height, 1));
        }
        if(m_items.empty()) {
            return QSize(width, height);
        }

        const double budgetBytes{static_cast<double>(INTERACTIVE_CACHE_BUDGET_MB) * 1024.0 * 1024.0};
        const double perChunkBytes{budgetBytes / static_cast<double>(m_items.size())};
        const double maxSide{std::sqrt(perChunkBytes / 4.0)};
        const double longestSide{static_cast<double>((std::max)(width, height))};
        const double scale{(std::min)(1.0, maxSide / longestSide)};

        return QSize((std::max)(1, static_cast<int>(static_cast<double>(width) * scale)),
                     (std::max)(1, static_cast<int>(static_cast<double>(height) * scale)));
    }

    void setInteractiveRendering(const bool interactive)
    {
        if(m_interactive == interactive) {
            return;
        }
        m_interactive = interactive;

        const QSize logicalSize{interactiveCacheSize()};
        for(SvgItem* item : m_items) {
            if(interactive) {
                // item 坐标缓存不随视口变换失效 ⇒ 缩放/拖动每个 tick 只是位图缩放合成
                item->setCacheMode(QGraphicsItem::ItemCoordinateCache, logicalSize);
            } else {
                // 先经 NoCache 清掉 item 坐标缓存(QGraphicsItem::setCacheMode 只重置缓存,
                // 不会清 fixedSize;残留会让 QGraphicsScene 继续按 item 坐标缓存分支走),
                // 再设回设备坐标缓存恢复清晰渲染。
                item->setCacheMode(QGraphicsItem::NoCache);
                item->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
                item->setMaximumCacheSize(QSize(SVG_ITEM_MAX_DEVICE_CACHE_SIZE, SVG_ITEM_MAX_DEVICE_CACHE_SIZE));
            }
        }
    }

    void removeShapes()
    {
        for(SvgItem* item : m_items) {
            q->removeItem(item);
            delete item;
        }
        m_items.clear();
        m_liveItem = nullptr;
        m_liveData.clear();
        m_liveShapeCount = 0;
        m_nextShapeId = 0;
        m_interactive = false; // 交互态随场景内容一起作废,新块一律以清晰的设备坐标缓存起
        tunePixmapCacheForChunkCount(); // 块数归零,把上限退回基准值
    }

private:
    ImageTaskSvgScene* q;

    std::uint32_t m_width;
    std::uint32_t m_height;
    std::vector<SvgItem*> m_items; ///> 已封板的块 + 当前块(按插入序,即绘制序)
    std::string m_liveData; ///> 当前块已累积的形状元素文本(不含 <svg> 包装)
    SvgItem* m_liveItem; ///> 当前块对应的 item(等于 m_items.back();封板后置空)
    std::size_t m_liveShapeCount; ///> 当前块已累积的形状数
    std::size_t m_nextShapeId; ///> 下一个形状的 SVG id(与 exportSVG 的逐形状编号语义一致)
    bool m_interactive; ///> 是否处于交互期(块 item 用降级的 item 坐标缓存)
};

ImageTaskSvgScene::ImageTaskSvgScene() : ImageTaskScene(), d{std::make_unique<ImageTaskSvgScene::ImageTaskSvgSceneImpl>(this)}
{
}

ImageTaskSvgScene::~ImageTaskSvgScene()
{
}

void ImageTaskSvgScene::addShapes(const std::vector<geometrize::ShapeResult>& shapes, const std::uint32_t width, const std::uint32_t height)
{
    d->addShapes(shapes, width, height);
}

void ImageTaskSvgScene::removeShapes()
{
    d->removeShapes();
}

void ImageTaskSvgScene::setInteractiveRendering(const bool interactive)
{
    d->setInteractiveRendering(interactive);
}

}

}
