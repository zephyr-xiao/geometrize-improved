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

### C.1.4 mapShapeBoundsToImage 上界 off-by-one(画布最右列/最下行永不落画)
- 位置:commonutil.cpp `mapShapeBoundsToImage`。未启用 bounds 时返回 `(0, 0, width-1, height-1)`,
  启用时百分比也按 `size-1` 换算;而**消费侧一律把该元组的 max 当排他上界**:
  `setup`/`mutate` 用 `randomRange(xMin, xMax-1)` 采样,各 `rasterize` 把 x clamp 到 `xMax-1`、
  把 y 过滤在 `[yMin, yMax)`,即形状的采样范围与光栅化裁剪范围同时被砍掉最后一列/最后一行。
  `runner/imagerunneroptions.h` 中 `shapeBounds` 的文档写的是"整幅图即 (0, 0, imageWidth, imageHeight)",
  应用侧 `drawBackgroundRectangle` 也用 `rasterize(rect, 0, 0, w, h)`,契约本就是排他上界,故 `-1` 是笔误。
- 症状:点阵图(位图)视图最右列与最下行永远是初始背景色,任何形状都画不到那里;矢量视图按浮点坐标
  绘制,故看不出缺口,两个视图在边缘 1px 上不一致。脚本模式的 `xMin/xMax` 全局量同源,内置
  `default_shape_mutators` 模板一律按 `xMax-1` 取闭区间,故脚本模式同样少 1px。
- 门禁处理:**默认行为不变**(库侧新增 `ImageRunnerOptions::fixShapeBoundsOffByOne`,默认 false = 上游语义),
  否则所有默认 bounds 用例的哈希都会变、bit-exact 矩阵整体失效。修复由显式开关开启:
  geobench `--fix-shape-bounds`,应用侧在 `task/imagetask.cpp` 固定开启(交付物按修复语义出图)。
  分叉由 `run_ab.ps1` 的 `fix_bounds_fork` / `fix_bounds_explicit_fork` 两个 EXPECTED_DIFF 用例锁定。
- 实测(tiny_64,200 步,all types,seed 9001,--dump-final 取末帧):关闭时末列/末行 4096 像素全为背景色
  (各 1 种颜色),开启后末列 32 种、末行 35 种颜色,整幅画布可达。

## C.2 健壮性修复(合法参数域内输出不变)

| 项 | 位置 | 说明 |
|---|---|---|
| Bitmap 索引乘法 int32 溢出 | bitmap.cpp getPixel/setPixel | width*y 超 int32 时原版 UB;升 uint64 后常规尺寸数值恒等 |
| trimScanlines float clamp | scanline.cpp | int 值经 float clamp 再转回,float ≤2²⁴ 无损表示,数值恒等 |
| catch(std::exception&) 按值抛副本 | model.cpp getHillClimbState | `throw e;`→`throw;` 保留异常多态 |
| throw 字符串字面量 | shape.h clone/getType | 改 std::logic_error |
| mutate(Polyline) size_t 符号警告 | shapemutator.cpp | 显式整型转换 |
| scanlinesContainTransparentPixels 尾像素漏检 | commonutil.cpp `x < scanline.x2` 应为 `<=` | 不在主循环,API 行为变化归 EXPECTED_DIFF 注明 |

## C.3 应用层缺陷(不改库输出,只改渲染路径)

### C.3.1 矢量视图图层数随形状数线性增长(形状越多越卡)
- 位置:应用 `scene/imagetasksvgscene.cpp`。上游实现每批新形状 `exportSVG` 出一份新文档、新建一个
  `SvgItem` 挂进场景,而每份文档的 viewBox 都是整幅画布,于是每个 item 的 boundingRect 都是整幅画布;
  设备坐标缓存下每个 item 还各持有一张全画布位图。形状数到几百后每帧要合成几百层全画布图层,
  内存与帧耗时随形状数线性增长。
- 修复:形状按块累积(每块 256 个形状,见 `SVG_CHUNK_SHAPE_LIMIT`)。当前块 append-only 累积
  SVG 片段、每批只序列化本批,文档重拼后整体替换渲染器(`SvgItem::setDocument`);块满即封板,
  封板后不再重解析也不再改动。图层数因此是 `形状数/256` 量级而非 `形状数/每批` 量级。
  文档包装走库新增的 `exporter::exportSVGDocument`,与 `exportSVG` 同源(`exportSVG` 改为调用它,
  输出逐字节不变,由 `test_exporter.cpp` 锁定)。
- 配套:`main.cpp` 把 `QPixmapCache` 上限从默认 10MB 提到 128MB。设备坐标缓存的 item 位图走这个
  全局缓存,默认上限按整幅画布的设备像素只装得下约 5 个图层;图层数一旦超过就开始逐帧重渲染,
  帧耗时从 0.2ms 量级跳到 70ms 量级(实测 5000 形状:提升前 68ms/帧,提升后 2.6ms/帧)。
- 离屏实测(512×384 图 / 900×600 视口 / 每批 4 形状,单位 ms,「刷新」= 本批内容变更 + 该帧渲染,
  「稳态」= 内容未变仅重绘;数字为一次实测,旧实现一侧受内存压力影响波动较大——同参数重跑
  5000 形状在 371~1000ms 之间,量级与趋势稳定,分块一侧稳定在个位数毫秒):

| 形状数 | 上游实现 刷新/稳态 | 改进后 刷新/稳态 | 改进后图层数 | 改进后缓存内存 |
|---|---|---|---|---|
| 300 | 8.2 / 7.6 | 1.0 / 0.2 | 2 | 4 MB |
| 600 | 36.9 / 34.3 | 1.4 / 0.4 | 3 | 6 MB |
| 1200 | 80.2 / 82.5 | 2.9 / 0.8 | 5 | 10 MB |
| 2400 | 142.3 / 151.2 | 3.0 / 1.0 | 10 | 21 MB |
| 5000 | 362.8 / 371.7 | 4.6 / 2.6 | 20 | 41 MB |
| 10000 | 732.1 / 685.4 | 5.8 / 4.9 | 40 | 82 MB |

- 画面等价性(600 形状同序列,与上游实现逐像素比):36% 像素差 1 个通道单位、0.03% 差 2,
  单通道最大差 2(分层合成多一次 8 位舍入);对照实验(阈值放大到不分块)与单 item 方案逐像素相同,
  确认差异只来自分层合成而非累积/封板逻辑。

## 上游怪癖(必须保留,非 bug)

- bestRandomState off-by-one:循环外求值 1 次 + for(i=0;i<=n;i++) 共 n+1 次 = n+2 次,RNG 消费次数参与确定性
- Ellipse 光栅化 dy=0 只画一行、dy>0 时上下对称行各一条的扫描策略与其去重语义
- drawLines 的 RGB 预计算 `sr |= sr<<8; sr *= a; sr /= 255` 数学等价重构存在舍入差异风险,不动
