// 矢量图形视图渲染开销对比(离屏,无交互)。四种实现:
//   旧      :每批新形状一个全画布 SvgItem(上游/当前)
//   单 item :全部形状累积在一个 SvgItem
//   分块    :多个 SvgItem,当前块累积刷新、块满封板
//   单件多渲:一个自绘 item + 多个分块渲染器(封板块解析一次不再重解析)
// 只依赖 Qt5 + geometrize-fast.lib(真实 exporter)。
// 用法: svgscene_bench.exe [分块阈值,默认 256]
#include <QApplication>
#include <QElapsedTimer>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QGraphicsSvgItem>
#include <QGraphicsView>
#include <QImage>
#include <QPainter>
#include <QPixmapCache>
#include <QSvgRenderer>
#include <QWidget>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "geometrize/exporter/svgexporter.h"
#include "geometrize/shape/circle.h"
#include "geometrize/shape/ellipse.h"
#include "geometrize/shape/rectangle.h"
#include "geometrize/shaperesult.h"

namespace
{

QByteArray documentBytes(const std::string& shapeData);

class SvgItem : public QGraphicsSvgItem
{
public:
    explicit SvgItem(const QByteArray& data) : QGraphicsSvgItem{}
    {
        setSharedRenderer(new QSvgRenderer(data));
        setFlag(ItemIsMovable, false);
    }
    ~SvgItem() override
    {
        delete renderer();
    }
    void setDocument(const QByteArray& data)
    {
        QSvgRenderer* previous{renderer()};
        setSharedRenderer(new QSvgRenderer(data));
        delete previous;
    }
};

// 候选实现:单个 item 自绘全部块。封板块的渲染器只解析一次并保留,当前块的渲染器按批重建。
class SvgChunksItem : public QGraphicsItem
{
public:
    SvgChunksItem(const int width, const int height) : m_bounds{0.0, 0.0, static_cast<double>(width), static_cast<double>(height)}
    {
    }
    ~SvgChunksItem() override
    {
        for(QSvgRenderer* renderer : m_sealed) {
            delete renderer;
        }
        delete m_live;
    }

    QRectF boundingRect() const override
    {
        return m_bounds;
    }

    void setChunkLimit(const std::size_t limit)
    {
        m_chunkLimit = limit;
    }

    // 追加一批形状的 SVG 片段;返回是否发生了封板
    bool appendBatch(const std::string& fragments, const std::size_t shapeCount)
    {
        bool sealedNow = false;
        if(m_live != nullptr && m_liveShapeCount >= m_chunkLimit) {
            m_sealed.push_back(m_live); // 封板:渲染器整体移交,不再重解析
            m_live = nullptr;
            m_liveData.clear();
            m_liveShapeCount = 0;
            sealedNow = true;
        }
        m_liveData.append(fragments);
        m_liveShapeCount += shapeCount;
        delete m_live;
        m_live = new QSvgRenderer(documentBytes(m_liveData));
        update(); // 使设备坐标缓存失效,下一帧重绘
        return sealedNow;
    }

    std::size_t rendererCount() const
    {
        return m_sealed.size() + (m_live != nullptr ? 1U : 0U);
    }

    void paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) override
    {
        for(QSvgRenderer* renderer : m_sealed) {
            renderer->render(painter, m_bounds);
        }
        if(m_live != nullptr) {
            m_live->render(painter, m_bounds);
        }
    }

private:
    QRectF m_bounds;
    std::vector<QSvgRenderer*> m_sealed;
    std::string m_liveData;
    QSvgRenderer* m_live{nullptr};
    std::size_t m_liveShapeCount{0};
    std::size_t m_chunkLimit{256};
};

constexpr int IMAGE_W = 512;
constexpr int IMAGE_H = 384;
constexpr int VIEW_W = 900;
constexpr int VIEW_H = 600;
constexpr int BATCH = 4; // 每个刷新窗口(33ms)累积的形状数

