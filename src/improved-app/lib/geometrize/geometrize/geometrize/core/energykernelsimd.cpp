// 融合能量内核的 AVX2 实现(第十八批,位精确)。
// 本 TU 由 CMake 单独按 /arch:AVX2(-mavx2)编译,与库其余部分指令集隔离;
// 所有入口只在 avx2EnergyKernelAvailable()(定义在 core.cpp,不带 AVX2 指令)为真时被调用。
// 等价性依据与扫描线契约见 core.h 的 defaultEnergyFunctionFusedAvx2 说明。
#include "../core.h"

#include <cmath>
#include <cstdint>
#include <vector>

#include "../bitmap/bitmap.h"
#include "../bitmap/rgba.h"
#include "../commonutil.h"
#include "../rasterizer/scanline.h"

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#define GEOMETRIZE_X86_SIMD 1
#include <immintrin.h>
#endif

#ifdef GEOMETRIZE_X86_SIMD

namespace
{

/// 混合公式的无除法标量复刻(尾部像素与向量式同源,返回 floor(V / (65535*256)))。
/// 展开:V = hi*65536 + lo 时 floor(V/65535) = hi + [hi + lo >= 65535];记 W = V >> 8,
/// 由嵌套整除 floor(V/16776960) == floor(W/65535),再代入数字和恒等式得下式。
inline std::int32_t blendChannel(const std::uint32_t d, const std::uint32_t aa, const std::uint32_t sTimesM)
{
    const std::uint32_t v{d * aa + sTimesM};
    const std::uint32_t w{v >> 8U};
    return static_cast<std::int32_t>((w + (w >> 16U) + 1U) >> 16U);
}

/// 256 位向量的 4×64 位车道归约(整数加法精确,分块求和与逐像素求和 mod 2^64 等价)
inline std::uint64_t sumLanes64(const __m256i v)
{
    const __m128i low{_mm256_castsi256_si128(v)};
    const __m128i high{_mm256_extracti128_si256(v, 1)};
    const __m128i sum{_mm_add_epi64(low, high)};
    return static_cast<std::uint64_t>(_mm_cvtsi128_si64(sum)) + static_cast<std::uint64_t>(_mm_extract_epi64(sum, 1));
}

/// 256 位向量的 8×32 位车道归约(平方累加器专用:车道存的是 8 个 32 位部分和,
/// 不能按 64 位车道读;车道总和受控 < 2^31,折叠相加不溢出)
inline std::uint64_t sumLanes32(const __m256i v)
{
    const __m128i halves{_mm_add_epi32(_mm256_castsi256_si128(v), _mm256_extracti128_si256(v, 1))};
    const __m128i pairs{_mm_hadd_epi32(halves, halves)};
    return static_cast<std::uint64_t>(static_cast<std::uint32_t>(_mm_cvtsi128_si32(_mm_hadd_epi32(pairs, pairs))));
}

/// computeColor 的 AVX2 版,与标量版同口径(空输入早退、浮点 a 值行、count 逐像素计数)。
/// 依据 Σ[(t-c)*a + c*257] == a*Σt + (257-a)*Σc(int64 恒等),像素循环退化为按通道求字节和:
/// 通道掩码 + vpsadbw 每条指令汇总 8 字节;越界行列按裁剪处理(契约内为恒等)。
geometrize::rgba computeColorAvx2(
        const geometrize::Bitmap& target,
        const geometrize::Bitmap& current,
        const std::vector<geometrize::Scanline>& lines,
        const std::uint8_t alpha)
{
    if(lines.empty()) {
        return geometrize::rgba{0, 0, 0, 0};
    }
    if(target.getDataRef().empty() || current.getDataRef().empty()) {
        return geometrize::rgba{0, 0, 0, 0};
    }

    // 与标量 computeColor 逐字节同源的浮点行(float 截断行为是可观察输出的一部分,原样保留)
    const std::int32_t a{static_cast<std::int32_t>(257.0f * 255.0f / static_cast<float>(alpha))};

    const auto* targetData = target.getDataRef().data();
    const auto* currentData = current.getDataRef().data();
    const std::size_t rowStride{static_cast<std::size_t>(target.getWidth()) * 4U};
    const std::int32_t width{static_cast<std::int32_t>(target.getWidth())};
    const std::int32_t height{static_cast<std::int32_t>(target.getHeight())};

    // 通道掩码:每像素 4 字节,分别保留第 0/1/2 个通道(-1 = 0xFF)
    const __m256i maskR{_mm256_setr_epi8(
        -1, 0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0,
        -1, 0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0)};
    const __m256i maskG{_mm256_setr_epi8(
        0, -1, 0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0, -1, 0, 0,
        0, -1, 0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0, -1, 0, 0)};
    const __m256i maskB{_mm256_setr_epi8(
        0, 0, -1, 0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0, -1, 0,
        0, 0, -1, 0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0, -1, 0)};
    const __m256i zero{_mm256_setzero_si256()};

    __m256i targetAccR{zero};
    __m256i targetAccG{zero};
    __m256i targetAccB{zero};
    __m256i currentAccR{zero};
    __m256i currentAccG{zero};
    __m256i currentAccB{zero};
    std::uint64_t tailTargetR{0};
    std::uint64_t tailTargetG{0};
    std::uint64_t tailTargetB{0};
    std::uint64_t tailCurrentR{0};
    std::uint64_t tailCurrentG{0};
    std::uint64_t tailCurrentB{0};
    std::int64_t count{0};

    for(const geometrize::Scanline& line : lines) {
        if(line.y < 0 || line.y >= height) {
            continue;
        }
        const std::int32_t xStart{line.x1 < 0 ? 0 : line.x1};
        const std::int32_t xEnd{line.x2 >= width ? width - 1 : line.x2};
        if(xStart > xEnd) {
            continue;
        }
        count += static_cast<std::int64_t>(xEnd - xStart) + 1;
        const auto* tRow = targetData + rowStride * static_cast<std::size_t>(line.y);
        const auto* cRow = currentData + rowStride * static_cast<std::size_t>(line.y);

        std::int32_t x{xStart};
        for(; x + 8 <= xEnd + 1; x += 8) {
            const __m256i tVec{_mm256_loadu_si256(reinterpret_cast<const __m256i*>(tRow + static_cast<std::size_t>(x) * 4U))};
            const __m256i cVec{_mm256_loadu_si256(reinterpret_cast<const __m256i*>(cRow + static_cast<std::size_t>(x) * 4U))};
            targetAccR = _mm256_add_epi64(targetAccR, _mm256_sad_epu8(_mm256_and_si256(tVec, maskR), zero));
            targetAccG = _mm256_add_epi64(targetAccG, _mm256_sad_epu8(_mm256_and_si256(tVec, maskG), zero));
            targetAccB = _mm256_add_epi64(targetAccB, _mm256_sad_epu8(_mm256_and_si256(tVec, maskB), zero));
            currentAccR = _mm256_add_epi64(currentAccR, _mm256_sad_epu8(_mm256_and_si256(cVec, maskR), zero));
            currentAccG = _mm256_add_epi64(currentAccG, _mm256_sad_epu8(_mm256_and_si256(cVec, maskG), zero));
            currentAccB = _mm256_add_epi64(currentAccB, _mm256_sad_epu8(_mm256_and_si256(cVec, maskB), zero));
        }
        for(; x <= xEnd; x++) {
            const std::size_t offset{static_cast<std::size_t>(x) * 4U};
            tailTargetR += tRow[offset];
            tailTargetG += tRow[offset + 1U];
            tailTargetB += tRow[offset + 2U];
            tailCurrentR += cRow[offset];
            tailCurrentG += cRow[offset + 1U];
            tailCurrentB += cRow[offset + 2U];
        }
    }

    if(count == 0) {
        // 裁剪后无像素可统计(契约外输入,如宿主 creator 产出的完全越界扫描线):
        // 退回标量取色,保持既有可见行为;契约内 count 恒 > 0,此分支不进热路径。
        return geometrize::core::computeColor(target, current, lines, alpha);
    }

    const std::int64_t sumTargetR{static_cast<std::int64_t>(sumLanes64(targetAccR) + tailTargetR)};
    const std::int64_t sumTargetG{static_cast<std::int64_t>(sumLanes64(targetAccG) + tailTargetG)};
    const std::int64_t sumTargetB{static_cast<std::int64_t>(sumLanes64(targetAccB) + tailTargetB)};
    const std::int64_t sumCurrentR{static_cast<std::int64_t>(sumLanes64(currentAccR) + tailCurrentR)};
    const std::int64_t sumCurrentG{static_cast<std::int64_t>(sumLanes64(currentAccG) + tailCurrentG)};
    const std::int64_t sumCurrentB{static_cast<std::int64_t>(sumLanes64(currentAccB) + tailCurrentB)};

    const std::int64_t totalRed{static_cast<std::int64_t>(a) * sumTargetR + static_cast<std::int64_t>(257 - a) * sumCurrentR};
    const std::int64_t totalGreen{static_cast<std::int64_t>(a) * sumTargetG + static_cast<std::int64_t>(257 - a) * sumCurrentG};
    const std::int64_t totalBlue{static_cast<std::int64_t>(a) * sumTargetB + static_cast<std::int64_t>(257 - a) * sumCurrentB};

    // 收尾与标量 computeColor 逐语句一致
    const std::int32_t rr{static_cast<std::int32_t>(totalRed / count) >> 8};
    const std::int32_t gg{static_cast<std::int32_t>(totalGreen / count) >> 8};
    const std::int32_t bb{static_cast<std::int32_t>(totalBlue / count) >> 8};

    const std::uint8_t r{static_cast<std::uint8_t>(geometrize::commonutil::clamp(rr, INT32_C(0), INT32_C(255)))};
    const std::uint8_t g{static_cast<std::uint8_t>(geometrize::commonutil::clamp(gg, INT32_C(0), INT32_C(255)))};
    const std::uint8_t b{static_cast<std::uint8_t>(geometrize::commonutil::clamp(bb, INT32_C(0), INT32_C(255)))};

    return geometrize::rgba{r, g, b, alpha};
}

}

