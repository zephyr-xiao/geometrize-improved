#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "bitmap/rgba.h"
#include "core/errorweightmap.h"
#include "rasterizer/scanline.h"
#include "state.h"

namespace geometrize
{
class Bitmap;
}

namespace geometrize
{

namespace core
{

/**
 * @brief HillClimbEnhancements 增强爬山搜索的开关参数集(算法增强轨道,opt-in)。
 * 任一开关打开时 Model 调度走 bestHillClimbStateEnhanced;全关时零参与,经典/金字塔路径不受影响。
 */
struct HillClimbEnhancements
{
    // ---- A2.2 自适应变异步长(1/5 成功法则) ----
    bool adaptiveStep{false}; ///< 是否启用自适应步长:连续拒绝升档(步长减半)、连续接受降档(回升)。
    std::uint32_t rejectHalveInterval{8}; ///< 连续拒绝多少次升一档(maxAge=100 预算下 8 连拒是强平台信号)。
    std::uint32_t acceptEscalateRun{4}; ///< 连续接受多少次降一档(回升步长,密集改善期才触发)。
    std::int32_t maxStepShift{3}; ///< 步长移位上限:0=基准,1=1/2,2=1/4,3=1/8;再深变异近似恒等,浪费评估。

    // ---- A2.3 逐形状最优 alpha 微搜索 ----
    bool alphaSearch{false}; ///< 是否启用逐候选 alpha 档位搜索:每候选对全部档位求能量取最优,胜者档回写 State::m_alpha。
    std::vector<std::uint8_t> alphaCandidates{64U, 128U, 192U}; ///< alpha 候选档位;使用前与用户 alpha 合并规范化。

    // ---- A2.1 误差图引导形状放置 ----
    bool errorGuide{false}; ///< 是否启用误差加权放置:候选 setup 后按误差图采样中心平移,ε 概率保持原位探索。
    std::uint32_t guideEpsilonPermill{100U}; ///< ε-greedy 千分比:该比例的候选保持均匀原位,其余按误差图引导;0=全引导,1000=全均匀(等效关闭但保留 roll 消耗)。

