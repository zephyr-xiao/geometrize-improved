#include "model.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>

#include "bitmap/bitmap.h"
#include "commonutil.h"
#include "core.h"
#include "core/errorweightmap.h"
#include "rasterizer/rasterizer.h"
#include "rasterizer/scanline.h"
#include "shape/shape.h"
#include "shaperesult.h"
#include "shape/shapetypes.h"

namespace
{

bool defaultAddShapePrecondition(
    const double lastScore,
    const double newScore,
    const geometrize::Shape&,
    const std::vector<geometrize::Scanline>&,
    const geometrize::rgba&,
    const geometrize::Bitmap&,
    const geometrize::Bitmap&,
    const geometrize::Bitmap&)
{
    return newScore < lastScore; // Adds the shape if the score improved (that is: the difference decreased)
}

// ---- 补丁式快照助手(B5):用覆盖区原值缓冲替代整图拷贝,消除 step 每步两次全图 memcpy ----

/// 单趟遍历扫描线:先把每个像素的 4 字节原值追加进 undo 缓冲,再按 drawLines 的原公式就地混合。
/// undo 布局 = 按 lines 顺序、行内 x 升序连续存放 4 字节 RGBA —— 与差分/回滚的遍历序天然对齐。
void collectAndDrawWithUndo(geometrize::Bitmap& image, const geometrize::rgba color, const std::vector<geometrize::Scanline>& lines, std::vector<std::uint8_t>& undo)
{
    // Convert the non-premultiplied color to alpha-premultiplied 16-bits per channel RGBA
    // In other words, scale the rgb color components by the alpha component
    std::uint32_t sr{color.r};
    sr |= sr << 8;
    sr *= color.a;
    sr /= UINT8_MAX;
    std::uint32_t sg{color.g};
    sg |= sg << 8;
    sg *= color.a;
    sg /= UINT8_MAX;
    std::uint32_t sb{color.b};
    sb |= sb << 8;
    sb *= color.a;
    sb /= UINT8_MAX;
    std::uint32_t sa{color.a};
    sa |= sa << 8;

    const std::uint32_t m{UINT16_MAX};
    const std::uint32_t aa{(m - sa) * 257U};

    std::size_t totalBytes{0};
    for(const geometrize::Scanline& line : lines) {
        if(line.x2 >= line.x1) {
            totalBytes += static_cast<std::size_t>(line.x2 - line.x1 + 1) * 4U;
        }
    }
    undo.clear();
    undo.reserve(totalBytes);

    auto* data = image.getDataRefMut().data();
    const std::size_t rowStride{static_cast<std::size_t>(image.getWidth()) * 4U};

    for(const geometrize::Scanline& line : lines) {
        if(line.y < 0) {
            continue;
        }
        auto* row = data + rowStride * static_cast<std::size_t>(line.y);

        for(std::int32_t x = line.x1; x <= line.x2; x++) {
            const std::size_t offset{static_cast<std::size_t>(x) * 4U};

            // 先存原值(differencePartial 需要的 before 值、拒绝时回滚都取自这里)
            undo.push_back(row[offset]);
            undo.push_back(row[offset + 1U]);
            undo.push_back(row[offset + 2U]);
            undo.push_back(row[offset + 3U]);

            // 再按 drawLines 原公式混合
            row[offset]     = static_cast<std::uint8_t>(((row[offset] * aa + sr * m) / m) >> 8);
            row[offset + 1U] = static_cast<std::uint8_t>(((row[offset + 1U] * aa + sg * m) / m) >> 8);
            row[offset + 2U] = static_cast<std::uint8_t>(((row[offset + 2U] * aa + sb * m) / m) >> 8);
            row[offset + 3U] = static_cast<std::uint8_t>(((row[offset + 3U] * aa + sa * m) / m) >> 8);
        }
    }
}

/// A2.4 分段版:先存原值再按 drawLinesSegmented 逐行颜色混合,undo 布局与单色版完全一致
/// (只依赖 lines 遍历序),差分/回滚代码零改动即可复用。
void collectAndDrawWithUndoSegmented(geometrize::Bitmap& image, const std::vector<geometrize::rgba>& colors, const std::vector<geometrize::Scanline>& lines, std::vector<std::uint8_t>& undo)
{
    const std::uint32_t m{UINT16_MAX};

    std::size_t totalBytes{0};
    for(const geometrize::Scanline& line : lines) {
        if(line.x2 >= line.x1) {
            totalBytes += static_cast<std::size_t>(line.x2 - line.x1 + 1) * 4U;
        }
    }
    undo.clear();
    undo.reserve(totalBytes);

    auto* data = image.getDataRefMut().data();
    const std::size_t rowStride{static_cast<std::size_t>(image.getWidth()) * 4U};

    for(std::size_t i = 0; i < lines.size(); i++) {
        const geometrize::Scanline& line = lines[i];
        if(line.y < 0) {
            continue;
        }
        const geometrize::rgba color = colors[i];

        // Convert the non-premultiplied color to alpha-premultiplied 16-bits per channel RGBA
        std::uint32_t sr{color.r};
        sr |= sr << 8;
        sr *= color.a;
        sr /= UINT8_MAX;
        std::uint32_t sg{color.g};
        sg |= sg << 8;
        sg *= color.a;
        sg /= UINT8_MAX;
        std::uint32_t sb{color.b};
        sb |= sb << 8;
        sb *= color.a;
        sb /= UINT8_MAX;
        std::uint32_t sa{color.a};
        sa |= sa << 8;
        const std::uint32_t aa{(m - sa) * 257U};

        auto* row = data + rowStride * static_cast<std::size_t>(line.y);

        for(std::int32_t x = line.x1; x <= line.x2; x++) {
            const std::size_t offset{static_cast<std::size_t>(x) * 4U};

            // 先存原值(differencePartial 需要的 before 值、拒绝时回滚都取自这里)
            undo.push_back(row[offset]);
            undo.push_back(row[offset + 1U]);
            undo.push_back(row[offset + 2U]);
            undo.push_back(row[offset + 3U]);

            // 再按 drawLinesSegmented 原公式混合
            row[offset]     = static_cast<std::uint8_t>(((row[offset] * aa + sr * m) / m) >> 8);
            row[offset + 1U] = static_cast<std::uint8_t>(((row[offset + 1U] * aa + sg * m) / m) >> 8);
            row[offset + 2U] = static_cast<std::uint8_t>(((row[offset + 2U] * aa + sb * m) / m) >> 8);
            row[offset + 3U] = static_cast<std::uint8_t>(((row[offset + 3U] * aa + sa * m) / m) >> 8);
        }
    }
}

/// 差分快路径:before 取自 undo 快照(k 游标与 lines 遍历同序推进)、after 读已写盘的现值。
/// 公式与 differencePartial 一致,仅 before 数据源换成补丁缓冲。
double differencePartialPatched(
        const geometrize::Bitmap& target,
        const std::uint8_t* afterData,
        const std::vector<std::uint8_t>& undo,
        const double score,
        const std::vector<geometrize::Scanline>& lines)
{
    const std::uint64_t rgbaCount{static_cast<std::uint64_t>(target.getWidth()) * target.getHeight() * 4U};
    std::uint64_t total{static_cast<std::uint64_t>((score * 255.0) * (score * 255.0) * static_cast<double>(rgbaCount))};

    const auto* targetData = target.getDataRef().data();
    const std::size_t rowStride{static_cast<std::size_t>(target.getWidth()) * 4U};

    std::size_t k{0}; // undo 游标:按遍历序单调推进,无需坐标映射
    for(const geometrize::Scanline& line : lines) {
        if(line.y < 0) {
            continue;
        }
        const auto* tRow = targetData + rowStride * static_cast<std::size_t>(line.y);
        const auto* aRow = afterData + rowStride * static_cast<std::size_t>(line.y);

        for(std::int32_t x = line.x1; x <= line.x2; x++) {
            const std::size_t offset{static_cast<std::size_t>(x) * 4U};

            const std::int32_t dtbr{static_cast<std::int32_t>(tRow[offset]) - static_cast<std::int32_t>(undo[k])};
            const std::int32_t dtbg{static_cast<std::int32_t>(tRow[offset + 1U]) - static_cast<std::int32_t>(undo[k + 1U])};
            const std::int32_t dtbb{static_cast<std::int32_t>(tRow[offset + 2U]) - static_cast<std::int32_t>(undo[k + 2U])};
            const std::int32_t dtba{static_cast<std::int32_t>(tRow[offset + 3U]) - static_cast<std::int32_t>(undo[k + 3U])};

            const std::int32_t dtar{static_cast<std::int32_t>(tRow[offset]) - static_cast<std::int32_t>(aRow[offset])};
            const std::int32_t dtag{static_cast<std::int32_t>(tRow[offset + 1U]) - static_cast<std::int32_t>(aRow[offset + 1U])};
            const std::int32_t dtab{static_cast<std::int32_t>(tRow[offset + 2U]) - static_cast<std::int32_t>(aRow[offset + 2U])};
            const std::int32_t dtaa{static_cast<std::int32_t>(tRow[offset + 3U]) - static_cast<std::int32_t>(aRow[offset + 3U])};

            total -= static_cast<std::uint64_t>(dtbr * dtbr + dtbg * dtbg + dtbb * dtbb + dtba * dtba);
            total += static_cast<std::uint64_t>(dtar * dtar + dtag * dtag + dtab * dtab + dtaa * dtaa);

            k += 4U;
        }
    }

    return std::sqrt(static_cast<double>(total) / static_cast<double>(rgbaCount)) / 255.0;
}

/// 拒绝回滚:把 undo 快照写回覆盖区。各扫描线区间互不相交,单趟写回即恢复,m_lastScore 不动。
void restoreFromUndo(geometrize::Bitmap& image, const std::vector<geometrize::Scanline>& lines, const std::vector<std::uint8_t>& undo)
{
    auto* data = image.getDataRefMut().data();
    const std::size_t rowStride{static_cast<std::size_t>(image.getWidth()) * 4U};

    std::size_t k{0};
    for(const geometrize::Scanline& line : lines) {
        if(line.y < 0) {
            continue;
        }
        auto* row = data + rowStride * static_cast<std::size_t>(line.y);

        for(std::int32_t x = line.x1; x <= line.x2; x++) {
            const std::size_t offset{static_cast<std::size_t>(x) * 4U};
            row[offset]     = undo[k];
            row[offset + 1U] = undo[k + 1U];
            row[offset + 2U] = undo[k + 2U];
            row[offset + 3U] = undo[k + 3U];
            k += 4U;
        }
    }
}

// ---- 持久线程池(B7):替代每步 std::async 新建线程 ----
// 语义保证:任务按 submit 序落位到结果下标(调用侧负责);dtor 等待全部任务结束。
class AsyncTaskPool
{
public:
    explicit AsyncTaskPool(std::uint32_t threadCount) : m_stop{false}
    {
        m_workers.reserve(threadCount);
        for(std::uint32_t i = 0; i < threadCount; i++) {
            m_workers.emplace_back([this] { workerLoop(); });
        }
    }

