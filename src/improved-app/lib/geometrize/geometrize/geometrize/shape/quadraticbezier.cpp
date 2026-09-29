#include "quadraticbezier.h"

#include <cstdint>
#include <memory>
#include "shape.h"

namespace geometrize
{

QuadraticBezier::QuadraticBezier(const float cx, const float cy, const float x1, const float y1, const float x2, const float y2) : Shape()
{
    m_cx = cx;
    m_cy = cy;
    m_x1 = x1;
    m_y1 = y1;
    m_x2 = x2;
    m_y2 = y2;
}

std::shared_ptr<geometrize::Shape> QuadraticBezier::clone() const
{
    std::shared_ptr<geometrize::QuadraticBezier> bezier{std::make_shared<geometrize::QuadraticBezier>()};
    bezier->m_x1 = m_x1;
    bezier->m_y1 = m_y1;
    bezier->m_cx = m_cx;
    bezier->m_cy = m_cy;
    bezier->m_x2 = m_x2;
    bezier->m_y2 = m_y2;
    bezier->setup = setup;
    bezier->mutate = mutate;
    bezier->mutateScaled = mutateScaled;
    bezier->rasterize = rasterize;
    // 拷贝而非重建:调用方可能只设 rasterizeInto(或绑了与 rasterize 不同的实现),无条件重建会丢弃它,
    // 并在 rasterize 为空时造出"非空但调用即抛 bad_function_call"的假可用句柄
    bezier->rasterizeInto = rasterizeInto;
    if(!bezier->rasterizeInto && rasterize) {
        bezier->rasterizeInto = [rf = rasterize](const geometrize::Shape& s, std::vector<geometrize::Scanline>& out) { out = rf(s); };
    }
    return bezier;
}

geometrize::ShapeTypes QuadraticBezier::getType() const
{
    return geometrize::ShapeTypes::QUADRATIC_BEZIER;
}

}
