#include "imagetaskgraphicsview.h"

#include <QApplication>
#include <QDebug>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QString>
#include <QTabletEvent>
#include <QTimer>
#include <QTouchEvent>
#include <QWheelEvent>

#include "customtabletevent.h"
#include "scene/imagetaskscene.h"
#include "scene/imagetasksvgscene.h"

namespace
{

geometrize::scene::TabletEventData makeCustomTabletEventData(const QTabletEvent& event, const QGraphicsView& view)
{
    const geometrize::scene::TabletEventType eventType = [&event]() {
        switch(event.type()) {
        case QEvent::Type::TabletMove:
            return geometrize::scene::TabletEventType::Move;
        case QEvent::Type::TabletPress:
            return geometrize::scene::TabletEventType::Press;
        case QEvent::Type::TabletRelease:
            return geometrize::scene::TabletEventType::Release;
        case QEvent::Type::TabletEnterProximity:
            return geometrize::scene::TabletEventType::EnterProximity;
        case QEvent::Type::TabletLeaveProximity:
            return geometrize::scene::TabletEventType::LeaveProximity;
        default:
            break;
        }
        return geometrize::scene::TabletEventType::Unknown;
    }();

    #if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
    const geometrize::scene::TabletEventPointerType pointerType = [&event]() {
        switch(event.pointerType()) {
        case QPointingDevice::PointerType::Pen:
            return geometrize::scene::TabletEventPointerType::Pen;
        case QPointingDevice::PointerType::Cursor:
            return geometrize::scene::TabletEventPointerType::Cursor;
        case QPointingDevice::PointerType::Eraser:
            return geometrize::scene::TabletEventPointerType::Eraser;
        default:
            break;
        }
        return geometrize::scene::TabletEventPointerType::UnknownPointer;
    }();
    #else
    const geometrize::scene::TabletEventPointerType pointerType = [&event]() {
        switch(event.pointerType()) {
        case QTabletEvent::PointerType::Pen:
            return geometrize::scene::TabletEventPointerType::Pen;
        case QTabletEvent::PointerType::Cursor:
            return geometrize::scene::TabletEventPointerType::Cursor;
        case QTabletEvent::PointerType::Eraser:
            return geometrize::scene::TabletEventPointerType::Eraser;
        default:
            break;
        }
        return geometrize::scene::TabletEventPointerType::UnknownPointer;
    }();
    #endif

    geometrize::scene::TabletEventData data;
    data.eventType = eventType;
    data.pointerType = pointerType;
    #if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
    data.xViewPos = static_cast<float>(event.position().x());
    data.yViewPos = static_cast<float>(event.position().y());
    data.xScenePos = static_cast<float>(view.mapToScene(event.position().toPoint()).x());
    data.yScenePos = static_cast<float>(view.mapToScene(event.position().toPoint()).y());
    #else
    data.xViewPos = static_cast<float>(event.pos().x());
    data.yViewPos = static_cast<float>(event.pos().y());
    data.xScenePos = static_cast<float>(view.mapToScene(event.pos()).x());
    data.yScenePos = static_cast<float>(view.mapToScene(event.pos()).y());
    #endif
    data.pressure = static_cast<float>(event.pressure());
    data.tangentialPressure = static_cast<float>(event.tangentialPressure());
    data.xTilt = static_cast<float>(event.xTilt());
    data.yTilt = static_cast<float>(event.yTilt());

    return data;
}

}

namespace geometrize
{

namespace scene
{

class ImageTaskGraphicsView::ImageTaskGraphicsViewImpl
{
public:
    ImageTaskGraphicsViewImpl(ImageTaskGraphicsView* pQ) : q{pQ}, m_totalScaleFactor(1.0f), m_interactionActive{false}
    {
    }
    ImageTaskGraphicsViewImpl operator=(const ImageTaskGraphicsViewImpl&) = delete;
    ImageTaskGraphicsViewImpl(const ImageTaskGraphicsViewImpl&) = delete;
    ~ImageTaskGraphicsViewImpl() = default;

