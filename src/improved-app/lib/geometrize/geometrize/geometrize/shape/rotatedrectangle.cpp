#include "rotatedrectangle.h"

#include <cstdint>
#include <memory>

#include "shape.h"

namespace geometrize
{

RotatedRectangle::RotatedRectangle(const float x1, const float y1, const float x2, const float y2, const float angle) : Shape()
{
    m_x1 = x1;
    m_y1 = y1;
    m_x2 = x2;
    m_y2 = y2;
    m_angle = angle;
}

std::shared_ptr<geometrize::Shape> RotatedRectangle::clone() const
{
    std::shared_ptr<geometrize::RotatedRectangle> rect{std::make_shared<geometrize::RotatedRectangle>()};
    rect->m_x1 = m_x1;
    rect->m_y1 = m_y1;
    rect->m_x2 = m_x2;
    rect->m_y2 = m_y2;
    rect->m_angle = m_angle;
    rect->setup = setup;
    rect->mutate = mutate;
    rect->mutateScaled = mutateScaled;
    rect->rasterize = rasterize;
    // 拷贝而非重建:调用方可能只设 rasterizeInto(或绑了与 rasterize 不同的实现),无条件重建会丢弃它,
    // 并在 rasterize 为空时造出"非空但调用即抛 bad_function_call"的假可用句柄
    rect->rasterizeInto = rasterizeInto;
    if(!rect->rasterizeInto && rasterize) {
        rect->rasterizeInto = [rf = rasterize](const geometrize::Shape& s, std::vector<geometrize::Scanline>& out) { out = rf(s); };
    }
    return rect;
}

geometrize::ShapeTypes RotatedRectangle::getType() const
{
    return geometrize::ShapeTypes::ROTATED_RECTANGLE;
}

}
