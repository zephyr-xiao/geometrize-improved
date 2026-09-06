// Model:快慢路径等价、确定性、过订阅、拒绝回滚、异常传播
#include "doctest.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "geometrize/bitmap/bitmap.h"
#include "geometrize/bitmap/rgba.h"
#include "geometrize/commonutil.h"
#include "geometrize/core.h"
#include "geometrize/model.h"
#include "geometrize/rasterizer/rasterizer.h"
#include "geometrize/shape/circle.h"
#include "geometrize/shape/shape.h"
#include "geometrize/shape/shapefactory.h"
#include "geometrize/shaperesult.h"

namespace
{

geometrize::Bitmap makeGradientBitmap(std::uint32_t width, std::uint32_t height)
{
    std::vector<std::uint8_t> data(width * height * 4U);
    for(std::uint32_t y = 0; y < height; y++) {
        for(std::uint32_t x = 0; x < width; x++) {
            const std::size_t offset{(static_cast<std::size_t>(width) * y + x) * 4U};
            data[offset] = static_cast<std::uint8_t>(x * 255U / (width - 1U));
            data[offset + 1U] = static_cast<std::uint8_t>(y * 255U / (height - 1U));
            data[offset + 2U] = static_cast<std::uint8_t>((x * x + y) * 255U / (width * width + height));
            data[offset + 3U] = 255U;
        }
    }
    return geometrize::Bitmap{width, height, std::move(data)};
}

// 固定形状序列:每次调用下一个圆(位置由计数器决定),保证两条路径输入一致
class DeterministicShapeCreator
{
public:
    explicit DeterministicShapeCreator(std::uint32_t gridSize)
        : m_gridSize{gridSize}
    {
    }

    std::shared_ptr<geometrize::Shape> operator()()
    {
        const float x = static_cast<float>(8 + (m_counter * 7) % (m_gridSize - 16));
        const float y = static_cast<float>(8 + (m_counter * 11) % (m_gridSize - 16));
        const float r = static_cast<float>(3 + m_counter % 6);
        m_counter++;
        auto circle = std::make_shared<geometrize::Circle>(x, y, r);
        circle->setup = [](geometrize::Shape&) {};
        circle->mutate = [](geometrize::Shape& s) {
            auto& circle = static_cast<geometrize::Circle&>(s);
            circle.m_x += 1.0f;
            circle.m_r = (circle.m_r > 2.0f) ? circle.m_r - 1.0f : circle.m_r;
        };
        const std::int32_t extent{static_cast<std::int32_t>(m_gridSize)};
        circle->rasterize = [extent](const geometrize::Shape& s) {
            return geometrize::rasterize(static_cast<const geometrize::Circle&>(s), 0, 0, extent, extent);
        };
        return circle;
    }

private:
    std::uint32_t m_gridSize;
    std::uint32_t m_counter{0};
};

} // namespace

TEST_CASE("Model 构造:current 初始化为 target 平均色")
{
    const auto target = makeGradientBitmap(16, 16);
    geometrize::Model model{target};

    // 梯度图平均色约在中央像素附近,中心像素值可作弱校验(四通道 alpha 恒 255)
    CHECK(model.getCurrent().getPixel(8, 8).a == 255);
    CHECK(model.getCurrent().getWidth() == 16);
    CHECK(model.getHeight() == 16);
}

TEST_CASE("step 快慢路径等价:同输入逐位一致(核心门禁)")
{
    const auto targetA = makeGradientBitmap(48, 48);
    const auto targetB = makeGradientBitmap(48, 48);

    // 路径 A:默认(无回调)→ 补丁快路径(collectAndDrawWithUndo + differencePartialPatched)
    geometrize::Model modelA{targetA};
    // 路径 B:注入恒真 noop 前条件 → 强制走整图路径(before 全图拷贝 + differencePartial)
    geometrize::Model modelB{targetB};

    DeterministicShapeCreator creatorA{48};
    DeterministicShapeCreator creatorB{48};

    const auto noopPrecondition = [](double, double, const geometrize::Shape&,
                                     const std::vector<geometrize::Scanline>&, const geometrize::rgba&,
                                     const geometrize::Bitmap&, const geometrize::Bitmap&,
                                     const geometrize::Bitmap&) {
        return true; // 恒接受:与默认前条件在新分数更低时的接受语义一致
    };

    for(int i = 0; i < 30; i++) {
        const auto resultsA = modelA.step(creatorA, 128, 1, 10, 1);
        const auto resultsB = modelB.step(creatorB, 128, 1, 10, 1, nullptr, noopPrecondition);

        // 恒真前条件会让 B 接受劣化形状,分数与 A 不同——但只要 A 接受了,B 也应接受,
        // 且位图变化一致。等价性断言仅在 A 有输出时逐位比对(B 的输出可能更多)。
        if(!resultsA.empty()) {
            REQUIRE(!resultsB.empty());
            // 位置一致(同形状序列)
            CHECK(resultsA.back().shape->getType() == resultsB.back().shape->getType());
        }
    }

    // 位图逐字节一致:两条路径对"接受了的形状"的像素效果必须相同。
    // 由于恒真前条件 B 可能多接受,等价比较改为:B 回滚语义检验 ——
    // 用与默认语义相同的"新分数更低"前条件重新对拍:
    SUBCASE("") {
        geometrize::Model modelC{makeGradientBitmap(48, 48)};
        geometrize::Model modelD{makeGradientBitmap(48, 48)};
        DeterministicShapeCreator creatorC{48};
        DeterministicShapeCreator creatorD{48};

        const auto defaultEquivPrecondition = [](double lastScore, double newScore, const geometrize::Shape&,
                                                 const std::vector<geometrize::Scanline>&, const geometrize::rgba&,
                                                 const geometrize::Bitmap&, const geometrize::Bitmap&,
                                                 const geometrize::Bitmap&) {
            return newScore < lastScore; // 与 defaultAddShapePrecondition 语义一致
        };

        for(int i = 0; i < 30; i++) {
            const auto resultsC = modelC.step(creatorC, 128, 1, 10, 1);
            const auto resultsD = modelD.step(creatorD, 128, 1, 10, 1, nullptr, defaultEquivPrecondition);
            CHECK(resultsC.size() == resultsD.size());
            CHECK(resultsC.empty() == resultsD.empty());
        }

        // 快路径(C)与整图路径(D)最终位图逐字节一致
        const auto& dataC = modelC.getCurrent().getDataRef();
        const auto& dataD = modelD.getCurrent().getDataRef();
        REQUIRE(dataC.size() == dataD.size());
        bool identical = true;
        for(std::size_t i = 0; i < dataC.size(); i++) {
            if(dataC[i] != dataD[i]) {
                identical = false;
                break;
            }
        }
        CHECK(identical);
    }
}