    ~AsyncTaskPool()
    {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_stop = true;
        }
        m_condition.notify_all();
        for(auto& worker : m_workers) {
            worker.join();
        }
    }

    AsyncTaskPool& operator=(const AsyncTaskPool&) = delete;
    AsyncTaskPool(const AsyncTaskPool&) = delete;

    std::future<void> submit(std::function<void()> task)
    {
        // std::function 要求可拷贝,promise 用 shared_ptr 满足该约束
        auto promise = std::make_shared<std::promise<void>>();
        auto future = promise->get_future();
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            if(m_stop) {
                throw std::runtime_error("submit on stopped AsyncTaskPool");
            }
            m_tasks.emplace([task{std::move(task)}, promise{std::move(promise)}]() mutable {
                try {
                    task();
                } catch(...) {
                    promise->set_exception(std::current_exception());
                    return;
                }
                promise->set_value();
            });
        }
        m_condition.notify_one();
        return future;
    }

private:
    void workerLoop()
    {
        for(;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_condition.wait(lock, [this] { return m_stop || !m_tasks.empty(); });
                if(m_stop && m_tasks.empty()) {
                    return;
                }
                task = std::move(m_tasks.front());
                m_tasks.pop();
            }
            task();
        }
    }

    std::vector<std::thread> m_workers;
    std::queue<std::function<void()>> m_tasks;
    std::mutex m_mutex;
    std::condition_variable m_condition;
    bool m_stop;
};

}

