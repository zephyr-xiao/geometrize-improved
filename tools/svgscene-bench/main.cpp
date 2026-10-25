// 矢量图形视图渲染开销对比(离屏,无交互)。四种实现:
//   旧      :每批新形状一个全画布 SvgItem(上游/当前)
//   单 item :全部形状累积在一个 SvgItem
//   分块    :多个 SvgItem,当前块累积刷新、块满封板(应用采用的实现)
//   单件多渲:一个自绘 item + 多个分块渲染器(封板块解析一次不再重解析)
//
// 第十九批续(F3.9 后续)追加**交互口径**——原来的两个口径(刷新帧/稳态帧)都在固定变换下测,
// 漏掉了真正让用户觉得卡的路径:缩放 tick、拖动 tick、生成帧。新增部分在同一进程内对
// 两种 maximumCacheSize 配置做同会话对比(跨轮比会被机器负载噪声污染,见 ROADMAP 陷阱 #28):
//   Qt 的 DeviceCoordinateCache 有一条"设备矩形超过 ExtraMaxDeviceCoordCacheSize 就完全旁路缓存"
//   的判定(qgraphicsscene.cpp),用的是**整幅 item 的设备矩形**(未裁视口);分块 item 的
//   boundingRect 是整幅画布,于是缩放一大就旁路 → 每次重绘都重新光栅化该块全部形状
//   → 每帧 O(形状数)。基准用它复现/验证该悬崖。
//
// 只依赖 Qt6 + geometrize-fast.lib(真实 exporter)。
// 用法: svgscene_bench.exe [分块阈值=256] [QPixmapCache 上限 MB=128] [最大设备缓存尺寸=4096] [四实现对比的形状数上限=10000]
//   第三个参数对应应用里的 setMaximumCacheSize;0 = 用 Qt 默认(1024x768),负数 = 给足(32767)。
//   第四个参数用于跳过最慢的"旧实现"大形状数行(2500 个 item 时单帧要秒级,历史数据已在 bugfix-triage C.3.1)。
#include <QApplication>
#include <QElapsedTimer>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QGraphicsSvgItem>
#include <QGraphicsView>
#include <QImage>
#include <QPainter>
#include <QPixmapCache>
#include <QScrollBar>
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

constexpr int VIEW_W = 900;
constexpr int VIEW_H = 600;
constexpr int BATCH = 4; // 每个刷新窗口(33ms)累积的形状数

// 四实现对比沿用历史口径 512x384(与 bugfix-triage C.3.1 可比);
// 交互口径改用应用默认处理分辨率 1024x768 —— 门槛 4096 在 1024 画布上缩放 4 倍即触顶,
// 正是用户"几千形状 + 放大看就卡"的场景。
int IMAGE_W = 512;
int IMAGE_H = 384;
constexpr int INTERACT_IMAGE_W = 1024;
constexpr int INTERACT_IMAGE_H = 768;

std::size_t CHUNK_LIMIT = 256;   // 与 imagetasksvgscene.cpp 的阈值一致
int MAX_CACHE_SIZE = 4096;       // 与 imagetasksvgscene.cpp 的 setMaximumCacheSize 一致;0=Qt 默认,<0=给足
bool CLIP_TO_SHAPE = true;       // 与应用一致:item 设 ItemClipsToShape(每帧 setClip 到形状)

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

// 复用同一张画布,避免每次 render 都重新分配(会盖住亚毫秒级差异)
QImage& renderCanvas(const QSize& size)
{
    static QImage canvas;
    if(canvas.size() != size) {
        canvas = QImage(size, QImage::Format_ARGB32_Premultiplied);
    }
    canvas.fill(Qt::black);
    return canvas;
}

