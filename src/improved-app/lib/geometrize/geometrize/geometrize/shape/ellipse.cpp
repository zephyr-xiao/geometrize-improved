#include "ellipse.h"

#include <cstdint>
#include <memory>

#include "shape.h"

namespace geometrize
{

Ellipse::Ellipse(const float x, const float y, const float rx, const float ry) : Shape()
{
    m_x = x;
    m_y = y;
    m_rx = rx;
    m_ry = ry;
}

std::shared_ptr<geometrize::Shape> Ellipse::clone() const
{
    std::shared_ptr<geometrize::Ellipse> ellipse{std::make_shared<geometrize::Ellipse>()};
    ellipse->m_x = m_x;
    ellipse->m_y = m_y;
    ellipse->m_rx = m_rx;
    ellipse->m_ry = m_ry;
    ellipse->setup = setup;
    ellipse->mutate = mutate;
    ellipse->mutateScaled = mutateScaled;
    ellipse->rasterize = rasterize;
    // 拷贝而非重建:调用方可能只设 rasterizeInto(或绑了与 rasterize 不同的实现),无条件重建会丢弃它,
    // 并在 rasterize 为空时造出"非空但调用即抛 bad_function_call"的假可用句柄
    ellipse->rasterizeInto = rasterizeInto;
    if(!ellipse->rasterizeInto && rasterize) {
        ellipse->rasterizeInto = [rf = rasterize](const geometrize::Shape& s, std::vector<geometrize::Scanline>& out) { out = rf(s); };
    }
    return ellipse;
}

geometrize::ShapeTypes Ellipse::getType() const
{
    return geometrize::ShapeTypes::ELLIPSE;
}

}
