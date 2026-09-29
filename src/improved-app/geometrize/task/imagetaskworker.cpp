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

namespace
{

/// 作用域守卫:进入即置位、任何退出路径(含异常)都复位。
/// 手动 set/reset 在 runner.step 抛异常时会留下 m_working==true,使 isStepping() 永久为真 ——
/// 按它门控的交互(区域框选 Ctrl+拖拽、撤销/重做按钮、任务延迟销毁)会一起失效。
/// 另外它把复位推迟到 didStep 投递之后:步进处理期间 isStepping() 为真,撤销/重做按钮不再有可点窗口。
struct WorkingGuard
{
    explicit WorkingGuard(std::atomic<bool>& flag) : m_flag{flag}
    {
        m_flag = true;
    }

    ~WorkingGuard()
    {
        m_flag = false;
    }

    WorkingGuard(const WorkingGuard&) = delete;
    WorkingGuard& operator=(const WorkingGuard&) = delete;

    std::atomic<bool>& m_flag;
};

}

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
    const WorkingGuard guard{m_working};
    const std::vector<geometrize::ShapeResult> results{m_runner.step(options, shapeCreator, energyFunction, addShapePreconditionFunction)};
    emit signal_didStep(results);
}

void ImageTaskWorker::drawShape(const std::shared_ptr<geometrize::Shape> shape, const geometrize::rgba color, const std::vector<geometrize::ScanlineColor> segments)
{
    emit signal_willStep();
    const WorkingGuard guard{m_working};
    // A2.4:segments 非空时按行级颜色重画(重做/重放复用存档,不重算)
    const geometrize::ShapeResult result{m_runner.getModel().drawShape(shape, color, segments)};
    emit signal_didStep({ result });
}

void ImageTaskWorker::replayShapes(std::vector<geometrize::ShapeResult> shapes, const geometrize::rgba resetColor)
{
    emit signal_willStep();
    const WorkingGuard guard{m_working};
    geometrize::Model& model{m_runner.getModel()};
    model.reset(resetColor);
    // drawShape 无条件绘制且不消费 RNG:重放后的位图/分数链与原轨迹逐位一致
    for(const geometrize::ShapeResult& result : shapes) {
        model.drawShape(result.shape, result.color, result.segments);
    }
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
