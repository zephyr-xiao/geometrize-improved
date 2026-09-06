#include "imagetaskscenemanager.h"

#include <memory>
#include <vector>

#include <QByteArray>
#include <QBrush>
#include <QGraphicsRectItem>
#include <QPen>
#include <QColor>
#include <QObject>
#include <QPixmap>

#include "geometrize/exporter/svgexporter.h"
#include "geometrize/shape/shape.h"
#include "geometrize/shaperesult.h"

#include "scene/areaofinfluenceshapeitem.h"
#include "scene/imagetaskgraphicsview.h"
#include "scene/imagetaskpixmapscene.h"
#include "scene/imagetasksvgscene.h"
#include "scene/imagetaskpixmapgraphicsitem.h"

namespace geometrize
{

namespace scene
{

class ImageTaskSceneManager::ImageTaskSceneManagerImpl
{
public:
    ImageTaskSceneManagerImpl(ImageTaskSceneManager* pQ) : q{pQ}
    {
        connectTargetItemSignals(m_pixmapScene.getTargetPixmapItem());
        connectTargetItemSignals(m_svgScene.getTargetPixmapItem());
    }
    ImageTaskSceneManagerImpl operator=(const ImageTaskSceneManagerImpl&) = delete;
    ImageTaskSceneManagerImpl(const ImageTaskSceneManagerImpl&) = delete;
    ~ImageTaskSceneManagerImpl() = default;

    void setViews(ImageTaskGraphicsView& pixmapView, ImageTaskGraphicsView& svgView)
	{
        pixmapView.setScene(&m_pixmapScene);
        svgView.setScene(&m_svgScene);
	}
	
    void setTargetPixmap(const QPixmap& pixmap)
    {
        m_pixmapScene.setTargetPixmap(pixmap);
        m_svgScene.setTargetPixmap(pixmap);
    }

    void setTargetPixmapOpacity(const float opacity)
    {
		m_pixmapScene.setTargetPixmapOpacity(opacity);
		m_svgScene.setTargetPixmapOpacity(opacity);
    }

    void updateScenes(const QPixmap& pixmap, const std::vector<geometrize::ShapeResult>& shapes)
    {
		m_pixmapScene.setWorkingPixmap(pixmap);
        m_svgScene.addShapes(shapes, static_cast<std::uint32_t>(pixmap.size().width()), static_cast<std::uint32_t>(pixmap.size().height()));
    }
	
	void reset()
	{
		m_svgScene.removeShapes();
	}

	// ---- F3.2 区域优先 overlay ----
	// NoButton + 不接受 hover:区域 item Z 序高于图像 item,若接收鼠标会截走 Ctrl+拖拽事件
	QGraphicsRectItem* makeRegionItem(QGraphicsScene& scene, const QRectF& rect, bool preview)
	{
		auto* item = new QGraphicsRectItem(rect);
		const QColor fill{preview ? QColor(255, 200, 0, 40) : QColor(255, 200, 0, 60)};
		const QColor border{preview ? QColor(255, 200, 0, 120) : QColor(255, 160, 0)};
		item->setBrush(QBrush(fill));
		item->setPen(QPen(border, 0.0));
		item->setZValue(1001.0);
		item->setAcceptedMouseButtons(Qt::NoButton);
		item->setAcceptHoverEvents(false);
		scene.addItem(item);
		return item;
	}

	void clearRegionItems(std::vector<QGraphicsRectItem*>& items)
	{
		for(QGraphicsRectItem* item : items) {
			if(item != nullptr && item->scene() != nullptr) {
				item->scene()->removeItem(item);
			}
			delete item;
		}
		items.clear();
	}

	void setPriorityRegions(const std::vector<QRectF>& regions)
	{
		clearRegionItems(m_priorityRegionItems);
		for(const QRectF& rect : regions) {
			m_priorityRegionItems.push_back(makeRegionItem(m_pixmapScene, rect, false));
			m_priorityRegionItems.push_back(makeRegionItem(m_svgScene, rect, false));
		}
	}

