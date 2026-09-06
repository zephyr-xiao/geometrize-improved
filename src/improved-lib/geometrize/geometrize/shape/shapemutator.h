#pragma once

#include <cstdint>

namespace geometrize
{
class Circle;
class Ellipse;
class Line;
class Polyline;
class QuadraticBezier;
class Rectangle;
class RotatedEllipse;
class RotatedRectangle;
class Shape;
class Triangle;
}

namespace geometrize
{

// Default implementations that perform initial setup on each type of shape
void setup(geometrize::Shape& s, std::int32_t xMin, std::int32_t yMin, std::int32_t xMax, std::int32_t yMax);
void setup(geometrize::Circle& s, std::int32_t xMin, std::int32_t yMin, std::int32_t xMax, std::int32_t yMax);
void setup(geometrize::Ellipse& s, std::int32_t xMin, std::int32_t yMin, std::int32_t xMax, std::int32_t yMax);
void setup(geometrize::Line& s, std::int32_t xMin, std::int32_t yMin, std::int32_t xMax, std::int32_t yMax);
void setup(geometrize::Polyline& s, std::int32_t xMin, std::int32_t yMin, std::int32_t xMax, std::int32_t yMax);
void setup(geometrize::QuadraticBezier& s, std::int32_t xMin, std::int32_t yMin, std::int32_t xMax, std::int32_t yMax);
void setup(geometrize::Rectangle& s, std::int32_t xMin, std::int32_t yMin, std::int32_t xMax, std::int32_t yMax);
void setup(geometrize::RotatedEllipse& s, std::int32_t xMin, std::int32_t yMin, std::int32_t xMax, std::int32_t yMax);
void setup(geometrize::RotatedRectangle& s, std::int32_t xMin, std::int32_t yMin, std::int32_t xMax, std::int32_t yMax);
void setup(geometrize::Triangle& s, std::int32_t xMin, std::int32_t yMin, std::int32_t xMax, std::int32_t yMax);

// Default implementations that mutate each type of shape.
// stepShift 为步长移位数(0=基准,1=1/2,2=1/4...):随机偏移区间按 2^-stepShift 缩小。
// 默认参数保证现存调用方(Shape::mutate 绑定、测试)零改动,取值分布与原实现逐字等价。
void mutate(geometrize::Shape& s, std::int32_t xMin, std::int32_t yMin, std::int32_t xMax, std::int32_t yMax, std::int32_t stepShift = 0);
void mutate(geometrize::Circle& s, std::int32_t xMin, std::int32_t yMin, std::int32_t xMax, std::int32_t yMax, std::int32_t stepShift = 0);
void mutate(geometrize::Ellipse& s, std::int32_t xMin, std::int32_t yMin, std::int32_t xMax, std::int32_t yMax, std::int32_t stepShift = 0);
void mutate(geometrize::Line& s, std::int32_t xMin, std::int32_t yMin, std::int32_t xMax, std::int32_t yMax, std::int32_t stepShift = 0);
void mutate(geometrize::Polyline& s, std::int32_t xMin, std::int32_t yMin, std::int32_t xMax, std::int32_t yMax, std::int32_t stepShift = 0);
void mutate(geometrize::QuadraticBezier& s, std::int32_t xMin, std::int32_t yMin, std::int32_t xMax, std::int32_t yMax, std::int32_t stepShift = 0);
void mutate(geometrize::Rectangle& s, std::int32_t xMin, std::int32_t yMin, std::int32_t xMax, std::int32_t yMax, std::int32_t stepShift = 0);
void mutate(geometrize::RotatedEllipse& s, std::int32_t xMin, std::int32_t yMin, std::int32_t xMax, std::int32_t yMax, std::int32_t stepShift = 0);
void mutate(geometrize::RotatedRectangle& s, std::int32_t xMin, std::int32_t yMin, std::int32_t xMax, std::int32_t yMax, std::int32_t stepShift = 0);
void mutate(geometrize::Triangle& s, std::int32_t xMin, std::int32_t yMin, std::int32_t xMax, std::int32_t yMax, std::int32_t stepShift = 0);

// Default implementations that translate each type of shape
void translate(geometrize::Shape& s, float x, float y);
void translate(geometrize::Circle& s, float x, float y);
void translate(geometrize::Ellipse& s, float x, float y);
void translate(geometrize::Line& s, float x, float y);
void translate(geometrize::Polyline& s, float x, float y);
void translate(geometrize::QuadraticBezier& s, float x, float y);
void translate(geometrize::Rectangle& s, float x, float y);
void translate(geometrize::RotatedEllipse& s, float x, float y);
void translate(geometrize::RotatedRectangle& s, float x, float y);
void translate(geometrize::Triangle& s, float x, float y);

/**
 * @brief shiftShapeCenter 平移形状使其自然中心落于 (centerX, centerY)。
 * 中心定义按类型:圆/椭圆类=(m_x,m_y);矩形类=两角中点;三角=质心;线=端点中点;
 * polyline=点集均值;bezier=三控制点均值。空点集/未知类型不平移。
 * 出界不做 clamp:rasterize/trim 兜底,后续 mutate 自会收敛。
 * 误差图引导轨道专用(setup 后整体搬位),零 RNG 消耗。
 */
void shiftShapeCenter(geometrize::Shape& s, float centerX, float centerY);

// Default implementations that scale each type of shape
void scale(geometrize::Shape& s, float scaleFactor);
void scale(geometrize::Circle& s, float scaleFactor);
void scale(geometrize::Ellipse& s, float scaleFactor);
void scale(geometrize::Line& s, float scaleFactor);
void scale(geometrize::Polyline& s, float scaleFactor);
void scale(geometrize::QuadraticBezier& s, float scaleFactor);
void scale(geometrize::Rectangle& s, float scaleFactor);
void scale(geometrize::RotatedEllipse& s, float scaleFactor);
void scale(geometrize::RotatedRectangle& s, float scaleFactor);
void scale(geometrize::Triangle& s, float scaleFactor);

// Default implementations that rotate each type of shape through an angle (those which support rotation anyway)
void rotate(geometrize::Shape& s, float angle);
void rotate(geometrize::Line& s, float angle);
void rotate(geometrize::RotatedEllipse& s, float angle);
void rotate(geometrize::RotatedRectangle& s, float angle);
void rotate(geometrize::Triangle& s, float angle);

}