TEST_CASE("step 确定性:同配置两跑位图一致;maxThreads=64 过订阅一致")
{
    const auto runOnce = [](std::uint32_t maxThreads) {
        geometrize::Model model{makeGradientBitmap(32, 32)};
        DeterministicShapeCreator creator{32};
        for(int i = 0; i < 8; i++) {
            model.step(creator, 128, 2, 10, maxThreads);
        }
        return model.getCurrent().copyData();
    };

    const auto first1 = runOnce(1);
    const auto first2 = runOnce(1);
    CHECK(first1 == first2); // 单线程两跑一致

#if defined(GEOTEST_FAST)
    const auto many1 = runOnce(64);
    const auto many2 = runOnce(64);
    CHECK(many1 == many2); // 过订阅两跑一致
    // B7 改进:多线程结果与单线程一致(种子按 submit 序预分配,与完成序无关)
    CHECK(first1 == many1);
#else
    // 上游 std::async 每步新建线程,64 过订阅下完成序不定、两跑可不一致(B7 修复点),不做断言
    const auto many1 = runOnce(64);
    (void)many1;
#endif
}

TEST_CASE("step 高拒绝率:纯色 target 连续空返回、位图不变(覆盖 restoreFromUndo)")
{
    const geometrize::Bitmap solid{32, 32, geometrize::rgba{100, 100, 100, 255}};
    geometrize::Model model{solid};
    const auto initialData = model.getCurrent().copyData();

    DeterministicShapeCreator creator{32};
    for(int i = 0; i < 10; i++) {
        const auto results = model.step(creator, 128, 4, 20, 4);
        CHECK(results.empty()); // 均匀纯色图无法改善,全部拒绝
    }

    CHECK(model.getCurrent().copyData() == initialData);
}

// 金字塔轨道(改进版专属):baseline 变体无 pyramidSearch 参数,整体宏分流。
// 半分辨率缓存懒维护的正确性依赖:接受落画/reset/drawShape 置脏 + 下次提交前重建。
#if defined(GEOTEST_FAST)
TEST_CASE("金字塔 step 确定性:同配置两跑位图一致(含半分辨率缓存重建路径)")
{
    const auto runOnce = [](std::uint32_t maxThreads) {
        geometrize::Model model{makeGradientBitmap(32, 32)};
        DeterministicShapeCreator creator{32};
        for(int i = 0; i < 8; i++) {
            model.step(creator, 128, 2, 10, maxThreads, nullptr, nullptr, true);
        }
        return model.getCurrent().copyData();
    };

    const auto first1 = runOnce(1);
    const auto first2 = runOnce(1);
    CHECK(first1 == first2); // 单线程两跑一致(含 m_currentHalf 懒重建时序)

    const auto many1 = runOnce(64);
    const auto many2 = runOnce(64);
    CHECK(many1 == many2);   // 过订阅两跑一致(半分辨率图只读共享 + 每任务私有 halfBuffer)
    CHECK(first1 == many1);  // 种子按 submit 序预分配:线程数不影响结果(与全分辨率轨道同不变量)
}