namespace geometrize
{

namespace core
{

double defaultEnergyFunctionFusedAvx2(
        const std::vector<geometrize::Scanline>& lines,
        const std::uint32_t alpha,
        const geometrize::Bitmap& target,
        const geometrize::Bitmap& current,
        geometrize::Bitmap& /*buffer*/,
        const double score)
{
    // 第 1 遍(只读):AVX2 取色,与标量 computeColor 逐位一致(见 computeColorAvx2)
    const geometrize::rgba color{computeColorAvx2(target, current, lines, alpha)};

    // 与标量融合版相同的基数与收尾公式(score 基数的浮点截断行为是输出的一部分,原样保留)
    const std::uint64_t rgbaCount{static_cast<std::uint64_t>(target.getWidth()) * target.getHeight() * 4U};
    std::uint64_t total{static_cast<std::uint64_t>((score * 255.0) * (score * 255.0) * static_cast<double>(rgbaCount))};
    if(target.getDataRef().empty() || current.getDataRef().empty()) {
        return std::sqrt(static_cast<double>(total) / static_cast<double>(rgbaCount)) / 255.0;
    }

    // drawLines 的单次预乘常量,与标量融合版逐语句同源(注意 d*aa + s*m 允许 uint32 回绕后再除)
    std::uint32_t sr{color.r};
    sr |= sr << 8;
    sr *= color.a;
    sr /= UINT8_MAX;
    std::uint32_t sg{color.g};
    sg |= sg << 8;
    sg *= color.a;
    sg /= UINT8_MAX;
    std::uint32_t sb{color.b};
    sb |= sb << 8;
    sb *= color.a;
    sb /= UINT8_MAX;
    std::uint32_t sa{color.a};
    sa |= sa << 8;

    const std::uint32_t m{UINT16_MAX};
    const std::uint32_t aa{(m - sa) * 257U};
    const std::uint32_t kR{sr * m};
    const std::uint32_t kG{sg * m};
    const std::uint32_t kB{sb * m};
    const std::uint32_t kA{sa * m};
    const std::uint32_t kChannels[4]{kR, kG, kB, kA};

    const auto* targetData = target.getDataRef().data();
    const auto* currentData = current.getDataRef().data();
    const std::size_t rowStride{static_cast<std::size_t>(target.getWidth()) * 4U};
    const std::int32_t width{static_cast<std::int32_t>(target.getWidth())};
    const std::int32_t height{static_cast<std::int32_t>(target.getHeight())};

    // V = d*aa + k 恒小于 2^32(见 core.h),按无符号解释走逻辑移位;k 向量按 RGBA 交错布局,
    // 一次覆盖两个像素的 8 个通道(所有通道共用 aa,只有加性常量不同)
    const __m256i aaVector{_mm256_set1_epi32(static_cast<int>(aa))};
    const __m256i kVector{_mm256_setr_epi32(
        static_cast<int>(kR), static_cast<int>(kG), static_cast<int>(kB), static_cast<int>(kA),
        static_cast<int>(kR), static_cast<int>(kG), static_cast<int>(kB), static_cast<int>(kA))};
    const __m256i oneVector{_mm256_set1_epi32(1)};

    __m256i afterAcc{_mm256_setzero_si256()};
    __m256i beforeAcc{_mm256_setzero_si256()};
    std::uint64_t afterSum{0};
    std::uint64_t beforeSum{0};
    std::uint32_t vectorsSinceFlush{0};

    // 32 位车道累加器的溢出上界:每向量每车道至多 255² = 65025,4096 向量 = 2.66e8 < 2^31
    const auto flushAccumulators = [&afterAcc, &beforeAcc, &afterSum, &beforeSum, &vectorsSinceFlush]() {
        if(vectorsSinceFlush == 0U) {
            return;
        }
        afterSum += sumLanes32(afterAcc);
        beforeSum += sumLanes32(beforeAcc);
        afterAcc = _mm256_setzero_si256();
        beforeAcc = _mm256_setzero_si256();
        vectorsSinceFlush = 0;
    };

    // 2 像素(8 通道)一组:无除法混合 → 与目标差分 → 平方累加(avx2 语义与标量逐位同源)
    const auto processQuad = [&afterAcc, &beforeAcc, aaVector, kVector, oneVector](const __m128i cBytes, const __m128i tBytes) {
        const __m256i c32{_mm256_cvtepu8_epi32(cBytes)};
        const __m256i t32{_mm256_cvtepu8_epi32(tBytes)};
        const __m256i v{_mm256_add_epi32(_mm256_mullo_epi32(c32, aaVector), kVector)};
        const __m256i w{_mm256_srli_epi32(v, 8)};
        const __m256i blended{_mm256_srli_epi32(_mm256_add_epi32(_mm256_add_epi32(w, _mm256_srli_epi32(w, 16)), oneVector), 16)};
        const __m256i dta{_mm256_sub_epi32(t32, blended)};
        const __m256i dtb{_mm256_sub_epi32(t32, c32)};
        afterAcc = _mm256_add_epi32(afterAcc, _mm256_mullo_epi32(dta, dta));
        beforeAcc = _mm256_add_epi32(beforeAcc, _mm256_mullo_epi32(dtb, dtb));
    };

    for(const geometrize::Scanline& line : lines) {
        // 越界行:与标量融合版一致跳过(库内扫描线恒已裁剪)
        if(line.y < 0 || line.y >= height) {
            continue;
        }
        const std::int32_t xStart{line.x1 < 0 ? 0 : line.x1};
        const std::int32_t xEnd{line.x2 >= width ? width - 1 : line.x2};
        if(xStart > xEnd) {
            continue;
        }
        const auto* tRow = targetData + rowStride * static_cast<std::size_t>(line.y);
        const auto* cRow = currentData + rowStride * static_cast<std::size_t>(line.y);

        std::int32_t x{xStart};
        for(; x + 8 <= xEnd + 1; x += 8) {
            const __m256i cVec{_mm256_loadu_si256(reinterpret_cast<const __m256i*>(cRow + static_cast<std::size_t>(x) * 4U))};
            const __m256i tVec{_mm256_loadu_si256(reinterpret_cast<const __m256i*>(tRow + static_cast<std::size_t>(x) * 4U))};
            const __m128i cLow{_mm256_castsi256_si128(cVec)};
            const __m128i tLow{_mm256_castsi256_si128(tVec)};
            const __m128i cHigh{_mm256_extracti128_si256(cVec, 1)};
            const __m128i tHigh{_mm256_extracti128_si256(tVec, 1)};
            processQuad(cLow, tLow);
            processQuad(_mm_srli_si128(cLow, 8), _mm_srli_si128(tLow, 8));
            processQuad(cHigh, tHigh);
            processQuad(_mm_srli_si128(cHigh, 8), _mm_srli_si128(tHigh, 8));
            if(++vectorsSinceFlush >= 4096U) {
                flushAccumulators();
            }
        }
        // 尾部不足 8 像素:走同式标量(每行最多 7 像素,不进热路径)
        for(; x <= xEnd; x++) {
            const std::size_t offset{static_cast<std::size_t>(x) * 4U};
            for(std::size_t channel = 0; channel < 4U; channel++) {
                const std::int32_t targetChannel{static_cast<std::int32_t>(tRow[offset + channel])};
                const std::int32_t currentChannel{static_cast<std::int32_t>(cRow[offset + channel])};
                const std::int32_t blended{blendChannel(cRow[offset + channel], aa, kChannels[channel])};
                const std::int32_t dtb{targetChannel - currentChannel};
                const std::int32_t dta{targetChannel - blended};
                beforeSum += static_cast<std::uint64_t>(dtb * dtb);
                afterSum += static_cast<std::uint64_t>(dta * dta);
            }
        }
    }
    flushAccumulators();

    total = total - beforeSum + afterSum;
    return std::sqrt(static_cast<double>(total) / static_cast<double>(rgbaCount)) / 255.0;
}

}

}

#else // !GEOMETRIZE_X86_SIMD

namespace geometrize
{

namespace core
{

// 非 x86 平台:融合内核只有标量实现(avx2EnergyKernelAvailable() 恒 false,运行时到不了这里;
// 保留定义只为满足链接,行为与标量融合版完全一致)。
double defaultEnergyFunctionFusedAvx2(
        const std::vector<geometrize::Scanline>& lines,
        const std::uint32_t alpha,
        const geometrize::Bitmap& target,
        const geometrize::Bitmap& current,
        geometrize::Bitmap& buffer,
        const double score)
{
    return defaultEnergyFunctionFusedScalar(lines, alpha, target, current, buffer, score);
}

}

}

#endif