std::size_t CHUNK_LIMIT = 256; // 与 imagetasksvgscene.cpp 的阈值一致

unsigned rngState = 12345U;
int nextRandom(const int low, const int high)
{
    rngState = rngState * 1103515245U + 12345U;
    const unsigned r = (rngState >> 16) & 0x7FFFU;
    return low + static_cast<int>(r % static_cast<unsigned>(high - low + 1));
}

std::vector<geometrize::ShapeResult> makeBatch(const int count)
{
    std::vector<geometrize::ShapeResult> shapes;
    shapes.reserve(static_cast<std::size_t>(count));
    for(int i = 0; i < count; i++) {
        std::shared_ptr<geometrize::Shape> shape;
        switch(nextRandom(0, 2)) {
        case 0:
            shape = std::make_shared<geometrize::Circle>(static_cast<float>(nextRandom(0, IMAGE_W - 1)), static_cast<float>(nextRandom(0, IMAGE_H - 1)), static_cast<float>(nextRandom(1, 24)));
            break;
        case 1:
            shape = std::make_shared<geometrize::Ellipse>(static_cast<float>(nextRandom(0, IMAGE_W - 1)), static_cast<float>(nextRandom(0, IMAGE_H - 1)), static_cast<float>(nextRandom(1, 24)), static_cast<float>(nextRandom(1, 24)));
            break;
        default:
            shape = std::make_shared<geometrize::Rectangle>(static_cast<float>(nextRandom(0, IMAGE_W - 8)), static_cast<float>(nextRandom(0, IMAGE_H - 8)), static_cast<float>(nextRandom(8, 40)), static_cast<float>(nextRandom(8, 40)));
            break;
        }
        const geometrize::rgba color{static_cast<std::uint8_t>(nextRandom(0, 255)), static_cast<std::uint8_t>(nextRandom(0, 255)), static_cast<std::uint8_t>(nextRandom(0, 255)), 128};
        shapes.push_back(geometrize::ShapeResult{0.0, color, shape, {}});
    }
    return shapes;
}

std::string fragmentsOf(const std::vector<geometrize::ShapeResult>& shapes, std::size_t& nextId)
{
    std::string body;
    for(const auto& r : shapes) {
        geometrize::exporter::SVGExportOptions options;
        options.itemId = nextId++;
        body.append(geometrize::exporter::getSingleShapeSVGData(r.color, *(r.shape), r.segments, options));
    }
    return body;
}

QByteArray documentBytes(const std::string& shapeData)
{
    const std::string document{geometrize::exporter::exportSVGDocument(shapeData, IMAGE_W, IMAGE_H)};
    return QByteArray{document.data(), static_cast<int>(document.size())};
}

double renderView(QGraphicsView& view)
{
    QImage canvas(view.viewport()->size(), QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::black);
    QElapsedTimer timer;
    timer.start();
    QPainter painter(&canvas);
    view.render(&painter);
    painter.end();
    return static_cast<double>(timer.nsecsElapsed()) / 1.0e6;
}

QImage grab(QGraphicsView& view)
{
    QImage canvas(view.viewport()->size(), QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::black);
    QPainter painter(&canvas);
    view.render(&painter);
    painter.end();
    return canvas;
}

struct SceneBase
{
    QGraphicsScene scene;
    QGraphicsView view;
    std::size_t itemCount{0};

    SceneBase()
    {
        view.setScene(&scene);
        view.resize(VIEW_W, VIEW_H);
        view.setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
    }

    SvgItem* createItem(const QByteArray& data)
    {
        auto* item = new SvgItem(data);
        item->setFlags(QGraphicsItem::ItemClipsToShape);
        item->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
        scene.addItem(item);
        item->setZValue(0);
        itemCount++;
        return item;
    }

    void fit()
    {
        view.fitInView(scene.itemsBoundingRect().adjusted(-20, -20, 20, 20), Qt::KeepAspectRatio);
    }
};