TEST_CASE("金字塔分叉哨兵:pyramid on 与 off 输出必须不同")
{
    // 目的:抓住"flag 被静默忽略"的回归(透传链断裂、开关失效)。金字塔是搜索启发式,
    // 若未来两模式输出偶然全同(例如半分辨率退化恒等),本用例红——那时应改写用例,
    // 而不是恢复"必须不同"的默认假设。
    const auto runPyramid = [](bool pyramid) {
        geometrize::Model model{makeGradientBitmap(48, 48)};
        DeterministicShapeCreator creator{48};
        for(int i = 0; i < 10; i++) {
            model.step(creator, 128, 2, 20, 4, nullptr, nullptr, pyramid);
        }
        return model.getCurrent().copyData();
    };

    const auto full = runPyramid(false);
    const auto half = runPyramid(true);
    CHECK(full != half);
}

TEST_CASE("自适应步长 step 确定性:同配置两跑位图一致(单线程+过订阅)")
{
    const auto runOnce = [](std::uint32_t maxThreads) {
        geometrize::core::HillClimbEnhancements enhancements;
        enhancements.adaptiveStep = true;
        geometrize::Model model{makeGradientBitmap(32, 32)};
        DeterministicShapeCreator creator{32};
        for(int i = 0; i < 8; i++) {
            model.step(creator, 128, 2, 10, maxThreads, nullptr, nullptr, false, enhancements);
        }
        return model.getCurrent().copyData();
    };

    const auto first1 = runOnce(1);
    const auto first2 = runOnce(1);
    CHECK(first1 == first2);

    const auto many1 = runOnce(64);
    const auto many2 = runOnce(64);
    CHECK(many1 == many2);
    CHECK(first1 == many1); // 种子按 submit 序:增强轨道同样线程数无关
}

TEST_CASE("自适应步长分叉哨兵:adaptiveStep on 与 off 输出必须不同")
{
    // 与金字塔哨兵同款:抓"增强开关被静默忽略"。注意步长状态机与外部确定性 creator
    // 的固定 mutate lambda 不冲突——creator 覆写了 mutate,mutateScaled 未覆写时会走
    // State::mutate 的降级路径(普通 mutate),但此用例注入链完整(用 factory 构形),
    // 故分叉成立。若未来偶现全同,改写用例而非恢复默认假设。
    const auto runAdaptive = [](bool adaptive) {
        geometrize::commonutil::seedRandomGenerator(9001);
        geometrize::core::HillClimbEnhancements enhancements;
        enhancements.adaptiveStep = adaptive;
        geometrize::Model model{makeGradientBitmap(64, 64)};
        const auto creator = geometrize::createDefaultShapeCreator(
            geometrize::ShapeTypes::ELLIPSE, 0, 0, 63, 63);
        model.setSeed(9001);
        for(int i = 0; i < 10; i++) {
            model.step(creator, 128, 4, 30, 4, nullptr, nullptr, false, enhancements);
        }
        return model.getCurrent().copyData();
    };

    const auto plain = runAdaptive(false);
    const auto adaptive = runAdaptive(true);
    CHECK(plain != adaptive);
}

TEST_CASE("增强轨道高拒绝率:纯色 target 连续空返回、位图不变")
{
    const geometrize::Bitmap solid{32, 32, geometrize::rgba{100, 100, 100, 255}};
    geometrize::core::HillClimbEnhancements enhancements;
    enhancements.adaptiveStep = true;
    geometrize::Model model{solid};
    const auto initialData = model.getCurrent().copyData();

    DeterministicShapeCreator creator{32};
    for(int i = 0; i < 10; i++) {
        const auto results = model.step(creator, 128, 4, 20, 4, nullptr, nullptr, false, enhancements);
        CHECK(results.empty());
    }

    CHECK(model.getCurrent().copyData() == initialData);
}

TEST_CASE("alpha 搜索 step 确定性:同配置两跑位图一致")
{
    const auto runOnce = [](std::uint32_t maxThreads) {
        geometrize::core::HillClimbEnhancements enhancements;
        enhancements.alphaSearch = true;
        geometrize::Model model{makeGradientBitmap(32, 32)};
        DeterministicShapeCreator creator{32};
        for(int i = 0; i < 8; i++) {
            model.step(creator, 128, 2, 10, maxThreads, nullptr, nullptr, false, enhancements);
        }
        return model.getCurrent().copyData();
    };

    const auto first1 = runOnce(1);
    const auto first2 = runOnce(1);
    CHECK(first1 == first2);

    const auto many1 = runOnce(64);
    const auto many2 = runOnce(64);
    CHECK(many1 == many2);
    CHECK(first1 == many1);
}

TEST_CASE("alpha 搜索分叉哨兵:alphaSearch on 与 off 输出必须不同")
{
    // 与金字塔/自适应步长哨兵同款:抓"增强开关被静默忽略"。
    const auto runAlphaSearch = [](bool alphaSearch) {
        geometrize::commonutil::seedRandomGenerator(9001);
        geometrize::core::HillClimbEnhancements enhancements;
        enhancements.alphaSearch = alphaSearch;
        geometrize::Model model{makeGradientBitmap(64, 64)};
        const auto creator = geometrize::createDefaultShapeCreator(
            geometrize::ShapeTypes::ELLIPSE, 0, 0, 63, 63);
        model.setSeed(9001);
        for(int i = 0; i < 10; i++) {
            model.step(creator, 128, 4, 30, 4, nullptr, nullptr, false, enhancements);
        }
        return model.getCurrent().copyData();
    };

    const auto plain = runAlphaSearch(false);
    const auto searched = runAlphaSearch(true);
    CHECK(plain != searched);
}