	void setPriorityRegionPreviewRect(const QRectF& rect)
	{
		if(!rect.isValid() || rect.isNull()) {
			clearRegionItems(m_previewRegionItem); // 单元素容器,等价隐藏
			return;
		}
		if(m_previewRegionItem.empty() || m_previewRegionItem.front() == nullptr) {
			clearRegionItems(m_previewRegionItem);
			m_previewRegionItem.push_back(makeRegionItem(m_pixmapScene, rect, true));
			m_previewRegionItem.push_back(makeRegionItem(m_svgScene, rect, true));
			return;
		}
		for(QGraphicsRectItem* item : m_previewRegionItem) {
			item->setRect(rect);
		}
	}
	
    void fitPixmapSceneInView(ImageTaskGraphicsView& pixmapView)
    {
        const double margin{m_defaultViewMargins};
        const QRectF imageViewRect{m_pixmapScene.itemsBoundingRect().adjusted(-margin, -margin, margin, margin)};
        pixmapView.fitInView(imageViewRect, Qt::KeepAspectRatio);
    }

    void fitVectorSceneInView(ImageTaskGraphicsView& svgView)
    {
        const double margin{m_defaultViewMargins};
        const QRectF svgRect{m_svgScene.itemsBoundingRect().adjusted(-margin, -margin, margin, margin)};
        svgView.fitInView(svgRect, Qt::KeepAspectRatio);
    }
	
    void fitScenesInViews(ImageTaskGraphicsView& pixmapView, ImageTaskGraphicsView& svgView)
    {
        fitPixmapSceneInView(pixmapView);
        fitVectorSceneInView(svgView);
    }

    void setAreaOfInfluenceShape(const geometrize::Shape& shape)
    {
        const std::uint32_t width = static_cast<std::uint32_t>(m_pixmapScene.getTargetPixmapItem().pixmap().width());
        const std::uint32_t height = static_cast<std::uint32_t>(m_pixmapScene.getTargetPixmapItem().pixmap().height());

        const QString svgString{QString::fromStdString(
            geometrize::exporter::exportSingleShapeSVG(geometrize::rgba{32, 32, 128, 128}, shape, width, height)
        )};
        const QByteArray svgData{svgString.toUtf8()};

        const auto makeItem = [](const QByteArray& data) -> AreaOfInfluenceShapeItem* {
            return new AreaOfInfluenceShapeItem(data);
        };

        auto pixmapInfluenceItem = makeItem(svgData);
        auto svgInfluenceItem = makeItem(svgData);

        m_pixmapScene.setOverlayShape(pixmapInfluenceItem);
        m_svgScene.setOverlayShape(svgInfluenceItem);
    }

private:
    void connectTargetItemSignals(geometrize::scene::ImageTaskPixmapGraphicsItem& item)
    {
        connect(&item, &geometrize::scene::ImageTaskPixmapGraphicsItem::signal_onCustomTabletEvent, [this](const geometrize::scene::CustomTabletEvent& event) {
            q->signal_onTargetImageTabletEvent(event);
        });
        connect(&item, &geometrize::scene::ImageTaskPixmapGraphicsItem::signal_onHoverMoveEvent, [this](const double lastX, const double lastY, const double x, const double y, const bool ctrlModifier) {
            q->signal_onTargetImageHoverMoveEvent(lastX, lastY, x, y, ctrlModifier);
        });
        connect(&item, &geometrize::scene::ImageTaskPixmapGraphicsItem::signal_onMouseMoveEvent, [this](const double lastX, const double lastY, const double x, const double y, const bool ctrlModifier) {
            q->signal_onTargetImageMouseMoveEvent(lastX, lastY, x, y, ctrlModifier);
        });
        connect(&item, &geometrize::scene::ImageTaskPixmapGraphicsItem::signal_onMousePressEvent, [this](const double x, const double y, const bool ctrlModifier) {
            q->signal_onTargetImageMousePressEvent(x, y, ctrlModifier);
        });
        connect(&item, &geometrize::scene::ImageTaskPixmapGraphicsItem::signal_onMouseReleaseEvent, [this](const double x, const double y, const bool ctrlModifier) {
            q->signal_onTargetImageMouseReleaseEvent(x, y, ctrlModifier);
        });
        connect(&item, &geometrize::scene::ImageTaskPixmapGraphicsItem::signal_onWheelEvent, [this](const double x, const double y, const int amount, const bool ctrlModifier) {
            q->signal_onTargetImageWheelEvent(x, y, amount, ctrlModifier);
        });
        connect(&item, &geometrize::scene::ImageTaskPixmapGraphicsItem::signal_onKeyPressEvent, [this](const int key, const bool ctrlModifier) {
            q->signal_onTargetImageKeyPressEvent(key, ctrlModifier);
        });
        connect(&item, &geometrize::scene::ImageTaskPixmapGraphicsItem::signal_onKeyReleaseEvent, [this](const int key, const bool ctrlModifier) {
            q->signal_onTargetImageKeyReleaseEvent(key, ctrlModifier);
        });
    }

