# Geometrize Improved

对 [Tw1ddle/geometrize](https://github.com/Tw1ddle/geometrize)(图像几何化桌面应用,2.1k stars)的改进分支:
**核心算法库性能优化 + bug 修复 + Qt 应用层优化 + 大图支持**,交付免安装绿色版桌面应用。

上游项目把图片近似重建为圆形/三角形/矩形/椭圆等几何图元的叠加。本分支在
**与上游输出逐位一致(bit-exact)** 的前提下系统性消除性能税源,i3-13100F(4C8T)实测综合提速 **2.3x ~ 3.6x**。

## 直接使用

打开 `dist\Geometrize-Improved\Geometrize.exe` 即用(免安装,文件夹可拷贝到任意 x64 Windows 机器)。
大图支持:主界面"打开图像"按钮下方有**处理分辨率上限**下拉(256/512/1024/2048/4096/无限制),
单边超限的图按比例缩小到限内;全局偏好的 Image Resizing 保留自定义宽高入口(512 图约 0.6s/步,
1024 图约 2.4s/步,2048² 约 6.2s/步,见 benchmarks/report.md)。

图片任务的**运行设置面板**底部另有四个算法增强勾选框(默认全关):
- **金字塔搜索(快速)**:半分辨率评估,大图大幅提速(1024 图 ~4x,质量代价 ~1-2%);
- **自适应变异步长 / 逐形状透明度搜索**:同步数质量更好但每步明显变慢,适合步数受限的精修;
- **误差图引导形状放置**:新形状优先落在误差最高处而非均匀随机,低预算(少候选/少变异)
  时收益最大(极端低预算 -6.7%,标准预算 -2~3%,时间成本≈0),适合快速草图模式;
- **框选优先区域(Ctrl+Drag)**:勾选后在预览图上框选区域(可多个,上限 16),区域内误差
  权重 ×10——形状优先落在所选区域(如人像脸部)。需开启误差图引导才生效;标准预算下
  区域内误差 -11.4%,全图几乎无损。清除用"清除区域"按钮。

**批量处理**(启动器窗口菜单打开 Task Queue):拖入多张图 + 选择脚本 + Run All 即可逐图批处理。
支持队列持久化(关窗重开不丢)、批进度显示(窗口标题 N/M + 任务栏进度条)、完成后自动导出
PNG/SVG 到指定目录(默认 文档\geometrize_batch_output\,同名自动加序号)。

**导出性能**:GIF/PNG 帧序列导出走增量帧渲染(O(N),百万形状级秒出)+ 帧数自动封顶 1000
(超限均匀抽稀,末帧必取)+ 后台线程执行(弹进度框,UI 不卡死)。帧画面与任务运行时位图
同源(同一套光栅化混合函数,分段颜色同样生效)。GIF 默认每帧 20 形状、20FPS(GIF 格式
延迟下限 20ms,实用上限 50FPS)、输出倍率 1。SVG 文件可直接拖进浏览器查看(命名空间已修复)。

## 成果摘要

| 指标 | 上游 | 改进版 |
|---|---|---|
| 基线管线(768×504,100 步,8 线程) | 12.9s | **3.9s(3.27x)** |
| 大图 2048²(20 步) | 325s | **124s(2.63x)** |
| 单线程管线 | 5.0s | **1.4s(3.59x)** |
| 输出 | — | **逐位一致(SHA-256 + 逐步指纹全对拍)** |

优化清单(全部在 bit-exact 门禁下落地,论证见 docs/bitwise-equivalence-notes.md):
- B1 Bitmap 访问器内联化 + 64 位索引防溢出
- B2 copyLines 行段 memcpy;drawLines/computeColor/differencePartial 裸指针行游标(公式逐字保留)
- B3 Circle 光栅化 O(r²)→O(r)(整数 isqrt,等价性证明在内)
- B4 scanlinesForPolygon 去 map/set 扁平化
- B5 Model::step 补丁式快照替代两次全图拷贝(自定义回调路径保留整图语义)
- B6 光栅化/扫描线向量跨 hill-climb 复用(rasterizeInto)
- B7 持久线程池替代每步 std::async(种子 submit 序分配,输出与完成序无关)

算法增强轨道(opt-in,输出与上游不同属预期):
- P1.1 金字塔搜索:hill-climb 半分辨率评估 + 全分辨率接受安全网,1024 图 3.95x / 2048 图 3.71x
  (P1.2 线程池负载均衡经核验否决:爬山链串行 RNG 依赖,位精确前提下无可分并行,结论存档 ROADMAP)
- A2.2 自适应步长(`--adaptive-step`):1/5 成功法则收缩/回升变异步长,同步数质量 +7.5%
- A2.3 alpha 档位搜索(`--alpha-search`):逐形状 {64,128,192} 穷举最优 alpha,同步数质量 +9.1%
  (两项均同步数口径;等时间口径反劣,适用场景与组合注意见 benchmarks/report.md)
- A2.1 误差图引导(`--error-guide`):16×16 块级 L1 误差图加权采样候选位置 + ε-greedy(默认 10% 探索),
  低预算场景质量 +2.4~6.7%,时间成本≈0;标准预算下爬山变异已稀释位置先验,收益 2~3%(实测诚实结论见
  benchmarks/report.md)
- F3.2 区域优先(`--priority-region x1,y1,x2,y2`,可重复;需配 --error-guide):区域内误差块 ×10,
  形状优先落所选区域——标准预算区域内 -11.4%,适合突出画面主体

修复清单(见 docs/bugfix-triage.md):
- 上游 Ellipse 光栅化越界笔误(y1>=xMin 应为 yMin)
- scale(Polyline) 错乱重写、scale(QuadraticBezier) 实现 TODO
- installShapeScript 解引用 map end 迭代器的 UB(**GUI 启动崩溃根因**,新 MSVC 下必现)
- 形状边界上界 off-by-one(C.1.4):上游 `mapShapeBoundsToImage` 返回闭区间上界 size-1,而
  setup/mutate/rasterize 全程按排他上界消费,导致**位图最右列与最下行永不落画**(点阵视图上是
  一条永不变化的背景边,矢量视图按浮点坐标绘制故看不出)。库侧默认保持上游语义以维持 bit-exact
  门禁,新增 `ImageRunnerOptions::fixShapeBoundsOffByOne` 显式开启;**应用已固定开启**(脚本模式的
  `xMin/xMax` 全局量同步为排他上界,内置模板的 `xMax-1` 因此取到真正的末列),geobench 侧开关为
  `--fix-shape-bounds`,分叉由两个 EXPECTED_DIFF 用例锁定
- 矢量(矢量图形)视图图层膨胀:上游每批新形状新建一个 `SvgItem`,每个 item 的 boundingRect 都是
  整幅画布且各持一张全画布缓存位图,形状到几百后每帧要合成几百层、内存线性增长(越用越卡)。
  改为按块累积(每块 256 形状,封板后不再重解析),并把 Qt 全局 pixmap 缓存上限 10MB→128MB
  (设备坐标缓存的 item 位图受它约束)。实测 5000 形状:帧耗时 371ms→2.6ms,缓存内存 2.5GB→41MB
- Qt 应用层:pro 缺 concurrent 模块、ChaiScript 需 pin 上游 commit(HEAD 在 MSVC 14.4x 下编不过)、
  步进刷新 33ms 合帧节流、SVG item 设备缓存、无 timed-update 脚本不启 100ms 轮询、
  加载链去三次整图拷贝、前条件脚本数据竞争强制单线程、默认缩放阈值 256→1024

**审查轮修复(2026-09-29,六路并行深审 + 逐条实测复现)**:
- 库:submit 抛异常时 `states` 先于仍在运行的任务析构 → **write-after-free**(实测堆损坏 0xC0000374);
  落画入口统一裁剪扫描线(宿主自定义 creator 越界时裸指针写穿缓冲,实测 16×16 图配 32 格 creator);
  1×1 图 + 库默认语义 → `randomRange(0,-1)` 违反前置条件(**实测挂死**);
  `exportShapeJson` 单元素数组输出尾逗号 → 非法 JSON;`getAverageImageColor` uint32 累加溢出(>4096² 亮图);
  drawLines/drawLinesSegmented/copyLines 补边界自守;copyLines 源图行距改用源宽度;
  `scale(QuadraticBezier)` 去掉 int32 截断;`rasterize(Ellipse)` 的 `dy*dy` 提升到 float 域;
  clone 改为拷贝 `rasterizeInto`;`maxStepShift` 钳位(≥32 时移位 UB);0 像素位图直接返回空
- 单测:启用 16 个 `#if 0 // CD` 的 Model 级哨兵(**暴露并修掉上述堆损坏**,原 15~30% 触发率归零);
  Ellipse 分叉的 fast 侧断言收紧为精确值;alpha 搜索断言不再把用户 alpha 当合格档;
  新增越界裁剪回归、scale 家族、rasterizeInto≡rasterize 等价用例(111 × fast / 39 × base)
- 工具链:`run_ab.ps1` 增 golden 值锁定(`-AcceptGolden` 显式重采并打印旧→新)、重试即 FAIL、
  退出码/超时、基线快照预检、被测 exe 陈旧拒绝、报告含 exe 哈希与 SHAPES_TOTAL;
  geobench 严格参数校验(未知 flag/形状名、非数字、超范围一律 exit 1);
  `verify_patches.py` 的 `--init` 需 `--accept-drift` 理由且先打印被吞掉的漂移、FAIL 保留排查现场、app 侧失败 exit 2
- 应用层:33ms 合帧改渲染**栅栏内取的位图快照**(消除与 worker 步进写位图的数据竞争)、合帧定时器改 Impl 持有(析构即停)、
  timed-update 轮询在脚本装载后重新判定(此前构造期判定必为假 → `on_timed_update` 脚本永久失效)、
  区域优先勾选框不再被强制回填(界面显示与真实模式相反)、GIF 末帧必取(此前被 frameSkipPredicate 提前丢弃)、
  BurstLinker 透传 loopCount("Loop Forever" 勾选恢复生效)、`CoInitialize` 只在 S_OK 时配对 Uninitialize、
  SvgItem 提升 maximumCacheSize(Qt 默认 1024×768 会让设备坐标缓存整体旁路)、前条件脚本改跑克隆引擎(消除主引擎竞争)

## 目录结构

```
├─ dist\Geometrize-Improved\   # 免安装发布包(双击即用)
├─ upstream\                    # 上游只读快照(lib + app + 全部子模块)
├─ patches\lib\                 # 核心库编号补丁系列(历史按特性归档,不可顺序重放,见 patches\regen\README)
├─ patches\qt\                  # 应用层改动(见下"应用构建")
├─ patches\regen\               # 规范全量补丁(权威可重放集,verify_patches.py 双门禁管护)
├─ src\
│   ├─ baseline-lib\           # 上游库快照(只读,对拍基准)
│   ├─ improved-lib\           # 全部补丁后的库(geobench-fast / geotest-fast 链接它)
│   ├─ geobench\               # CLI 基准器 + CMake 统一构建入口(SHA-256 + FNV 滚动指纹双口径对拍)
│   ├─ test\                   # doctest 单元测试(双变体链接,111 用例 × fast / 39 × base;
│   │                          #   Model 级哨兵(区域/分段/组合/异常传播/越界裁剪)已全部启用)
│   └─ improved-app\           # 应用改进工作树(含改进版库 + 全部应用层 patch,可 qmake 构建)
├─ tools\run_ab.ps1             # A/B 对拍矩阵(26 用例:11 bit-exact + 15 EXPECTED_DIFF 分叉哨兵)
│                               #   + tools\goldens.csv 值级锁定(双端双口径哈希逐值比对)
├─ tools\goldens.csv            # 对拍 golden:只断言"与 base 不同"抓不到"分叉的值变了",值级锁定靠它
├─ tools\verify_patches.py      # 补丁↔工作树一致性双门禁(重放复现 + 归档新鲜度;lib 失败 exit 1,app 失败 exit 2)
├─ tools\svgscene-bench\        # 矢量视图渲染开销离屏基准(四方案对比 + 画面等价性,见 F3.9)
├─ benchmarks\report.md         # 性能报告(分阶段数据 + 大图可行性)
└─ docs\                        # 等价性论证 / bug 分级 / Qt 评估
```

## 复现

```powershell
# 核心库基准与对拍(CMake ≥3.20 + MSVC)
cd src\geobench
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
cd ..\..                      # 门禁脚本按仓库根定位自身,PSScriptRoot 无关,但路径写成 tools\ 便于阅读
powershell -ExecutionPolicy Bypass -File tools\run_ab.ps1   # 26/26 PASS(含 15 个 EXPECTED_DIFF 分叉锁定用例)
# 判据三层:结构断言(bit-exact 必须相同 / EXPECTED_DIFF 必须不同)+ base 双跑自洽 + golden 值比对
#   tools\goldens.csv 记录每条用例的 base/fast 双口径哈希;值漂移 = FAIL,重采要显式:
#   powershell -File tools\run_ab.ps1 -AcceptGolden     # 打印每条旧→新差异后重写 goldens.csv
# 其它开关:-Case <子串> 只跑匹配用例;-BaselineExe <exe> 对拍外部基线;失败自动留双方 .raw 供 diff
# 预检:基线快照必须与 upstream\geometrize-lib 逐字节一致、被测 exe 必须比源码新(漏编译即拒绝跑)
# 报告含两个 exe 的 SHA-256/mtime、重试次数、SHAPES_TOTAL;发生重试即按 FAIL 计(偶发故障不静默放过)
# geobench 参数:--shape-bounds x1,y1,x2,y2(百分比视口)、--dump-final PATH(位图留痕);
#   算法增强轨道 --pyramid / --adaptive-step / --alpha-search / --error-guide / --segment-colors;
#   修复开关 --fix-shape-bounds(C.1.4;默认关 = 上游语义,应用侧已固定开启)
#   参数拼错会直接报错 exit 1(未知形状名/未知 flag/非数字值/超范围值),不再静默空转成"哈希相同"

# 补丁↔工作树一致性校验(双门禁:G1 重放复现 + G2 归档新鲜度;改库/改应用后必须重跑并重新提交 patches\regen\)
python tools\verify_patches.py   # lib 失败 exit 1;app 侧失败 exit 2(--allow-app-drift 显式放行,--skip-app 只跑 lib)
# 归档缺失默认 FAIL(防"删归档静默过门禁");确认工作树后重新导出:
python tools\verify_patches.py --init --accept-drift "改动原因"   # 漂移场景下必须给理由,且会先打印被吞掉的漂移明细

# 单元测试(双变体:geotest-base 链上游快照 / geotest-fast 链改进版,111 用例 × fast / 39 × base)
cmake --build build --config Release --target geotest-base geotest-fast
ctest --test-dir build -C Release --output-on-failure        # 2/2 PASS
# 双变体全绿 = 函数级 bit-exact 门禁;
# 变体分叉点(Ellipse 边界笔误修复、scanlines 末像素、move 构造、异常重抛、线程池保序)
# 用 GEOTEST_BASE / GEOTEST_FAST 宏各自锁定行为。
# (注:test_goldens.h 仍是预留占位(无消费方);函数级等价由双变体行为断言承载,
#  端到端值级锁定由 tools\goldens.csv 承载)
# 注:Model 级用例在 Debug 构建下会被 drain 里的 assert(0) 中断(异常传播用例属预期路径),
#    门禁一律用 Release(NDEBUG)跑

# 应用构建(Qt 5.15.2 win64_msvc2019_64,用 aqtinstall 安装)
cd src\improved-app
# MSVC x64 环境里:
qmake geometrize.pro "CONFIG+=release"
python scripts/generate_geometrize_qrcs.py   # 在 resources cwd 下运行
nmake
windeployqt release\Geometrize.exe
```

应用层改动集中在这些文件(相对上游):
- `geometrize.pro`(concurrent 模块 + /utf-8)
- `dialog/launchwindow.ui/.cpp`(处理分辨率上限下拉,见 patches/qt/0016)
- `dialog/taskqueuewindow.*`(批处理增强:完成感知/队列持久化/自动导出/任务栏进度,见 patches/qt/0026)
- `dialog/imagetaskwindow.h/.cpp`(完成信号 signal_didStopConditionMet + getShapes 访问器,见 patches/qt/0026)
- `dialog/imagetaskrunnerwidget.ui/.cpp`(算法增强四勾选框,见 patches/qt/0017/0020)
- `task/shapecollection.*` + `task/imagetaskworker.*` + `task/imagetask.*` + `dialog/imagetaskwindow.cpp`
  (撤销/重做:worker 批量重放 + 经典双栈语义,见 patches/qt/0021)
- `scene/imagetaskscenemanager.*` + `serialization/imagetaskpreferencesdata.h`(区域优先框选与
  overlay,见 patches/qt/0022)
- `dialog/imagetaskwindow.cpp`(33ms 合帧节流、定时器按需启动)
- `scene/imagetasksvgscene.cpp` + `scene/svgitem.cpp/.h`(矢量视图分块累积:每块 256 形状,
  片段 append-only + 封板,设备坐标缓存,见 docs/bugfix-triage.md C.3.1)
- `main.cpp`(QPixmapCache 上限 10MB→128MB,配合上面的图层缓存)
- `image/imageloader.cpp`(加载链精简 + Bitmap move)
- `task/imagetask.cpp`(前条件脚本单线程门控 + 固定开启形状边界 off-by-one 修复)
- `preferences/globalpreferences.cpp`(阈值 1024)
- `script/geometrizerengine.h`(end 迭代器 UB 修复)
- `lib/geometrize/.../commonutil.cpp` + `runner/imagerunneroptions.h`(C.1.4 边界修复开关)、
  `exporter/svgexporter.cpp/.h`(exportSVGDocument,供矢量视图复用文档包装)

## 关键约束

- **确定性**:线程数参与结果(seed 分配随 maxThreads),对拍必须固定 `--threads`。
- bestRandomState 的 off-by-one 怪癖(RNG 消费 n+2 次)是可观察行为,原样保留。
- AVX2 手写 SIMD 经插桩实测否决(2026-08-31):误差内核占比 ~30% 未过 40% 启用门槛,
  三口径占比一致,结论存档 ROADMAP §1;P1.4 buffer 池化同批实测负收益一并否决。

## 许可

上游应用 GPL v3(c) Sam Twidale;上游库 MIT(c) Sam Twidale。本分支延续同等许可义务,
见 LICENSES\ 与发布包内 LICENSE.txt。第三方:stb_image.h(public domain)、doctest(MIT,未启用)。