    // 进入交互期:通知场景把渲染缓存降级(矢量场景 → item 坐标缓存),并重启空闲计时。
    // 连续缩放/拖动期间每个 tick 都调,重复调用只重启计时。
    void beginInteraction()
    {
        if(!m_interactionActive) {
            m_interactionActive = true;
            if(auto* svgScene = qobject_cast<geometrize::scene::ImageTaskSvgScene*>(q->scene())) {
                svgScene->setInteractiveRendering(true);
            }
        }
        if(m_idleTimer != nullptr) {
            m_idleTimer->start();
        }
    }

    // 空闲到期:恢复清晰渲染。用延迟恢复而不是鼠标释放即恢复,是为了让连续滚轮/连续拖动
    // 之间不来回切换缓存模式(每次切换都会触发一次整块重光栅化)。
    void endInteraction()
    {
        if(!m_interactionActive) {
            return;
        }
        m_interactionActive = false;
        if(auto* svgScene = qobject_cast<geometrize::scene::ImageTaskSvgScene*>(q->scene())) {
            svgScene->setInteractiveRendering(false);
        }
    }

    ImageTaskGraphicsView* q;

    float m_totalScaleFactor;

    bool m_interactionActive;
    QTimer* m_idleTimer{nullptr}; ///> 交互空闲计时(到期恢复清晰渲染)
};

// 停手多久算交互结束。取值要大于一次连续滚轮/拖动的间隔、又小于人眼觉得"画面一直糊"的时长。
constexpr int INTERACTION_IDLE_MS{200};

ImageTaskGraphicsView::ImageTaskGraphicsView(QWidget* parent) : QGraphicsView(parent), d{std::make_unique<ImageTaskGraphicsViewImpl>(this)}
{
    viewport()->setAttribute(Qt::WA_AcceptTouchEvents);
    setInteractive(true);
    setMouseTracking(true);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setDragMode(QGraphicsView::ScrollHandDrag);
    setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    // 交互空闲计时:连续缩放/拖动期间保持降级渲染,停手后恢复清晰(见 Impl::beginInteraction)
    d->m_idleTimer = new QTimer(this);
    d->m_idleTimer->setSingleShot(true);
    d->m_idleTimer->setInterval(INTERACTION_IDLE_MS);
    connect(d->m_idleTimer, &QTimer::timeout, this, [this]() {
        d->endInteraction();
    });

    populateUi();
}

ImageTaskGraphicsView::~ImageTaskGraphicsView()
{
}

bool ImageTaskGraphicsView::viewportEvent(QEvent* event)
{
    switch(event->type()) {
    case QEvent::TabletMove:
    case QEvent::TabletPress:
    case QEvent::TabletRelease:
    case QEvent::TabletEnterProximity:
    case QEvent::TabletLeaveProximity:
    {
        // Forward an event containing relevant tablet event data to the graphics scene
        const auto customEventData = makeCustomTabletEventData(static_cast<QTabletEvent&>(*event), *this);
        geometrize::scene::CustomTabletEvent customEvent(customEventData, this);
        QCoreApplication::sendEvent(dynamic_cast<geometrize::scene::ImageTaskScene*>(scene()), &customEvent);
        event->accept();
        return true;
    }
    // TODO implement pinch-to-zoom
    /*
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    {
        QTouchEvent *touchEvent = static_cast<QTouchEvent *>(event);
        QList<QTouchEvent::TouchPoint> touchPoints = touchEvent->touchPoints();
        if (touchPoints.count() == 2) {
            // determine scale factor
            const QTouchEvent::TouchPoint &touchPoint0 = touchPoints.first();
            const QTouchEvent::TouchPoint &touchPoint1 = touchPoints.last();
            qreal currentScaleFactor =
                    QLineF(touchPoint0.pos(), touchPoint1.pos()).length()
                    / QLineF(touchPoint0.startPos(), touchPoint1.startPos()).length();
            if (touchEvent->touchPointStates() & Qt::TouchPointReleased) {
                // if one of the fingers is released, remember the current scale
                // factor so that adding another finger later will continue zooming
                // by adding new scale factor to the existing remembered value.
                d->m_totalScaleFactor *= currentScaleFactor;
                currentScaleFactor = 1;
            }
            setTransform(QTransform().scale(d->m_totalScaleFactor * currentScaleFactor,
                                            d->m_totalScaleFactor * currentScaleFactor));
        }
        event->accept();
        return true;
    }
    */
    default:
        break;
    }
    return QGraphicsView::viewportEvent(event);
}

void ImageTaskGraphicsView::wheelEvent(QWheelEvent* e)
{
    if(e->modifiers() & Qt::ControlModifier) {
        // Pass down to scene/children in scene if control is pressed
        QGraphicsView::wheelEvent(e);
        return;
    }

    // Handle zooming
    if (e->angleDelta().x() == 0) {
        #if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
        const QPointF pos{e->position()};
        const QPointF posf{mapToScene(pos.toPoint())};
        const QPointF newPos{mapToScene(pos.toPoint())};
        #else
        const QPoint pos{e->pos()};
        const QPointF posf{mapToScene(pos)};
        const QPointF newPos{mapToScene(pos)};
        #endif

        const double angle{static_cast<double>(e->angleDelta().y())};
        double by{0.0};
        if(angle > 0) {
            by = 1 + (angle / 360 * 0.1);
        } else if (angle < 0) {
            by = 1 - (-angle / 360 * 0.1);
        } else {
            by = 1;
        }

        scale(by, by);

        const int w{viewport()->width()};
        const int h{viewport()->height()};
        const double wf{mapToScene(QPoint(w - 1, 0)).x() - mapToScene(QPoint(0,0)).x()};
        const double hf{mapToScene(QPoint(0, h - 1)).y() - mapToScene(QPoint(0,0)).y()};
        const double lf{posf.x() - pos.x() * wf / w};
        const double tf{posf.y() - pos.y() * hf / h};

        ensureVisible(lf, tf, wf, hf, 0, 0);
        ensureVisible(QRectF(QPointF(lf, tf) - newPos + posf, QSizeF(wf, hf)), 0, 0);

        // 缩放期间切降级渲染:否则每个滚轮 tick 都会让全部块设备坐标缓存失效、
        // 按新倍率重光栅化"整幅形状"(块数 = 形状数/256 ⇒ 每 tick O(形状数))
        d->beginInteraction();

        e->accept();
    }
}

void ImageTaskGraphicsView::mousePressEvent(QMouseEvent* event)
{
    // 拖动(ScrollHandDrag)/框选期间同样按交互处理:平移虽只走缓存 blit,
    // 但一旦有 item 被旁路缓存就会退化成每帧全量重光栅化
    d->beginInteraction();
    QGraphicsView::mousePressEvent(event);
}

void ImageTaskGraphicsView::mouseReleaseEvent(QMouseEvent* event)
{
    QGraphicsView::mouseReleaseEvent(event);
    if(d->m_idleTimer != nullptr) {
        d->m_idleTimer->start(); // 松手后仍在交互期,等空闲计时到期再恢复清晰
    }
}

void ImageTaskGraphicsView::mouseMoveEvent(QMouseEvent* event)
{
    QGraphicsView::mouseMoveEvent(event);
    // 只有"按着键拖"才算交互在继续。视图开着 setMouseTracking,不加这个判断的话
    // 单纯移动鼠标就会不断续期,降级渲染会一直挂着(画面长期发糊)。
    if(d->m_interactionActive && event->buttons() != Qt::NoButton && d->m_idleTimer != nullptr) {
        d->m_idleTimer->start();
    }
}

void ImageTaskGraphicsView::hideEvent(QHideEvent* event)
{
    // 视图被隐藏(切到另一个视图)时立刻结束交互期:否则降级缓存会留在场景里,
    // 下次显示出来的是位图放大的糊图,要等下一次交互才恢复清晰。
    if(d->m_idleTimer != nullptr) {
        d->m_idleTimer->stop();
    }
    d->endInteraction();
    QGraphicsView::hideEvent(event);
}

void ImageTaskGraphicsView::keyPressEvent(QKeyEvent* event)
{
    QGraphicsView::keyPressEvent(event);
}

void ImageTaskGraphicsView::keyReleaseEvent(QKeyEvent* event)
{
    QGraphicsView::keyReleaseEvent(event);
}

void ImageTaskGraphicsView::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::LanguageChange) {
        populateUi();
    }
    QGraphicsView::changeEvent(event);
}

void ImageTaskGraphicsView::populateUi()
{

}

}

}