TEST_CASE("alpha 搜索胜者回写:接受结果的 color.a ∈ 规范化档位集")
{
    // 规范化档位 = {64,128,192} ∪ {用户 200} 升序 = {64,128,192,200};
    // 胜者档经 State::m_alpha → computeColor 的 alpha 通道 → ShapeResult.color.a 传导
    geometrize::core::HillClimbEnhancements enhancements;
    enhancements.alphaSearch = true;
    enhancements.alphaCandidates = {64U, 128U, 192U};
    const std::uint8_t userAlpha = 200U;

    geometrize::Model model{makeGradientBitmap(48, 48)};
    const auto creator = geometrize::createDefaultShapeCreator(
        geometrize::ShapeTypes::ELLIPSE, 0, 0, 47, 47);
    model.setSeed(777);

    int acceptedCount = 0;
    for(int i = 0; i < 20 && acceptedCount < 10; i++) {
        const auto results = model.step(creator, userAlpha, 4, 30, 4, nullptr, nullptr, false, enhancements);
        for(const auto& result : results) {
            acceptedCount++;
            const bool inTiers = (result.color.a == 64U) || (result.color.a == 128U)
                || (result.color.a == 192U) || (result.color.a == userAlpha);
            CHECK(inTiers);
        }
    }
    CHECK(acceptedCount > 0); // 梯度图上必能接受若干形状,否则回写通道无从检验
}

TEST_CASE("增强组合(adaptive+alpha)step 确定性:两跑位图一致")
{
    const auto runOnce = [] {
        geometrize::core::HillClimbEnhancements enhancements;
        enhancements.adaptiveStep = true;
        enhancements.alphaSearch = true;
        geometrize::Model model{makeGradientBitmap(32, 32)};
        DeterministicShapeCreator creator{32};
        for(int i = 0; i < 8; i++) {
            model.step(creator, 128, 2, 10, 4, nullptr, nullptr, false, enhancements);
        }
        return model.getCurrent().copyData();
    };

    const auto first = runOnce();
    const auto second = runOnce();
    CHECK(first == second);
}

TEST_CASE("误差图引导 step 确定性:同配置两跑位图一致(单线程+过订阅)")
{
    const auto runOnce = [](std::uint32_t maxThreads) {
        geometrize::core::HillClimbEnhancements enhancements;
        enhancements.errorGuide = true;
        geometrize::Model model{makeGradientBitmap(32, 32)};
        DeterministicShapeCreator creator{32};
        for(int i = 0; i < 8; i++) {
            model.step(creator, 128, 2, 10, maxThreads, nullptr, nullptr, false, enhancements);
        }
        const auto data = model.getCurrent().copyData();
        return data;
    };

    const auto first1 = runOnce(1);
    const auto first2 = runOnce(1);
    CHECK(first1 == first2); // 单线程两跑一致

    const auto many1 = runOnce(64);
    const auto many2 = runOnce(64);
    CHECK(many1 == many2); // 过订阅两跑一致

    // 注意:不做 first1 == many1 跨线程数断言。引导采样在任务内消费 seed 相关 RNG,
    // 不同任务(seed 不同)采样中心不同 → 任务数变化改变候选池 → 全局最优可变。
    // B7 的"线程数无关"承诺仅对任务内无 RNG 消耗的轨道(DSC 固定变异)成立。
}

TEST_CASE("误差图引导分叉哨兵:errorGuide on 与 off 输出必须不同")
{
    // 与金字塔/自适应步长/alpha 搜索哨兵同款:抓"增强开关被静默忽略"
    // (透传链断裂、Model 侧 errorMap 没接上、core 侧放置逻辑失效等)。
    // 引导改变候选位置 → 序列分叉;若未来偶现全同,改写用例而非恢复默认假设。
    const auto runGuide = [](bool errorGuide) {
        geometrize::commonutil::seedRandomGenerator(9001);
        geometrize::core::HillClimbEnhancements enhancements;
        enhancements.errorGuide = errorGuide;
        geometrize::Model model{makeGradientBitmap(64, 64)};
        const auto creator = geometrize::createDefaultShapeCreator(
            geometrize::ShapeTypes::ELLIPSE, 0, 0, 63, 63);
        model.setSeed(9001);
        for(int i = 0; i < 10; i++) {
            model.step(creator, 128, 4, 30, 4, nullptr, nullptr, false, enhancements);
        }
        return model.getCurrent().copyData();
    };

    const auto plain = runGuide(false);
    const auto guided = runGuide(true);
    CHECK(plain != guided);
}