double renderView(QGraphicsView& view)
{
    QImage& canvas = renderCanvas(view.viewport()->size());
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

double median(std::vector<double> values)
{
    if(values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
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
        view.setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
        view.setDragMode(QGraphicsView::ScrollHandDrag);
        view.setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        view.setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    }

    SvgItem* createItem(const QByteArray& data)
    {
        auto* item = new SvgItem(data);
        if(CLIP_TO_SHAPE) {
            item->setFlags(QGraphicsItem::ItemClipsToShape);
        }
        item->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
        if(MAX_CACHE_SIZE != 0) {
            // 与应用同一处开关:超过该设备尺寸就完全旁路设备坐标缓存(每帧重光栅化)
            item->setMaximumCacheSize(QSize(MAX_CACHE_SIZE, MAX_CACHE_SIZE));
        }
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

// ---- 交互口径(F3.9 后续) ----

// 缩放观察倍率:1024x768 画布下 4x 已越过 4096 门槛,6x/8x/16x 必然越过得更多
const std::vector<double> ZOOM_FACTORS{1.0, 2.0, 4.0, 6.0, 8.0, 16.0};
// 拖动/生成各测"门槛之下"与"门槛之上"两档
const std::vector<double> DRAG_FACTORS{2.0, 6.0};
const std::vector<double> GEN_FACTORS{1.0, 6.0};

struct InteractiveSample
{
    std::size_t items{0};
    double switchMs{0.0};        // 进入交互期(切 item 坐标缓存)的一次性代价 = 手势起点的顿挫
    std::vector<double> zoomMs;  // 与 ZOOM_FACTORS 一一对应
    std::vector<double> dragMs;  // 与 DRAG_FACTORS 一一对应
    std::vector<double> genMs;   // 与 GEN_FACTORS 一一对应
};

void applyFactor(ChunkedScene& s, const double factor, const QPointF& center)
{
    s.view.resetTransform();
    s.view.scale(factor, factor);
    s.view.centerOn(center);
    renderView(s.view); // 预热该倍率下的缓存
}

// 在给定配置下建一个"分块实现"场景并测三种交互 tick。
// 同进程内换配置重建场景 = 同会话受控 A/B(避免跨轮机器负载噪声)。
struct InteractConfig
{
    const char* label;
    int maxCacheSize;         // setMaximumCacheSize 的值(0=Qt 默认,<0=给足)
    bool interactiveDegrade;  // 模拟"交互期切 item 坐标缓存"的降级渲染
    bool clipToShape;         // item 是否设 ItemClipsToShape
    int cacheMb;              // QPixmapCache 上限
};

InteractiveSample runInteractive(const int n, const InteractConfig& config)
{
    const int savedMax = MAX_CACHE_SIZE;
    const bool savedClip = CLIP_TO_SHAPE;
    MAX_CACHE_SIZE = config.maxCacheSize;
    CLIP_TO_SHAPE = config.clipToShape;
    QPixmapCache::setCacheLimit(config.cacheMb * 1024);

    ChunkedScene s;
    rngState = 12345U;
    for(int i = 0; i + BATCH < n; i += BATCH) {
        addBatchChunked(s, makeBatch(BATCH));
    }
    const QPointF center{s.scene.itemsBoundingRect().center()};

    InteractiveSample out;
    out.items = s.itemCount;

    if(config.interactiveDegrade) {
        // 与应用 setInteractiveRendering(true) 同一手法:item 坐标缓存不随视口变换失效,
        // 缩放/拖动每个 tick 只做位图缩放合成(逻辑尺寸按画布,应用侧另有按块数的内存钳制)。
        // 切换本身会让各块重新光栅化一次——这就是手势起点的一次性顿挫,单独计时。
        QElapsedTimer switchTimer;
        switchTimer.start();
        for(QGraphicsItem* item : s.scene.items()) {
            item->setCacheMode(QGraphicsItem::ItemCoordinateCache, QSize(IMAGE_W, IMAGE_H));
        }
        renderView(s.view); // 触发新缓存的实际建立(下一次绘制才发生)
        out.switchMs = static_cast<double>(switchTimer.nsecsElapsed()) / 1.0e6;
    }

    // 缩放 tick:置到目标倍率并预热,再连续缩放若干 tick 取中位
    for(const double factor : ZOOM_FACTORS) {
        applyFactor(s, factor, center);
        std::vector<double> ticks;
        for(int k = 0; k < 5; k++) {
            s.view.scale(1.05, 1.05);
            ticks.push_back(renderView(s.view));
        }
        out.zoomMs.push_back(median(ticks));
    }

    // 拖动 tick:每次平移 8 像素后重绘
    for(const double factor : DRAG_FACTORS) {
        applyFactor(s, factor, center);
        std::vector<double> drags;
        for(int k = 0; k < 10; k++) {
            s.view.horizontalScrollBar()->setValue(s.view.horizontalScrollBar()->value() + 8);
            drags.push_back(renderView(s.view));
        }
        out.dragMs.push_back(median(drags));
    }

    // 生成帧:追加一批形状(含重拼文档 + 换渲染器)后重绘
    for(const double factor : GEN_FACTORS) {
        applyFactor(s, factor, center);
        std::vector<double> gens;
        for(int k = 0; k < 10; k++) {
            addBatchChunked(s, makeBatch(BATCH));
            gens.push_back(renderView(s.view));
        }
        out.genMs.push_back(median(gens));
    }

    MAX_CACHE_SIZE = savedMax;
    CLIP_TO_SHAPE = savedClip;
    return out;
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
    int cacheMb = 128;
    if(argc > 2) {
        // 提高 Qt 全局 pixmap 缓存上限(默认 10MB):设备坐标缓存的 item 位图受它约束
        cacheMb = std::atoi(argv[2]);
    }
    QPixmapCache::setCacheLimit(cacheMb * 1024);
    if(argc > 3) {
        MAX_CACHE_SIZE = std::atoi(argv[3]);
    }
    int maxShapes = 10000;
    if(argc > 4) {
        maxShapes = std::atoi(argv[4]);
    }

    std::printf("画布 %dx%d,视口 %dx%d,每批 %d 形状,分块阈值 %zu,QPixmapCache %d MB,最大设备缓存尺寸 %d\n\n",
                IMAGE_W, IMAGE_H, VIEW_W, VIEW_H, BATCH, CHUNK_LIMIT, cacheMb, MAX_CACHE_SIZE);
    std::printf("%7s | %6s %8s %8s | %6s %8s %8s | %6s %8s %8s %7s | %6s %8s %8s %7s\n",
                "形状数", "旧:item", "旧:刷新", "旧:稳态",
                "单:item", "单:刷新", "单:稳态",
                "块:item", "块:刷新", "块:稳态", "块:内存",
                "件:item", "件:刷新", "件:稳态", "件:内存");
    std::printf("---------------------------------------------------------------------------------------------------------------------------------\n");

    const double mbPerItem = static_cast<double>(VIEW_W) * VIEW_H * 4.0 / (1024.0 * 1024.0);
    for(const int n : {100, 300, 600, 1200, 2400, 5000, 10000}) {
        if(n > maxShapes) {
            continue;
        }
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

    // ---- 交互口径:同会话对比各配置 ----
    IMAGE_W = INTERACT_IMAGE_W;
    IMAGE_H = INTERACT_IMAGE_H;

    const std::vector<InteractConfig> configs{
        {"现状(4096/裁剪/128MB)", 4096, false, true, 128},
        {"+交互降级", 4096, true, true, 128},
        {"+降级+不裁剪", 4096, true, false, 128},
        {"+降级+缓存512MB", 4096, true, true, 512},
        {"给足(32767)+降级", -1, true, true, 128},
    };
    const std::vector<int> shapeCounts{1000, 2000, 5000};

    std::printf("\n交互口径(分块实现;画布 %dx%d,视口 %dx%d;每格 = 该 tick 的中位耗时 ms)\n",
                INTERACT_IMAGE_W, INTERACT_IMAGE_H, VIEW_W, VIEW_H);
    std::printf("缩放列是 tick 的起始倍率;拖动/生成各测门槛之下(1x/2x)与门槛之上(6x)\n\n");
    std::printf("%7s %5s | %-22s | %8s |", "形状数", "item", "配置", "手势起点");
    for(const double f : ZOOM_FACTORS) {
        std::printf(" %6s", (std::to_string(static_cast<int>(f)) + "x缩放").c_str());
    }
    std::printf(" |");
    for(const double f : DRAG_FACTORS) {
        std::printf(" %6s", (std::to_string(static_cast<int>(f)) + "x拖动").c_str());
    }
    std::printf(" |");
    for(const double f : GEN_FACTORS) {
        std::printf(" %6s", (std::to_string(static_cast<int>(f)) + "x生成").c_str());
    }
    std::printf("\n");

    for(const int n : shapeCounts) {
        for(std::size_t ci = 0; ci < configs.size(); ci++) {
            const InteractConfig& config = configs[ci];
            const InteractiveSample s = runInteractive(n, config);
            if(ci == 0) {
                std::printf("%7d %5zu | %-22s |", n, s.items, config.label);
            } else {
                std::printf("%7s %5s | %-22s |", "", "", config.label);
            }
            if(config.interactiveDegrade) {
                std::printf(" %6.1f   |", s.switchMs);
            } else {
                std::printf(" %8s |", "-");
            }
            for(const double ms : s.zoomMs) {
                std::printf(" %6.1f ", ms);
            }
            std::printf(" |");
            for(const double ms : s.dragMs) {
                std::printf(" %6.1f ", ms);
            }
            std::printf(" |");
            for(const double ms : s.genMs) {
                std::printf(" %6.1f ", ms);
            }
            std::printf("\n");
            std::fflush(stdout);
        }
    }

    // 交互降级(ItemCoordinateCache)与常态(DeviceCoordinateCache)的画面差异:
    // 1x 下逻辑缓存尺寸 = 画布,应基本一致;放大后是预期的位图放大(用户已同意交互期降质)。
    std::printf("\n交互降级画面差异(600 形状,分块实现,基准=常态设备坐标缓存):\n");
    for(const double factor : {1.0, 4.0}) {
        QImage normal;
        QImage degraded;
        for(int pass = 0; pass < 2; pass++) {
            MAX_CACHE_SIZE = 4096;
            CLIP_TO_SHAPE = true;
            ChunkedScene c;
            rngState = 12345U;
            for(int i = 0; i + BATCH < 600; i += BATCH) {
                addBatchChunked(c, makeBatch(BATCH));
            }
            const QPointF center{c.scene.itemsBoundingRect().center()};
            if(pass == 1) {
                for(QGraphicsItem* item : c.scene.items()) {
                    item->setCacheMode(QGraphicsItem::ItemCoordinateCache, QSize(IMAGE_W, IMAGE_H));
                }
            }
            c.view.resetTransform();
            c.view.scale(factor, factor);
            c.view.centerOn(center);
            renderView(c.view);
            (pass == 0 ? normal : degraded) = grab(c.view);
        }
        compare(normal, degraded, (std::to_string(static_cast<int>(factor)) + "x 降级").c_str());
    }

    IMAGE_W = 512;
    IMAGE_H = 384;
    QPixmapCache::setCacheLimit(cacheMb * 1024);

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

    // ItemClipsToShape 是否影响画面:SVG 内容本就落在 viewBox 内,理论上裁剪是多余的
    {
        CLIP_TO_SHAPE = false;
        ChunkedScene c;
        run(c, addBatchChunked, 600, &imageC);
        CLIP_TO_SHAPE = true;
        compare(imageA, imageC, "分块(不裁剪)");
    }

    return 0;
}
