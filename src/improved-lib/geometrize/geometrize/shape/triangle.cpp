#include "triangle.h"

#include <cstdint>
#include <memory>

#include "shape.h"

namespace geometrize
{

Triangle::Triangle(const float x1, const float y1, const float x2, const float y2, const float x3, const float y3) : Shape()
{
    m_x1 = x1;
    m_y1 = y1;
    m_x2 = x2;
    m_y2 = y2;
    m_x3 = x3;
    m_y3 = y3;
}

std::shared_ptr<geometrize::Shape> Triangle::clone() const
{
    std::shared_ptr<geometrize::Triangle> triangle{std::make_shared<geometrize::Triangle>()};
    triangle->m_x1 = m_x1;
    triangle->m_y1 = m_y1;
    triangle->m_x2 = m_x2;
    triangle->m_y2 = m_y2;
    triangle->m_x3 = m_x3;
    triangle->m_y3 = m_y3;
    triangle->setup = setup;
    triangle->mutate = mutate;
    triangle->mutateScaled = mutateScaled;
    triangle->rasterize = rasterize;
    // 拷贝而非重建:调用方可能只设 rasterizeInto(或绑了与 rasterize 不同的实现),无条件重建会丢弃它,
    // 并在 rasterize 为空时造出"非空但调用即抛 bad_function_call"的假可用句柄
    triangle->rasterizeInto = rasterizeInto;
    if(!triangle->rasterizeInto && rasterize) {
        triangle->rasterizeInto = [rf = rasterize](const geometrize::Shape& s, std::vector<geometrize::Scanline>& out) { out = rf(s); };
    }
    return triangle;
}

geometrize::ShapeTypes Triangle::getType() const
{
    return ShapeTypes::TRIANGLE;
}

}
