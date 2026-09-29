#include "line.h"

#include <cstdint>
#include <memory>

#include "shape.h"

namespace geometrize
{

Line::Line(const float x1, const float y1, const float x2, const float y2) : Shape()
{
    m_x1 = x1;
    m_y1 = y1;
    m_x2 = x2;
    m_y2 = y2;
}

std::shared_ptr<geometrize::Shape> Line::clone() const
{
    std::shared_ptr<geometrize::Line> line{std::make_shared<geometrize::Line>()};
    line->m_x1 = m_x1;
    line->m_y1 = m_y1;
    line->m_x2 = m_x2;
    line->m_y2 = m_y2;
    line->setup = setup;
    line->mutate = mutate;
    line->mutateScaled = mutateScaled;
    line->rasterize = rasterize;
    // 拷贝而非重建:调用方可能只设 rasterizeInto(或绑了与 rasterize 不同的实现),无条件重建会丢弃它,
    // 并在 rasterize 为空时造出"非空但调用即抛 bad_function_call"的假可用句柄
    line->rasterizeInto = rasterizeInto;
    if(!line->rasterizeInto && rasterize) {
        line->rasterizeInto = [rf = rasterize](const geometrize::Shape& s, std::vector<geometrize::Scanline>& out) { out = rf(s); };
    }
    return line;
}

geometrize::ShapeTypes Line::getType() const
{
    return geometrize::ShapeTypes::LINE;
}

}