struct PerBatchScene : SceneBase
{
    std::size_t nextId{0};
};
void addBatchPerBatch(PerBatchScene& s, const std::vector<geometrize::ShapeResult>& shapes)
{
    s.createItem(documentBytes(fragmentsOf(shapes, s.nextId)));
}

struct SingleItemScene : SceneBase
{
    std::string accumulated;
    SvgItem* live{nullptr};
    std::size_t nextId{0};
};
void addBatchSingle(SingleItemScene& s, const std::vector<geometrize::ShapeResult>& shapes)
{
    s.accumulated.append(fragmentsOf(shapes, s.nextId));
    const QByteArray data = documentBytes(s.accumulated);
    if(s.live == nullptr) {
        s.live = s.createItem(data);
    } else {
        s.live->setDocument(data);
    }
}

struct ChunkedScene : SceneBase
{
    std::string liveData;
    SvgItem* live{nullptr};
    std::size_t liveShapeCount{0};
    std::size_t nextId{0};
};
void addBatchChunked(ChunkedScene& s, const std::vector<geometrize::ShapeResult>& shapes)
{
    if(s.live != nullptr && s.liveShapeCount >= CHUNK_LIMIT) {
        s.live = nullptr;
        s.liveData.clear();
        s.liveShapeCount = 0;
    }
    s.liveData.append(fragmentsOf(shapes, s.nextId));
    s.liveShapeCount += shapes.size();
    const QByteArray data = documentBytes(s.liveData);
    if(s.live == nullptr) {
        s.live = s.createItem(data);
    } else {
        s.live->setDocument(data);
    }
}

struct ChunksItemScene : SceneBase
{
    SvgChunksItem* item{nullptr};
    std::size_t nextId{0};
};
void addBatchChunksItem(ChunksItemScene& s, const std::vector<geometrize::ShapeResult>& shapes)
{
    if(s.item == nullptr) {
        s.item = new SvgChunksItem(IMAGE_W, IMAGE_H);
        s.item->setChunkLimit(CHUNK_LIMIT);
        s.item->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
        s.item->setZValue(0);
        s.scene.addItem(s.item);
        s.itemCount = 1;
    }
    s.item->appendBatch(fragmentsOf(shapes, s.nextId), shapes.size());
}

struct Sample
{
    double refreshMs{0.0}; // 刷新一帧:本批内容变更 + 该帧渲染
    double steadyMs{0.0};  // 稳态帧:内容未变,仅重绘
    std::size_t items{0};
};

template<typename Scene, typename AddBatch>
Sample run(Scene& s, const AddBatch& addBatch, const int n, QImage* out = nullptr)
{
    rngState = 12345U;
    for(int i = 0; i + BATCH < n; i += BATCH) {
        addBatch(s, makeBatch(BATCH));
    }
    s.fit();
    renderView(s.view); // 预热设备坐标缓存

    QElapsedTimer timer;
    timer.start();
    addBatch(s, makeBatch(BATCH));
    const double updateMs = static_cast<double>(timer.nsecsElapsed()) / 1.0e6;
    const double renderAfterUpdateMs = renderView(s.view);
    const double steadyMs = renderView(s.view);

    if(out != nullptr) {
        *out = grab(s.view);
    }
    return Sample{updateMs + renderAfterUpdateMs, steadyMs, s.itemCount};
}