namespace geometrize
{

class Model::ModelImpl
{
public:
    ModelImpl(const geometrize::Bitmap& target) :
        m_target{target},
        m_current{target.getWidth(), target.getHeight(), geometrize::commonutil::getAverageImageColor(m_target)},
        m_lastScore{geometrize::core::differenceFull(m_target, m_current)},
        m_halfDirty{false},
        m_baseRandomSeed{0U},
        m_randomSeedOffset{0U}
    {}

    ModelImpl(const geometrize::Bitmap& target, const geometrize::Bitmap& initial) :
        m_target{target},
        m_current{initial},
        m_lastScore{geometrize::core::differenceFull(m_target, m_current)},
        m_halfDirty{false},
        m_baseRandomSeed{0U},
        m_randomSeedOffset{0U}
    {
        assert(m_target.getWidth() == m_current.getWidth());
        assert(m_target.getHeight() == m_current.getHeight());
    }

    ~ModelImpl()
    {
        // 显式先释放线程池:池的析构 join 全部 worker,须在其他成员仍存活时进行
        m_threadPool.reset();
    }
    ModelImpl& operator=(const ModelImpl&) = delete;
    ModelImpl(const ModelImpl&) = delete;

    void reset(const geometrize::rgba backgroundColor)
    {
        m_current.fill(backgroundColor);
        m_lastScore = geometrize::core::differenceFull(m_target, m_current);
        markHalfDirty();
        markErrorDirty();
    }

