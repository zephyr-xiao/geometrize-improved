#include "shapecollection.h"

#include <cassert>
#include <vector>

#include "geometrize/shaperesult.h"

namespace geometrize
{

namespace task
{

bool ShapeCollection::empty() const
{
    return m_shapes.empty();
}

std::size_t ShapeCollection::size() const
{
    return m_shapes.size();
}

void ShapeCollection::clear()
{
    m_shapes.clear();
    emit signal_sizeChanged(m_shapes.size());
}

const std::vector<geometrize::ShapeResult>& ShapeCollection::getShapeVector() const
{
    return m_shapes;
}

void ShapeCollection::appendShapes(const std::vector<geometrize::ShapeResult>& shapes)
{
    emit signal_beforeAppendShapes(shapes);
    std::copy(shapes.begin(), shapes.end(), std::back_inserter(m_shapes));
    emit signal_sizeChanged(m_shapes.size());
    emit signal_afterAppendShapes(shapes);
}

geometrize::ShapeResult& ShapeCollection::back()
{
    return m_shapes.back();
}

void ShapeCollection::truncate(const std::size_t newSize)
{
    assert(newSize <= m_shapes.size() && "Cannot truncate to more shapes than the collection holds");
    if(newSize >= m_shapes.size()) {
        return;
    }
    // ShapeResult 成员 const、赋值被删除:区间构造(仅拷贝构造)+ swap,全程无元素赋值
    std::vector<geometrize::ShapeResult> kept(m_shapes.begin(), m_shapes.begin() + static_cast<std::ptrdiff_t>(newSize));
    m_shapes.swap(kept);
    emit signal_sizeChanged(m_shapes.size());
}

}
}
