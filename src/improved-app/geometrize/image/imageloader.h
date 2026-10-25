#pragma once

#include <string>

// 纯转换函数(createBitmap/createImage/createPixmap)已拆至 image/imageconvert.h
// (headless 工具只需要转换而不拖偏好依赖);此处转发 include 保旧调用方兼容
#include "image/imageconvert.h"

class QImage;

namespace geometrize
{
class Bitmap;
}

namespace geometrize
{

namespace image
{

/**
 * @brief imageToBitmapWithDownscaling Creates a bitmap data object from the given image, downscaling in the process based on global preferences for image resizing
 * @param image The image to create the bitmap data from.
 * @return The new bitmap data.
 */
geometrize::Bitmap convertImageToBitmapWithDownscaling(const QImage& image);

/**
 * @brief loadImage Loads an image from the image at the file path. Converts to RGBA8888 format.
 * @param filePath The file path to the image.
 * @return The image loaded from the image file.
 */
QImage loadImage(const std::string& filePath);

/**
 * @brief convertImageToRgba8888 Returns a copy of the image in the RGBA8888 format.
 * @param image The image to convert.
 * @return A copy of the given image in RGBA8888 format.
 */
QImage convertImageToRgba8888(const QImage& image);

}

}
