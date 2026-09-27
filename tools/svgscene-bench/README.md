# svgscene-bench — 矢量视图渲染开销基准(离屏)

给应用的矢量(矢量图形)预览视图做的渲染开销基准,对应 `docs/ROADMAP.md` F3.9 与
`docs/bugfix-triage.md` C.3.1。用来回答"形状累积到几百/几千时,这个视图每帧要花多少时间、
占多少内存",以及验证候选实现之间的画面等价性。

不依赖 GUI 交互:用 `QGraphicsScene` + `QGraphicsView` 离屏渲染,形状与 SVG 片段走真实的
`geometrize::exporter`(`getSingleShapeSVGData` / `exportSVGDocument`)。

## 四种被测实现

| 名称 | 说明 |
|---|---|
| 旧 | 每批新形状一个全画布 `SvgItem`(上游与应用修复前的实现) |
| 单 item | 全部形状累积在一个 item(片段 append-only + 整体替换渲染器) |
| 分块 | 当前块累积刷新、块满封板不再重解析(**应用采用的实现**) |
| 单件多渲染 | 一个自绘 item + 多个分块渲染器(实测画面差异大,已否决,见 ROADMAP F3.9) |

每种实现各测两个口径:
- **刷新帧**:本批内容变更 + 该帧渲染(形状到来时的真实开销);
- **稳态帧**:内容未变仅重绘(鼠标移动、其他 item 更新触发的重绘)。

## 构建与运行

```powershell
# 前置:先构建 geobench(本基准链接它的 geometrize-fast.lib)
cd src\geobench
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release

# 本基准
cd tools\svgscene-bench
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=D:/Qt/5.15.2/msvc2019_64
cmake --build build --config Release
# 参数: [分块阈值=256] [QPixmapCache 上限 MB(默认 Qt 的 10MB)]
$env:QT_QPA_PLATFORM="offscreen"; .\build\Release\svgscene_bench.exe 256 128
```

`QT_QPA_PLATFORM=offscreen` 必须有;不需要显示器,可在 CI/无桌面环境跑。

## 关键结论(2026-09-22 实测,512×384 图 / 900×600 视口 / 每批 4 形状)

- 旧实现每帧要合成"形状数/4"层全画布图层,5000 形状时帧耗时 371ms、缓存内存 2.5GB;
  分块实现为 2.6ms / 41MB,10000 形状仍 4.9ms / 82MB。
  绝对数值受机器负载与内存压力影响(旧实现一侧同参数重跑 5000 形状在 371~1000ms 之间),
  看量级与趋势,别当基准分;分块一侧稳定在个位数毫秒。
- **QPixmapCache 上限是隐藏变量**:设备坐标缓存的 item 位图走 Qt 全局缓存,默认 10MB
  只装得下约 5 个全画布图层;超出后逐帧重渲染(5000 形状 68ms/帧)。应用在 `main.cpp`
  把上限提到 128MB。跑本基准时第二个参数就是用来复现/验证这一点的。
- 画面等价性:分块实现与旧实现逐像素比,36% 像素差 1 个通道单位、0.03% 差 2(分层合成多一次
  8 位舍入);把阈值放大到不分块时与"单 item"方案逐像素相同——差异只来自分层合成,不是累积逻辑。
