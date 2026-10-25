#pragma once

#include <memory>
#include <vector>

#include <QGraphicsScene>

#include "imagetaskscene.h"

namespace geometrize
{
struct ShapeResult;
}

namespace geometrize
{

namespace scene
{

/**
 * @brief The ImageTaskSvgScene class models a graphics scene for viewing the shape data of an image task as an SVG.
 */
class ImageTaskSvgScene : public ImageTaskScene
{
    Q_OBJECT

public:
    ImageTaskSvgScene();
    virtual ~ImageTaskSvgScene();

    /**
     * @brief addShapes Updates the SVG data based on the given shapes.
     * @param shapes The shapes to draw in the scene.
     * @param width The width of the SVG.
     * @param height The height of the SVG.
     */
    void addShapes(const std::vector<geometrize::ShapeResult>& shapes, const std::uint32_t width, const std::uint32_t height);

    /**
     * @brief removeShapes Removes all shapes from the scene.
     */
    void removeShapes();

    /**
     * @brief setInteractiveRendering 交互期把分块 item 从"设备坐标缓存"切到"item 坐标缓存":
     * 后者不随视口变换失效,缩放/拖动每个 tick 只做位图缩放合成,不再逐 tick 重光栅化整块 SVG;
     * 代价是放大时略糊,交互结束后调用方切回设备坐标缓存恢复清晰。
     */
    void setInteractiveRendering(bool interactive) override;

private:
    class ImageTaskSvgSceneImpl;
    std::unique_ptr<ImageTaskSvgSceneImpl> d;
};

}

}
