// 变异器:步长移位参数(stepShift)语义与 RNG 流不变量(自适应步长轨道 T9-T13)
// mutateScaled 注入链:factory 绑定 → clone 拷贝 → State::mutate(stepShift) 分发
#include "doctest.h"

#include <cstdint>
#include <memory>
#include <vector>

#include "geometrize/commonutil.h"
#include "geometrize/exporter/shapejsonexporter.h"
#include "geometrize/shaperesult.h"
#include "geometrize/shape/circle.h"
#include "geometrize/shape/ellipse.h"
#include "geometrize/shape/line.h"
#include "geometrize/shape/polyline.h"
#include "geometrize/shape/quadraticbezier.h"
#include "geometrize/shape/rectangle.h"
#include "geometrize/shape/rotatedellipse.h"
#include "geometrize/shape/rotatedrectangle.h"
#include "geometrize/shape/shape.h"
#include "geometrize/shape/shapefactory.h"
#include "geometrize/shape/shapemutator.h"
#include "geometrize/shape/shapetypes.h"
#include "geometrize/shape/triangle.h"
#include "geometrize/state.h"

namespace
{

// 宽松 bounds(避免 clamp 吞掉步长语义):覆盖足够大,坐标变动都落在界内
constexpr std::int32_t BOUND_MIN{0};
constexpr std::int32_t BOUND_MAX{1 << 20};

std::shared_ptr<geometrize::Shape> makeShape(geometrize::ShapeTypes type)
{
    // 以 factory 绑定完整注入链(setup/mutate/mutateScaled/rasterize)
    const auto creator = geometrize::createDefaultShapeCreator(type, BOUND_MIN, BOUND_MIN, BOUND_MAX, BOUND_MAX);
    std::shared_ptr<geometrize::Shape> shape = creator();
    shape->setup(*shape);
    return shape;
}

// JSON 串序列化坐标:同一形状类型的全部可变字段(m_x/m_r 等)都进导出串
std::string shapeJson(const geometrize::Shape& shape)
{
    std::vector<geometrize::ShapeResult> results;
    results.emplace_back(geometrize::ShapeResult{0.0, geometrize::rgba{0, 0, 0, 0}, std::shared_ptr<geometrize::Shape>(shape.clone())});
    return geometrize::exporter::exportShapeJson(results);
}

} // namespace

// mutateScaled 是改进版专属(baseline 无此符号),整文件宏分流
#if defined(GEOTEST_FAST)
TEST_CASE("T9 shift=0 与普通 mutate 逐字等价(同种子同序列)")
{
    for(const geometrize::ShapeTypes type : {
            geometrize::ShapeTypes::RECTANGLE, geometrize::ShapeTypes::ROTATED_RECTANGLE,
            geometrize::ShapeTypes::TRIANGLE, geometrize::ShapeTypes::ELLIPSE,
            geometrize::ShapeTypes::ROTATED_ELLIPSE, geometrize::ShapeTypes::CIRCLE,
            geometrize::ShapeTypes::LINE, geometrize::ShapeTypes::QUADRATIC_BEZIER,
            geometrize::ShapeTypes::POLYLINE}) {
        // 平行宇宙式对比:thread_local RNG 是共享的,交替消费会串流,
        // 故两边各自从同一种子起步整段跑完,再对比逐步 JSON 序列
        const auto runSeries = [type](bool useScaled) {
            geometrize::commonutil::seedRandomGenerator(1000 + static_cast<int>(type));
            auto shape = makeShape(type);
            REQUIRE(shape->mutateScaled.operator bool());
            REQUIRE(shape->mutate.operator bool());
            std::vector<std::string> series;
            for(int k = 0; k < 20; k++) {
                if(useScaled) {
                    shape->mutateScaled(*shape, 0);
                } else {
                    shape->mutate(*shape);
                }
                series.push_back(shapeJson(*shape));
            }
            return series;
        };

        const auto scaledSeries = runSeries(true);
        const auto plainSeries = runSeries(false);
        REQUIRE(scaledSeries.size() == plainSeries.size());
        for(std::size_t i = 0; i < scaledSeries.size(); i++) {
            CHECK(scaledSeries[i] == plainSeries[i]);
        }
    }
}

TEST_CASE("T10 shift 确定性:同 seed 两跑序列一致")
{
    const auto runSeries = [](std::int32_t stepShift) {
        geometrize::commonutil::seedRandomGenerator(777);
        auto shape = makeShape(geometrize::ShapeTypes::CIRCLE);
        std::vector<std::string> series;
        for(int k = 0; k < 15; k++) {
            shape->mutateScaled(*shape, stepShift);
            series.push_back(shapeJson(*shape));
        }
        return series;
    };

    const auto first = runSeries(2);
    const auto second = runSeries(2);
    REQUIRE(first.size() == second.size());
    for(std::size_t i = 0; i < first.size(); i++) {
        CHECK(first[i] == second[i]);
    }
}