TEST_CASE("误差图引导高拒绝率:纯色 target 收敛后连续空返回、位图不变")
{
    // 误差图全零退化:sampleCenter 恒 false → 形状保持原 setup 位置,不崩溃不引导
    const geometrize::Bitmap solid{32, 32, geometrize::rgba{100, 100, 100, 255}};
    geometrize::core::HillClimbEnhancements enhancements;
    enhancements.errorGuide = true;
    geometrize::Model model{solid};
    const auto initialData = model.getCurrent().copyData();

    DeterministicShapeCreator creator{32};
    for(int i = 0; i < 10; i++) {
        const auto results = model.step(creator, 128, 4, 20, 4, nullptr, nullptr, false, enhancements);
        CHECK(results.empty());
    }

    CHECK(model.getCurrent().copyData() == initialData);
}

#if 0 // CD
#if 0 // CD
TEST_CASE("误差图引导 reset 后继续 step 确定性:重建时序覆盖")
{
    // reset 置脏 → 下次 step 提交前按 reset 后的 m_current 重建误差图;
    // 两跑(各含一次 reset)位图必须一致,锁定重建时序与置脏链
    const auto runOnce = [] {
        geometrize::core::HillClimbEnhancements enhancements;
        enhancements.errorGuide = true;
        geometrize::Model model{makeGradientBitmap(32, 32)};
        DeterministicShapeCreator creator{32};
        for(int i = 0; i < 4; i++) {
            model.step(creator, 128, 2, 10, 4, nullptr, nullptr, false, enhancements);
        }
        model.reset(geometrize::rgba{0, 0, 0, 255});
        for(int i = 0; i < 4; i++) {
            model.step(creator, 128, 2, 10, 4, nullptr, nullptr, false, enhancements);
        }
        return model.getCurrent().copyData();
    };

    const auto first = runOnce();
    const auto second = runOnce();
    CHECK(first == second);
}

#endif // CD
#if 0 // CD
#if 0 // CD
#if 0 // CD
#endif // CD
TEST_CASE("增强全组合(errorGuide+adaptive+alpha+pyramid)step 确定性:两跑位图一致")
{
    // 四开关全开:引导采样坐标全分辨率、评估半分辨率——锁定组合坐标系自洽
    const auto runOnce = [] {
        geometrize::core::HillClimbEnhancements enhancements;
        enhancements.errorGuide = true;
        enhancements.adaptiveStep = true;
        enhancements.alphaSearch = true;
        geometrize::Model model{makeGradientBitmap(48, 48)};
        DeterministicShapeCreator creator{48};
        for(int i = 0; i < 8; i++) {
            model.step(creator, 128, 2, 10, 4, nullptr, nullptr, true, enhancements);
        }
        return model.getCurrent().copyData();
    };

    const auto first = runOnce();
    const auto second = runOnce();
    CHECK(first == second);
}


#endif // CD
#if 0 // CD
#endif // CD
#if 0 // CD
#endif // CD
#if 0 // CD
TEST_CASE("T7 不变量:经典 step 后 ShapeResult.color.a == 传入 alpha(双变体)")
{
    // 锁定"m_alpha 恒等于外部 alpha"不变量 —— step() 落画改用 it->m_alpha 的安全前提。
    // 本用例双变体都跑(base 侧也在):若未来某路径破坏不变量,此用例先红。
    // 7 参调用形式:双变体签名兼容(baseline 无 pyramid/enhancements 参数,默认值路径)
    geometrize::Model model{makeGradientBitmap(48, 48)};
    const auto creator = geometrize::createDefaultShapeCreator(
        geometrize::ShapeTypes::ELLIPSE, 0, 0, 47, 47);
    model.setSeed(4242);

    const std::uint8_t userAlpha = 123U; // 非默认值,避免与初始化值巧合相等
    int acceptedCount = 0;
    for(int i = 0; i < 20 && acceptedCount < 5; i++) {
        const auto results = model.step(creator, userAlpha, 4, 30, 2);
        for(const auto& result : results) {
            acceptedCount++;
            CHECK(result.color.a == userAlpha);
        }
    }
    CHECK(acceptedCount > 0);
}

#endif // CD
#if 0 // CD
#endif // CD
#if 0 // CD
#endif // CD
#if 0 // CD
TEST_CASE("drawShape 无条件绘制、reset 恢复基线")
{
    const auto target = makeGradientBitmap(16, 16);
    geometrize::Model model{target};
    const auto initialData = model.getCurrent().copyData();

    auto shape = std::make_shared<geometrize::Circle>(8.0f, 8.0f, 4.0f);
    shape->setup = [](geometrize::Shape&) {};
    shape->mutate = [](geometrize::Shape&) {};
    shape->rasterize = [](const geometrize::Shape& s) {
        return geometrize::rasterize(static_cast<const geometrize::Circle&>(s), 0, 0, 16, 16);
    };

    const auto result = model.drawShape(shape, geometrize::rgba{255, 0, 0, 255});
    CHECK(result.shape.get() != nullptr);
    CHECK(model.getCurrent().copyData() != initialData);

    model.reset(geometrize::rgba{0, 0, 0, 255});
    CHECK(model.getCurrent().getPixel(0, 0).r == 0);
    CHECK(model.getCurrent().getPixel(0, 0).a == 255);
}

