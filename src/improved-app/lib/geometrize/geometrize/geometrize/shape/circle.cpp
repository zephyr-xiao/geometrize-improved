#include "circle.h"

#include <cstdint>
#include <memory>

#include "shape.h"

namespace geometrize
{

Circle::Circle(const float x, const float y, const float r) : Shape()
{
    m_x = x;
    m_y = y;
    m_r = r;
}

std::shared_ptr<geometrize::Shape> Circle::clone() const
{
    std::shared_ptr<geometrize::Circle> circle{std::make_shared<geometrize::Circle>()};
    circle->m_x = m_x;
    circle->m_y = m_y;
    circle->m_r = m_r;
    circle->setup = setup;
    circle->mutate = mutate;
    circle->mutateScaled = mutateScaled;
    circle->rasterize = rasterize;
    // 拷贝而非重建:调用方可能只设 rasterizeInto(或绑了与 rasterize 不同的实现),无条件重建会丢弃它,
    // 并在 rasterize 为空时造出"非空但调用即抛 bad_function_call"的假可用句柄
    circle->rasterizeInto = rasterizeInto;
    if(!circle->rasterizeInto && rasterize) {
        circle->rasterizeInto = [rf = rasterize](const geometrize::Shape& s, std::vector<geometrize::Scanline>& out) { out = rf(s); };
    }
    return circle;
}

geometrize::ShapeTypes Circle::getType() const
{
    return geometrize::ShapeTypes::CIRCLE;
}

}
