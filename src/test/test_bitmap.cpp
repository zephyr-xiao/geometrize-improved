// 位图基础:索引/读写/fill/copyData/move 语义
#include "doctest.h"

#include "geometrize/bitmap/bitmap.h"

TEST_CASE("bitmap 基础读写往返")
{
    geometrize::Bitmap bitmap{4, 3, geometrize::rgba{1, 2, 3, 4}};

    CHECK(bitmap.getWidth() == 4);
    CHECK(bitmap.getHeight() == 3);
    CHECK(bitmap.getDataRef().size() == 4 * 3 * 4U);

    // 全图 fill 后四通道逐角往返
    bitmap.fill(geometrize::rgba{10, 20, 30, 40});
    for(std::uint32_t y = 0; y < 3; y++) {
        for(std::uint32_t x = 0; x < 4; x++) {
            const geometrize::rgba pixel = bitmap.getPixel(x, y);
            CHECK(pixel.r == 10);
            CHECK(pixel.g == 20);
            CHECK(pixel.b == 30);
            CHECK(pixel.a == 40);
        }
    }

    // setPixel 单点写入不串扰邻点
    bitmap.setPixel(2, 1, geometrize::rgba{255, 128, 0, 7});
    const geometrize::rgba changed = bitmap.getPixel(2, 1);
    CHECK(changed.r == 255);
    CHECK(changed.g == 128);
    CHECK(changed.b == 0);
    CHECK(changed.a == 7);
    const geometrize::rgba neighbor = bitmap.getPixel(1, 1);
    CHECK(neighbor.r == 10);
}

TEST_CASE("bitmap copyData 深拷贝独立")
{
    geometrize::Bitmap bitmap{2, 2, geometrize::rgba{1, 2, 3, 4}};
    std::vector<std::uint8_t> data = bitmap.copyData();
    REQUIRE(data.size() == 16U);

    // 修改副本不影响源位图
    data[0] = 99;
    CHECK(bitmap.getPixel(0, 0).r == 1);
}

TEST_CASE("bitmap move 构造接管数据(免深拷)")
{
    std::vector<std::uint8_t> data(2 * 2 * 4U, 0);
    data[0] = 7;
    const std::uint8_t* originalData = data.data();

    geometrize::Bitmap bitmap{2, 2, std::move(data)};

#if defined(GEOTEST_FAST)
    // move 后底层缓冲指针不变 = 零拷贝接管(B1 改进版才有的 move 构造语义)
    CHECK(bitmap.getDataRef().data() == originalData);
#else
    // baseline 无 move 构造,退化为 const 引用版深拷,只验证值正确性
    CHECK(bitmap.getDataRef().data() != originalData);
#endif
    CHECK(bitmap.getPixel(0, 0).r == 7);
}

// downsampleHalf 只存在于改进版(金字塔轨道函数);baseline 变体无此符号,
// 用宏分流保证双变体编译都能过(与 move 构造分叉同款处理)
#if defined(GEOTEST_FAST)
TEST_CASE("downsampleHalf 2×2 box 取整与奇数尺寸")
{
    // 单通道可辨识图:r=x*2,g=y*2,b=7 —— 验证四通道各自正确聚合
    const std::uint32_t w{5}, h{5};
    std::vector<std::uint8_t> data(w * h * 4U);
    for(std::uint32_t y = 0; y < h; y++) {
        for(std::uint32_t x = 0; x < w; x++) {
            const std::size_t offset{(static_cast<std::size_t>(w) * y + x) * 4U};
            data[offset] = static_cast<std::uint8_t>(x * 2U);
            data[offset + 1U] = static_cast<std::uint8_t>(y * 2U);
            data[offset + 2U] = 7U;
            data[offset + 3U] = 255U;
        }
    }
    const geometrize::Bitmap source{w, h, data};

    const geometrize::Bitmap half = geometrize::downsampleHalf(source);
    CHECK(half.getWidth() == 3);  // ceil(5/2)
    CHECK(half.getHeight() == 3); // ceil(5/2)

    // 偶数对 (0,1):r 值 {0,2} → (0+2+1)/2 = 1(四舍五入)
    const geometrize::rgba p00 = half.getPixel(0, 0);
    CHECK(p00.r == 1);
    CHECK(p00.g == 1);
    CHECK(p00.b == 7);
    CHECK(p00.a == 255);

    // 奇数尾列 x=4(x 无配对):{8} → 8
    const geometrize::rgba p20 = half.getPixel(2, 0);
    CHECK(p20.r == 8);
    CHECK(p20.g == 1);

    // 奇数尾行 y=4(y 无配对)与奇数尾角 (4,4):单像素原值直传
    const geometrize::rgba p22 = half.getPixel(2, 2);
    CHECK(p22.r == 8);
    CHECK(p22.g == 8);
}

TEST_CASE("downsampleHalf 确定性:同输入两跑逐位一致")
{
    std::vector<std::uint8_t> data(8 * 8 * 4U);
    for(std::size_t i = 0; i < data.size(); i++) {
        data[i] = static_cast<std::uint8_t>(i * 31U + 7U);
    }
    const geometrize::Bitmap source{8, 8, data};

    const auto first = geometrize::downsampleHalf(source).copyData();
    const auto second = geometrize::downsampleHalf(source).copyData();
    REQUIRE(first.size() == second.size());
    CHECK(first == second);
}

TEST_CASE("downsampleHalf 1×1 退化输入")
{
    const geometrize::Bitmap single{1, 1, geometrize::rgba{9, 8, 7, 6}};
    const geometrize::Bitmap half = geometrize::downsampleHalf(single);
    CHECK(half.getWidth() == 1);
    CHECK(half.getHeight() == 1);
    const geometrize::rgba pixel = half.getPixel(0, 0);
    CHECK(pixel.r == 9);
    CHECK(pixel.g == 8);
    CHECK(pixel.b == 7);
    CHECK(pixel.a == 6);
}
#endif