TEST_CASE("T11 步长上界:shift=1 时单步坐标位移 ≤ 8(宽 bounds 不被 clamp 截断)")
{
    geometrize::commonutil::seedRandomGenerator(2024);
    auto shape = makeShape(geometrize::ShapeTypes::RECTANGLE);
    // BOUND_MAX 极大,clamp 不会介入;Rectangle mutate 每次 ±16>>shift
    float prevX1 = std::static_pointer_cast<geometrize::Rectangle>(shape)->m_x1;
    float prevY1 = std::static_pointer_cast<geometrize::Rectangle>(shape)->m_y1;
    float prevX2 = std::static_pointer_cast<geometrize::Rectangle>(shape)->m_x2;
    float prevY2 = std::static_pointer_cast<geometrize::Rectangle>(shape)->m_y2;
    for(int k = 0; k < 30; k++) {
        shape->mutateScaled(*shape, 1);
        const auto rect = std::static_pointer_cast<geometrize::Rectangle>(shape);
        CHECK(std::abs(rect->m_x1 - prevX1) <= 8.0f);
        CHECK(std::abs(rect->m_y1 - prevY1) <= 8.0f);
        CHECK(std::abs(rect->m_x2 - prevX2) <= 8.0f);
        CHECK(std::abs(rect->m_y2 - prevY2) <= 8.0f);
        prevX1 = rect->m_x1; prevY1 = rect->m_y1; prevX2 = rect->m_x2; prevY2 = rect->m_y2;
    }
}

TEST_CASE("T12 RNG 消费次数不变:shift 只改取值不改流位置")
{
    // shift=0 跑 K 次 mutate 后下一次 randomRange 的值,与 shift=2 跑 K 次后同参数 randomRange 的值
    // 必须相同 —— 步长缩放不得挪动 RNG 流(RNG 流不变量,增强轨道确定性的根基)
    const auto consumeAndProbe = [](std::int32_t stepShift, int iterations) {
        geometrize::commonutil::seedRandomGenerator(31337);
        auto shape = makeShape(geometrize::ShapeTypes::ELLIPSE);
        for(int k = 0; k < iterations; k++) {
            shape->mutateScaled(*shape, stepShift);
        }
        return geometrize::commonutil::randomRange(0, 1000);
    };

    CHECK(consumeAndProbe(0, 10) == consumeAndProbe(2, 10));
    CHECK(consumeAndProbe(0, 37) == consumeAndProbe(3, 37));
}

TEST_CASE("T13 克隆保留 mutateScaled(9 类型 clone 陷阱兜底)")
{
    for(const geometrize::ShapeTypes type : {
            geometrize::ShapeTypes::RECTANGLE, geometrize::ShapeTypes::ROTATED_RECTANGLE,
            geometrize::ShapeTypes::TRIANGLE, geometrize::ShapeTypes::ELLIPSE,
            geometrize::ShapeTypes::ROTATED_ELLIPSE, geometrize::ShapeTypes::CIRCLE,
            geometrize::ShapeTypes::LINE, geometrize::ShapeTypes::QUADRATIC_BEZIER,
            geometrize::ShapeTypes::POLYLINE}) {
        auto shape = makeShape(type);
        auto cloned = shape->clone();
        REQUIRE(cloned.get() != nullptr); // doctest 比较 shared_ptr 用 .get()(陷阱 10)
        // clone 未拷贝 mutateScaled 会导致 State::mutate(stepShift) 静默退化为普通 mutate
        REQUIRE(cloned->mutateScaled.operator bool());
        cloned->mutateScaled(*cloned, 2); // 可执行且不崩溃即通过(坐标界内由 clamp 保证)
    }
}

TEST_CASE("State::mutate(stepShift) 分发与优雅降级")
{
    // 注入链完整:State::mutate(2) 走 mutateScaled
    geometrize::commonutil::seedRandomGenerator(555);
    auto shape = makeShape(geometrize::ShapeTypes::CIRCLE);
    geometrize::State state;
    state.m_alpha = 128;
    state.m_score = -1.0;
    state.m_shape = shape;
    const auto undo = state.mutate(2);
    CHECK(state.m_score == -1.0); // mutate 后分数置脏
    CHECK(undo.m_shape.get() != nullptr);

    // 优雅降级:外部自设形状未绑定 mutateScaled → 回退普通 mutate,不崩溃
    auto bare = std::make_shared<geometrize::Circle>(8.0f, 8.0f, 4.0f);
    bare->setup = [](geometrize::Shape&) {};
    bare->mutate = [](geometrize::Shape& s) { static_cast<geometrize::Circle&>(s).m_x += 1.0f; };
    geometrize::State bareState;
    bareState.m_alpha = 128;
    bareState.m_score = -1.0;
    bareState.m_shape = bare;
    bareState.mutate(3);
    CHECK(std::static_pointer_cast<geometrize::Circle>(bareState.m_shape)->m_x == 9.0f); // 退化为普通 mutate 生效
}

