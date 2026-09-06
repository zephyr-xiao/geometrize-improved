#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <QObject>

#include "geometrize/core.h"
#include "geometrize/bitmap/bitmap.h"
#include "geometrize/bitmap/rgba.h"
#include "geometrize/model.h"
#include "geometrize/shaperesult.h"
#include "geometrize/runner/imagerunneroptions.h"
#include "geometrize/shape/shape.h"

#include "preferences/imagetaskpreferences.h"

namespace geometrize
{

namespace script
{
class GeometrizerEngine;
}

}

namespace chaiscript
{
class ChaiScript;
}

Q_DECLARE_METATYPE(std::vector<geometrize::ShapeResult>) ///< Shape data passed around by the image task worker thread.
Q_DECLARE_METATYPE(std::vector<geometrize::ScanlineColor>) ///< A2.4 per-scanline colors passed around by the image task worker thread.
Q_DECLARE_METATYPE(geometrize::ImageRunnerOptions) ///< Image runner options passed to the image task worker thread.
Q_DECLARE_METATYPE(std::function<std::shared_ptr<geometrize::Shape>()>) ///< Function that generates shapes passed to the image task worker thread.
Q_DECLARE_METATYPE(std::shared_ptr<geometrize::Shape>) ///< Shape passed to the image task worker thread.
Q_DECLARE_METATYPE(geometrize::rgba) ///< Shape color passed to the image task worker thread.
Q_DECLARE_METATYPE(geometrize::core::EnergyFunction); ///< Energy function passed to the image task worker thread.
Q_DECLARE_METATYPE(geometrize::ShapeAcceptancePreconditionFunction); ///< Shape-add precondition function passed to the image task worker thread.

namespace geometrize
{

namespace task
{

/**
 * @brief The ImageTask class transforms a source image into a collection of shapes approximating the source image.
 */
class ImageTask : public QObject
{
    Q_OBJECT
public:
    ImageTask(Bitmap& target, Qt::ConnectionType workerConnectionType = Qt::QueuedConnection);
    ImageTask(Bitmap& target, Bitmap& background, Qt::ConnectionType workerConnectionType = Qt::QueuedConnection);
    ImageTask(const std::string& displayName, Bitmap& bitmap, Qt::ConnectionType workerConnectionType = Qt::QueuedConnection);
    ImageTask(const std::string& displayName, Bitmap& bitmap, const Bitmap& initial, Qt::ConnectionType workerConnectionType = Qt::QueuedConnection);

    ImageTask& operator=(const ImageTask&) = delete;
    ImageTask(const ImageTask&) = delete;
    virtual ~ImageTask();

    /**
     * @brief getGeometrizer Gets a reference to the script-based engine used to turn images into shapes.
     * @return The script-based engine used to turn images into shapes.
     */
    geometrize::script::GeometrizerEngine& getGeometrizer();

    /**
     * @brief getTarget Gets the target bitmap, non-const edition.
     * @return The target bitmap.
     */
    Bitmap& getTargetMutable();

    /**
     * @brief getCurrent Gets the current bitmap, non-const edition.
     * @return The current bitmap.
     */
    Bitmap& getCurrentMutable();

    /**
     * @brief getTarget Gets the target bitmap, const-edition.
     * @return The target bitmap.
     */
    const Bitmap& getTarget() const;

    /**
     * @brief getCurrent Gets the current bitmap, const-edition.
     * @return The current bitmap.
     */
    const Bitmap& getCurrent() const;

    /**
     * @brief getWidth Gets the width of the images used by the image task.
     * @return The width of the images used by the image task.
     */
    std::uint32_t getWidth() const;

    /**
     * @brief getHeight Gets the height of the images used by the image task.
     * @return The height of the images used by the image task.
     */
    std::uint32_t getHeight() const;

    /**
     * @brief getDisplayName Gets the display name of the image task.
     * @return The display name of the image task.
     */
    std::string getDisplayName() const;

    /**
     * @brief getTaskId Gets the unique id of the image task.
     * @return The unique id of the image task.
     */
     std::size_t getTaskId() const;

