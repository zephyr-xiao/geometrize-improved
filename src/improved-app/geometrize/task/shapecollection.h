#pragma once

#include <vector>

#include <QObject>

#include "geometrize/shaperesult.h"

namespace geometrize
{

namespace task
{

/**
 * @brief The ShapeCollection class is a wrapper for the vector of shapes that have been added to the image task so far.
 */
class ShapeCollection : public QObject
{
    Q_OBJECT
public:
    ShapeCollection() = default;
    ShapeCollection& operator=(const ShapeCollection&) = delete;
    ShapeCollection(const ShapeCollection&) = delete;
    ~ShapeCollection() = default;

    bool empty() const;

    std::size_t size() const;

    void clear();

    const std::vector<geometrize::ShapeResult>& getShapeVector() const;

    void appendShapes(const std::vector<geometrize::ShapeResult>& shapes);

    geometrize::ShapeResult& back();

    /**
     * @brief truncate Removes shapes from the end of the collection, keeping the first newSize results.
     * Used for undo: shape results are immutable (const members), so the list is rebuilt as a prefix
     * rather than edited in place.
     * @param newSize The number of shapes to keep. Must not exceed the current size.
     */
    void truncate(std::size_t newSize);

signals:
    void signal_beforeAppendShapes(const std::vector<geometrize::ShapeResult>&);
    void signal_sizeChanged(std::size_t to);
    void signal_afterAppendShapes(const std::vector<geometrize::ShapeResult>&);

private:
    std::vector<geometrize::ShapeResult> m_shapes; ///> The shapes and score results created by the image task
};

}
}
