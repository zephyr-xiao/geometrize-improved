#include "image/imageloader.h"

#include <cassert>

#include <QFile>
#include <QImage>
#include <QPixmap>
#include <QString>
#include <QUrl>

#include "geometrize/bitmap/bitmap.h"
#include "geometrize/core.h"

#include "preferences/globalpreferences.h"

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

geometrize::Bitmap convertImageToBitmapWithDownscaling(const QImage& image)
{
    const geometrize::preferences::GlobalPreferences& prefs{geometrize::preferences::getGlobalPreferences()};
    if(prefs.isImageTaskImageResizeEnabled()) {
        const std::pair<std::uint32_t, std::uint32_t> sizeThreshold{prefs.getImageTaskResizeThreshold()};

        if(sizeThreshold.first < static_cast<unsigned int>(image.width())
                || sizeThreshold.second < static_cast<unsigned int>(image.height())) {
            // 仅在确实需要缩放时进入拷贝/转换链,原尺寸路径零多余拷贝
            QImage scaled = image.scaled(sizeThreshold.first, sizeThreshold.second, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            return geometrize::image::createBitmap(scaled.convertToFormat(QImage::Format_RGBA8888));
        }
    }
    // 原尺寸路径同样强制 RGBA8888:convertToFormat 对已是目标格式的输入只做浅共享拷贝,
    // 近零成本;不做转换时 createBitmap 的格式断言在 Release 失效,非 RGBA8888 输入
    // (如脚本绑定直接传入)会按原始字节 memcpy,通道错乱/预乘损坏
    return geometrize::image::createBitmap(image.convertToFormat(QImage::Format_RGBA8888));
}
QImage createImage(const Bitmap& data)
{
    if(data.getWidth() == 0 || data.getHeight() == 0) {
        assert(0 && "Bad bitmap data");
        return QImage();
    }

    // Note! This takes a shallow copy of the data, and so depends on the bitmap itself continuing to live on
    return QImage(data.getDataRef().data(), data.getWidth(), data.getHeight(), QImage::Format_RGBA8888);
}

QPixmap createPixmap(const Bitmap& data)
{
    return QPixmap::fromImage(createImage(data));
}

QImage loadImage(const std::string& filePath)
{
    const QString path = QString::fromStdString(filePath);
    QImage image;
    if(QFile(path).exists()) {
        image = QImage(path);
    } else {
        image = QImage(QUrl(path).toLocalFile());
    }

    if(image.isNull()) {
        assert(0 && "Bad image data");
        return image;
    }

    return image.convertToFormat(QImage::Format_RGBA8888);
}

QImage convertImageToRgba8888(const QImage& image)
{
    return image.convertToFormat(QImage::Format_RGBA8888);
}

}

}
