#include "imagetasksvgscene.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <QByteArray>
#include <QGraphicsSvgItem>
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

}

class ImageTaskSvgScene::ImageTaskSvgSceneImpl
{
public:
    ImageTaskSvgSceneImpl(ImageTaskSvgScene* pQ) : q{pQ}, m_width{0}, m_height{0}, m_liveItem{nullptr}, m_liveShapeCount{0}, m_nextShapeId{0}
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
                q->addItem(m_liveItem);
                m_liveItem->setZValue(0);
                m_items.push_back(m_liveItem);
            } else {
                m_liveItem->setDocument(documentData);
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

}

}
