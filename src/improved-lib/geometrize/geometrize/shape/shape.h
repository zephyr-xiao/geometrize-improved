#pragma once

#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>

#include "../rasterizer/scanline.h"
#include "shapetypes.h"

namespace geometrize
{

/**
 * Base class for shape rasterization and manipulation.
 * @author Sam Twidale (https://samcodes.co.uk/)
 */
class Shape
{
public:
    Shape() = default;
    virtual ~Shape() = default;
    Shape& operator=(const geometrize::Shape& other) = default;
    Shape(const geometrize::Shape& other) = default;

    std::function<void(geometrize::Shape&)> setup;
    std::function<void(geometrize::Shape&)> mutate;
    // 可选的带步长变异:stepShift 为 2 的幂次移位数(0=基准步长,1=1/2,2=1/4...)。
    // 自适应步长轨道专用。未设置时 State::mutate(stepShift) 回退普通 mutate(外部自设形状优雅降级)。
    std::function<void(geometrize::Shape&, std::int32_t)> mutateScaled;
    std::function<std::vector<geometrize::Scanline>(const geometrize::Shape&)> rasterize;
    // 可选的复用版光栅化:结果写入调用方向量(热路径消堆分配)。未设置时走 rasterize。
    // 保持独立成员而非改 rasterize 签名,脚本绑定与外部调用方语义零扰动。
    std::function<void(const geometrize::Shape&, std::vector<geometrize::Scanline>&)> rasterizeInto;

    /**
     * @brief clone Clones the shape, a virtual copy constructor.
     * @return A clone of the shape.
     */
    virtual std::shared_ptr<geometrize::Shape> clone() const
    {
        throw std::logic_error("Unimplemented"); // NOTE not pure virtual because it breaks simple upcast in Chaiscript (and can't see how else to do it)
    }

    /**
     * @brief getType Gets the ShapeType of the shape.
     * @return The ShapeType of the shape.
     */
    virtual geometrize::ShapeTypes getType() const
    {
        throw std::logic_error("Unimplemented");
    }
};

}