    geometrize::scene::ImageTaskPixmapScene m_pixmapScene; ///> The scene containing the raster/pixel-based representation of the shapes
    std::vector<QGraphicsRectItem*> m_priorityRegionItems; ///> 区域优先(F3.2):已提交区域的 overlay 项(pixmap/svg 交替)
    std::vector<QGraphicsRectItem*> m_previewRegionItem; ///> 区域优先(F3.2):拖拽中的预览矩形项(最多 2 项)
    geometrize::scene::ImageTaskSvgScene m_svgScene; ///> The scene containing the vector-based representation of the shapes

    ImageTaskSceneManager* q;

    const double m_defaultViewMargins{20.0}; ///> Margins around the graphics shown in the views
};

ImageTaskSceneManager::ImageTaskSceneManager() : d{std::make_unique<ImageTaskSceneManager::ImageTaskSceneManagerImpl>(this)}
{
}

ImageTaskSceneManager::~ImageTaskSceneManager()
{
}

void ImageTaskSceneManager::setViews(ImageTaskGraphicsView& pixmapView, ImageTaskGraphicsView& svgView)
{
	d->setViews(pixmapView, svgView);
}

void ImageTaskSceneManager::setTargetPixmap(const QPixmap& pixmap)
{
    d->setTargetPixmap(pixmap);
}

void ImageTaskSceneManager::setTargetPixmapOpacity(const float opacity)
{
    d->setTargetPixmapOpacity(opacity);
}

void ImageTaskSceneManager::updateScenes(const QPixmap& pixmap, const std::vector<geometrize::ShapeResult>& shapes)
{
    d->updateScenes(pixmap, shapes);
}

void ImageTaskSceneManager::reset()
{
	d->reset();
}

void ImageTaskSceneManager::setPriorityRegions(const std::vector<QRectF>& regions)
{
	d->setPriorityRegions(regions);
}

void ImageTaskSceneManager::setPriorityRegionPreviewRect(const QRectF& rect)
{
	d->setPriorityRegionPreviewRect(rect);
}

void ImageTaskSceneManager::fitPixmapSceneInView(ImageTaskGraphicsView& pixmapView)
{
	d->fitPixmapSceneInView(pixmapView);
}

void ImageTaskSceneManager::fitVectorSceneInView(ImageTaskGraphicsView& svgView)
{
	d->fitVectorSceneInView(svgView);
}

void ImageTaskSceneManager::fitScenesInViews(ImageTaskGraphicsView& pixmapView, ImageTaskGraphicsView& svgView)
{
	d->fitScenesInViews(pixmapView, svgView);
}

void ImageTaskSceneManager::setAreaOfInfluenceShape(const geometrize::Shape& shape)
{
    d->setAreaOfInfluenceShape(shape);
}

}

}