    // ---- A2.4 分段颜色(行级取色) ----
    bool segmentColors{false}; ///< 是否启用按扫描线行级取色:形状横跨明暗边界时每行独立求最优色,消除单平均色的发灰。
    // 开启即路由增强轨道(自定义 energyFunction 同既有语义被忽略);线型形状(全单像素行)自动退化为单色。
};

/**
 * The core functions for Geometrize.
 * @author Sam Twidale (https://samcodes.co.uk/)
 */

/**
 * @brief EnergyFunction Type alias for a function that calculates a measure of the improvement adding the scanlines of a shape provides - lower energy is better.
 * @param lines The scanlines of the shape.
 * @param alpha The alpha of the scanlines.
 * @param target The target bitmap.
 * @param current The current bitmap.
 * @param buffer The buffer bitmap.
 * @param score The score.
 * @return The energy measure.
 */
using EnergyFunction = std::function<double(
    const std::vector<geometrize::Scanline>& lines,
    const std::uint32_t alpha,
    const geometrize::Bitmap& target,
    const geometrize::Bitmap& current,
    geometrize::Bitmap& buffer,
    double score)>;

/**
 * @brief defaultEnergyFunction The default/built-in energy function that calculates a measure of the improvement adding the scanlines of a shape provides - lower energy is better.
 * @param lines The scanlines of the shape.
 * @param alpha The alpha of the scanlines.
 * @param target The target bitmap.
 * @param current The current bitmap.
 * @param buffer The buffer bitmap.
 * @param score The score.
 * @return The energy measure.
 */
double defaultEnergyFunction(
        const std::vector<geometrize::Scanline>& lines,
        const std::uint32_t alpha,
        const geometrize::Bitmap& target,
        const geometrize::Bitmap& current,
        geometrize::Bitmap& buffer,
        double score);

/**
 * @brief defaultEnergyFunctionSegmented A2.4 分段颜色版内置能量函数:与 defaultEnergyFunction 同构,
 * 差异仅在取色(computeColor → computeSegmentColors)与混色(drawLines → drawLinesSegmented)。
 * 线型形状(computeSegmentColors 返回空)内部退化为单色路径,与 defaultEnergyFunction 逐位一致。
 * 差分公式与累加次序与 defaultEnergyFunction 完全相同(differencePartial 对颜色来源无感知)。
 */
double defaultEnergyFunctionSegmented(
        const std::vector<geometrize::Scanline>& lines,
        const std::uint32_t alpha,
        const geometrize::Bitmap& target,
        const geometrize::Bitmap& current,
        geometrize::Bitmap& buffer,
        double score);

/**
 * @brief computeSegmentColors A2.4:每条扫描线独立按混合公式求最优色。
 * 返回向量与 lines 一一对应;线型形状(全部扫描线均为单像素宽)返回空向量,
 * 调用方(评估侧/落画侧)据此统一退回单色路径——判定共享保证两侧颜色模型一致。
 * 内部复用 computeColor 同一公式与浮点怪癖行(a 值每调用只算一次,见 computeColor 实现)。
 */
std::vector<geometrize::rgba> computeSegmentColors(
        const geometrize::Bitmap& target,
        const geometrize::Bitmap& current,
        const std::vector<geometrize::Scanline>& lines,
        std::uint8_t alpha);

/**
 * @brief isLineLikeShape A2.4 线型形状判定:全部扫描线均为单像素宽(x1==x2)。
 * Line/Polyline/QuadraticBezier 的光栅化恒满足;面状形状必有宽行。
 * 评估侧与落画侧共用此判定,保证分段/单色的选择两侧一致。
 */
bool isLineLikeShape(const std::vector<geometrize::Scanline>& lines);

/**
 * @brief computeColor Calculates the color of the scanlines.
 * @param target The target image.
 * @param current The current image.
 * @param lines The scanlines.
 * @param alpha The alpha of the scanline.
 * @return The color of the scanlines.
 */
geometrize::rgba computeColor(
        const geometrize::Bitmap& target,
        const geometrize::Bitmap& current,
        const std::vector<geometrize::Scanline>& lines,
        std::uint8_t alpha);

/**
 * @brief differenceFull Calculates the root-mean-square error between two bitmaps.
 * @param first The first bitmap.
 * @param second The second bitmap.
 * @return The difference/error measure between the two bitmaps.
 */
double differenceFull(const geometrize::Bitmap& first, const geometrize::Bitmap& second);

/**
 * @brief differencePartial Calculates the root-mean-square error between the parts of the two bitmaps within the scanline mask.
 * This is for optimization purposes, it lets us calculate new error values only for parts of the image we know have changed.
 * @param target The target bitmap.
 * @param before The bitmap before the change.
 * @param after The bitmap after the change.
 * @param score The score.
 * @param lines The scanlines.
 * @return The difference/error between the two bitmaps, masked by the scanlines.
 */
double differencePartial(
        const geometrize::Bitmap& target,
        const geometrize::Bitmap& before,
        const geometrize::Bitmap& after,
        double score,
        const std::vector<Scanline>& lines);

/**
 * @brief bestHillClimbState Gets the best state using a hill climbing algorithm.
 * @param shapeCreator A function that will create the shapes that will be chosen from.
 * @param alpha The opacity of the shape.
 * @param n The number of random states to generate.
 * @param age The number of hillclimbing steps.
 * @param target The target bitmap.
 * @param current The current bitmap.
 * @param buffer The buffer bitmap.
 * @param lastScore The last score.
 * @param customEnergyFunction An optional function to calculate the energy (if unspecified a default implementation is used).
 * @return The best state acquired from hill climbing i.e. the one with the lowest energy.
 */
geometrize::State bestHillClimbState(
        const std::function<std::shared_ptr<geometrize::Shape>(void)>& shapeCreator,
        std::uint32_t alpha,
        std::uint32_t n,
        std::uint32_t age,
        const geometrize::Bitmap& target,
        const geometrize::Bitmap& current,
        geometrize::Bitmap& buffer,
        double lastScore,
        const EnergyFunction& customEnergyFunction = nullptr);

/**
 * @brief bestHillClimbStatePyramid 金字塔搜索变体:候选生成与爬山评估在半分辨率图上进行,
 * 形状坐标始终留在全分辨率空间(scanline 层投影,不做几何缩放)。RNG 消费序列与
 * bestHillClimbState 完全一致,两模式可逐候选对照。搜索启发式轨道,输出与全分辨率
 * 搜索不必相同属预期;最终接受判定由调用方在全分辨率进行。
 * @param shapeCreator A function that will create the shapes that will be chosen from.
 * @param alpha The opacity of the shape.
 * @param n The number of random states to generate.
 * @param age The number of hillclimbing steps.
 * @param target The half-resolution target bitmap.
 * @param current The half-resolution current bitmap.
 * @param buffer The half-resolution buffer bitmap (private to the calling task).
 * @param lastScore The last full-resolution score (all in-step evaluations share this constant baseline).
 * @param halfWidth The width of the half-resolution bitmaps.
 * @param halfHeight The height of the half-resolution bitmaps.
 * @return The best state acquired from hill climbing i.e. the one with the lowest energy.
 */
geometrize::State bestHillClimbStatePyramid(
        const std::function<std::shared_ptr<geometrize::Shape>(void)>& shapeCreator,
        std::uint32_t alpha,
        std::uint32_t n,
        std::uint32_t age,
        const geometrize::Bitmap& target,
        const geometrize::Bitmap& current,
        geometrize::Bitmap& buffer,
        double lastScore,
        std::int32_t halfWidth,
        std::int32_t halfHeight);

/**
 * @brief bestHillClimbStateEnhanced 增强爬山变体:按 HillClimbEnhancements 开关组合启用
 * 自适应步长 / alpha 档位搜索 / 误差图引导放置;halfWidth>0 时评估在半分辨率图上进行
 * (与金字塔轨道同语义)。自适应步长与 alpha 搜索保持 RNG 消费位置与经典路径一致;
 * 误差图引导改变每候选 RNG 消耗(+1 ε-roll,引导时 +3 采样 draw)——增强轨道自洽
 * (同 seed 同输入同输出)即可,与经典路径不再逐候选对照,输出不同属预期。
 * @param halfWidth 0 表示全分辨率评估;>0 时 target/current/buffer 均须为对应半分辨率位图。
 * @param errorMap 误差加权放置的权重表(须与 target/current 同图同尺寸;全分辨率图,
 * 金字塔组合时采样坐标仍是全分辨率形状坐标);空指针 = 引导关闭。
 */
geometrize::State bestHillClimbStateEnhanced(
        const std::function<std::shared_ptr<geometrize::Shape>(void)>& shapeCreator,
        std::uint32_t alpha,
        std::uint32_t n,
        std::uint32_t age,
        const geometrize::Bitmap& target,
        const geometrize::Bitmap& current,
        geometrize::Bitmap& buffer,
        double lastScore,
        std::int32_t halfWidth,
        std::int32_t halfHeight,
        const HillClimbEnhancements& enhancements,
        const ErrorWeightMap* errorMap = nullptr);

}

}
