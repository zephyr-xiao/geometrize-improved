#include "core.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "bitmap/bitmap.h"
#include "bitmap/rgba.h"
#include "commonutil.h"
#include "core/errorweightmap.h"
#include "rasterizer/rasterizer.h"
#include "rasterizer/scanline.h"
#include "shape/shape.h"
#include "shape/shapemutator.h"
#include "state.h"

namespace
{

// rasterizeInto 辅助:优先走形状的复用入口,未设置时回退传统路径(保持所有调用方行为一致)
void rasterizeIntoVector(const geometrize::Shape& shape, std::vector<geometrize::Scanline>& lines)
{
    if(shape.rasterizeInto) {
        shape.rasterizeInto(shape, lines);
    } else {
        lines = shape.rasterize(shape);
    }
}

// B6 scratch 复用:光栅化结果写入调用方提供的向量,消除爬山循环里每 age 一次的堆分配往返。
// 能量函数签名吃 const 引用,值内容与新建向量完全一致。

/// hillClimb 的 scratch 版本,行为与上游逐位一致(仅容器复用差异)
geometrize::State hillClimbScratch(
        const geometrize::State& state,
        const std::uint32_t maxAge,
        const geometrize::Bitmap& target,
        const geometrize::Bitmap& current,
        geometrize::Bitmap& buffer,
        const double lastScore,
        const geometrize::core::EnergyFunction& energyFunction,
        std::vector<geometrize::Scanline>& lines)
{
    geometrize::State s(state);
    geometrize::State bestState(state);
    double bestEnergy{bestState.m_score};

    std::uint32_t age{0};
    while(age < maxAge) {
        const geometrize::State undo{s.mutate()};
        rasterizeIntoVector(*s.m_shape, lines);
        s.m_score = energyFunction(lines, s.m_alpha, target, current, buffer, lastScore);
        const double energy = s.m_score;
        if(energy >= bestEnergy) {
            s = undo;
        } else {
            bestEnergy = energy;
            bestState = s;
            age = -1;
        }
        age++;
    }

    return bestState;
}

/// bestRandomState 的 scratch 版本,off-by-one 怪癖(RNG 消费 n+2 次)原样保留
geometrize::State bestRandomStateScratch(
        const std::function<std::shared_ptr<geometrize::Shape>(void)>& shapeCreator,
        const std::uint32_t alpha,
        const std::uint32_t n,
        const geometrize::Bitmap& target,
        const geometrize::Bitmap& current,
        geometrize::Bitmap& buffer,
        const double lastScore,
        const geometrize::core::EnergyFunction& energyFunction,
        std::vector<geometrize::Scanline>& lines)
{
    // 上游语义:State 构造时恰好 setup 一次(shapeCreator 的建形不含 setup)。
    // 本函数手动 setup 一次后以裸 State 装配,避免 State 构造函数二次 setup 消耗双倍 RNG。
    const auto makeState = [alpha](std::shared_ptr<geometrize::Shape> shape) {
        geometrize::State state;
        state.m_alpha = alpha;
        state.m_score = -1.0;
        state.m_shape = std::move(shape);
        return state;
    };

    std::shared_ptr<geometrize::Shape> firstShape{shapeCreator()};
    firstShape->setup(*firstShape);
    rasterizeIntoVector(*firstShape, lines);
    geometrize::State bestState = makeState(firstShape);
    bestState.m_score = energyFunction(lines, bestState.m_alpha, target, current, buffer, lastScore);
    double bestEnergy = bestState.m_score;

    for(std::uint32_t i = 0; i <= n; i++) {
        std::shared_ptr<geometrize::Shape> shape{shapeCreator()};
        shape->setup(*shape);
        rasterizeIntoVector(*shape, lines);

        geometrize::State state = makeState(shape);
        state.m_score = energyFunction(lines, state.m_alpha, target, current, buffer, lastScore);
        const double energy = state.m_score;
        if(i == 0 || energy < bestEnergy) {
            bestEnergy = energy;
            bestState = state;
        }
    }

    return bestState;
}

/// hillClimb 的金字塔 scratch 变体:评估在半分辨率图上进行,搜索语义与全分辨率版一致。
/// 与 hillClimbScratch 故意保持复制而非参数化——bit-exact 路径一个字节不动,
/// 金字塔路径的 scanline 投影开销也不进全分辨率热循环。
geometrize::State hillClimbScratchPyramid(
        const geometrize::State& state,
        const std::uint32_t maxAge,
        const geometrize::Bitmap& target,
        const geometrize::Bitmap& current,
        geometrize::Bitmap& buffer,
        const double lastScore,
        const geometrize::core::EnergyFunction& energyFunction,
        std::vector<geometrize::Scanline>& lines,
        std::vector<geometrize::Scanline>& halfLines,
        const std::int32_t halfWidth,
        const std::int32_t halfHeight)
{
    geometrize::State s(state);
    geometrize::State bestState(state);
    double bestEnergy{bestState.m_score};

    std::uint32_t age{0};
    while(age < maxAge) {
        const geometrize::State undo{s.mutate()};
        rasterizeIntoVector(*s.m_shape, lines);
        halfLines = geometrize::downscaleScanlines(lines, halfWidth, halfHeight);
        s.m_score = energyFunction(halfLines, s.m_alpha, target, current, buffer, lastScore);
        const double energy = s.m_score;
        if(energy >= bestEnergy) {
            s = undo;
        } else {
            bestEnergy = energy;
            bestState = s;
            age = -1;
        }
        age++;
    }

    return bestState;
}

/// bestRandomState 的金字塔 scratch 变体:RNG 消费序列与全分辨率版完全一致
/// (shapeCreator/setup 次序不变,评估分辨率不影响 RNG 流),仅评估换半分辨率。
geometrize::State bestRandomStateScratchPyramid(
        const std::function<std::shared_ptr<geometrize::Shape>(void)>& shapeCreator,
        const std::uint32_t alpha,
        const std::uint32_t n,
        const geometrize::Bitmap& target,
        const geometrize::Bitmap& current,
        geometrize::Bitmap& buffer,
        const double lastScore,
        const geometrize::core::EnergyFunction& energyFunction,
        std::vector<geometrize::Scanline>& lines,
        std::vector<geometrize::Scanline>& halfLines,
        const std::int32_t halfWidth,
        const std::int32_t halfHeight)
{
    const auto makeState = [alpha](std::shared_ptr<geometrize::Shape> shape) {
        geometrize::State state;
        state.m_alpha = alpha;
        state.m_score = -1.0;
        state.m_shape = std::move(shape);
        return state;
    };

    std::shared_ptr<geometrize::Shape> firstShape{shapeCreator()};
    firstShape->setup(*firstShape);
    rasterizeIntoVector(*firstShape, lines);
    halfLines = geometrize::downscaleScanlines(lines, halfWidth, halfHeight);
    geometrize::State bestState = makeState(firstShape);
    bestState.m_score = energyFunction(halfLines, bestState.m_alpha, target, current, buffer, lastScore);
    double bestEnergy = bestState.m_score;

    for(std::uint32_t i = 0; i <= n; i++) {
        std::shared_ptr<geometrize::Shape> shape{shapeCreator()};
        shape->setup(*shape);
        rasterizeIntoVector(*shape, lines);
        halfLines = geometrize::downscaleScanlines(lines, halfWidth, halfHeight);

        geometrize::State state = makeState(shape);
        state.m_score = energyFunction(halfLines, state.m_alpha, target, current, buffer, lastScore);
        const double energy = state.m_score;
        if(i == 0 || energy < bestEnergy) {
            bestEnergy = energy;
            bestState = state;
        }
    }

    return bestState;
}

// ---- 增强爬山轨道(A2.2 自适应步长 / A2.3 alpha 搜索;opt-in) ----
// 设计约束:RNG 消费位置与经典路径完全相同——步长缩放只改 randomRange 的区间不改
// draw 次数,alpha 搜索不消费 RNG。同 seed 下增强路径与经典路径生成同一候选序列,
// 输出差异只来自搜索语义本身,这是 opt-in 模式自身确定性的根基。

/// alpha 档位规范化:用户 alpha 恒可达(模型历史围绕它优化,常是胜者),合并去重后升序。
/// 升序保证平局取低档(确定且偏向保守叠加);入口纯函数,零 RNG。
std::vector<std::uint8_t> normalizeAlphaTiers(const std::uint8_t alpha, const std::vector<std::uint8_t>& candidates)
{
    std::vector<std::uint8_t> tiers{candidates};
    tiers.push_back(alpha);
    std::sort(tiers.begin(), tiers.end());
    tiers.erase(std::unique(tiers.begin(), tiers.end()), tiers.end());
    return tiers;
}

/// 增强求值(含 A2.3 alpha 穷举):投影到评估分辨率后,对全部档位求能量取最优。
/// 胜者档写回 stateAlpha(调用方传入 State::m_alpha 的引用,回流到落画管线)。
/// 档位评估顺序固定升序、严格 < 取先评估者——同输入同输出。
/// 返回能量值;evalOut 引用传出本次评估所用的扫描线组(halfWidth>0 时是 halfLines,否则 lines)。
double evaluateEnhanced(
        const std::vector<geometrize::Scanline>& lines,
        std::vector<geometrize::Scanline>& halfLines,
        const std::vector<geometrize::Scanline>*& evalOut,
        const geometrize::core::EnergyFunction& energyFunction,
        std::uint8_t& stateAlpha,
        const std::uint8_t userAlpha,
        const std::vector<std::uint8_t>& alphaTiers,
        const geometrize::Bitmap& target,
        const geometrize::Bitmap& current,
        geometrize::Bitmap& buffer,
        const double lastScore,
        const std::int32_t halfWidth,
        const std::int32_t halfHeight)
{
    if(halfWidth > 0) {
        halfLines = geometrize::downscaleScanlines(lines, halfWidth, halfHeight);
        evalOut = &halfLines;
    } else {
        evalOut = &lines;
    }

    // 单档退化:与经典路径的求值完全同构(alphaTiers 只含用户 alpha 一个元素时)
    if(alphaTiers.size() <= 1) {
        stateAlpha = userAlpha;
        return energyFunction(*evalOut, stateAlpha, target, current, buffer, lastScore);
    }

    // 全档穷举:固定升序、严格 < 取先。buffer 每档求值前由 copyLines 重写覆盖区,档位间无串染。
    double bestEnergy{0.0};
    std::uint8_t bestAlpha{userAlpha};
    for(std::size_t i = 0; i < alphaTiers.size(); i++) {
        const std::uint8_t tier{alphaTiers[i]};
        const double energy{energyFunction(*evalOut, tier, target, current, buffer, lastScore)};
        if(i == 0 || energy < bestEnergy) {
            bestEnergy = energy;
            bestAlpha = tier;
        }
    }
    stateAlpha = bestAlpha;
    return bestEnergy;
}

/// hillClimb 的增强 scratch 变体:步长状态机为每候选独立的局部变量(种子纯函数,
/// 不跨候选泄漏、不挂到 Shape 上——undo 回滚克隆不到它,无状态污染)。
geometrize::State hillClimbScratchEnhanced(
        const geometrize::State& state,
        const std::uint32_t maxAge,
        const geometrize::Bitmap& target,
        const geometrize::Bitmap& current,
        geometrize::Bitmap& buffer,
        const double lastScore,
        const geometrize::core::EnergyFunction& energyFunction,
        std::vector<geometrize::Scanline>& lines,
        std::vector<geometrize::Scanline>& halfLines,
        const std::int32_t halfWidth,
        const std::int32_t halfHeight,
        const geometrize::core::HillClimbEnhancements& enhancements)
{
    const std::vector<std::uint8_t> alphaTiers{normalizeAlphaTiers(state.m_alpha, enhancements.alphaCandidates)};

    geometrize::State s(state);
    geometrize::State bestState(state);
    double bestEnergy{bestState.m_score};

    std::int32_t stepShift{0};       // 0=基准,1=1/2,2=1/4,3=1/8(上限 maxStepShift,下限 0 不超基准)
    std::uint32_t acceptRun{0};      // 连续接受计数(降档用)

    std::uint32_t age{0};
    while(age < maxAge) {
        const geometrize::State undo{s.mutate(enhancements.adaptiveStep ? stepShift : 0)};
        rasterizeIntoVector(*s.m_shape, lines);
        const std::vector<geometrize::Scanline>* eval{nullptr};
        const double energy{evaluateEnhanced(lines, halfLines, eval, energyFunction, s.m_alpha, state.m_alpha, alphaTiers, target, current, buffer, lastScore, halfWidth, halfHeight)};
        s.m_score = energy;
        if(energy >= bestEnergy) {
            s = undo; // stepShift 不在 Shape 上,回滚不携带步长状态;alpha 由下一候选重写
            acceptRun = 0;
        } else {
            bestEnergy = energy;
            bestState = s;
            age = -1;
            if(enhancements.adaptiveStep) {
                acceptRun++;
                if(acceptRun >= enhancements.acceptEscalateRun && stepShift > 0) {
                    stepShift--;
                    acceptRun = 0;
                }
            }
        }
        age++;
        // age 本身就是"连续拒绝计数"(接受时被 -1 清零):每满 rejectHalveInterval 升一档
        if(enhancements.adaptiveStep && age > 0 && (age % enhancements.rejectHalveInterval) == 0
                && stepShift < enhancements.maxStepShift) {
            stepShift++;
        }
    }

    return bestState;
}

/// 误差图引导放置:ε-greedy 判定通过且采样成功时,把 setup 完的形状平移到高误差区。
/// RNG 记账:每候选恒定 +1 roll(ε 判定),引导成功再 +3(1 CDF + 2 块内抖动);
/// 探索分支与全零退化分支不额外消费——增强轨道自洽两跑一致的基准。
void placeShapeByErrorMap(
        geometrize::Shape& shape,
        const geometrize::core::ErrorWeightMap& errorMap,
        const geometrize::core::HillClimbEnhancements& enhancements)
{
    if(geometrize::commonutil::randomRange(0, 999) >= static_cast<std::int32_t>(enhancements.guideEpsilonPermill)) {
        std::int32_t cx{0};
        std::int32_t cy{0};
        if(errorMap.sampleCenter(cx, cy)) {
            geometrize::shiftShapeCenter(shape, static_cast<float>(cx), static_cast<float>(cy));
        }
    }
}

/// bestRandomState 的增强 scratch 变体:候选阶段不爬山,自适应步长只作用于爬山阶段
/// (与经典版两阶段结构一致)。alpha 搜索逐候选全档穷举;误差图引导候选放置。
geometrize::State bestRandomStateScratchEnhanced(
        const std::function<std::shared_ptr<geometrize::Shape>(void)>& shapeCreator,
        const std::uint32_t alpha,
        const std::uint32_t n,
        const geometrize::Bitmap& target,
        const geometrize::Bitmap& current,
        geometrize::Bitmap& buffer,
        const double lastScore,
        const geometrize::core::EnergyFunction& energyFunction,
        std::vector<geometrize::Scanline>& lines,
        std::vector<geometrize::Scanline>& halfLines,
        const std::int32_t halfWidth,
        const std::int32_t halfHeight,
        const geometrize::core::HillClimbEnhancements& enhancements,
        const geometrize::core::ErrorWeightMap* errorMap)
{
    const std::vector<std::uint8_t> alphaTiers{normalizeAlphaTiers(static_cast<std::uint8_t>(alpha), enhancements.alphaCandidates)};

    const auto makeState = [alpha](std::shared_ptr<geometrize::Shape> shape) {
        geometrize::State state;
        state.m_alpha = alpha;
        state.m_score = -1.0;
        state.m_shape = std::move(shape);
        return state;
    };

    const auto setupCandidate = [errorMap, &enhancements](std::shared_ptr<geometrize::Shape>& shape) {
        shape->setup(*shape);
        if(errorMap != nullptr) {
            placeShapeByErrorMap(*shape, *errorMap, enhancements);
        }
    };

    std::shared_ptr<geometrize::Shape> firstShape{shapeCreator()};
    setupCandidate(firstShape);
    rasterizeIntoVector(*firstShape, lines);
    geometrize::State bestState = makeState(firstShape);
    {
        const std::vector<geometrize::Scanline>* eval{nullptr};
        bestState.m_score = evaluateEnhanced(lines, halfLines, eval, energyFunction, bestState.m_alpha, static_cast<std::uint8_t>(alpha), alphaTiers, target, current, buffer, lastScore, halfWidth, halfHeight);
    }
    double bestEnergy = bestState.m_score;

    for(std::uint32_t i = 0; i <= n; i++) {
        std::shared_ptr<geometrize::Shape> shape{shapeCreator()};
        setupCandidate(shape);
        rasterizeIntoVector(*shape, lines);

        geometrize::State state = makeState(shape);
        const std::vector<geometrize::Scanline>* eval{nullptr};
        state.m_score = evaluateEnhanced(lines, halfLines, eval, energyFunction, state.m_alpha, static_cast<std::uint8_t>(alpha), alphaTiers, target, current, buffer, lastScore, halfWidth, halfHeight);
        const double energy = state.m_score;
        if(i == 0 || energy < bestEnergy) {
            bestEnergy = energy;
            bestState = state;
        }
    }

    return bestState;
}

}