void compare(const QImage& a, const QImage& b, const char* label)
{
    long long diffPixels = 0;
    long long over1 = 0;
    int maxChannel = 0;
    double sumAbs = 0.0;
    for(int y = 0; y < a.height(); y++) {
        const auto* pa = reinterpret_cast<const QRgb*>(a.constScanLine(y));
        const auto* pb = reinterpret_cast<const QRgb*>(b.constScanLine(y));
        for(int x = 0; x < a.width(); x++) {
            const int dr = std::abs(qRed(pa[x]) - qRed(pb[x]));
            const int dg = std::abs(qGreen(pa[x]) - qGreen(pb[x]));
            const int db = std::abs(qBlue(pa[x]) - qBlue(pb[x]));
            const int m = (std::max)(dr, (std::max)(dg, db));
            if(m != 0) {
                diffPixels++;
            }
            if(m > 1) {
                over1++;
            }
            maxChannel = (std::max)(maxChannel, m);
            sumAbs += (dr + dg + db) / 3.0;
        }
    }
    const double total = static_cast<double>(a.width()) * static_cast<double>(a.height());
    std::printf("  %-22s 不同像素 %7lld (%5.2f%%),单通道差>1 的像素 %6lld (%5.2f%%),单通道最大差 %d,平均绝对差 %.4f\n",
                label, diffPixels, 100.0 * diffPixels / total, over1, 100.0 * over1 / total, maxChannel, sumAbs / total);
}

}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    if(argc > 1) {
        CHUNK_LIMIT = static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10));
    }
    if(argc > 2) {
        // 提高 Qt 全局 pixmap 缓存上限(默认 10MB):设备坐标缓存的 item 位图受它约束
        const int mb = std::atoi(argv[2]);
        QPixmapCache::setCacheLimit(mb * 1024);
        std::printf("QPixmapCache limit = %d MB\n", mb);
    }

    std::printf("画布 %dx%d,视口 %dx%d,每批 %d 形状,分块阈值 %zu\n\n", IMAGE_W, IMAGE_H, VIEW_W, VIEW_H, BATCH, CHUNK_LIMIT);
    std::printf("%7s | %6s %8s %8s | %6s %8s %8s | %6s %8s %8s %7s | %6s %8s %8s %7s\n",
                "形状数", "旧:item", "旧:刷新", "旧:稳态",
                "单:item", "单:刷新", "单:稳态",
                "块:item", "块:刷新", "块:稳态", "块:内存",
                "件:item", "件:刷新", "件:稳态", "件:内存");
    std::printf("---------------------------------------------------------------------------------------------------------------------------------\n");

    const double mbPerItem = static_cast<double>(VIEW_W) * VIEW_H * 4.0 / (1024.0 * 1024.0);
    for(const int n : {100, 300, 600, 1200, 2400, 5000, 10000}) {
        Sample sa;
        Sample sb;
        Sample sc;
        Sample sd;
        {
            PerBatchScene a;
            sa = run(a, addBatchPerBatch, n);
        }
        {
            SingleItemScene b;
            sb = run(b, addBatchSingle, n);
        }
        {
            ChunkedScene c;
            sc = run(c, addBatchChunked, n);
        }
        {
            ChunksItemScene d;
            sd = run(d, addBatchChunksItem, n);
        }
        std::printf("%7d | %6zu %5.1f ms %5.1f ms | %6zu %5.1f ms %5.1f ms | %6zu %5.1f ms %5.1f ms %4.0f MB | %6zu %5.1f ms %5.1f ms %4.0f MB\n",
                    n,
                    sa.items, sa.refreshMs, sa.steadyMs,
                    sb.items, sb.refreshMs, sb.steadyMs,
                    sc.items, sc.refreshMs, sc.steadyMs, static_cast<double>(sc.items) * mbPerItem,
                    sd.items, sd.refreshMs, sd.steadyMs, static_cast<double>(sd.items) * mbPerItem);
        std::fflush(stdout);
    }

    std::printf("\n等价性检查(600 形状,同形状序列,基准=旧实现):\n");
    QImage imageA;
    QImage imageB;
    QImage imageC;
    QImage imageD;
    {
        PerBatchScene a;
        run(a, addBatchPerBatch, 600, &imageA);
    }
    {
        SingleItemScene b;
        run(b, addBatchSingle, 600, &imageB);
    }
    {
        ChunkedScene c;
        run(c, addBatchChunked, 600, &imageC);
    }
    {
        ChunksItemScene d;
        run(d, addBatchChunksItem, 600, &imageD);
    }
    compare(imageA, imageB, "单 item");
    compare(imageA, imageC, "分块");
    compare(imageA, imageD, "单件多渲染器");

    return 0;
}
