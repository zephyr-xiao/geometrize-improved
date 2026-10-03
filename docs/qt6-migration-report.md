# Qt6 迁移落地报告(第十四批,2026-10-03)

> 前身:`docs/qt6-migration-assessment.md`(Q4.4 评估,2026-08-31,只评估不动手)。
> 本报告 = 迁移实施 + 验收证据。结论:**迁移完成,Qt 6.8.3 LTS 单轨切换,行为回归全部落在预期内**。

## 1. 范围与决策(拷问定案)

| 决策点 | 结论 |
|---|---|
| 迁移策略 | 就地切换单轨 Qt6;`geometrize.pro`/qmake 链路保留但不再维护;Qt 5.15.2 安装留盘作应急 |
| 目标版本 | **Qt 6.8.3 LTS**(win64_msvc2022_64);组件 qtbase/qtsvg/qttools/qttranslations + 模块 qtimageformats |
| 验收 | headless 脚本对拍(Qt5 旧包 vs Qt6 新包)+ 真机全功能巡检;不做缩放实验(本机单屏 1080p@100%) |
| 通过标准 | 分层:lib 产物逐字节严格;位图 PNG 逐像素严格;GIF 逐字节;SVG 光栅化量化 |
| 对拍资产 | 入库长期化:`tools/qt_render_ab.py` + 用例 + 冻结参考 |

## 2. 代码改动(4 个文件)

| 文件 | 改动 |
|---|---|
| `src/improved-app/CMakeLists.txt` | `find_package(Qt6 6.8 REQUIRED ... SvgWidgets)` + 链接 `Qt6::*`(QtSvgWidgets 是 Qt6 拆出的模块);WinMain 转发由 `Qt6::Core` 按 `WIN32_EXECUTABLE` 生成式自动链接,未手工加 EntryPoint |
| `src/improved-app/build_cmake.bat` | Qt 版本单点变量 `QT_DIR=D:\Qt\6.8.3\msvc2022_64`;`windeployqt --no-translations`(应用翻译已编进 qrc) |
| `geometrize/dialog/appsplashscreen.cpp` | Qt6 移除了 `QSplashScreen(QWidget*)` 重载 → 改默认构造(编译期唯一报错点) |
| `geometrize/task/imagetask.cpp` | **上游 bug 修复**:worker→task 回传连接(`signal_willStep/didStep/didReplay`)原先硬编码 `BlockingQueuedConnection`,与同步任务的 `DirectConnection` 组合会"同线程阻塞等自己"→ 控制台/脚本模式必死锁(上游示例 `imagejob.chai` 同样挂死)。改为与入向连接同型:DirectConnection 场景全直连,GUI 的 QueuedConnection 路径保持原背压语义 |

评估报告预判的 `QString::SkipEmptyParts` 实际**无需改**——代码自带 `QT_VERSION < 5.14` 版本守卫,
Qt6 走 `Qt::SkipEmptyParts` 分支;其余 Qt5 移除 API 全量扫描(含 setMargin/enterEvent/setCodec/
buttonClicked/SIGNAL/SLOT/QVariant::Type 等)零命中。

## 3. 构建结果

- Qt6 全量构建:**0 错误 0 告警**(/W3);仅 1 处编译错误(appsplashscreen)修复后一次通过。
- 部署产物:`Geometrize.exe` + Qt6Core/Gui/Widgets/Svg/SvgWidgets/Network.dll + 插件集
  (platforms/styles/imageformats/iconengines/generic/tls/networkinformation)+ D3Dcompiler_47.dll;
  未部署 opengl32sw(纯 raster widgets 不需要)与 translations(已内嵌 qrc)——
  相对 Qt5 包少 22.7MB 的 Qt5/ANGLE 残留,包体更精简。