    std::int32_t getWidth() const
    {
        return m_target.getWidth();
    }

    std::int32_t getHeight() const
    {
        return m_target.getHeight();
    }

    std::vector<geometrize::State> getHillClimbState(
            const std::function<std::shared_ptr<geometrize::Shape>(void)> shapeCreator,
            const std::uint8_t alpha,
            const std::uint32_t shapeCount,
            const std::uint32_t maxShapeMutations,
            std::uint32_t maxThreads,
            const geometrize::core::EnergyFunction energyFunction,
            const bool pyramidSearch,
            const geometrize::core::HillClimbEnhancements& enhancements,
            const std::vector<geometrize::core::RegionRect>& priorityRegions)
    {
        // Ensure that the maximum number of threads is a sane value
        if(maxThreads == 0) {
            maxThreads = std::thread::hardware_concurrency();
            if(maxThreads == 0) {
                assert(0 && "Failed to get the number of concurrent threads supported by the implementation");
                maxThreads = defaultMaxThreads;
            }
        }

        std::vector<geometrize::State> states{maxThreads};

        // 持久线程池(B7):结果按下标归位保 submit 序收割 —— 上游用 std::async 时
        // 种子在主线程 submit 循环内 base+offset++ 递增、futures 按 submit 序 get,
        // 结果向量是 submit 序的函数而与线程完成顺序无关,本实现保持该性质逐位不变。
        {
            // 调度三分支:增强轨道(自适应步长/alpha 搜索/误差图引导/分段颜色)优先,金字塔次之,经典路径兜底。
            // 金字塔+增强可组合(halfWidth>0 走半分辨率评估);金字塔+自定义能量函数仍静默忽略。
            const bool useEnhanced{enhancements.adaptiveStep || enhancements.alphaSearch || enhancements.errorGuide || enhancements.segmentColors};
            const bool usePyramid{!useEnhanced && pyramidSearch && !energyFunction};
            if((useEnhanced && pyramidSearch) || usePyramid) {
                refreshHalfResolution();
            }
            const bool evalHalf{(useEnhanced && pyramidSearch) || usePyramid};
            const geometrize::Bitmap* halfTarget{evalHalf ? &m_targetHalf : nullptr};
            const geometrize::Bitmap* halfCurrent{evalHalf ? &m_currentHalf : nullptr};

            // 误差图引导:提交前重建(futures 全 join 后主线程才动共享态,重建与任务并发天然错开),
            // 任务期间只读;全分辨率图,金字塔组合时采样坐标仍是全分辨率形状坐标,零换算
            const bool guideEnabled{useEnhanced && enhancements.errorGuide};
            // 区域变化也要触发重建:dirty 短路只看 m_current,否则"改区域→空步→不重建"
            const bool regionsChanged{priorityRegions != m_lastRegions};
            if(guideEnabled && (m_errorDirty || m_errorMap.blockCount() == 0U || regionsChanged)) {
                m_errorMap.rebuild(m_target, m_current, priorityRegions);
                m_errorDirty = false;
                m_lastRegions = priorityRegions;
            }
            const core::ErrorWeightMap* errorMap{guideEnabled ? &m_errorMap : nullptr};

            std::vector<std::future<void>> futures(maxThreads);
            for(std::uint32_t i = 0; i < maxThreads; i++) {
                const std::uint32_t seed = m_baseRandomSeed + m_randomSeedOffset++;
                futures[i] = m_threadPool->submit([this, &states, i, seed, lastScore = m_lastScore, shapeCreator, alpha, shapeCount, maxShapeMutations, energyFunction, useEnhanced, usePyramid, halfTarget, halfCurrent, errorMap, enhancements]() {
                    // 每个任务体第一件事必须播种:pooled 线程残留的 thread_local RNG 状态由此永不被读到
                    geometrize::commonutil::seedRandomGenerator(seed);

                    if(useEnhanced) {
                        // 增强轨道:halfWidth==0 全分辨率;>0 半分辨率(金字塔可组合)
                        const bool half{usePyramid || (halfTarget != nullptr)};
                        const std::int32_t halfWidth{half ? static_cast<std::int32_t>(halfTarget->getWidth()) : 0};
                        const std::int32_t halfHeight{half ? static_cast<std::int32_t>(halfTarget->getHeight()) : 0};
                        if(half) {
                            geometrize::Bitmap halfBuffer{*halfCurrent};
                            states[i] = core::bestHillClimbStateEnhanced(shapeCreator, alpha, shapeCount, maxShapeMutations, *halfTarget, *halfCurrent, halfBuffer, lastScore, halfWidth, halfHeight, enhancements, errorMap);
                        } else {
                            geometrize::Bitmap buffer{m_current};
                            states[i] = core::bestHillClimbStateEnhanced(shapeCreator, alpha, shapeCount, maxShapeMutations, m_target, m_current, buffer, lastScore, 0, 0, enhancements, errorMap);
                        }
                    } else if(usePyramid) {
                        geometrize::Bitmap halfBuffer{*halfCurrent};
                        states[i] = core::bestHillClimbStatePyramid(shapeCreator, alpha, shapeCount, maxShapeMutations, *halfTarget, *halfCurrent, halfBuffer, lastScore, static_cast<std::int32_t>(halfTarget->getWidth()), static_cast<std::int32_t>(halfTarget->getHeight()));
                    } else {
                        geometrize::Bitmap buffer{m_current};
                        states[i] = core::bestHillClimbState(shapeCreator, alpha, shapeCount, maxShapeMutations, m_target, m_current, buffer, lastScore, energyFunction);
                    }
                });
            }
            // 先 drain 全部任务再重抛:pool 的 packaged_task future 析构不等待,
            // 立即重抛会让未完成任务继续写已析构的 states(上游 std::async 的
            // future 析构阻塞,恰好屏蔽了这一点)。保留上游"抛首个异常"的可见行为。
            std::exception_ptr firstError;
            for(auto& f : futures) {
                try {
                    f.get();
                } catch(std::exception& e) {
                    assert(0 && "Encountered exception when getting hill climb state");
                    std::cout << e.what() << std::endl;
                    if(!firstError) {
                        firstError = std::current_exception();
                    }
                } catch (...) {
                    assert(0 && "Encountered exception when getting hill climb state");
                    if(!firstError) {
                        firstError = std::current_exception();
                    }
                }
            }
            if(firstError) {
                std::rethrow_exception(firstError); // 按原异常对象重抛(上游按值 throw e 会切掉多态)
            }
        }

        return states;
    }