#endif // CD
#if 0 // CD
#if 0 // CD
#endif // CD
#endif // CD
TEST_CASE("step 异常传播:shapeCreator 抛异常不吞掉、池存活可继续")
{
    const auto target = makeGradientBitmap(16, 16);
    geometrize::Model model{target};

    int callCount = 0;
    const auto throwingCreator = [&callCount]() -> std::shared_ptr<geometrize::Shape> {
        callCount++;
        throw std::runtime_error("test exception from creator");
    };

#if defined(GEOTEST_FAST)
    // 改进版按原异常对象重抛(保留 runtime_error 类型)
    CHECK_THROWS_AS(model.step(throwingCreator, 128, 2, 5, 1), std::runtime_error);

    // 池未被异常损坏:正常 creator 随后仍可步进(持久线程池 B7 的存活承诺)
    // (grid 必须比形状坐标余量大:creator 内部 m_gridSize-16 作模,16 会除零)
    DeterministicShapeCreator creator{32};
    const auto results = model.step(creator, 128, 2, 10, 1);
    (void)results; // 是否接受取决于分数,只要不崩溃、不挂起即为通过
    CHECK(callCount > 0);
#else
    // 上游:异常不吞掉即可(重抛类型切片、async 后续行为均属已知差异,不在断言范围)
    CHECK_THROWS_AS(model.step(throwingCreator, 128, 2, 5, 1), std::exception);
#endif
}

// ---- F3.2 区域优先绘制 ----

#endif // CD
#if 0 // CD
#endif // CD
#if 0 // CD
TEST_CASE("区域优先哨兵:区域 on 与 off 输出必须不同")
{
    // 抓"区域被静默忽略"(透传断裂/rebuild 未接 regions/m_lastRegions 比较失效)
    const auto run = [](bool withRegion) {
        geometrize::commonutil::seedRandomGenerator(4242);
        geometrize::core::HillClimbEnhancements enhancements;
        enhancements.errorGuide = true;
        geometrize::Model model{makeGradientBitmap(64, 64)};
        const auto creator = geometrize::createDefaultShapeCreator(
            geometrize::ShapeTypes::ELLIPSE, 0, 0, 63, 63);
        model.setSeed(4242);
        std::vector<geometrize::core::RegionRect> regions;
        if(withRegion) {
            regions.push_back(geometrize::core::RegionRect{8, 8, 40, 40});
        }
        for(int i = 0; i < 10; i++) {
            model.step(creator, 128, 4, 30, 4, nullptr, nullptr, false, enhancements, regions);
        }
        return model.getCurrent().copyData();
    };

    const auto without = run(false);
    const auto with = run(true);
    CHECK(without != with);
}

#endif // CD
#if 0 // CD
#endif // CD
#if 0 // CD
TEST_CASE("区域优先 step 确定性:同配置两跑位图一致")
{
    const auto runOnce = [] {
        geometrize::core::HillClimbEnhancements enhancements;
        enhancements.errorGuide = true;
        const std::vector<geometrize::core::RegionRect> regions{geometrize::core::RegionRect{4, 4, 28, 28}};
        geometrize::Model model{makeGradientBitmap(32, 32)};
        DeterministicShapeCreator creator{32};
        for(int i = 0; i < 8; i++) {
            model.step(creator, 128, 2, 10, 4, nullptr, nullptr, false, enhancements, regions);
        }
        return model.getCurrent().copyData();
    };

    const auto first = runOnce();
    const auto second = runOnce();
    CHECK(first == second);
}

#endif // CD
#if 0 // CD
#endif // CD
#if 0 // CD
TEST_CASE("区域优先:errorGuide 关时区域无效(逐位一致)")
{
    // 锁定语义:区域是误差图引导的修饰参数,引导关 = 区域完全不参与(连 rebuild 都不做)
    const auto run = [](bool withRegion) {
        geometrize::commonutil::seedRandomGenerator(9001);
        geometrize::core::HillClimbEnhancements enhancements; // errorGuide 默认关
        geometrize::Model model{makeGradientBitmap(32, 32)};
        DeterministicShapeCreator creator{32};
        std::vector<geometrize::core::RegionRect> regions;
        if(withRegion) {
            regions.push_back(geometrize::core::RegionRect{0, 0, 31, 31});
        }
        for(int i = 0; i < 8; i++) {
            model.step(creator, 128, 2, 10, 4, nullptr, nullptr, false, enhancements, regions);
        }
        return model.getCurrent().copyData();
    };

    const auto without = run(false);
    const auto with = run(true);
    CHECK(without == with);
}

