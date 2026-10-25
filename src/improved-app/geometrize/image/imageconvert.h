#pragma once

// QImage ↔ Bitmap 的纯转换函数(无偏好/无 IO 依赖,headless 工具与 GUI 共用)。
// 从 imageloader 拆出(第二十三批):gifexporter 等 headless 消费者只需要这几个转换,
// 而 imageloader.cpp 的缩放入口依赖 GlobalPreferences,整编会把偏好/工具链拖进 CLI 链接。

class QImage;
class QPixmap;

namespace geometrize
{
class Bitmap;
}

namespace geometrize
{

namespace image
{

/**
 * @brief createBitmap Creates a bitmap data object from the given image.
 * @param image The image to create the bitmap data from (must be RGBA8888).
 * @return The new bitmap data.
 */
Bitmap createBitmap(const QImage& image);

/**
 * @brief createImage Creates an image from the bitmap data object. Assumes RGBA8888 format.
 * @param data The bitmap data, RGBA8888 bytes (must be a multiple of 4).
 * @return The image created from the bytes data (shallow copy - the bitmap must outlive it).
 */
QImage createImage(const Bitmap& data);

/**
 * @brief createPixmap Creates a pixmap from the bitmap data object. Assumes RGBA8888 format.
 * @param data The bitmap data, RGBA8888 bytes (must be a multiple of 4).
 * @return The pixmap created from the bytes data.
 */
QPixmap createPixmap(const Bitmap& data);

}

}