    std::vector<geometrize::ShapeResult> step(
            const std::function<std::shared_ptr<geometrize::Shape>(void)> shapeCreator,
            const std::uint8_t alpha,
            const std::uint32_t shapeCount,
            const std::uint32_t maxShapeMutations,
            const std::uint32_t maxThreads,
            const geometrize::core::EnergyFunction& energyFunction,
            const geometrize::ShapeAcceptancePreconditionFunction& addShapePrecondition,
            const bool pyramidSearch,
            const geometrize::core::HillClimbEnhancements& enhancements,
            const std::vector<geometrize::core::RegionRect>& priorityRegions)
    {
        std::vector<geometrize::State> states{getHillClimbState(shapeCreator, alpha, shapeCount, maxShapeMutations, maxThreads, energyFunction, pyramidSearch, enhancements, priorityRegions)};
        if(states.empty()) {
            assert(0 && "Failed to get a hill climb state");
            return {};
        }

        std::vector<geometrize::State>::iterator it = std::min_element(states.begin(), states.end(), [](const geometrize::State& a, const geometrize::State& b) {
            return a.m_score < b.m_score;
        });

        // Draw the shape onto the image
        const std::shared_ptr<geometrize::Shape> shape = it->m_shape;
        std::vector<geometrize::Scanline> lines;
        if(shape->rasterizeInto) {
            shape->rasterizeInto(*shape, lines);
        } else {
            lines = shape->rasterize(*shape);
        }
        // 胜者 alpha 回写通道:经典路径下 m_alpha 恒等于外部 alpha(不变量,单测锁定),
        // alpha 搜索开启时 m_alpha 是逐形状搜出的胜者档,自然传导到落画/SVG/指纹
        const geometrize::rgba color(geometrize::core::computeColor(m_target, m_current, lines, it->m_alpha));
        // A2.4:开关开启时按行级取色;线型形状返回空 → 走既有单色路径(逐位不变)
        std::vector<geometrize::rgba> lineColors;
        if(enhancements.segmentColors) {
            lineColors = geometrize::core::computeSegmentColors(m_target, m_current, lines, it->m_alpha);
        }

        const auto& addShapeCondition = addShapePrecondition ? addShapePrecondition : defaultAddShapePrecondition;
        double newScore = 0.0;

        if(!energyFunction && !addShapePrecondition) {
            // 快路径:纯 C++ default 前条件忽略全部 Bitmap 参数,用补丁式快照替代整图拷贝
            // (自定义回调可能读取任意全图像素,必须保留整图语义)
            std::vector<std::uint8_t> undo;
            if(lineColors.empty()) {
                collectAndDrawWithUndo(m_current, color, lines, undo);
            } else {
                collectAndDrawWithUndoSegmented(m_current, lineColors, lines, undo);
            }
            newScore = differencePartialPatched(m_target, m_current.getDataRef().data(), undo, m_lastScore, lines);
            if(!addShapeCondition(m_lastScore, newScore, *shape, lines, color, m_current, m_current, m_target)) {
                restoreFromUndo(m_current, lines, undo);
                return {};
            }
        } else {
            // 整图路径:与上游逐行一致(before 全图拷贝 + 拒绝时回滚)
            const geometrize::Bitmap before{m_current};
            if(lineColors.empty()) {
                geometrize::drawLines(m_current, color, lines);
            } else {
                geometrize::drawLinesSegmented(m_current, lineColors, lines);
            }

            newScore = geometrize::core::differencePartial(m_target, before, m_current, m_lastScore, lines);
            if(!addShapeCondition(m_lastScore, newScore, *shape, lines, color, before, m_current, m_target)) {
                m_current = before;
                return {};
            }
        }

        // Improvement - set new baseline and return the new shape
        m_lastScore = newScore;
        markHalfDirty(); // m_current 已变化,半分辨率缓存失真待重建
        markErrorDirty();
        // A2.4:分段颜色随结果存档(导出/重放/重做的唯一权威来源;事后重算是错的——位图已被后续步覆盖)。
        // color 仍是整形状平均色,前条件回调/旧导出语义不变。
        std::vector<geometrize::ScanlineColor> segments;
        if(!lineColors.empty()) {
            segments.reserve(lines.size());
            for(std::size_t i = 0; i < lines.size(); i++) {
                segments.push_back(geometrize::ScanlineColor{lines[i].y, lines[i].x1, lines[i].x2, lineColors[i]});
            }
        }
        const geometrize::ShapeResult result{m_lastScore, color, shape, std::move(segments)};
        return { result };
    }