#endif // CD
#if 0 // CD
#if 0 // CD
#endif // CD
#if 0 // CD
TEST_CASE("区域优先:区域中途变化触发重建生效")
{
    // 先空区域 3 步(建立基线)→ 换区域继续;与"全程带区域"对比,中途区域必须产生效应
    // (覆盖 m_lastRegions 变化检测:区域变化即使 m_current 已变也会走重建分支)
    const auto run = [](bool switchMidway) {
        geometrize::core::HillClimbEnhancements enhancements;
        enhancements.errorGuide = true;
        const std::vector<geometrize::core::RegionRect> emptyRegions;
        const std::vector<geometrize::core::RegionRect> regions{geometrize::core::RegionRect{2, 2, 20, 20}};
        geometrize::Model model{makeGradientBitmap(32, 32)};
        DeterministicShapeCreator creator{32};
        for(int i = 0; i < 3; i++) {
            model.step(creator, 128, 2, 10, 4, nullptr, nullptr, false, enhancements, emptyRegions);
        }
        const auto midSnapshot = model.getCurrent().copyData();
        const auto& later = switchMidway ? regions : emptyRegions;
        for(int i = 0; i < 3; i++) {
            model.step(creator, 128, 2, 10, 4, nullptr, nullptr, false, enhancements, later);
        }
        return std::make_pair(midSnapshot, model.getCurrent().copyData());
    };

    const auto switched = run(true);
    const auto unswitched = run(false);
    CHECK(switched.first == unswitched.first); // 前 3 步两版本一致(同参数同序列)
    CHECK(switched.second != unswitched.second); // 换区域后分叉
}

// ---- A2.4 分段颜色:落画传导、确定性、分叉哨兵、线型旁路、重放复现 ----

#endif // CD
#if 0 // CD
#endif // CD
#if 0 // CD
#endif // CD
#if 0 // CD
TEST_CASE("分段落画传导:segmentColors on 时接受结果携带对齐 segments")
{
    geometrize::core::HillClimbEnhancements enhancements;
    enhancements.segmentColors = true;

    geometrize::Model model{makeGradientBitmap(48, 48)};
    const auto creator = geometrize::createDefaultShapeCreator(
        geometrize::ShapeTypes::ELLIPSE, 0, 0, 47, 47);
    model.setSeed(4242);

    int acceptedCount = 0;
    for(int i = 0; i < 20 && acceptedCount < 5; i++) {
        const auto results = model.step(creator, 128, 4, 30, 4, nullptr, nullptr, false, enhancements);
        for(const auto& result : results) {
            acceptedCount++;
            REQUIRE(!result.segments.empty());
            // segments 与重光栅化结果逐行对齐(传导正确性)
            const auto lines = result.shape->rasterize(*result.shape);
            REQUIRE(lines.size() == result.segments.size());
            for(std::size_t k = 0; k < lines.size(); k++) {
                CHECK(result.segments[k].y == lines[k].y);
                CHECK(result.segments[k].x1 == lines[k].x1);
                CHECK(result.segments[k].x2 == lines[k].x2);
                // alpha 通道 = 胜者档(增强轨道 normalizeAlphaTiers 恒并入默认三档,
                // alphaSearch=false 下胜者也可能是 64/192,既有语义,不锁死 128)
                CHECK((result.segments[k].color.a == 64U || result.segments[k].color.a == 128U || result.segments[k].color.a == 192U));
            }
        }
    }
    CHECK(acceptedCount > 0); // 梯度图上必能接受若干形状,否则传导无从检验
}

#endif // CD
#if 0 // CD
#endif // CD
#if 0 // CD
#endif // CD
#if 0 // CD
TEST_CASE("分段 step 确定性:同配置两跑位图一致(单线程+过订阅)")
{
    const auto runOnce = [](std::uint32_t maxThreads) {
        geometrize::core::HillClimbEnhancements enhancements;
        enhancements.segmentColors = true;
        geometrize::Model model{makeGradientBitmap(32, 32)};
        DeterministicShapeCreator creator{32};
        for(int i = 0; i < 8; i++) {
            model.step(creator, 128, 2, 10, maxThreads, nullptr, nullptr, false, enhancements);
        }
        return model.getCurrent().copyData();
    };

    CHECK(runOnce(1) == runOnce(1));
    CHECK(runOnce(64) == runOnce(64));
}

#endif // CD
#if 0 // CD
#endif // CD
#if 0 // CD
#endif // CD
#if 0 // CD
TEST_CASE("分段颜色分叉哨兵:segmentColors on 与 off 输出必须不同")
{
    // 与金字塔/自适应步长/alpha 搜索/误差图哨兵同款:抓"增强开关被静默忽略"
    const auto runSegmented = [](bool segmentColors) {
        geometrize::commonutil::seedRandomGenerator(9001);
        geometrize::core::HillClimbEnhancements enhancements;
        enhancements.segmentColors = segmentColors;
        geometrize::Model model{makeGradientBitmap(64, 64)};
        const auto creator = geometrize::createDefaultShapeCreator(
            geometrize::ShapeTypes::ELLIPSE, 0, 0, 63, 63);
        model.setSeed(9001);
        for(int i = 0; i < 10; i++) {
            model.step(creator, 128, 4, 30, 4, nullptr, nullptr, false, enhancements);
        }
        return model.getCurrent().copyData();
    };

    const auto plain = runSegmented(false);
    const auto segmented = runSegmented(true);
    CHECK(plain != segmented);
}

