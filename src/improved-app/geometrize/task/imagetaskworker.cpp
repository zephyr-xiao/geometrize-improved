#include "imagetaskworker.h"

#include "geometrize/core.h"
#include "geometrize/bitmap/bitmap.h"
#include "geometrize/bitmap/rgba.h"
#include "geometrize/model.h"
#include "geometrize/runner/imagerunner.h"
#include "geometrize/shaperesult.h"

namespace geometrize
{

namespace task
{

ImageTaskWorker::ImageTaskWorker(Bitmap& bitmap) : QObject(), m_runner{bitmap}, m_working{false}
{
}

ImageTaskWorker::ImageTaskWorker(Bitmap& bitmap, const Bitmap& initial) : QObject(), m_runner{bitmap, initial}, m_working{false}
{
}

ImageTaskWorker::~ImageTaskWorker()
{
}

void ImageTaskWorker::step(const geometrize::ImageRunnerOptions options,
                           const std::function<std::shared_ptr<geometrize::Shape>()> shapeCreator,
                           const geometrize::core::EnergyFunction energyFunction,
                           const geometrize::ShapeAcceptancePreconditionFunction addShapePreconditionFunction)
{
    emit signal_willStep();
    m_working = true;
    const std::vector<geometrize::ShapeResult> results{m_runner.step(options, shapeCreator, energyFunction, addShapePreconditionFunction)};
    m_working = false;
    emit signal_didStep(results);
}

void ImageTaskWorker::drawShape(const std::shared_ptr<geometrize::Shape> shape, const geometrize::rgba color, const std::vector<geometrize::ScanlineColor> segments)
{
    emit signal_willStep();
    m_working = true;
    // A2.4:segments 非空时按行级颜色重画(重做/重放复用存档,不重算)
    const geometrize::ShapeResult result{m_runner.getModel().drawShape(shape, color, segments)};
    m_working = false;
    emit signal_didStep({ result });
}

void ImageTaskWorker::replayShapes(std::vector<geometrize::ShapeResult> shapes, const geometrize::rgba resetColor)
{
    emit signal_willStep();
    m_working = true;
    geometrize::Model& model{m_runner.getModel()};
    model.reset(resetColor);
    // drawShape 无条件绘制且不消费 RNG:重放后的位图/分数链与原轨迹逐位一致
    for(const geometrize::ShapeResult& result : shapes) {
        model.drawShape(result.shape, result.color, result.segments);
    }
    m_working = false;
    emit signal_didReplay(shapes);
}

geometrize::Bitmap& ImageTaskWorker::getCurrent()
{
    return m_runner.getCurrent();
}

geometrize::Bitmap& ImageTaskWorker::getTarget()
{
    return m_runner.getTarget();
}

const geometrize::Bitmap& ImageTaskWorker::getCurrent() const
{
    return m_runner.getCurrent();
}

const geometrize::Bitmap& ImageTaskWorker::getTarget() const
{
    return m_runner.getTarget();
}

geometrize::ImageRunner& ImageTaskWorker::getRunner()
{
    return m_runner;
}

bool ImageTaskWorker::isStepping() const
{
    return m_working;
}

}

}