    geometrize::ShapeResult drawShape(
            const std::shared_ptr<geometrize::Shape> shape,
            const geometrize::rgba color)
    {
        const std::vector<geometrize::Scanline> lines{shape->rasterize(*shape)};
        const geometrize::Bitmap before{m_current};
        geometrize::drawLines(m_current, color, lines);

        m_lastScore = geometrize::core::differencePartial(m_target, before, m_current, m_lastScore, lines);
        markHalfDirty();
        markErrorDirty();

        const geometrize::ShapeResult result{m_lastScore, color, shape};
        return result;
    }

    geometrize::ShapeResult drawShape(
            const std::shared_ptr<geometrize::Shape> shape,
            const geometrize::rgba color,
            const std::vector<geometrize::ScanlineColor>& segments)
    {
        const std::vector<geometrize::Scanline> lines{shape->rasterize(*shape)};
        const geometrize::Bitmap before{m_current};

        // 复用存储 segments 重画:行数对齐时按行级颜色混合(与原 step 落画逐字节一致);
        // 不对齐(不应发生)防御性退回单色。空 segments 走单色等价于两参重载。
        std::vector<geometrize::rgba> lineColors;
        if(segments.size() == lines.size()) {
            lineColors.reserve(lines.size());
            for(std::size_t i = 0; i < lines.size(); i++) {
                // 防御坐标一致性:光栅化不变量被破坏时逐位结果无从保证,退单色
                if(segments[i].y != lines[i].y || segments[i].x1 != lines[i].x1 || segments[i].x2 != lines[i].x2) {
                    lineColors.clear();
                    break;
                }
                lineColors.push_back(segments[i].color);
            }
        }
        if(lineColors.empty()) {
            geometrize::drawLines(m_current, color, lines);
        } else {
            geometrize::drawLinesSegmented(m_current, lineColors, lines);
        }

        m_lastScore = geometrize::core::differencePartial(m_target, before, m_current, m_lastScore, lines);
        markHalfDirty();
        markErrorDirty();

        const geometrize::ShapeResult result{m_lastScore, color, shape, segments};
        return result;
    }