- dist 同步:`dist\Geometrize-Improved\` 已换 Qt6 全量(exe SHA-256 与 build/Release 一致),
  旧 Qt5 DLL/ANGLE/bearer/Qt5 样式插件已送回收站(旧整包备份在 D:\tmp\qt5-dist-backup-20261003.zip)。

## 4. 脚本对拍验收(核心证据)

方法:同一批 headless ChaiScript 用例(`--script_file` 控制台模式,固定种子/线程/参数),
分别在 Qt5 旧包与 Qt6 新包上执行,按分层判据比对产物。工具与用例见 `tools/qt_render_ab/`。

| 用例 | 覆盖路径 | Qt5 vs Qt6 结论 |
|---|---|---|
| 01_bitmap_tiny | 全形状类型 / 单线程 | 位图 PNG 逐像素一致;shape JSON 逐字节一致 |
| 02_alpha_wide | 透明通道(alpha≠255) | 位图 PNG 逐像素一致;shape JSON 逐字节一致 |
| 03_svg_raster | SVG 光栅化(QSvgRenderer,Qt 6.7+ 重写过模块) | **最大通道差 0,差异像素 0/16384** |
| 04_gif | GIF 帧切片累积 + BurstLinker 编码 | 文件逐字节一致 |
| 05_threads_bounds | 固定 4 线程 + 形状边界窗口 | 位图 PNG 逐像素一致;shape JSON 逐字节一致 |
| 06_image_scaling | Qt 图像缩放输入(诊断用例) | **1 LSB / 62.527% 像素**,属预期(见 §5) |

确定性对照:**Qt5 重跑 vs 首跑、Qt6 重跑 vs 首跑均逐位一致**(含 06),应用路径无随机性;
跨版本差异只来自 Qt 侧行为变更,不是随机漂移。

原始比对报告(逐用例逐产物判定与量化值)入库:`benchmarks\runs\qt_render_ab_qt5-vs-qt6_20261003.json`。

## 5. 已知且预期的行为差异(迁移的诚实边界)

1. **Qt 图像缩放(输入阶段)**:`QImage` 平滑缩放在 Qt 5.15 → 6.8 间有 1 个灰阶的取整差异
   (512→256 实测 62.5% 像素差 1 LSB)。该差异经形状链混沌放大后,最终图像与 Qt5 版不再逐位一致。
   触发条件是"图片经过 Qt 缩放"(应用默认处理分辨率上限 1024,超过即缩到限内)。
   性质:Qt 实现的改进型变更,非本项目缺陷,无法也不应"修回";质量分布无变化(误差指标同量级)。
   影响面:仅影响"同一张图在 Qt5/Qt6 版之间"的逐位复现,不影响 Qt6 版自身确定性与质量。
2. **Qt6 强制高 DPI**:本机单屏 1080p@100% 下无观感差异;混合 DPI 多屏场景本机物理上无法覆盖,
   记录为未覆盖项(升级到高分屏机器时按 §7 陷阱清单复查 QGraphicsView 坐标映射与设备坐标缓存)。
3. **qmake 链路冻结**:`geometrize.pro` 保留但不再维护(Qt6 下官方已由 CMake 取代 qmake);
   回退路径 = git revert 迁移提交 + 盘上 Qt 5.15.2 重建。

## 6. 门禁与交付状态

| 项 | 状态 |
|---|---|
| ctest(双变体) | 2/2 PASS |
| verify_patches(双门禁 G1/G2) | 全 PASS(app 侧补丁已按迁移原因重导出,含 0028 归档) |
| run_ab(26 用例矩阵) | **26/26 PASS**(迁移后终检,报告 benchmarks\runs\report_20261003_223806.csv;库侧零改动,零扰动确认) |
| 脚本对拍 | 见 §4 |
| dist 包 check | 通过(交付包 exe 对冻结参考逐项一致) |
| GUI 冒烟(dist 包) | 启动正常(窗口 Geometrize 1.0.1,响应中,干净退出) |
| 真机全功能巡检 | **已通过(2026-10-03,用户实测)**:启动/模板网格、运行(默认+增强勾选)、撤销重做、区域框选、脚本控制台、导出 PNG/序列/SVG/GIF、批处理队列、偏好/语言、任务栏进度、关闭——全项无异常 |

## 7. 复现方式(下次升级 Qt 版本时照抄)

```powershell
# 1) 装 Qt(aqtinstall,阿里云镜像;qtimageformats 是模块,qtsvg 是 archive——Qt 的坑)
python -m aqt install-qt -O D:/Qt -b https://mirrors.aliyun.com/qt windows desktop 6.8.3 win64_msvc2022_64 `
    --archives qtbase qtsvg qttools qttranslations -m qtimageformats
# 2) 构建 + 部署(改 build_cmake.bat 顶部 QT_DIR 即可换版本)
cd src\improved-app; .\build_cmake.bat
# 3) 对拍验收(旧版本包 vs 新构建;输出必须落工作区内,沙箱下应用进程写不了工作区外)
python tools\qt_render_ab.py run --exe <旧包 exe> --out .tmp_qt_render_ab\ref-<旧版本>
python tools\qt_render_ab.py run --exe src\improved-app\build\Release\Geometrize.exe --out .tmp_qt_render_ab\cand-<新版本>
python tools\qt_render_ab.py compare --ref-dir .tmp_qt_render_ab\ref-<旧版本> --cand-dir .tmp_qt_render_ab\cand-<新版本>
# 4) 冻结核准参考(验收通过后,长期门禁用)
python tools\qt_render_ab.py accept --exe src\improved-app\build\Release\Geometrize.exe
```

## 8. 遗留与后续可选

- 对拍矩阵可扩展:分段颜色/区域优先/增强开关当前未进脚本 API(仅 GUI 勾选),如需覆盖可在
  `ImageTaskPreferences` 脚本绑定补 setter(属应用层增强,另开批次)。
- Qt 6.8 LTS 之外的新补丁(troll 商业专享,开源用户拿不到):若将来需要更新的 Qt,走 §7 流程即可。
