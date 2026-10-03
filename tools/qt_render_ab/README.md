# qt_render_ab — Qt 渲染侧对拍工具

用途：用**无界面脚本**驱动应用（`--script_file` 控制台模式）跑固定用例，对导出物按分层判据比对。
两个场景：

1. **双 exe 对比（迁移验收）**：同一批用例分别在两个版本 exe 上跑（如 Qt5 旧包 vs Qt6 新包），
   验证换 Qt 版本没有改变应用行为；
2. **冻结参考回归（长期门禁）**：当前 exe 对 `tools/qt_goldens.csv` + `tools/qt_render_ref/`
   冻结参考校验，改应用层渲染代码或再升 Qt 版本后可重跑。

## 判据分层（与验收约定一致）

| 产物 | 类别 | 判据 |
|---|---|---|
| `*.json`（形状 JSON） | `bytes-json` | 逐字节严格（lib 导出，与 Qt 无关） |
| `*.gif` | `bytes-gif` | 逐字节严格（帧画面由位图切片累积，BurstLinker 纯 STL 编码） |
| `*.bitmap.png` | `png-strict` | 解码后逐像素严格（位图直转 QImage，无绘制参与；文件字节受 PNG 编码器版本影响，不作判据） |
| `*.raster.png` | `png-tolerant` | 走 QSvgRenderer 光栅化（Qt 6.7+ 重写过 SVG 模块）→ 量化：最大通道差 ≤ 16 且差异像素占比 ≤ 2%（阈值在 `tools/qt_render_ab.py` 顶部 `TOL_*`） |

## 用法

```powershell
# 只运行用例（产物留在输出目录供人工检查）
python tools\qt_render_ab.py run --exe <exe路径> [--out <目录>] [--cases 01,03]

# 双 exe 对比：先分别 run 出两个目录，再 compare
python tools\qt_render_ab.py run --exe dist\Geometrize-Improved\Geometrize.exe --out D:\tmp\qt_render_ab\qt5-ref
python tools\qt_render_ab.py run --exe src\improved-app\build\Release\Geometrize.exe --out D:\tmp\qt_render_ab\qt6-cand
python tools\qt_render_ab.py compare --ref-dir D:\tmp\qt_render_ab\qt5-ref --cand-dir D:\tmp\qt_render_ab\qt6-cand [--json <报告.json>]

# 长期门禁：当前 exe 对冻结参考校验 / 重采参考（重采需人工确认输出合理）
python tools\qt_render_ab.py check  --exe <exe>
python tools\qt_render_ab.py accept --exe <exe>
```

退出码：`0` 全部一致/在容差内；`1` 严格项存在差异；`2` 用例运行失败（看输出目录的
`*.status.txt` / `*.run.log`）；`3` 光栅化差异超容差或出现参考里没有的新产物。

## 用例与产物

用例脚本在 `cases/`，`@IN@`（测试图目录 `src/testdata/images`）与 `@OUT@`（本次输出目录）
由驱动替换。当前矩阵：

| 用例 | 覆盖路径 | 产物 |
|---|---|---|
| `01_bitmap_tiny` | 全形状类型 / 固定种子 / 单线程 | `*.bitmap.png` + `*.json` |
| `02_alpha_wide` | 透明通道（alpha≠255） | `*.bitmap.png` + `*.json` |
| `03_svg_raster` | SVG 光栅化（2× 放大） | `*.raster.png` |
| `04_gif` | GIF 帧切片累积 + BurstLinker 编码 | `*.gif` |
| `05_threads_bounds` | 固定 4 线程 + 形状边界窗口 | `*.bitmap.png` + `*.json` |

## 已知约束（踩过的坑）

- exe 是 GUI 子系统程序：**脚本一旦抛异常，runScript 会弹"脚本评估失败"模态框挂死**
  （表现为进程不退、`MainWindowTitle` 有标题）。因此：用例脚本一律用 `try/catch` 落
  `*.status.txt`；驱动带 `RUN_TIMEOUT_SEC` 超时强杀；调用脚本 API 前先确认函数**确实已注册**
  （例如 `createImage` 未注册、`exportSVG` 因缺省参数不可直接调）。
- `SynchronousImageTask` 的步进依赖 DirectConnection 全链路（回传信号也是 Direct），
  否则在脚本路径死锁——见 ROADMAP §7 陷阱速查。
- 线程数参与种子分配：对拍必须显式固定 `setMaxThreads`。
- 依赖 Python + Pillow + numpy（本机已装）。