    geometrize::Bitmap& getTarget()
    {
        return m_target;
    }

    geometrize::Bitmap& getCurrent()
    {
        return m_current;
    }

    const geometrize::Bitmap& getTarget() const
    {
        return m_target;
    }

    const geometrize::Bitmap& getCurrent() const
    {
        return m_current;
    }

    void setSeed(const std::uint32_t seed)
    {
        m_baseRandomSeed = seed;
    }

private:
    // ---- 金字塔搜索轨道:半分辨率位图懒维护 ----
    // targetHalf 不变只建一次;currentHalf 由脏标记驱动,在下一次 getHillClimbState
    // 提交前重建(futures 全 join 后主线程才动 m_current,重建与任务并发天然错开)。
    // 奇数尺寸:ceil(W/2)×ceil(H/2),floor 坐标映射 x → x/2 下恰好覆盖无盲区。
    void refreshHalfResolution()
    {
        if(m_targetHalf.getWidth() == 0) {
            m_targetHalf = geometrize::downsampleHalf(m_target);
        }
        if(m_halfDirty || m_currentHalf.getWidth() == 0) {
            m_currentHalf = geometrize::downsampleHalf(m_current);
            m_halfDirty = false;
        }
    }

    void markHalfDirty()
    {
        m_halfDirty = true;
    }

    // ---- 误差图引导轨道(A2.1):块级误差权重表懒维护 ----
    // 与半分辨率缓存同一套生命周期:脏标记驱动、getHillClimbState 提交前主线程重建、任务期只读。
    // 拒绝路径回滚 m_current 不置脏(误差图回到接受前状态,一致)。
    void markErrorDirty()
    {
        m_errorDirty = true;
    }

