#pragma once

#include <memory>

#include <QGraphicsScene>

#include "imagetaskpixmapgraphicsitem.h"
#include "svgitem.h"

class QEvent;
class QFocusEvent;
class QKeyEvent;
class QGraphicsSceneWheelEvent;

namespace geometrize
{

namespace scene
{

/**
 * @brief The ImageTaskScene class models a graphics scene for viewing the results of image tasks.
 */
class ImageTaskScene : public QGraphicsScene
{
    Q_OBJECT

public:
    explicit ImageTaskScene(QObject* parent = nullptr);
    virtual ~ImageTaskScene();

    /**
     * @brief setTargetPixmapOpacity Sets the opacity of the target pixmap.
     * @param opacity The opacity value.
     */
    void setTargetPixmapOpacity(float opacity);

    /**
     * @brief setTargetPixmap Sets the pixmap that provides the target/goal image visualization.
     * @param pixmap The pixmap to set as the target/goal image visualization.
     */
    void setTargetPixmap(const QPixmap& pixmap);

    /**
      * @brief getTargetPixmapItem Gets a refernece to the graphics item that houses the target pixmap.
      * @return The target pixmap item.
      */
     ImageTaskPixmapGraphicsItem& getTargetPixmapItem();

     /**
      * @brief setOverlayItem Sets the given overlay shape upon the scene.
      * @param item The item to add.
      */
     void setOverlayShape(SvgItem* item);

     /**
      * @brief setOverlayShapeVisibility Whether the overlay shape(s) in the scene should be visible.
      * @param visible True if visible, false for invisible
      */
     void setOverlayShapeVisibility(bool visible);

     /**
      * @brief setInteractiveRendering 通知场景"用户正在连续缩放/拖动",渲染缓存可以临时降级换流畅度。
      * 默认空实现;矢量场景据此在交互期改用 item 坐标缓存(视口变换不再使缓存失效),
      * 停手后由调用方切回清晰渲染。
      * @param interactive True 表示进入交互期,false 表示交互结束、恢复清晰渲染。
      */
     virtual void setInteractiveRendering(bool interactive);

protected:
     bool event(QEvent* event) override;
     void mousePressEvent(QGraphicsSceneMouseEvent* event) override;
     void mouseMoveEvent(QGraphicsSceneMouseEvent* event) override;
     void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override;
     void wheelEvent(QGraphicsSceneWheelEvent* event) override;
     void keyPressEvent(QKeyEvent* event) override;
     void keyReleaseEvent(QKeyEvent* event) override;
     void focusInEvent(QFocusEvent* event) override;
     void focusOutEvent(QFocusEvent* event) override;

private:
    class ImageTaskSceneImpl;
    std::unique_ptr<ImageTaskSceneImpl> d;
};

}

}
