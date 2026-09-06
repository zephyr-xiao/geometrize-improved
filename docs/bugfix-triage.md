# Bug 分级清单(基于上游 geometrize-lib 快照)

## C.1 修复后会改变输出的 bug

均不在主循环(hillClimb/setup/mutate/rasterize)触发路径上;对拍门禁矩阵默认全图 bounds,不受影响。

### C.1.1 Ellipse 光栅化笔误
- 位置:rasterizer.cpp,`rasterize(const Ellipse&…)` 中 `if (y1 >= xMin && y1 < yMax)` 应为 `y1 >= yMin`
- 症状:启用 shapeBounds 且 yMin>0 时,椭圆上半部分丢行或错判越界
- 门禁处理:默认全图 bounds(xMin=yMin=0)下修复前后无差异;另设显式 bounds 用例归 EXPECTED_DIFF

### C.1.2 scale(Polyline) 配对错乱
- 位置:shapemutator.cpp,`scale(geometrize::Polyline&)` 以步长 2 遍历并错位重组 m_points,输出点数减半且坐标错乱
- 触发:仅当宿主/脚本直接调 `geometrize::scale(polyline, factor)`。已核实:①shapefactory 只绑定 setup/mutate;②ChaiScript 绑定面(bindingscreator.cpp createGeometrizeLibraryBindings)未暴露 scale/translate/rotate 自由函数;③应用代码零调用。上游 templates 子模块需在集成时抽查 grep `scale\(`
- 修复:重写为绕质心缩放全部点

### C.1.3 scale(QuadraticBezier) 未实现
- 位置:shapemutator.cpp,`assert(0 && "TODO")` —— Release(NDEBUG)下静默 no-op,Debug 下中止
- 触发与影响同上;修复为绕质心缩放三控制点

## C.2 健壮性修复(合法参数域内输出不变)

| 项 | 位置 | 说明 |
|---|---|---|
| Bitmap 索引乘法 int32 溢出 | bitmap.cpp getPixel/setPixel | width*y 超 int32 时原版 UB;升 uint64 后常规尺寸数值恒等 |
| trimScanlines float clamp | scanline.cpp | int 值经 float clamp 再转回,float ≤2²⁴ 无损表示,数值恒等 |
| catch(std::exception&) 按值抛副本 | model.cpp getHillClimbState | `throw e;`→`throw;` 保留异常多态 |
| throw 字符串字面量 | shape.h clone/getType | 改 std::logic_error |
| mutate(Polyline) size_t 符号警告 | shapemutator.cpp | 显式整型转换 |
| scanlinesContainTransparentPixels 尾像素漏检 | commonutil.cpp `x < scanline.x2` 应为 `<=` | 不在主循环,API 行为变化归 EXPECTED_DIFF 注明 |

## 上游怪癖(必须保留,非 bug)

- bestRandomState off-by-one:循环外求值 1 次 + for(i=0;i<=n;i++) 共 n+1 次 = n+2 次,RNG 消费次数参与确定性
- Ellipse 光栅化 dy=0 只画一行、dy>0 时上下对称行各一条的扫描策略与其去重语义
- drawLines 的 RGB 预计算 `sr |= sr<<8; sr *= a; sr /= 255` 数学等价重构存在舍入差异风险,不动
