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
- Qt 应用层:pro 缺 concurrent 模块、ChaiScript 需 pin 上游 commit(HEAD 在 MSVC 14.4x 下编不过)、
  步进刷新 33ms 合帧节流、SVG item 设备缓存、无 timed-update 脚本不启 100ms 轮询、
  加载链去三次整图拷贝、前条件脚本数据竞争强制单线程、默认缩放阈值 256→1024

## 目录结构

```
├─ dist\Geometrize-Improved\   # 免安装发布包(双击即用)
├─ upstream\                    # 上游只读快照(lib + app + 全部子模块)
├─ patches\lib\                 # 核心库编号补丁系列(apply 到 fresh clone 即得改进版)
├─ patches\qt\                  # 应用层改动(见下"应用构建")
├─ src\
│   ├─ baseline-lib\           # 上游库快照(只读,对拍基准)
│   ├─ improved-lib\           # 全部补丁后的库(geobench-fast / geotest-fast 链接它)
│   ├─ geobench\               # CLI 基准器 + CMake 统一构建入口(SHA-256 + FNV 滚动指纹双口径对拍)
│   ├─ test\                   # doctest 单元测试(双变体链接,41 用例 × 2,见"复现")
│   └─ improved-app\           # 应用改进工作树(含改进版库 + 全部应用层 patch,可 qmake 构建)
├─ tools\run_ab.ps1             # A/B 对拍矩阵(12 用例,全 PASS 才允许合入;含 1 个 EXPECTED_DIFF 分叉锁定)
├─ benchmarks\report.md         # 性能报告(分阶段数据 + 大图可行性)
└─ docs\                        # 等价性论证 / bug 分级 / Qt 评估
```

## 复现

```powershell
# 核心库基准与对拍(CMake ≥3.20 + MSVC)
cd src\geobench
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
powershell -ExecutionPolicy Bypass -File tools\run_ab.ps1   # 12/12 PASS(含 1 个 EXPECTED_DIFF 分叉锁定用例)
# geobench 额外参数:--shape-bounds x1,y1,x2,y2(百分比视口)、--dump-final PATH(位图留痕)
# 算法增强轨道:--pyramid(金字塔搜索)、--adaptive-step(自适应步长)、--alpha-search( alpha 档位搜索)、
#   --quality-report N(每 N 步打印相似度分数)
# run_ab.ps1 可加 -BaselineExe <exe> 对拍任意外部基线,失败自动保存双方 .raw 供 diff

# 单元测试(双变体:geotest-base 链上游快照 / geotest-fast 链改进版,65 用例 × fast / 41 × base)
cmake --build build --config Release --target geotest-base geotest-fast
ctest --test-dir build -C Release --output-on-failure        # 2/2 PASS
# 共享 golden 用例双变体全绿 = 函数级 bit-exact 门禁;
# 变体分叉点(Ellipse 边界笔误修复、scanlines 末像素、move 构造、异常重抛、线程池保序)
# 用 GEOTEST_BASE / GEOTEST_FAST 宏各自锁定行为。
# golden 常量重采集:build\Release\geotest-fast.exe --dump-golden(输出粘贴进 src\test\test_goldens.h)

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
- `scene/imagetasksvgscene.cpp`(DeviceCoordinateCache)
- `image/imageloader.cpp`(加载链精简 + Bitmap move)
- `task/imagetask.cpp`(前条件脚本单线程门控)
- `preferences/globalpreferences.cpp`(阈值 1024)
- `script/geometrizerengine.h`(end 迭代器 UB 修复)

## 关键约束

- **确定性**:线程数参与结果(seed 分配随 maxThreads),对拍必须固定 `--threads`。
- bestRandomState 的 off-by-one 怪癖(RNG 消费 n+2 次)是可观察行为,原样保留。
- AVX2 手写 SIMD 经插桩实测否决(2026-08-31):误差内核占比 ~30% 未过 40% 启用门槛,
  三口径占比一致,结论存档 ROADMAP §1;P1.4 buffer 池化同批实测负收益一并否决。

## 许可

上游应用 GPL v3(c) Sam Twidale;上游库 MIT(c) Sam Twidale。本分支延续同等许可义务,
见 LICENSES\ 与发布包内 LICENSE.txt。第三方:stb_image.h(public domain)、doctest(MIT,未启用)。
