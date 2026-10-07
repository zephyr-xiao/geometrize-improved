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

**审查遗留清理(第十五批,2026-10-06)**——§4.6 跟踪项的 C 组大头清账(复现证据与论证见 ROADMAP §5/§7 陷阱 #23):
- **导出尺寸护栏**:Qt6/64 位 QImage 无固定单边上限,内存压力下 `scaled` 返回空图会让 GIF 量化器
  按 init 尺寸读 0 像素缓冲(堆越界读,守护页实测 0xC0000005);GIF 头部宽高 uint16 超 65535 静默截断。
  防线 = 导出面板三入口(PNG 单帧/序列/GIF)预检弹窗(宽×高×4 ≤2GB、GIF 每边 ≤65535)+
  `gifexporter` 内部空图检查/尺寸护栏 + 帧失败接线(删除半截文件返回 false)。
- **批处理导出命名防覆盖**:纯内存计数器改"内存∨磁盘"取首个空闲名,重启后重跑同名批次不再覆盖旧导出。
- **队列移除回调延迟删除**:原代码在 itemWidget 自己的信号栈内 `delete item`(销毁 sender),改延迟执行。
- 顺手:16 区域上限状态栏提示、脚本下拉 `operator[]` 读副作用、`test_goldens.h`/`--dump-golden`
  死机制删除、geobench `--dump-final` 改 .part 原子写、`sha256.h` 补 include、
  `gen_test_images.py` 补上游资产复制(`tree_under_clouds.png`)。
- 验证:四道门禁全绿(ctest 2/2、run_ab 26/26、verify_patches、qt_render_ab check 含 GIF 字节级一致)。

**序列导出告知与确认(第十六批,2026-10-07)**——真机回归反馈驱动:
- **根因**:中文界面 `Save Image` / `Save Images` 同译"保存图像",第二个按钮会把渲染过程每步各存一张
  PNG(`exported_image_<i>.png`,最多 1000 张、超过均匀抽稀)直接铺进所选目录,目录选择框标题也只有
  "保存图像",全程无提示无确认(用户实测把整批序列图存到了桌面)。
- **二次确认**:选目录后、写盘前弹确认框,明确写出"每步一张"、实际张数与目标目录;默认按钮与 Esc
  都落在"否";张数与写盘循环共用同一公式(新增 `exporter::exportedFrameCount`,消除两处漂移);
  0 形状不弹"将写入 0 张"的空确认。
- **文案**:按钮中文改"保存序列图像"、目录选择标题改"选择序列图像导出目录"、按钮加 tooltip、
  进度框带张数;英文 source 字符串未动(其他 28 个语言的翻译不失效)。
- **Qt 标准按钮中文化**:资源里中文只有 Qt5 时代目录(无 `QPlatformTheme` 上下文),Qt6 下标准按钮
  OK/Yes/No 全回退英文;补入 Qt 6.8.3 的 `qtbase_zh_CN.qm`(存为 `qtbase_zh.qm` + `qtbase_zh_CN.qm`,
  对齐其余 20 个语言的 `qtbase_<lang>.qm` 惯例),确认框按钮显示"是/否"、其余对话框"确定/取消"同步转中文。
- 验证:ctest 2/2、run_ab 26/26、verify_patches 重导出 PASS;真机(Computer Use 冒烟 + 用户实测复核):
  28 形状 → 弹窗报 28 张 → 目录落盘 28 个 PNG 一一对应;Esc/"否"路径零写盘;尺寸护栏补测
  (4096×4096 图 + 输出倍率 8 → 弹出"导出尺寸超限")与确认框/中文化按钮等全项通过。

**大图评估内核融合(第十七批,2026-10-07)**——真机反馈"4096 画布单步几十秒"驱动(诊断与实测见 benchmarks/report.md):
- **诊断**:4096² 下每次候选评估平均扫描 **161 万像素**(约为画布的 10%),单步约 9000 次评估;
  插桩定量显示瓶颈是**逐像素整数算术**(混合+差分约 50 条指令/像素),不是中间位图的内存往返。
  基线:改进版 64.4 s/步(上游原版 163.0 s/步)。
- **改动(位精确,输出零变化)**:新增 `core::defaultEnergyFunctionFused`——内置默认能量函数把
  "混色 → 差分"两遍扫描并为**一遍只读扫描**(混合值不写 scratch buffer,逐像素现算),
  且逐像素混合换成 **4×256 项通道查找表**(表值用与 drawLines 完全相同的表达式生成,含 uint32 回绕语义);
  内置评估路径(经典+金字塔)切换过去,并省掉每线程每步的 scratch 整图拷贝;
  `defaultEnergyFunction` 原样保留给脚本绑定/自定义能量函数。
- **实测(STEP_FINGERPRINT 与改动前逐位相同)**:4096² 无金字塔 64.4 → **36.4 s/步(1.77x)**;
  4096² + 金字塔 14.3 → **8.5 s/步(1.69x)**;2048² 10.6 → **5.5 s/步(1.93x)**;
  4096² 相对上游原版 163.0 → 36.4 s/步(4.48x,含历年优化)。
- **大图建议**:4096 场景勾选运行面板"**金字塔搜索(快速)**"可再得约 4.5x(算法增强,输出与关闭时不同属预期);
  两者叠加相对改动前共约 7.6x。
- 门禁:ctest 2/2(新增融合一致性单测 72 断言)、run_ab 26/26(golden 全 OK)、verify_patches 重导出 PASS。
- 后续可选项:融合遍再做 SIMD(当前 6~7 ns/px 仍算术主导,预估还有 1.5~2x);增强轨道(分段颜色/自适应步长)按同模式融合。

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
│   └─ improved-app\           # 应用改进工作树(含改进版库 + 全部应用层 patch;Qt6 迁移后走 CMake,见"复现")
├─ tools\run_ab.ps1             # A/B 对拍矩阵(26 用例:11 bit-exact + 15 EXPECTED_DIFF 分叉哨兵)
│                               #   + tools\goldens.csv 值级锁定(双端双口径哈希逐值比对)
├─ tools\goldens.csv            # 对拍 golden:只断言"与 base 不同"抓不到"分叉的值变了",值级锁定靠它
├─ tools\qt_render_ab.py        # Qt 渲染侧对拍(Qt 升级/应用层渲染改动时的验收工具,见 tools\qt_render_ab\README.md)
├─ tools\qt_render_ab\cases\    #   用例脚本(headless ChaiScript,固定种子/线程)
├─ tools\qt_goldens.csv         #   Qt 侧冻结参考哈希(严格项)+ tools\qt_render_ref\ 光栅化参考图
├─ tools\verify_patches.py      # 补丁↔工作树一致性双门禁(重放复现 + 归档新鲜度;lib 失败 exit 1,app 失败 exit 2)
├─ tools\svgscene-bench\        # 矢量视图渲染开销离屏基准(四方案对比 + 画面等价性,见 F3.9)
├─ benchmarks\report.md         # 性能报告(分阶段数据 + 大图可行性)
└─ docs\                        # 等价性论证 / bug 分级 / Qt 评估与 Qt6 迁移报告
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
# (注:函数级等价由双变体行为断言承载,端到端值级锁定由 tools\goldens.csv 承载)
# 注:Model 级用例在 Debug 构建下会被 drain 里的 assert(0) 中断(异常传播用例属预期路径),
#    门禁一律用 Release(NDEBUG)跑

# 应用构建(Qt 6.8.3 LTS win64_msvc2022_64;Qt6 迁移后 CMake 是唯一在维护的链路,qmake/.pro 保留但不再维护)
python -m aqt install-qt -O D:/Qt -b https://mirrors.aliyun.com/qt windows desktop 6.8.3 win64_msvc2022_64 `
    --archives qtbase qtsvg qttools qttranslations -m qtimageformats
cd src\improved-app
.\build_cmake.bat              # 配置+构建+windeployqt 一键(换 Qt 版本只改顶部 QT_DIR);产物 build\Release\Geometrize.exe
# dist 不会随构建自动更新,交付前必须同步并比对哈希(陷阱 #18):
copy build\Release\Geometrize.exe ..\..\dist\Geometrize-Improved\
# 本机另有坑:工作区内新建/更新的 exe 会继承 dsh 沙箱的 Low 完整性标签,双击运行时
# 无法另存为到工作区外(误报"没有权限")——交付/复测前跑一次(每次重建后都要):
python tools\fix_integrity_label.py        # 见 docs\ROADMAP.md 陷阱 #24
# 回退路径:git revert 迁移提交 + 盘上 Qt 5.15.2 重建(评估与实施细节见 docs\qt6-migration-report.md)

cd ..\..
# Qt 渲染侧对拍(升级 Qt / 改动应用层渲染时的验收;判据分层与用法见 tools\qt_render_ab\README.md)
# 注意:应用 exe 若带继承来的 Low 完整性标签(新建/重建后默认如此),写工作区外路径会被拒(导出静默失败);
#      跑 python tools\fix_integrity_label.py 修复标签后即无此限制(见 docs\ROADMAP.md 陷阱 #24)
python tools\qt_render_ab.py run     --exe <旧版本包 exe> --out .tmp_qt_render_ab\ref
python tools\qt_render_ab.py run     --exe src\improved-app\build\Release\Geometrize.exe --out .tmp_qt_render_ab\cand
python tools\qt_render_ab.py compare --ref-dir .tmp_qt_render_ab\ref --cand-dir .tmp_qt_render_ab\cand
python tools\qt_render_ab.py check   --exe src\improved-app\build\Release\Geometrize.exe   # 长期门禁:对冻结参考校验
python tools\qt_render_ab.py accept  --exe src\improved-app\build\Release\Geometrize.exe   # 验收通过后重采参考
```

应用层改动集中在这些文件(相对上游):
- `CMakeLists.txt` + `build_cmake.bat`(Q4.2 引入;Qt6 迁移后为唯一在维护的构建链,见 patches/qt/0025/0028)
- `geometrize.pro`(concurrent 模块 + /utf-8;Qt6 后冻结不再维护)
- `dialog/appsplashscreen.cpp`(Qt6 移除 QSplashScreen 的 QWidget* 父参重载,改默认构造,见 patches/qt/0028)
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
- **Qt 版本**:Qt 6.8.3 LTS(msvc2022_64);Qt 侧行为差异(Qt 图像缩放 1 LSB、强制高 DPI)与
  验收证据见 docs\qt6-migration-report.md;Qt 渲染侧回归用 tools\qt_render_ab.py 对拍/校验。
- bestRandomState 的 off-by-one 怪癖(RNG 消费 n+2 次)是可观察行为,原样保留。
- AVX2 手写 SIMD 经插桩实测否决(2026-08-31):误差内核占比 ~30% 未过 40% 启用门槛,
  三口径占比一致,结论存档 ROADMAP §1;P1.4 buffer 池化同批实测负收益一并否决。

## 许可

上游应用 GPL v3(c) Sam Twidale;上游库 MIT(c) Sam Twidale。本分支延续同等许可义务,
见 LICENSES\ 与发布包内 LICENSE.txt。第三方:stb_image.h(public domain)、doctest(MIT,未启用)。