    geometrize::Bitmap m_target; ///< The target bitmap, the bitmap we aim to approximate.
    geometrize::Bitmap m_current; ///< The current bitmap.
    double m_lastScore; ///< Score derived from calculating the difference between bitmaps.
    geometrize::Bitmap m_targetHalf; ///< 金字塔轨道:半分辨率目标图(懒建一次)。
    geometrize::Bitmap m_currentHalf; ///< 金字塔轨道:半分辨率当前图(脏标记驱动懒重建)。
    bool m_halfDirty; ///< m_current 是否已领先于 m_currentHalf。
    core::ErrorWeightMap m_errorMap; ///< 误差图引导轨道:块级误差权重表(脏标记驱动懒重建)。
    bool m_errorDirty{true}; ///< m_current 是否已领先于 m_errorMap 的重建基线。
    std::vector<core::RegionRect> m_lastRegions; ///< 上次重建误差图所用的优先区域(变化检测,防 dirty 短路吞掉区域更新)。
    const static std::uint32_t defaultMaxThreads{4};
    std::unique_ptr<AsyncTaskPool> m_threadPool{std::make_unique<AsyncTaskPool>(std::max(1U, std::thread::hardware_concurrency()))}; ///< 持久线程池:容量取硬件并发数,过订阅场景由调用侧 futures 数量决定
    std::atomic<std::uint32_t> m_baseRandomSeed; ///< The base value used for seeding the random number generator (the one the user has control over).
    std::atomic<std::uint32_t> m_randomSeedOffset; ///< Seed used for random number generation. Note: incremented by each std::async call used for model stepping.
};

Model::Model(const geometrize::Bitmap& target) : d{std::unique_ptr<Model::ModelImpl>(new Model::ModelImpl(target))}
{}

Model::Model(const geometrize::Bitmap& target, const geometrize::Bitmap& initial) : d{std::unique_ptr<Model::ModelImpl>(new Model::ModelImpl(target, initial))}
{}

Model::~Model()
{}

void Model::reset(const geometrize::rgba backgroundColor)
{
    d->reset(backgroundColor);
}

std::int32_t Model::getWidth() const
{
    return d->getWidth();
}

std::int32_t Model::getHeight() const
{
    return d->getHeight();
}

std::vector<geometrize::ShapeResult> Model::step(
        const std::function<std::shared_ptr<geometrize::Shape>(void)>& shapeCreator,
        const std::uint8_t alpha,
        const std::uint32_t shapeCount,
        const std::uint32_t maxShapeMutations,
        const std::uint32_t maxThreads,
        const geometrize::core::EnergyFunction& energyFunction,
        const geometrize::ShapeAcceptancePreconditionFunction& addShapePrecondition,
        const bool pyramidSearch,
        const geometrize::core::HillClimbEnhancements& enhancements,
        const std::vector<geometrize::core::RegionRect>& priorityRegions)
{
    return d->step(shapeCreator, alpha, shapeCount, maxShapeMutations, maxThreads, energyFunction, addShapePrecondition, pyramidSearch, enhancements, priorityRegions);
}

geometrize::ShapeResult Model::drawShape(std::shared_ptr<geometrize::Shape> shape, geometrize::rgba color)
{
    return d->drawShape(shape, color);
}

geometrize::ShapeResult Model::drawShape(std::shared_ptr<geometrize::Shape> shape, geometrize::rgba color, const std::vector<geometrize::ScanlineColor>& segments)
{
    return d->drawShape(shape, color, segments);
}

geometrize::Bitmap& Model::getTarget()
{
    return d->getTarget();
}

geometrize::Bitmap& Model::getCurrent()
{
    return d->getCurrent();
}

const geometrize::Bitmap& Model::getTarget() const
{
    return d->getTarget();
}

const geometrize::Bitmap& Model::getCurrent() const
{
    return d->getCurrent();
}

void Model::setSeed(const std::uint32_t seed)
{
    d->setSeed(seed);
}

}
