# Qt6 迁移评估报告(Q4.4,2026-08-31)

> 定位:**只评估不动手**(ROADMAP 原案)。结论先行:可行,工作量约 3~5 天,无阻断性障碍;
> 建议在 Qt 6.5 LTS 上落地,与 Q4.2 的 CMake 链路配合执行;qmake 链路不迁移。

---

## 1. 现状基线

| 项 | 现状 |
|---|---|
| 当前版本 | Qt 5.15.2(msvc2019_64),本机无 Qt6 安装(评估基于源码静态扫描 + 官方迁移文档) |
| 模块依赖 | Core / Gui / Widgets / Svg / Network / Concurrent(Qt6 下 Svg→需加 SvgWidgets) |
| 代码基数 | 应用 81 cpp + 87 h;geometrize 库副本 28 cpp + 31 h;burstlinker 18 cpp + 21 h |
| 构建链 | CMake(第十批已就绪,`find_package(Qt5 5.15 ...)` 一处即改点);qmake 保留但不迁移 |
| ChaiScript | pin 2898ae6,纯 C++ 头文件库,与 Qt 版本无关 |

## 2. 代码敏感面盘点(全库静态扫描结果)

### 2.1 必改项(编译不过的)

| 点 | 位置 | 改法 | 量 |
|---|---|---|---|
| `QString::SkipEmptyParts` | localization.cpp:139(另一处 :90 已是 `Qt::` 前缀) | 改 `Qt::SkipEmptyParts` | 1 处 |
| `QGraphicsSvgItem` / `QSvgWidget` | scene/svgitem.h(.ui 的 promoted widget 若用 QSvgWidget 同改) | Qt6 拆到 **QtSvgWidgets** 模块:CMake 加 `find_package(Qt6 ... SvgWidgets)`,include 改 `<QGraphicsSvgItem>` 不变但链接 `Qt6::SvgWidgets` | 2~3 处 |
| `.pro` 的 Qt6 分支 | geometrize.pro:3-5(qmake 链路) | 不迁移 qmake,无需动 | — |

### 2.2 行为差异项(编译过但要验证的)

| 点 | 影响 | 处置 |
|---|---|---|
| **高 DPI 缩放强制启用** | Qt6 移除 AA_EnableHighDpiScaling 开关(恒开)。main.cpp 未设置任何 AA_ 属性(已核实),5.15 下默认未启用高 DPI → **Qt6 后非 100% 缩放的屏幕上窗口/坐标观感全变** | 需要在混合 DPI 多显示器上做一轮 UI 巡检;scene/graphics 的 68 处使用点重点查(坐标映射、DeviceCoordinate 缓存) |
| `QtConcurrent::run` 返回类型 | Qt6 返回 `QFuture<T>`(未 wait 时调用 result() 行为有差异;QFutureWatcher 模式不变) | templatebutton.cpp:60 与 taskitemwidget.cpp:49 两处都是 watcher 模式,预期无感;验证加载与关窗 |
| QPainter 枚举/默认值 | Qt6 下若干默认值微调(Antialiasing 行为、pixmap 转换) | 全量目检渲染输出(scene 渲染、导出位图) |
| `event.type()`(QEvent) | imagetaskgraphicsview.cpp:21 用 `event.type()`——Qt6 无变化,仅风格 | 无需动 |
| 事件坐标(QGraphicsSceneMouseEvent 等 17 处) | Qt6 的 hover/scene 坐标 API 未变 | 巡检即可 |
| Windeployqt 参数 | Qt6 需 `--no-translations` 保持 dist 精简(qt_*.qm 已内嵌) | bat 尾部一行 |

### 2.3 已确认的"零命中"清单(扫过,无需担心)

`QRegExp` / `QStringRef` / `qrand` / `toTime_t` / `QTextCodec` / `QDesktopWidget` / `QApplication::desktop` / `QSignalMapper` / `QLinkedList` / `QMatrix` / `setMatrix` / `QPainter::HighQualityAntialiasing` / `QVariant::type()` / `toSet()` / `QMap::unite` / `endl`(std:: 外)/ `AA_EnableHighDpiScaling`——**全部零命中**。

### 2.4 第三方依赖

| 库 | Qt6 兼容性 |
|---|---|
| ChaiScript(pin 2898ae6) | 纯 C++17,与 Qt 无关,**零影响** |
| cereal | 纯 C++,零影响 |
| BurstLinker | **零 Qt 依赖**(GIF 编码纯 STL,已核实 lib 内无 QImage/QColor include) |
| dataslinger | 不编译(option OFF),零影响 |

## 3. 工作量与步骤(建议,若决定迁移)

1. 装 Qt 6.5 LTS(msvc2019_64 或 msvc2022)→ `D:\Qt\6.5.x\msvc2019_64`
2. CMakeLists 抽 `QT_VERSION_MAJOR` 变量,Qt6 分支加 SvgWidgets(半天)
3. 2.1 表的 3~4 处必改(1~2 小时)
4. 全量编译 + 警告清理(/W3 下 Qt6 头的新告警,半天)
5. 行为验证:全套人工清单 + P1.5 打点对比 + run_ab/ctest(lib 与 Qt 无关应零扰动)(半天)
6. 高 DPI 巡检 + 观感回归(混合 DPI 屏)(半天)
7. build_cmake.bat 加 Qt6 路径参数化(Qt5/Qt6 双开)(1 小时)

**合计 ~3 天**。前置条件:CMake 链路(已就绪 ✓)、Qt6 安装(缺,需下载 ~2GB)。

## 4. 结论与建议

- **技术可行性:高**。代码库对 Qt6 友好(敏感面仅 1 处 SkipEmptyParts + SvgWidgets 模块拆分),第三方库零阻碍,CMake 化已完成(正是 Qt6 的硬前置)。
- **收益**:Qt6.5 LTS 支持到 2027+;高 DPI 原生支持(本机高分屏观感提升);ChaiScript/C++17 工具链不动。
- **成本**:~3 天 + 一次全量 UI 回归;**qmake 链路废弃**(Qt6 下 qmake 已被 CMake 取代,正好 Q4.2 后我们已有 CMake)。
- **建议**:不急。当前 Qt 5.15.2 支持到 2025-10(EOL 已过但自用无安全面——应用不处理不可信输入)。待下一次"需要新 Qt 特性/新机器部署"时顺手做,届时按本文步骤执行即可;本报告即实施说明书。
