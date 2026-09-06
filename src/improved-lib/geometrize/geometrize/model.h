#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "core.h"
#include "shaperesult.h"

namespace geometrize
{
class Bitmap;
class Scanline;
class Shape;
}

namespace geometrize
{

/**
 * @brief ShapeAcceptancePreconditionFunction Type alias for a function that is used to decide whether or not to finally add a shape to the image
 * @param lastScore The image similarity score prior to adding the shape
 * @param newScore What the image similarity score would be after adding the shape
 * @param shape The shape that this function shall decide whether to add
 * @param lines The scanlines for the pixels in the shape
 * @param color The colour of the shape
 * @param before The image prior to adding the shape
 * @param after The image as it would be after adding the shape
 * @param target The image that we are trying to replicate
 * @return True to add the shape to the image, false not to
 */
using ShapeAcceptancePreconditionFunction = std::function<bool(
    double lastScore,
    double newScore,
    const geometrize::Shape& shape,
    const std::vector<geometrize::Scanline>& lines,
    const geometrize::rgba& color,
    const geometrize::Bitmap& before,
    const geometrize::Bitmap& after,
    const geometrize::Bitmap& target)>;

/**
 * @brief The Model class is the model for the core optimization/fitting algorithm.
 * @author Sam Twidale (https://samcodes.co.uk/)
 */
class Model
{
public:
    /**
     * @brief Model Creates a model that will aim to replicate the target bitmap with shapes.
     * @param target The target bitmap to replicate with shapes.
     */
    Model(const geometrize::Bitmap& target);

    /**
     * @brief Model Creates a model that will optimize for the given target bitmap, starting from the given initial bitmap.
     * The target bitmap and initial bitmap must be the same size (width and height).
     * @param target The target bitmap to replicate with shapes.
     * @param initial The starting bitmap.
     */
    Model(const geometrize::Bitmap& target, const geometrize::Bitmap& initial);
    ~Model();
    Model& operator=(const Model&) = delete;
    Model(const Model&) = delete;

    /**
     * @brief reset Resets the model back to the state it was in when it was created.
     * @param backgroundColor The starting background color to use.
     */
    void reset(geometrize::rgba backgroundColor);

    /**
     * @brief getWidth Gets the width of the target bitmap.
     * @return The width of the target bitmap.
     */
    std::int32_t getWidth() const;

    /**
     * @brief getHeight Gets the height of the target bitmap.
     * @return The height of the target bitmap.
     */
    std::int32_t getHeight() const;

    /**
     * @brief step Steps the primitive optimization/fitting algorithm.
     * @param shapeCreator A function that will produce the shapes.
     * @param alpha The alpha of the shape.
     * @param shapeCount The number of random shapes to generate (only 1 is chosen in the end).
     * @param maxShapeMutations The maximum number of times to mutate each random shape.
     * @param maxThreads The maximum number of threads to use during this step.
     * @param energyFunction An optional function to calculate the energy (if unspecified a default implementation is used).
     * @param addShapePrecondition An optional function to determine whether to accept a shape (if unspecified a default implementation is used).
     * @param pyramidSearch 金字塔搜索开关:hill-climb 评估在半分辨率图上进行(算法增强轨道,默认关)。
     * 自定义 energyFunction 存在时被忽略(自定义回调语义按全分辨率位图约定)。
     * @param enhancements 增强爬山开关集(自适应步长/alpha 搜索/误差图引导;默认全关,经典路径零参与)。
     * 误差图引导开启时 Model 自动维护权重表(提交前重建、任务期只读),自定义 energyFunction 与其并存时同样被忽略(增强轨道既有语义)。
     * @param priorityRegions 优先区域列表(全分辨率像素坐标,闭区间),区域内误差块加权(F3.2 区域优先)。
     * 仅 errorGuide 开启时生效;区域变化即使无形状被接受也会触发误差图重建;空列表与不传逐位一致。
     * @return A vector containing data about the shapes added to the model in this step. This may be empty if no shape that improved the image could be found.
     */
    std::vector<geometrize::ShapeResult> step(
            const std::function<std::shared_ptr<geometrize::Shape>(void)>& shapeCreator,
            std::uint8_t alpha,
            std::uint32_t shapeCount,
            std::uint32_t maxShapeMutations,
            std::uint32_t maxThreads,
            const geometrize::core::EnergyFunction& energyFunction = nullptr,
            const geometrize::ShapeAcceptancePreconditionFunction& addShapePrecondition = nullptr,
            bool pyramidSearch = false,
            const geometrize::core::HillClimbEnhancements& enhancements = {},
            const std::vector<geometrize::core::RegionRect>& priorityRegions = {});

    /**
     * @brief drawShape Draws a shape on the model. Typically used when to manually add a shape to the image (e.g. when setting an initial background).
     * NOTE this unconditionally draws the shape, even if it increases the difference between the source and target image.
     * @param shape The shape to draw.
     * @param color The color (including alpha) of the shape.
     * @return Data about the shape drawn on the model.
     */
    geometrize::ShapeResult drawShape(std::shared_ptr<geometrize::Shape> shape, geometrize::rgba color);

    /**
     * @brief drawShape A2.4 带分段颜色的绘制重载(撤销重放/重做复用 ShapeResult 里存储的 segments)。
     * segments 非空时按行级颜色重画;与原 step 轨迹逐字节一致(同形状 + 同存储颜色 → 同位图、同分数链)。
     * segments 与重光栅化结果行数不一致时(不应发生)防御性退回单色。
     * @param shape The shape to draw.
     * @param color The average color (including alpha) of the shape.
     * @param segments The per-scanline colors recorded when the shape was originally drawn.
     * @return Data about the shape drawn on the model.
     */
    geometrize::ShapeResult drawShape(std::shared_ptr<geometrize::Shape> shape, geometrize::rgba color, const std::vector<geometrize::ScanlineColor>& segments);

    /**
     * @brief getCurrent Gets the current bitmap.
     * @return The current bitmap.
     */
    geometrize::Bitmap& getCurrent();

    /**
     * @brief getTarget Gets the target bitmap.
     * @return The target bitmap.
     */
    geometrize::Bitmap& getTarget();

    /**
     * @brief getCurrent Gets the current bitmap, const-edition.
     * @return The current bitmap.
     */
    const geometrize::Bitmap& getCurrent() const;

    /**
     * @brief getTarget Gets the target bitmap, const-edition.
     * @return The target bitmap.
     */
    const geometrize::Bitmap& getTarget() const;

    /**
     * @brief setSeed Sets the seed that the random number generators of this model use. Note that the model also uses an internal seed offset which is incremented when the model is stepped.
     * @param seed The random number generator seed.
     */
    void setSeed(std::uint32_t seed);

private:
    class ModelImpl;
    std::unique_ptr<Model::ModelImpl> d;
};

}
