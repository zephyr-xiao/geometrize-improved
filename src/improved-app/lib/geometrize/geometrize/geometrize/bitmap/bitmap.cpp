#include "bitmap.h"

#include <cassert>

#include "rgba.h"

namespace geometrize
{

// 访问器实现已全部移至 bitmap.h 内联定义,此处仅保留构造函数

Bitmap::Bitmap(const std::uint32_t width, const std::uint32_t height, const geometrize::rgba color) : m_width{width}, m_height{height}, m_data(static_cast<std::size_t>(width) * height * 4U)
{
    fill(color);
}

Bitmap::Bitmap(const std::uint32_t width, const std::uint32_t height, const std::vector<std::uint8_t>& data) : m_width{width}, m_height{height}, m_data{data}
{
    assert((static_cast<std::size_t>(width) * height * 4U) == data.size());
}

geometrize::Bitmap downsampleHalf(const geometrize::Bitmap& image)
{
    const std::uint32_t halfWidth{(image.getWidth() + 1U) / 2U};
    const std::uint32_t halfHeight{(image.getHeight() + 1U) / 2U};
    const std::vector<std::uint8_t>& src{image.getDataRef()};
    const std::uint32_t srcWidth{image.getWidth()};

    std::vector<std::uint8_t> dst(static_cast<std::size_t>(halfWidth) * halfHeight * 4U);

    // 2×2 box 求和取整:(sum + count/2) / count,count 由源图奇偶决定,
    // 全程整数——金字塔轨道不新增浮点参与点,保证同输入逐位可复现
    for(std::uint32_t y = 0; y < halfHeight; y++) {
        // 该输出行覆盖的源行数:源行 y*2+1 存在则成对,否则(奇数尾行)单行
        const std::uint32_t rowPairs{(y * 2U + 1U < image.getHeight()) ? 2U : 1U};
        for(std::uint32_t x = 0; x < halfWidth; x++) {
            // 该输出像素覆盖的源列数:源列 x*2+1 存在则成对,否则(奇数尾列)单列
            const std::uint32_t colPairs{(x * 2U + 1U < image.getWidth()) ? 2U : 1U};
            const std::size_t dstIndex{(static_cast<std::size_t>(halfWidth) * y + x) * 4U};
            for(std::uint32_t c = 0; c < 4U; c++) {
                std::uint32_t sum{0};
                for(std::uint32_t dy = 0; dy < rowPairs; dy++) {
                    const std::size_t srcIndex{(static_cast<std::size_t>(srcWidth) * (y * 2U + dy) + x * 2U) * 4U + c};
                    sum += src[srcIndex];
                    if(colPairs == 2U) {
                        sum += src[srcIndex + 4U];
                    }
                }
                const std::uint32_t count{rowPairs * colPairs};
                dst[dstIndex + c] = static_cast<std::uint8_t>((sum + count / 2U) / count);
            }
        }
    }

    return geometrize::Bitmap(halfWidth, halfHeight, std::move(dst));
}

}
