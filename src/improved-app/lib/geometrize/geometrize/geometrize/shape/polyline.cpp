#include "polyline.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "shape.h"

namespace geometrize
{

Polyline::Polyline(const std::vector<std::pair<float, float>>& points) : Shape()
{
    m_points = points;
}

std::shared_ptr<geometrize::Shape> Polyline::clone() const
{
    std::shared_ptr<geometrize::Polyline> polyline{std::make_shared<geometrize::Polyline>()};
    polyline->m_points = m_points;
    polyline->setup = setup;
    polyline->mutate = mutate;
    polyline->mutateScaled = mutateScaled;
    polyline->rasterize = rasterize;
    // 拷贝而非重建:调用方可能只设 rasterizeInto(或绑了与 rasterize 不同的实现),无条件重建会丢弃它,
    // 并在 rasterize 为空时造出"非空但调用即抛 bad_function_call"的假可用句柄
    polyline->rasterizeInto = rasterizeInto;
    if(!polyline->rasterizeInto && rasterize) {
        polyline->rasterizeInto = [rf = rasterize](const geometrize::Shape& s, std::vector<geometrize::Scanline>& out) { out = rf(s); };
    }
    return polyline;
}

geometrize::ShapeTypes Polyline::getType() const
{
    return geometrize::ShapeTypes::POLYLINE;
}

}
