// golden 常量采集(双变体共享,任何分歧都是行为分叉信号,禁止静默采纳单侧值)
// 生成方式:cmake --build build --config Release --target geotest-fast 后
//   .\build\Release\geotest-fast.exe --dump-golden > goldens.txt
// 把输出粘贴到本文件对应常量;再跑 geotest-base 确认两变体一致。
//
// 所有 golden 用固定 64x64 合成位图 + 固定种子采集,不含外部输入。
#pragma once

namespace goldens
{

// test_core:differenceFull 对已知 16x16 图案对的分数(%.17g 十七位有效数字精确比较)
// 采集占位:首次 --dump-golden 后填充
inline constexpr double DIFF_FULL_PATTERN = -1.0; // TODO(golden): 待采集

} // namespace goldens
