// State 语义:setup 恰好一次、深克隆、mutate 返回旧状态
#include "doctest.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "geometrize/rasterizer/scanline.h"
#include "geometrize/shape/circle.h"
#include "geometrize/shape/shape.h"
#include "geometrize/state.h"

namespace
{

// 计数 setup 调用次数的测试形状:mutate 做可观察的位移
class CountingShape : public geometrize::Shape
{
public:
    CountingShape()
    {
        setup = [this](geometrize::Shape&) { setupCalls++; };
        mutate = [](geometrize::Shape&) {};
        rasterize = [](const geometrize::Shape&) {
            return std::vector<geometrize::Scanline>{geometrize::Scanline{0, 0, 0}};
        };
    }

    std::shared_ptr<geometrize::Shape> clone() const override
    {
        auto copy = std::make_shared<CountingShape>();
        copy->setupCalls = setupCalls; // 计数器随克隆体走,便于在克隆体上继续观察
        return copy;
    }

    geometrize::ShapeTypes getType() const override { return geometrize::ShapeTypes::CIRCLE; }

    int setupCalls{0};
};

} // namespace

TEST_CASE("State 默认构造:m_shape 为 null、score 为 -1")
{
    const geometrize::State state;
    CHECK(state.m_score == doctest::Approx(-1.0));
    CHECK(state.m_shape.get() == nullptr);
}

TEST_CASE("State 参数构造:setup 恰好调用一次")
{
    auto shape = std::make_shared<CountingShape>();
    geometrize::State state{shape, 128};

    CHECK(state.m_alpha == 128);
    CHECK(state.m_score == doctest::Approx(-1.0));
    CHECK(shape->setupCalls == 1);
}

TEST_CASE("State 拷贝构造:深克隆(指针不同、互不影响)")
{
    auto shape = std::make_shared<geometrize::Circle>(10.0f, 10.0f, 5.0f);
    shape->setup = [](geometrize::Shape&) {};
    shape->mutate = [](geometrize::Shape&) {};
    shape->rasterize = [](const geometrize::Shape&) { return std::vector<geometrize::Scanline>{}; };

    geometrize::State original{shape, 128};
    original.m_score = 0.5;
    const geometrize::State copy{original};

    REQUIRE(copy.m_shape.get() != nullptr);
    CHECK(copy.m_shape.get() != original.m_shape.get()); // 新的 Shape 实例(裸指针比较,避开 doctest 对 shared_ptr 的 stringification)
    CHECK(copy.m_alpha == original.m_alpha);
    CHECK(copy.m_score == doctest::Approx(0.5));

    // 克隆体与原体光栅化一致(值相等)
    std::vector<geometrize::Scanline> originalLines;
    std::vector<geometrize::Scanline> copyLines;
    originalLines = original.m_shape->rasterize(*original.m_shape);
    copyLines = copy.m_shape->rasterize(*copy.m_shape);
    CHECK(originalLines == copyLines);
}

TEST_CASE("State 拷贝赋值:自赋值保护与深克隆")
{
    auto shape = std::make_shared<geometrize::Circle>(3.0f, 4.0f, 5.0f);
    shape->setup = [](geometrize::Shape&) {};
    shape->mutate = [](geometrize::Shape&) {};
    shape->rasterize = [](const geometrize::Shape&) { return std::vector<geometrize::Scanline>{}; };

    geometrize::State state{shape, 200};

    // 自赋值不崩溃且状态不变
    geometrize::State& ref = state;
    state = ref;
    CHECK(state.m_shape.get() != nullptr);
    CHECK(state.m_alpha == 200);

    // 赋值是深克隆
    geometrize::State other{std::make_shared<CountingShape>(), 1};
    other = state;
    CHECK(other.m_shape.get() != state.m_shape.get());
}

TEST_CASE("State::mutate:返回旧状态、自身 score 重置")
{
    auto shape = std::make_shared<geometrize::Circle>(10.0f, 10.0f, 5.0f);
    shape->setup = [](geometrize::Shape&) {};
    shape->mutate = [](geometrize::Shape& s) {
        auto& circle = static_cast<geometrize::Circle&>(s);
        circle.m_x += 1.0f;
    };
    shape->rasterize = [](const geometrize::Shape&) { return std::vector<geometrize::Scanline>{}; };

    geometrize::State state{shape, 64};
    state.m_score = 0.25;

    const geometrize::State oldState = state.mutate();

    // 自身:变异生效、分数作废
    const auto* mutated = static_cast<const geometrize::Circle*>(state.m_shape.get());
    CHECK(mutated->m_x == doctest::Approx(11.0));
    CHECK(state.m_score == doctest::Approx(-1.0));

    // 返回的旧状态:原坐标、原分数
    const auto* oldCircle = static_cast<const geometrize::Circle*>(oldState.m_shape.get());
    REQUIRE(oldCircle != nullptr);
    CHECK(oldCircle->m_x == doctest::Approx(10.0));
    CHECK(oldState.m_score == doctest::Approx(0.25));

    // 旧状态是独立克隆:再变异自身不影响旧状态
    state.mutate();
    const auto* oldAgain = static_cast<const geometrize::Circle*>(oldState.m_shape.get());
    CHECK(oldAgain->m_x == doctest::Approx(10.0));
}