     /**
      * @brief isStepping Returns true if the internal model is currently stepping.
      * @return True if the internal model is currently stepping, else false.
      */
     bool isStepping() const;

     /**
      * @brief stepModel Steps the internal model, typically adding a shape.
      */
     void stepModel();

     /**
      * @brief drawShape Draws a shape with the given color to the internal model.
      * @param shape The shape to add to the model.
      * @param color The color of the shape.
      */
     void drawShape(const std::shared_ptr<geometrize::Shape> shape, geometrize::rgba color);

     /**
      * @brief drawShape A2.4 带分段颜色的绘制重载(重做/重放复用 ShapeResult 里存储的 segments,
      * 不重算——分段色依赖落画时刻的位图)。空 segments 等价于单色重载。
      * @param shape The shape to add to the model.
      * @param color The average color of the shape.
      * @param segments The per-scanline colors recorded when the shape was originally drawn.
      */
     void drawShape(const std::shared_ptr<geometrize::Shape> shape, geometrize::rgba color, std::vector<geometrize::ScanlineColor> segments);

     /**
      * @brief drawBackgroundRectangle Convenience function that draws a background rectangle shape.
      * @param color The color of the background rectangle.
      */
     void drawBackgroundRectangle(geometrize::rgba color);

    /**
     * @brief replayShapes Rebuilds the task model by resetting it to the given background color and
     * redrawing the given shapes in order (undo support). A single willStep bracket covers the batch;
     * on completion signal_modelDidReplay is emitted (not signal_modelDidStep, so replaying does not
     * re-trigger shape-added hooks or chained stepping).
     * @param shapes The shapes to redraw, in original order.
     * @param resetColor The background color to reset the model to first.
     */
    void replayShapes(const std::vector<geometrize::ShapeResult>& shapes, geometrize::rgba resetColor);

     /**
      * @brief getPreferences Gets a reference to the current preferences of this task.
      * @return A reference to the current preferences of this task.
      */
     geometrize::preferences::ImageTaskPreferences& getPreferences();

     /**
      * @brief setPreferences Sets the preferences for this task.
      * @param preferences The preferences to set.
      */
     void setPreferences(preferences::ImageTaskPreferences preferences);

signals:
     /**
      * @brief signal_step Signal that the image task emits to make the internal model step.
      */
     void signal_step(geometrize::ImageRunnerOptions options, std::function<std::shared_ptr<geometrize::Shape>()> shapeCreator, geometrize::core::EnergyFunction energyFunction, geometrize::ShapeAcceptancePreconditionFunction addShapePrecondition);

     /**
      * @brief signal_drawShape Signal that the image task emits to draw a shape to the internal model.
      * @param shape The shape to draw.
      * @param color The color of the shape to draw.
      */
     void signal_drawShape(std::shared_ptr<geometrize::Shape> shape, geometrize::rgba color, std::vector<geometrize::ScanlineColor> segments);

     /**
      * @brief signal_modelWillStep Signal that is emitted immediately before the underlying image task model is stepped.
      */
     void signal_modelWillStep();

     /**
      * @brief signal_modelDidStep Signal that is emitted immediately after the underlying image task model is stepped.
      * @param shapes The shapes that were added in the last step.
      */
     void signal_modelDidStep(std::vector<geometrize::ShapeResult> shapes);

     /**
      * @brief signal_replayShapes Internal signal that makes the worker rebuild the model state (undo support).
      */
     void signal_replayShapes(std::vector<geometrize::ShapeResult> shapes, geometrize::rgba resetColor);

     /**
      * @brief signal_modelDidReplay Signal that is emitted when a replayShapes batch has finished applying.
      * @param shapes The shapes that are now present in the model.
      */
     void signal_modelDidReplay(std::vector<geometrize::ShapeResult> shapes);

     /**
      * @brief signal_preferencesSet Signal that is emitted immediately after the image task preferences are set.
      */
     void signal_preferencesSet();

private:
    void modelWillStep();
    void modelDidStep(std::vector<geometrize::ShapeResult> shapes);
    void modelDidReplay(std::vector<geometrize::ShapeResult> shapes);

    class ImageTaskImpl;
    std::unique_ptr<ImageTaskImpl> d;
};

}

}