#endif // CD
#if 0 // CD
#endif // CD
#if 0 // CD
#endif // CD
#if 0 // CD
TEST_CASE("分段纯色高拒绝:target 纯色时行级取色与单色等价,连续空返回")
{
    // 纯色 target 上分段取色每行结果相同,接受行为应与开关关一致(高拒绝)
    geometrize::core::HillClimbEnhancements enhancements;
    enhancements.segmentColors = true;

    const geometrize::Bitmap solid{64, 64, geometrize::rgba{77, 88, 99, 255}};
    geometrize::Model model{solid};
    const auto creator = geometrize::createDefaultShapeCreator(
        geometrize::ShapeTypes::ELLIPSE, 0, 0, 63, 63);
    model.setSeed(31337);

    int acceptedCount = 0;
    for(int i = 0; i < 10; i++) {
        const auto results = model.step(creator, 128, 4, 30, 4, nullptr, nullptr, false, enhancements);
        for(const auto& result : results) {
            acceptedCount++;
            // 纯色上每行颜色都相同且等于整形状平均(取色公式在同质区域收敛)
            REQUIRE(!result.segments.empty());
            for(const auto& seg : result.segments) {
                CHECK(seg.color.r == result.color.r);
                CHECK(seg.color.g == result.color.g);
                CHECK(seg.color.b == result.color.b);
            }
        }
    }
    (void)acceptedCount; // 纯色上接受率可高可低,此处只验证行为一致性
}

#endif // CD
#if 0 // CD
#endif // CD
#if 0 // CD
#endif // CD
#if 0 // CD
TEST_CASE("Line 形状 segments 为空:线型旁路保持单色语义")
{
    geometrize::core::HillClimbEnhancements enhancements;
    enhancements.segmentColors = true;

    geometrize::Model model{makeGradientBitmap(64, 64)};
    const auto creator = geometrize::createDefaultShapeCreator(
        geometrize::ShapeTypes::LINE, 0, 0, 63, 63);
    model.setSeed(5150);

    int acceptedCount = 0;
    for(int i = 0; i < 30 && acceptedCount < 3; i++) {
        const auto results = model.step(creator, 128, 4, 30, 4, nullptr, nullptr, false, enhancements);
        for(const auto& result : results) {
            acceptedCount++;
            CHECK(result.segments.empty()); // 逐像素扫描线不分段,导出爆炸防线
        }
    }
    // 梯度图上线形改善概率高;若未来偶现全拒,改写用例而非放松断言
    CHECK(acceptedCount > 0);
}

#endif // CD
#if 0 // CD
#endif // CD
#if 0 // CD
#endif // CD
#if 0 // CD
TEST_CASE("分段重放逐位复现:三参 drawShape 复用存储 segments 复原位图与分数链")
{
    // 锁"分段颜色依赖落画时刻位图,重放必须复用存档不能重算"的设计裁决
    geometrize::core::HillClimbEnhancements enhancements;
    enhancements.segmentColors = true;

    geometrize::Model model{makeGradientBitmap(48, 48)};
    const auto creator = geometrize::createDefaultShapeCreator(
        geometrize::ShapeTypes::ELLIPSE, 0, 0, 47, 47);
    model.setSeed(8080);

    std::vector<geometrize::ShapeResult> drawn;
    for(int i = 0; i < 10; i++) {
        const auto results = model.step(creator, 128, 4, 30, 4, nullptr, nullptr, false, enhancements);
        for(const auto& result : results) {
            drawn.push_back(result);
        }
    }
    REQUIRE(!drawn.empty());
    REQUIRE(std::any_of(drawn.begin(), drawn.end(), [](const geometrize::ShapeResult& r) { return !r.segments.empty(); }));
    const auto originalBitmap = model.getCurrent().copyData();

    // 新模型从同一背景重放:reset 背景色 = 原模型构造背景(getAverageImageColor)
    const auto target = makeGradientBitmap(48, 48);
    geometrize::Model replayed{target};
    replayed.reset(geometrize::commonutil::getAverageImageColor(target));
    for(const auto& result : drawn) {
        replayed.drawShape(result.shape, result.color, result.segments);
    }

    // 重放零 RNG 消费、复用存档分段色 → 位图逐字节复原
    // (若改为此刻重算分段色,后续形状下的 m_current 已被覆盖,结果必然偏离)
    CHECK(replayed.getCurrent().copyData() == originalBitmap);
}

#endif // CD
#if 0 // CD
#endif // CD
#if 0 // CD
#endif // CD
#if 0 // CD
TEST_CASE("分段组合确定性:segmentColors+adaptive+alpha+pyramid 两跑一致")
{
    const auto runOnce = [] {
        geometrize::core::HillClimbEnhancements enhancements;
        enhancements.segmentColors = true;
        enhancements.adaptiveStep = true;
        enhancements.alphaSearch = true;
        geometrize::Model model{makeGradientBitmap(32, 32)};
        DeterministicShapeCreator creator{32};
        for(int i = 0; i < 8; i++) {
            model.step(creator, 128, 2, 10, 4, nullptr, nullptr, false, enhancements);
        }
        return model.getCurrent().copyData();
    };

    CHECK(runOnce() == runOnce());
}

#endif

#endif // CD
#endif // CD
#endif // CD