TEST_CASE("shiftShapeCenter:9 类型自然中心落于目标点(零 RNG)")
{
    // 中心语义:圆/椭圆类=圆心;矩形类=两角中点;三角=质心;线=端点中点;
    // polyline=点集均值;bezier=三控制点均值
    auto checkCenter = [](geometrize::Shape& shape, float targetX, float targetY) {
        geometrize::shiftShapeCenter(shape, targetX, targetY);
        switch(shape.getType()) {
        case geometrize::ShapeTypes::CIRCLE: {
            const auto& c = static_cast<const geometrize::Circle&>(shape);
            CHECK(c.m_x == doctest::Approx(targetX));
            CHECK(c.m_y == doctest::Approx(targetY));
            break;
        }
        case geometrize::ShapeTypes::ELLIPSE: {
            const auto& e = static_cast<const geometrize::Ellipse&>(shape);
            CHECK(e.m_x == doctest::Approx(targetX));
            CHECK(e.m_y == doctest::Approx(targetY));
            break;
        }
        case geometrize::ShapeTypes::ROTATED_ELLIPSE: {
            const auto& e = static_cast<const geometrize::RotatedEllipse&>(shape);
            CHECK(e.m_x == doctest::Approx(targetX));
            CHECK(e.m_y == doctest::Approx(targetY));
            break;
        }
        case geometrize::ShapeTypes::RECTANGLE: {
            const auto& r = static_cast<const geometrize::Rectangle&>(shape);
            CHECK((r.m_x1 + r.m_x2) / 2.0F == doctest::Approx(targetX));
            CHECK((r.m_y1 + r.m_y2) / 2.0F == doctest::Approx(targetY));
            break;
        }
        case geometrize::ShapeTypes::ROTATED_RECTANGLE: {
            const auto& r = static_cast<const geometrize::RotatedRectangle&>(shape);
            CHECK((r.m_x1 + r.m_x2) / 2.0F == doctest::Approx(targetX));
            CHECK((r.m_y1 + r.m_y2) / 2.0F == doctest::Approx(targetY));
            break;
        }
        case geometrize::ShapeTypes::TRIANGLE: {
            const auto& t = static_cast<const geometrize::Triangle&>(shape);
            // 除以 3 的 float 舍入误差约 1e-2,epsilon 需覆盖
            CHECK((t.m_x1 + t.m_x2 + t.m_x3) / 3.0F == doctest::Approx(targetX).epsilon(0.001));
            CHECK((t.m_y1 + t.m_y2 + t.m_y3) / 3.0F == doctest::Approx(targetY).epsilon(0.001));
            break;
        }
        case geometrize::ShapeTypes::LINE: {
            const auto& l = static_cast<const geometrize::Line&>(shape);
            CHECK((l.m_x1 + l.m_x2) / 2.0F == doctest::Approx(targetX));
            CHECK((l.m_y1 + l.m_y2) / 2.0F == doctest::Approx(targetY));
            break;
        }
        case geometrize::ShapeTypes::QUADRATIC_BEZIER: {
            const auto& b = static_cast<const geometrize::QuadraticBezier&>(shape);
            CHECK((b.m_x1 + b.m_cx + b.m_x2) / 3.0F == doctest::Approx(targetX).epsilon(0.001));
            CHECK((b.m_y1 + b.m_cy + b.m_y2) / 3.0F == doctest::Approx(targetY).epsilon(0.001));
            break;
        }
        case geometrize::ShapeTypes::POLYLINE: {
            const auto& p = static_cast<const geometrize::Polyline&>(shape);
            float mx{0.0F};
            float my{0.0F};
            for(const auto& point : p.m_points) {
                mx += point.first;
                my += point.second;
            }
            mx /= static_cast<float>(p.m_points.size());
            my /= static_cast<float>(p.m_points.size());
            CHECK(mx == doctest::Approx(targetX));
            CHECK(my == doctest::Approx(targetY));
            break;
        }
        default:
            FAIL("未覆盖的形状类型");
        }
    };

    // RNG 流不变量:shiftShapeCenter 消费零 draw
    for(const geometrize::ShapeTypes type : {
            geometrize::ShapeTypes::RECTANGLE, geometrize::ShapeTypes::ROTATED_RECTANGLE,
            geometrize::ShapeTypes::TRIANGLE, geometrize::ShapeTypes::ELLIPSE,
            geometrize::ShapeTypes::ROTATED_ELLIPSE, geometrize::ShapeTypes::CIRCLE,
            geometrize::ShapeTypes::LINE, geometrize::ShapeTypes::QUADRATIC_BEZIER,
            geometrize::ShapeTypes::POLYLINE}) {
        auto shape = makeShape(type);
        checkCenter(*shape, 512.0F, 384.0F);
    }
}

TEST_CASE("shiftShapeCenter:polyline 空点集不平移不崩溃")
{
    geometrize::Polyline empty;
    geometrize::shiftShapeCenter(empty, 100.0F, 100.0F);
    CHECK(empty.m_points.empty());
}
#endif

