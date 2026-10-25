#include "image/imageconvert.h"

#include <cassert>
#include <vector>

#include <QImage>
#include <QPixmap>

#include "geometrize/bitmap/bitmap.h"

namespace geometrize
{

namespace image
{

Bitmap createBitmap(const QImage& image)
{
    assert(!image.isNull() && "Image is null, will fail to create bitmap data");
    assert((image.width() != 0 && image.height() != 0) && "Image has zero width or height");
    assert((image.format() == QImage::Format_RGBA8888) && "Cannot create bitmap data from a non-RGBA8888 image");

    std::vector<uchar> data(image.bits(), image.bits() + image.sizeInBytes());
    // move 语义直接把数据搬进 Bitmap,消掉上游按 const 引用传入后的二次深拷
    return Bitmap(image.width(), image.height(), std::move(data));
}

QImage createImage(const Bitmap& data)
{
    if(data.getWidth() == 0 || data.getHeight() == 0) {
        assert(0 && "Bad bitmap data");
        return QImage();
    }

    // Note! This takes a shallow copy of the data, and so depends on the bitmap itself continuing to live on
    return QImage(data.getDataRef().data(), static_cast<qsizetype>(data.getWidth()), static_cast<qsizetype>(data.getHeight()), QImage::Format_RGBA8888);
}

QPixmap createPixmap(const Bitmap& data)
{
    return QPixmap::fromImage(createImage(data));
}

}

}
