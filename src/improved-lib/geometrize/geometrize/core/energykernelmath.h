// 融合能量内核的共享算术(标量融合实现与 AVX2 实现同源,防止两份公式漂移)。
// 这里只放"位等价由构造保证"的整数恒等式,不含任何近似:
//   ① blendChannel:drawLines/drawLinesSegmented 的表项 ((d*aa + s*m)/65535)>>8 的无除法等价式
//      (全输入域 256³ 组合已穷举比对,见 docs/ROADMAP.md §1 P1.7);
//   ② premultiply:drawLines 的预乘常量(sr/sg/sb/sa 与 aa),逐语句照搬含 uint32 回绕的写法。
#pragma once

#include <cstdint>

#include "../bitmap/rgba.h"

namespace geometrize
{

namespace core
{

namespace energykernel
{

/**
 * @brief blendChannel 混合公式的无除法等价式:floor((d*aa + s*m) / (65535*256))。
 * 展开:V = d*aa + s*m(上界 65535²,已证不回绕);记 W = V >> 8,
 * floor(V/16776960) == floor(W/65535) == (W + (W>>16) + 1) >> 16。
 */
inline std::int32_t blendChannel(const std::uint32_t d, const std::uint32_t aa, const std::uint32_t sTimesM)
{
    const std::uint32_t v{d * aa + sTimesM};
    const std::uint32_t w{v >> 8U};
    return static_cast<std::int32_t>((w + (w >> 16U) + 1U) >> 16U);
}

/**
 * @brief PremultipliedColor 混合用的预乘常量:k[0..3] 依次为 R/G/B/A 通道的 s*m,aa 为公共乘子。
 */
struct PremultipliedColor
{
    std::uint32_t k[4];
    std::uint32_t aa;
};

/**
 * @brief premultiply 由形状颜色求混合常量,表达式与 drawLines/drawLinesSegmented 完全一致
 * (含 uint32 回绕语义;sa 用 257*a,aa = (65535 - sa) * 257)。
 */
inline PremultipliedColor premultiply(const geometrize::rgba& color)
{
    const std::uint32_t m{UINT16_MAX};

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

    PremultipliedColor out;
    out.k[0] = sr * m;
    out.k[1] = sg * m;
    out.k[2] = sb * m;
    out.k[3] = sa * m;
    out.aa = (m - sa) * 257U;
    return out;
}

}

}

}