namespace geometrize
{

namespace core
{

double defaultEnergyFunction(
        const std::vector<geometrize::Scanline>& lines,
        const std::uint32_t alpha,
        const geometrize::Bitmap& target,
        const geometrize::Bitmap& current,
        geometrize::Bitmap& buffer,
        const double score)
{
    const geometrize::rgba color(geometrize::core::computeColor(target, current, lines, alpha)); // Calculate best color for areas covered by the scanlines
    geometrize::copyLines(buffer, current, lines); // Copy area covered by scanlines to buffer bitmap
    geometrize::drawLines(buffer, color, lines); // Blend scanlines into the buffer using the color calculated earlier
    return geometrize::core::differencePartial(target, current, buffer, score, lines); // Get error measure between areas of current and modified buffers covered by scanlines
}

double defaultEnergyFunctionSegmented(
        const std::vector<geometrize::Scanline>& lines,
        const std::uint32_t alpha,
        const geometrize::Bitmap& target,
        const geometrize::Bitmap& current,
        geometrize::Bitmap& buffer,
        const double score)
{
    // A2.4:行级取色(线型形状返回空 → 退回单色整形状平均,与经典路径逐位一致)
    const std::vector<geometrize::rgba> colors{geometrize::core::computeSegmentColors(target, current, lines, static_cast<std::uint8_t>(alpha))};
    geometrize::copyLines(buffer, current, lines); // Copy area covered by scanlines to buffer bitmap
    if(colors.empty()) {
        const geometrize::rgba color(geometrize::core::computeColor(target, current, lines, static_cast<std::uint8_t>(alpha)));
        geometrize::drawLines(buffer, color, lines);
    } else {
        geometrize::drawLinesSegmented(buffer, colors, lines);
    }
    // 差分侧对颜色来源无感知,与经典路径完全相同
    return geometrize::core::differencePartial(target, current, buffer, score, lines);
}

geometrize::rgba computeColor(
        const geometrize::Bitmap& target,
        const geometrize::Bitmap& current,
        const std::vector<geometrize::Scanline>& lines,
        const std::uint8_t alpha)
{
    // Early out to avoid integer divide by 0
    if(lines.empty()) {
        return geometrize::rgba{0, 0, 0, 0};
    }

    std::int64_t totalRed{0};
    std::int64_t totalGreen{0};
    std::int64_t totalBlue{0};
    std::int64_t count{0};
    // 注意:此浮点表达式是全管线唯一的浮点参与点,float 截断行为是可观察输出的一部分,必须原样保留
    const std::int32_t a{static_cast<std::int32_t>(257.0f * 255.0f / static_cast<float>(alpha))};

    if(target.getDataRef().empty() || current.getDataRef().empty()) {
        return geometrize::rgba{0, 0, 0, 0};
    }

    // 裸指针行游标:混色公式与求和次序逐像素保持
    const auto* targetData = target.getDataRef().data();
    const auto* currentData = current.getDataRef().data();
    const std::size_t rowStride{static_cast<std::size_t>(target.getWidth()) * 4U};

    // For each scanline
    for(const geometrize::Scanline& line : lines) {
        if(line.y < 0) {
            continue;
        }
        const auto* tRow = targetData + rowStride * static_cast<std::size_t>(line.y);
        const auto* cRow = currentData + rowStride * static_cast<std::size_t>(line.y);

        for(std::int32_t x = line.x1; x <= line.x2; x++) {
            const std::size_t offset{static_cast<std::size_t>(x) * 4U};

            // Mix the red, green and blue components, blending by the given alpha value
            totalRed += static_cast<std::int64_t>((tRow[offset] - cRow[offset]) * a + cRow[offset] * 257);
            totalGreen += static_cast<std::int64_t>((tRow[offset + 1U] - cRow[offset + 1U]) * a + cRow[offset + 1U] * 257);
            totalBlue += static_cast<std::int64_t>((tRow[offset + 2U] - cRow[offset + 2U]) * a + cRow[offset + 2U] * 257);
            count++;
        }
    }

    const std::int32_t rr{static_cast<std::int32_t>(totalRed / count) >> 8};
    const std::int32_t gg{static_cast<std::int32_t>(totalGreen / count) >> 8};
    const std::int32_t bb{static_cast<std::int32_t>(totalBlue / count) >> 8};

    // Scale totals down to 0-255 range and return average blended color
    const std::uint8_t r{static_cast<std::uint8_t>(commonutil::clamp(rr, INT32_C(0), INT32_C(255)))};
    const std::uint8_t g{static_cast<std::uint8_t>(commonutil::clamp(gg, INT32_C(0), INT32_C(255)))};
    const std::uint8_t b{static_cast<std::uint8_t>(commonutil::clamp(bb, INT32_C(0), INT32_C(255)))};

    return geometrize::rgba{r, g, b, alpha};
}

bool isLineLikeShape(const std::vector<geometrize::Scanline>& lines)
{
    // 空向量不判线型(空 lines 在调用方走 computeColor 空早退,语义等价)
    if(lines.empty()) {
        return false;
    }
    // 全部扫描线均为单像素宽 = 线型形状(Line/Polyline/QuadraticBezier 的光栅化恒满足)。
    // 按几何判定而非形状类型:能量函数侧拿不到 Shape,共享谓词保证评估/落画两侧颜色模型一致。
    for(const geometrize::Scanline& line : lines) {
        if(line.x1 != line.x2) {
            return false;
        }
    }
    return true;
}

std::vector<geometrize::rgba> computeSegmentColors(
        const geometrize::Bitmap& target,
        const geometrize::Bitmap& current,
        const std::vector<geometrize::Scanline>& lines,
        const std::uint8_t alpha)
{
    // 线型形状返回空向量,调用方统一退回单色路径(逐像素取色会让导出爆炸且线型无发灰问题)
    if(lines.empty() || isLineLikeShape(lines)) {
        return {};
    }

    std::vector<geometrize::rgba> colors;
    colors.reserve(lines.size());

    // 注意:此浮点表达式是全管线唯一的浮点参与点,float 截断行为是可观察输出的一部分,必须原样保留
    const std::int32_t a{static_cast<std::int32_t>(257.0f * 255.0f / static_cast<float>(alpha))};

    if(target.getDataRef().empty() || current.getDataRef().empty()) {
        return colors;
    }

    // 裸指针行游标:行内求和次序与 computeColor 逐像素一致,仅把累加范围从整形状收窄到单行
    const auto* targetData = target.getDataRef().data();
    const auto* currentData = current.getDataRef().data();
    const std::size_t rowStride{static_cast<std::size_t>(target.getWidth()) * 4U};

    for(const geometrize::Scanline& line : lines) {
        if(line.y < 0) {
            colors.push_back(geometrize::rgba{0, 0, 0, alpha}); // 与 computeColor 的跳行语义对齐,保持下标一一对应
            continue;
        }
        const auto* tRow = targetData + rowStride * static_cast<std::size_t>(line.y);
        const auto* cRow = currentData + rowStride * static_cast<std::size_t>(line.y);

        std::int64_t totalRed{0};
        std::int64_t totalGreen{0};
        std::int64_t totalBlue{0};
        std::int64_t count{0};

        for(std::int32_t x = line.x1; x <= line.x2; x++) {
            const std::size_t offset{static_cast<std::size_t>(x) * 4U};

            totalRed += static_cast<std::int64_t>((tRow[offset] - cRow[offset]) * a + cRow[offset] * 257);
            totalGreen += static_cast<std::int64_t>((tRow[offset + 1U] - cRow[offset + 1U]) * a + cRow[offset + 1U] * 257);
            totalBlue += static_cast<std::int64_t>((tRow[offset + 2U] - cRow[offset + 2U]) * a + cRow[offset + 2U] * 257);
            count++;
        }

        // count==0 的行(理论上 trim 后不存在)防御性占位,保持返回向量与 lines 一一对应
        if(count == 0) {
            colors.push_back(geometrize::rgba{0, 0, 0, alpha});
            continue;
        }

        const std::int32_t rr{static_cast<std::int32_t>(totalRed / count) >> 8};
        const std::int32_t gg{static_cast<std::int32_t>(totalGreen / count) >> 8};
        const std::int32_t bb{static_cast<std::int32_t>(totalBlue / count) >> 8};

        const std::uint8_t r{static_cast<std::uint8_t>(commonutil::clamp(rr, INT32_C(0), INT32_C(255)))};
        const std::uint8_t g{static_cast<std::uint8_t>(commonutil::clamp(gg, INT32_C(0), INT32_C(255)))};
        const std::uint8_t b{static_cast<std::uint8_t>(commonutil::clamp(bb, INT32_C(0), INT32_C(255)))};

        colors.push_back(geometrize::rgba{r, g, b, alpha});
    }

    return colors;
}

double differenceFull(const geometrize::Bitmap& first, const geometrize::Bitmap& second)
{
    assert(first.getWidth() == second.getWidth());
    assert(first.getHeight() == second.getHeight());

    const std::size_t width{first.getWidth()};
    const std::size_t height{first.getHeight()};
    std::uint64_t total{0};

    // 裸指针行游标:公式与累加次序逐像素保持
    const auto* firstData = first.getDataRef().data();
    const auto* secondData = second.getDataRef().data();
    const std::size_t rowStride{width * 4U};

    for(std::size_t y = 0; y < height; y++) {
        const auto* fRow = firstData + rowStride * y;
        const auto* sRow = secondData + rowStride * y;
        for(std::size_t x = 0; x < width; x++) {
            const std::size_t offset{x * 4U};

            const std::int32_t dr = {static_cast<std::int32_t>(fRow[offset]) - static_cast<std::int32_t>(sRow[offset])};
            const std::int32_t dg = {static_cast<std::int32_t>(fRow[offset + 1U]) - static_cast<std::int32_t>(sRow[offset + 1U])};
            const std::int32_t db = {static_cast<std::int32_t>(fRow[offset + 2U]) - static_cast<std::int32_t>(sRow[offset + 2U])};
            const std::int32_t da = {static_cast<std::int32_t>(fRow[offset + 3U]) - static_cast<std::int32_t>(sRow[offset + 3U])};
            total += (dr * dr + dg * dg + db * db + da * da);
        }
    }
    return std::sqrt(static_cast<double>(total) / (static_cast<double>(width) * static_cast<double>(height) * 4.0)) / 255.0;
}

double differencePartial(
        const geometrize::Bitmap& target,
        const geometrize::Bitmap& before,
        const geometrize::Bitmap& after,
        const double score,
        const std::vector<Scanline>& lines)
{
    const std::uint64_t rgbaCount{static_cast<std::uint64_t>(target.getWidth()) * target.getHeight() * 4U};
    std::uint64_t total{static_cast<std::uint64_t>((score * 255.0) * (score * 255.0) * static_cast<double>(rgbaCount))};

    if(target.getDataRef().empty() || before.getDataRef().empty() || after.getDataRef().empty()) {
        return std::sqrt(static_cast<double>(total) / static_cast<double>(rgbaCount)) / 255.0;
    }

    // 裸指针行游标:差分公式与累加次序逐像素保持
    const auto* targetData = target.getDataRef().data();
    const auto* beforeData = before.getDataRef().data();
    const auto* afterData = after.getDataRef().data();
    const std::size_t rowStride{static_cast<std::size_t>(target.getWidth()) * 4U};

    for(const geometrize::Scanline& line : lines) {
        if(line.y < 0) {
            continue;
        }
        const auto* tRow = targetData + rowStride * static_cast<std::size_t>(line.y);
        const auto* bRow = beforeData + rowStride * static_cast<std::size_t>(line.y);
        const auto* aRow = afterData + rowStride * static_cast<std::size_t>(line.y);

        for(std::int32_t x = line.x1; x <= line.x2; x++) {
            const std::size_t offset{static_cast<std::size_t>(x) * 4U};

            const std::int32_t dtbr{static_cast<std::int32_t>(tRow[offset]) - static_cast<std::int32_t>(bRow[offset])};
            const std::int32_t dtbg{static_cast<std::int32_t>(tRow[offset + 1U]) - static_cast<std::int32_t>(bRow[offset + 1U])};
            const std::int32_t dtbb{static_cast<std::int32_t>(tRow[offset + 2U]) - static_cast<std::int32_t>(bRow[offset + 2U])};
            const std::int32_t dtba{static_cast<std::int32_t>(tRow[offset + 3U]) - static_cast<std::int32_t>(bRow[offset + 3U])};

            const std::int32_t dtar{static_cast<std::int32_t>(tRow[offset]) - static_cast<std::int32_t>(aRow[offset])};
            const std::int32_t dtag{static_cast<std::int32_t>(tRow[offset + 1U]) - static_cast<std::int32_t>(aRow[offset + 1U])};
            const std::int32_t dtab{static_cast<std::int32_t>(tRow[offset + 2U]) - static_cast<std::int32_t>(aRow[offset + 2U])};
            const std::int32_t dtaa{static_cast<std::int32_t>(tRow[offset + 3U]) - static_cast<std::int32_t>(aRow[offset + 3U])};

            total -= static_cast<std::uint64_t>(dtbr * dtbr + dtbg * dtbg + dtbb * dtbb + dtba * dtba);
            total += static_cast<std::uint64_t>(dtar * dtar + dtag * dtag + dtab * dtab + dtaa * dtaa);
        }
    }

    const double result{std::sqrt(static_cast<double>(total) / static_cast<double>(rgbaCount)) / 255.0};
    return result;
}

geometrize::State bestHillClimbState(
        const std::function<std::shared_ptr<geometrize::Shape>(void)>& shapeCreator,
        const std::uint32_t alpha,
        const std::uint32_t n,
        const std::uint32_t age,
        const geometrize::Bitmap& target,
        const geometrize::Bitmap& current,
        geometrize::Bitmap& buffer,
        const double lastScore,
        const EnergyFunction& customEnergyFunction)
{
    const EnergyFunction& e = customEnergyFunction ? customEnergyFunction : geometrize::core::defaultEnergyFunction;

    // B6:lines 向量跨 bestRandomState/hillClimb 全程复用,值行为与上游逐位一致
    std::vector<geometrize::Scanline> lines;
    const geometrize::State state{bestRandomStateScratch(shapeCreator, alpha, n, target, current, buffer, lastScore, e, lines)};
    return ::hillClimbScratch(state, age, target, current, buffer, lastScore, e, lines);
}

geometrize::State bestHillClimbStatePyramid(
        const std::function<std::shared_ptr<geometrize::Shape>(void)>& shapeCreator,
        const std::uint32_t alpha,
        const std::uint32_t n,
        const std::uint32_t age,
        const geometrize::Bitmap& target,
        const geometrize::Bitmap& current,
        geometrize::Bitmap& buffer,
        const double lastScore,
        const std::int32_t halfWidth,
        const std::int32_t halfHeight)
{
    // 金字塔轨道只走内置能量函数:自定义回调的语义按全分辨率位图约定,不接受半分辨率图
    const EnergyFunction& e = geometrize::core::defaultEnergyFunction;

    std::vector<geometrize::Scanline> lines;
    std::vector<geometrize::Scanline> halfLines;
    const geometrize::State state{bestRandomStateScratchPyramid(shapeCreator, alpha, n, target, current, buffer, lastScore, e, lines, halfLines, halfWidth, halfHeight)};
    return ::hillClimbScratchPyramid(state, age, target, current, buffer, lastScore, e, lines, halfLines, halfWidth, halfHeight);
}

geometrize::State bestHillClimbStateEnhanced(
        const std::function<std::shared_ptr<geometrize::Shape>(void)>& shapeCreator,
        const std::uint32_t alpha,
        const std::uint32_t n,
        const std::uint32_t age,
        const geometrize::Bitmap& target,
        const geometrize::Bitmap& current,
        geometrize::Bitmap& buffer,
        const double lastScore,
        const std::int32_t halfWidth,
        const std::int32_t halfHeight,
        const HillClimbEnhancements& enhancements,
        const ErrorWeightMap* errorMap)
{
    // A2.4 分段取色:开关开启时评估侧换分段能量函数(评估与落画同一颜色模型)。
    // 自定义能量函数在增强轨道本就被忽略(既有文档语义),不新增优先级分支。
    const EnergyFunction& e = enhancements.segmentColors
        ? geometrize::core::defaultEnergyFunctionSegmented
        : geometrize::core::defaultEnergyFunction;

    std::vector<geometrize::Scanline> lines;
    std::vector<geometrize::Scanline> halfLines;
    const geometrize::State state{bestRandomStateScratchEnhanced(shapeCreator, alpha, n, target, current, buffer, lastScore, e, lines, halfLines, halfWidth, halfHeight, enhancements, errorMap)};
    return ::hillClimbScratchEnhanced(state, age, target, current, buffer, lastScore, e, lines, halfLines, halfWidth, halfHeight, enhancements);
}

}

}
