# Geometrize Improved — 迭代路线图

> 版本基准:2026-08-31 第十二次交付(F3.3 批处理增强落地;P1.4/P1.3 实测盖棺否决,ROADMAP 全清单至此全部处置完毕,本文件已按最终态整顿)
> 下次会话:原 ROADMAP 清单已全部收官(功能轨/质量轨/性能轨/工程轨 + 第十二批 F3.3);可从 §5 的"后续可选方向"挑项或开新方向,动手前查 §7 陷阱速查(14 条)
> 机器基准:i3-13100F(4C8T 全 P 核)/ RX 5700XT / Win11 / VS2022(MSVC 14.44)/ Qt 5.15.2
> 使用约定:**双轨制**——纯性能优化维持 bit-exact 门禁(与上游逐位一致);算法增强做成独立开关(默认关),不破坏验证体系。暂自用,不排开源工程项。

---

## 0. 现状基线(迭代前必读)

| 项 | 状态 | 备注 |
|---|---|---|
| 核心库 | B1-B7 全部落地 + P1.1 金字塔(opt-in) | 内联/memcpy/isqrt 圆/扁平化 polygon/补丁快照/scratch 复用/持久线程池;金字塔=搜索启发式轨道 |
| 交付物 | `dist\Geometrize-Improved\` | 免安装绿色包(与 release exe 同步) |
| 端到端门禁 | `tools\run_ab.ps1` **26 用例矩阵** | 11 个 bit-exact + 15 个 EXPECTED_DIFF(边界修复/增强/区域/分段哨兵);`-BaselineExe` 支持外部基线;FAIL 自动留痕 .raw,单侧无输出自动重试一次(陷阱 #11) |
| 单测门禁 | `src\test\` doctest 双变体 | 89 用例 × fast / 39 × base(ctest);另有 16 个 Model 级用例暂禁用(§4 Q4.5);变体分叉宏分流锁定 |
| 应用层 | 撤销重做 + 区域优先框选 + 增强五勾选框 + 分段颜色 + GIF/PNG 导出参数化 + F3.5 分辨率下拉 + 全量中文化 + 启动优化(网格铺满 0.5s)+ 动态线程 + 批处理增强(F3.3)+ 导出 O(N) 化(F3.7) | patches\qt 0001-0027 已归档 |
| 构建链 | qmake(权威)+ **CMake 并行**(build_cmake.bat 一键含 windeployqt) | Qt6 迁移的硬前置已就绪 |
| 翻译 | 300/300 全中文 | zh.ts 双 context(短名供 uic / 全名供嵌套类 tr) |
| 已知残留 | 偏好文件旧值持久化 | 全局偏好 JSON 里旧字段会被沿用,改默认值时注意用户机已有文件 |

**改动铁律**(每次迭代都适用):
1. 每个 patch 独立、可 revert,落地即跑**双门禁**(ctest 2/2 + run_ab 24/24);
2. 注释只写意图不写历史;
3. 上游怪癖是可观察行为,不得"顺手修":bestRandomState off-by-one(RNG 消费 n+2 次)、`257.0f*255.0f/alpha` 唯一浮点点、drawLines 的 RGB 预计算公式——ellipse_bounds_fork 用例会替你盯着的;
4. bat 必须 CRLF;qmake 构建须在 MSVC x64 环境;qrc 生成在 resources/ cwd 下跑;ChaiScript 保持 pin `2898ae6`;
5. 涉及 UI 配色/新控件,先做预览确认再实施(项目惯例)。

---

## 1. 性能主攻(bit-exact 轨道)

### P1.2 线程池负载均衡 — ✗ 经核验否决(2026-08-29,不再回头)
原预估 1.5~2x。两轮独立代码核验(探索+Plan agent 交叉验证)结论:
1. 爬山阶段是严格串行 RNG 依赖链——第 t+1 次变异的作用对象取决于第 t 次评估结果,接受/拒绝路径零 RNG 消费但改变被变异 state,位精确前提下任务内不可并行;
2. 默认 maxThreads == worker 数,每 worker 恰一个任务,过订阅场景现有共享 FIFO 队列本身已动态均衡;
3. 唯一位精确空间是候选生成阶段(占 21~34%),池化理论收益 1.0~1.1x。"线程间负载差 10 倍+"是单候选级差异,被 52 候选求和的集中效应稀释,1.5~2x 预估不成立。
后续性能优先级转到 P1.1 金字塔(已落地)与 P1.4。

### P1.1 下采样金字塔搜索 — ✓ 已落地(2026-08-29,opt-in)
实际形态与原计划不同:整体做成 opt-in 开关(`pyramidSearch`,默认关),而非 bit-exact 内优化——爬山评估在半分辨率图上进行,最终接受判定仍全分辨率,输出与上游不同属预期。
- 文件:`model.cpp`(半分辨率位图懒维护 m_halfDirty + 金字塔任务分支)、`core.cpp`(bestHillClimbStatePyramid 复制变体)、`bitmap`(downsampleHalf 整数 2×2 box)、`scanline`(downscaleScanlines 行对投影)、`imagerunneroptions`(开关)
- 设计要点与确定性论证:docs/bitwise-equivalence-notes.md §D(整数降采样零浮点、RNG 流不变、每任务私有 halfBuffer、懒重建时序、尺度自洽)
- 实测:1024²×30 步 20.5s→5.2s(**3.95x**,质量 +1.2%);2048²×20 步 160.6s→43.3s(**3.71x**,质量 +2.1%)
- 单测:金字塔确定性两跑一致(单线程+过订阅)、on/off 分叉哨兵、downsampleHalf/downscaleScanlines 纯函数语义锁定(共 +7 用例,fast 侧 49)
- 门禁:run_ab 12/12 PASS(默认关,bit-exact 不破坏)+ ctest 双变体全绿
- 遗留:Qt UI 勾选框透传(下次迭代);custom energyFunction 时金字塔静默忽略(文档已声明)

### P1.4 大对象内存带宽(2048+ 图场景) — ✗ 经实测否决(2026-08-31,第十二批盖棺,不再回头)
两轮核算 + 实测:
1. **原案(胜者 buffer 回写,省二次光栅化+混合)**:step 尾部二次评估只占每步总评估次数 ~0.4%(爬山每步数百次评估 vs 尾部 1 次),且胜者的 lines/混合像素在爬山确定最优后被后续变异覆盖,需跨 core.cpp 三函数加侧信道传三元组——收益 ~0.4% 配结构性改动,不成立;
2. **替代靶点(评估 buffer 池化,免每步每线程全图深拷贝)**:读路径穷查确认理论上可行(内置能量函数对 buffer 只做覆盖区级读写,覆盖区外从不被读),已实现实测——1024²×60 步 ×4 线程**负收益 -2.3%**(14360ms→14686ms,8 连跑均值),2048²×20 步**负收益 -5.4%**(96367ms→101562ms)。深拷贝 4MB 顺序写 warm cache 仅 ~0.4ms/次,8 任务并发占比 <0.7%;负收益来自池间接寻址 cache 代价与池常驻内存(每 Model maxThreads×全图+maxThreads×半图)。
结论:深拷贝不是瓶颈,实验代码已回退,形状数值逐位一致(FINAL_SHA256 与基线一致验证)。

### P1.5 启动与 IO — ✓ 已落地(2026-08-31,qt 0024)
实际形态与原案不同:调研纠偏"148 个 qrc QSvgRenderer"归因——模板全为 JPG+JSON(qrc 内嵌,零 SVG)。
实际瓶颈与改法(零行为变化,不做磁盘缓存):A 铺开节奏(16*i 定时器→批 16 singleShot(0) 分批自调度,
铺满 2.7s→0.5s/-82%)+ D ChaiScript 引擎懒建(templategrid 的引擎移出首帧关键路径,TemplateButton
构造改 provider;首帧 -16%)+ C 单趟扫描(getFirstFileWithAnyExtension,老函数保留——已绑脚本)
+ B 信号链修复(**上游遗留 bug:signal_templateLoaded 从未转发,搜索补全恒空**;签名加 templateName,
补全从空变可用)+ E 启动打点(logStartupTiming → %APPDATA%/startup_timing.log)。
- 文件:`dialog/templategrid.cpp/.h`、`dialog/templatebutton.cpp/.h`、`dialog/launchwindow.cpp`、`common/util.h/.cpp`、`logging/*`、`main.cpp`
- 已知限制:TemplateButton 析构 waitForFinished 在启动后半秒内立刻关窗的病态场景仍阻塞(实测 54ms 退出,无感);绝不能用 QThreadPool::clear() 兜底(future 永不完成死等)。
- 详见 benchmarks/report.md P1.5 节。

### P1.3 AVX2 单 TU 误差内核 — ✗ 经实测否决(2026-08-31,第十二批盖棺,不再回头)
临时插桩实测(#ifdef GEOMETRIZE_TIMING 全局累计器,跑完即删),三口径占比惊人一致:
| 函数 | 1024²×100 步×1线程 | ×4线程 | 2048²×30 步×4线程 |
|---|---|---|---|
| differencePartial | 17.6% | 17.8% | 17.6% |
| computeColor | 12.9% | 12.1% | 12.0% |
| drawLines | 16.4% | 17.1% | 16.5% |
| copyLines | 2.5% | 2.6% | 3.8% |
| rasterizeIntoVector | 0.8% | 0.6% | 0.2% |
判定:computeColor+differencePartial = **~30%,未过 >40% 启用门槛**;且另一半大头 drawLines(17%)不在 AVX2 原方案(误差内核)覆盖范围,B1/B2 后整数循环已被自动向量化,理论再收益 <15% 成立。插桩代码已删除,清理后 FINAL_SHA256 与基线逐位一致。

---

## 2. 画质 / 收敛速度(算法轨道,opt-in 开关,不进 bit-exact 门禁)

**统一开关设计**:ImageRunnerOptions::enhancements(HillClimbEnhancements 结构体)+ geobench CLI 开关;勾选时输出与上游不同属预期,用 geobench --quality-report 曲线验收。增强轨道统一走 `bestHillClimbStateEnhanced`(core.cpp),与金字塔半分辨率评估可组合;经典/金字塔路径零参与。

### A2.2 自适应变异步长 — ✓ 已落地(2026-08-29,--adaptive-step)
1/5 成功法则:连续 8 次拒绝步长减半(上限 ×1/8),连续 4 次接受回升;状态机每候选独立、不挂 Shape(undo 回滚零污染)。步长注入走 Shape::mutateScaled 独立成员(镜像 rasterizeInto 先例),9 形状全链绑定。
- 实测(1024²×60 步):同步数 diff 0.0736→0.0681(**-7.5%**);代价单步 4.4x(爬山链变长,等时间口径不占优)
- 单测:T9-T13(shapemutator 平行序列等价/RNG 流不变/clone 陷阱)+ T1/T4/T8;哨兵 adaptive_step_fork ×2

### A2.3 逐形状最优 alpha 微搜索 — ✓ 已落地(2026-08-29,--alpha-search)
每候选对 {64,128,192}∪{用户 alpha} 升序穷举取最优,胜者档回写 State::m_alpha → step() 落画(SVG/指纹联动)。回写通道 = model.cpp 落画改用 it->m_alpha(m_alpha==外部 alpha 是经典路径不变量,T7 双变体锁定;注:T7 现暂以 #if 0 // CD 禁用,见 test_model.cpp,不变量同时由 run_ab 逐位对拍矩阵承载)。
- 实测(1024²×60 步):同步数 diff 0.0736→0.0669(**-9.1%**);代价单步 5.6x
- **等时间口径的诚实结论**:穷举成本 >质量收益,等预算下反劣。适用场景 = 步数受限的精修;或与金字塔组合时用宽档位(默认三档在自适应步长的形状序列上胜者恒为用户 alpha,组合下退化为无效付费,宽档位 --alpha-tiers 可解)
- 单测:T2/T5/T6/T15 + 哨兵 alpha_search_fork、enhanced_combo_fork

### A2.1 误差图引导形状放置 — ✓ 已落地(2026-08-30,opt-in --error-guide / GUI 勾选框)
实际形态:16×16 块级 L1 误差图(整数缩放 CDF 二分采样)+ "setup 后平移中心"(shiftShapeCenter,9 形状全链)+ ε-greedy(默认 100‰)。全零误差精确退化均匀,零特判。
- 文件:`core/errorweightmap.h/.cpp`(新类)、`core.h/cpp`(errorGuide + errorMap 尾参 + placeShapeByErrorMap)、`shapemutator.h/cpp`(shiftShapeCenter)、`model.cpp`(置脏三点+提交前重建)
- **质量实测(诚实结论):15% 预估不成立**——标准预算(50×100)-2~3%,极端低预算(2×3)-6.7%,与 adaptive 组合 -3.7%。原因:爬山变异链本身是强位置优化器,初始先验被稀释。适用场景 = 低预算快速草图;时间成本 ≈ 0。详见 benchmarks/report.md A2.1 节。
- 测试:ErrorWeightMap 7 用例 + shiftShapeCenter 2 用例 + Model 5 用例;run_ab 3 例 EXPECTED_DIFF 哨兵
- 排障记录:测试基建 DSC 的 rasterize 视口(1<<30)在引导把形状平移出图后失去兜底 → 越界写堆损坏;已对齐位图尺寸。另:引导采样消耗任务内 RNG,跨线程数一致性断言不适用(用例注释已声明)。

### A2.4 分段颜色(行级取色) — ✓ 已落地(2026-08-30,opt-in --segment-colors / GUI 勾选框)
实际形态:每条扫描线独立求最优混合色(computeSegmentColors),形状横跨明暗边界不再发灰。
- 文件:`shaperesult.h`(ScanlineColor + ShapeResult.segments 尾成员)、`core.h/cpp`(segmentColors 开关 + isLineLikeShape + computeSegmentColors + defaultEnergyFunctionSegmented + core 接入点三元)、`rasterizer`(drawLinesSegmented)、`model.h/cpp`(useEnhanced 第四开关 + step 落画两分支 + collectAndDrawWithUndoSegmented + drawShape 三参重载)、`svgexporter`(色带组:stable_sort + 合并段容差 ≤2 + crispEdges)、`shapejsonexporter`(可选 segments 数组)
- 质量实测:100 步 gradnoise **-30.4%**、tree **-56.2%**——增强轨道里收益最大(改变颜色模型本身,非搜索启发式);时间 ~4.5x。详见 benchmarks/report.md A2.4 节。
- 设计裁决:线型形状(全单像素行)不分段(判定按扫描线几何,评估/落画共享谓词);SVG 色带全覆盖替代基元(叠加补丁双重混色不忠实);重放/重做复用 ShapeResult.segments 存档不重算(分段色依赖落画时刻位图);开关开启即路由增强轨道(A2.3 档位穷举恒开,故 line-only 也与 base 分叉——run_ab 用例语义已按此锁定)
- 导出连锁:SVG/PNG/GIF 同步生效(色带组);JSON 加可选字段;Array/网页导出回退单色(文档声明);QtSvg 整数倍缩放无缝已 spike 验证
- 测试:纯函数 6 + 落画/重放 7 + 导出 3 用例(fast 分流);run_ab 3 例(segment_color_fork/threads1/lines_bypass)

### A2.5 新形状类型(可选)
五角星/心形等曲线形状需走"多边形逼近 + scanlinesForPolygon"路径,现成基础设施可用。注意:ShapeTypes 枚举新增位会改变 `SHAPE_COUNT` 与 `--types all=511` 的语义(geobench 与 UI 联动),RNG 选取概率面也变——只做 opt-in,且默认不启用新形状。

---

## 3. 功能增强(用户可见)

### F3.1 GIF/APNG 动图导出增强 ★★(第一批遗留,功能轨第一项)
现状考证:`exportGIF` 链路可用(BurstLinker 已链),但参数全硬编码——`imagetaskexportwidget.cpp:153` 隔帧取样(`frameIdx % 2`)、固定 x3 超采样。导出对话框无任何 GIF 选项。
方案:导出对话框加"每 N 步一帧"选项(默认 2,与现行为一致)+ 帧率;导出时回放 shapes 序列重新渲染帧(只存 shape 列表,内存可控)。
- 文件:`imagetaskexportwidget.ui/.cpp`、`common/uiactions.cpp`(对话框)
- 工作量:2 天。

### F3.2 区域优先绘制 — ✓ 已落地(2026-08-30,lib 0015 + qt 0022)
实际形态:ErrorWeightMap rebuild 加尾参(区域矩形列表 ×factor,块中心命中判定,除数按乘后总权一次安全化);Model::step 第 10 参 + m_lastRegions 变化检测(防 dirty 短路吞区域更新);ImageRunnerOptions.priorityRegions(百分比)+ mapPriorityRegionsToImage 换算;复用 errorGuide 开关(区域非空 = 加权生效,guide 关 = 区域不参与)。
UI:runner 面板"框选优先区域(Ctrl+Drag)"会话态勾选框 + 计数标签 + 清除按钮;Ctrl+拖拽经既有信号链分流(模式关 = 脚本行为逐位不变);SceneManager 半透明 overlay(NoButton 防拦截);区域上限 16,任务级持久化(百分比)。
质量实测:标准预算区域内 -11.4%(全图 +2.7% 几乎无损)、低预算区域内 -7.1%(详见 benchmarks/report.md F3.2 节)。
排障记录:rebuild 两趟分别做命中判定与乘 factor 导致分布失真(区域外也被乘)——重构为单趟判定 + 暂存乘后权重。

### F3.3 批处理增强(任务队列) — ✓ 已落地(2026-08-31,qt 0026,纯应用层 lib 零改动)
实际形态:完成感知(diff-connect,对任意脚本透明)+ 队列持久化 + 自动导出 + 任务栏进度(裁剪决策:QSystemTrayIcon 气泡砍掉,标题 N/M+任务栏进度覆盖通知价值)。
- **完成感知**:`ImageTaskWindow` 新信号 `signal_didStopConditionMet`(仅 stop-condition 命中分支 emit——setShouldKeepStepping(false) 有三调用点,信号不能挪进 setter 否则手动暂停被误判完成);TaskQueueWindow::runScript eval 前快照 getExistingImageTaskWindows、eval 后 diff 接管新窗口,**成功与异常路径都执行**(脚本中途抛错时前面已创建的窗口仍在跑;open_using_every_shape_type 一图 9 窗全收)。connect receiver 必须传 q,任务窗口 WA_DeleteOnClose 销毁时 Qt 自动断连。500ms 轮询扩展:清 dead QPointer(销毁=完成无导出)、窗口标题 Task Queue (N/M)。
- **队列持久化**:QSettings task_queue/ 组(pending_paths/selected_script),每次变更即写,启动恢复;恢复的路径文件可不存在(TaskItemWidget 容忍)。
- **自动导出**:checkable GroupBox(输出目录+Browse+PNG/SVG 勾选);PNG=exportBitmap(getCurrent 最终位图),SVG=exportSVG(getShapes 矢量)+writeStringToFile;命名=源图同名、冲突自动 _2 递增;mkpath 失败 qWarning+状态栏不弹模态框;默认目录 文档/geometrize_batch_output/。
- **任务栏进度**:Win32 原生 COM ITaskbarList3(不引 QtWinExtras);防御式 CoInitializeEx,S_FALSE/RPC_E_CHANGED_MODE 均视为可用且成功才配对 Uninitialize;CoCreateInstance 失败静默禁用;pro 加 -lole32 -luuid、CMake 加 ole32 uuid。
- **翻译**:zh.ts 双 context 6+3 条,qm 已 lrelease(300/300);顺带修复 F3.2 遗留漏翻
  "Priority Regions" 计数标签(三处:cpp 的 QObject::tr 改成员 tr、短名 context 补
  "Priority Regions: 0" 字面量、全名 context 补 "%1" 条目——同文案 cpp tr 与 uic 各走
  一个 context,只见于 cpp 设置文本+ui 初始文本组合的控件)。qm 终态 302/302。
- 已知残留:手动 Stop 不关窗 / 脚本无 stop_condition → 计数悬挂(用户 Stop=主动干预,与上游停止条件语义一致;关窗由轮询兜底)。QPointer 在 Qt5 无 qHash → 受管集合向量线性查找。
- 大图 SVG 光栅化在完成回调同步执行可能卡 UI 数百 ms:每任务一次可接受,不异步化(避免窗口已亡仍导出的生命周期问题)。

### F3.7 导出性能优化 — ✓ 已落地(2026-09-01,qt 0027 + lib 0017,用户 1052276 线 GIF 卡死实测驱动)
实际形态:GIF 导出 O(N²)→O(N) + 帧数封顶 + 异步化 + 通道序修复 + SVG 命名空间修复。
- **增量帧渲染**:`IncrementalFrameRenderer`(gifexporter.cpp)——Bitmap 画布逐帧累积新形状
  光栅化混合(rasterize + drawLines/drawLinesSegmented 与任务落画同一套函数,分段色复用
  ShapeResult.segments 存档),替代旧链路"每帧前缀 SVG 序列化 + QSvgRenderer 整帧重放"
  的 O(N²)(百万形状 = 10^11 级绘制指令,不可完成)。
- **帧数封顶 1000**:超限自动均匀抽稀(末帧必取)——105 万形状 ÷ 每帧 2 本会产出 52 万帧/
  数 GB;PNG 帧序列(exportRasterizedSvgs)同款问题一并修复。
- **导出异步化**:GIF/PNG 序列走 QtConcurrent + QProgressDialog(不可取消,窗口模态)+
  期间禁用窗口;shapes 以 shared_ptr 列表浅拷贝持有,任务销毁不影响导出。
- **通道序修复(GIF 红蓝互换实测)**:BurstLinker reinterpret_cast RGB{r,g,b} 要求小端
  [r,g,b] 内存;addFrame 的 scaled 中转使 QImage 实际按 ARGB32 布局走 makeImageData 时
  红蓝读反。修复 = 输入深拷贝钉死 Format_ARGB32 + makeImageData 按 ARGB 内存序读通道。
- **GIF 控件默认值**:每帧形状数 1-1000(默认 20)、FPS 1-50(默认 20,GIF 延迟下限 20ms
  格式上限 50)、输出倍率默认 3→1。
- **SVG 命名空间修复(lib 0017,上游 bug)**:exportSVG 的 xmlns 误用 https:// 协议头,
  浏览器拒绝渲染退回 XML 文档树;改 http://(Qt QSvgRenderer 宽松,坑只在浏览器直开时现)。
- 观感说明:帧画面从 SVG 抗锯齿重放变为运行时同源扫描线混合,像素与旧路线略有差异;
  GIF 是采样视图,不涉 bit-exact 门禁对象。单帧 PNG(Save Image)只渲染一次,未动。
- 验证:双链路 0 错误;GUI 冒烟 2189 形状弹进度框 UI 不卡;用户实测通过。

### F3.4 撤销/重做 — ✓ 已落地(2026-08-30,qt 补丁 0021,纯应用层 lib 零改动)
实际形态:Ctrl+Z/Ctrl+Y + Edit 菜单 + 运行面板 Undo/Redo 按钮;撤销下限=1(index 0 恒为背景矩形)。
重放走 worker 批量 slot `replayShapes`(一次 willStep 括号包 reset+N 次 drawShape,事件队列 FIFO 与 step 串行);完成走专用信号 `signal_modelDidReplay`(不复用 didStep,避免误触 append/afterAdd 脚本/链式步进);Redo 走正常 drawShape 全协议,新形状到来清 redo 栈(经典语义)。重放零 RNG 消费,撤销后下一步与原轨迹逐位一致。内存内重放天然同库版本,ROADMAP 原警示(存 JSON)不适用于本设计。
- 排障记录:ShapeResult 成员 const 不可赋值,vector 移动赋值与 insert 在 MSVC 下均实例化失败——truncate 用区间构造 + swap(全程无元素赋值)。

### F3.6 导出分辨率增强 — ✓ 已落地(2026-08-30,qt 补丁 0019)
现状考证纠偏:PNG "Save Image" 本就是矢量重放链路(exportRasterizedSvg),且硬编码 ×3。
实际改动:导出面板 PNG 组加"Output Scale"QSpinBox(1-8,默认 3 对齐旧行为),saveRasterizedSVG/saveRasterizedSVGs 读控件值。零 lib 改动、零翻译新增(复用 GIF 组 "Output Scale" 条目)。已知限制:QImage 上限 32767px(4096 源 ×8 越限)。

### F3.9 矢量视图渲染开销(图层数随形状数线性增长)— ✓ 已落地(2026-09-22)
用户实测驱动:"矢量图形视图形状到几百后开始卡,越多越卡"。根因两条叠加:①上游每批新形状新建一个
SvgItem,而每份文档 viewBox 都是整幅画布,于是每个 item 都持有整幅画布的设备坐标缓存位图;
②Qt 全局 QPixmapCache 上限默认 10MB,按整幅画布的设备像素只装得下约 5 个图层,超限后逐帧重渲染。
实际形态:形状按块累积(每块 256,封板后不再重解析,`scene/imagetasksvgscene.cpp`)+ QPixmapCache
上限 10MB→128MB(`main.cpp`)。离屏实测(512×384 图 / 900×600 视口 / 每批 4 形状)5000 形状:
帧耗时 371ms→2.6ms,缓存内存 2.5GB→41MB;10000 形状仍为 4.9ms/82MB(旧实现一侧受内存压力影响
波动较大,同参数重跑 5000 形状 371~1000ms;分块一侧稳定在个位数毫秒)。
- 否决方案:**单 item + 多分块渲染器**(自绘 item 内依次 render 各块渲染器,理论最优:1 张缓存位图
  + 封板块零重解析)。实测画面与上游实现差异单通道最大 184(其余方案 ≤2),在未定位该渲染差异前不采用;
  其刷新耗时仍是 O(N)(单张缓存位图每次失效都要重画全部形状),收益也不足以抵消风险。
- 画面等价性:分块方案与上游实现逐像素比,36% 像素差 1 个通道单位、0.03% 差 2(分层合成多一次 8 位舍入);
  对照实验(阈值放大到不分块)与单 item 方案逐像素相同。
- 残留:块数随形状数线性增长(10000 形状 = 40 块),再往上缓存内存与每帧合成层数仍会涨;
  若要彻底 O(1),下一步是"等大小块二分合并"(log-structured merge,块数 O(log N)、摊还解析 O(N log N)),
  当前规模(万级形状)无必要。
- 基准可复现:`tools/svgscene-bench`(离屏,四方案对比 + 画面等价性 + QPixmapCache 上限对照)

---

## 4. 工程质量

### Q4.2 CMake 化应用构建 — ✓ 已落地(2026-08-31,qt 0025,与 qmake 并行)
实际形态与原案不同:调研纠偏——148 个 qrc 与 29 个 .qm **已预生成在 resources/ 源树**(qm 实测全部比 ts 新),
AUTORCC 的 file(GLOB) 一键收集,**零自定义命令**(推翻原案"资源脚本改为构建前自定义命令");
qrc 生成脚本仅在新增/删除模板时手工重跑。dataslinger 目录直接不编(qmake 下它是空 TU)。
- 文件:`src/improved-app/CMakeLists.txt`(新)、`build_cmake.bat`(新,CRLF,VS 生成器无需 vcvars,
  --parallel 替代 jom,windeployqt 挂尾)、`resources/app_icons/app.manifest`(新:Common-Controls SxS 依赖,
  target_link_options 传 /MANIFESTDEPENDENCY 在 VS 生成器下被空格拆散,manifest 文件是干净解法)
- 关键 flag:/utf-8 /bigobj /Zc:__cplusplus + UNICODE/_UNICODE/_ENABLE_EXTENDED_ALIGNED_STORAGE;
  /permissive- 带上了且 ChaiScript 6.x 编译通过(pin 2898ae6);qtmain 由 Qt5 配置包按 WIN32 属性自动链
- 验证:autogen 产物数 moc 46/ui 22/qrc 148 与 qmake 对齐;VersionInfo/图标/Common-Controls manifest 齐;
  GUI 全中文 + 网格渲染 + 打点日志;--functional_tests exit 0;dist 同步抽测通过
- 已知差异:警告级别 /W3(qmake /W0)多些告警;二进制不同属构建器差异
- 陷阱:bat 里 Python 字符串写 Windows 路径必须 raw string(  会被转义吃掉);构建产物与 qmake 的
  obj/qt_gen 物理隔离(build/ out-of-source);缺 windeployqt 会弹缺 DLL 错误框且进程不退(勿误判为运行中)
- qmake 保留兼容(权威链路);后续可切换主链路

### Q4.4 Qt6 迁移评估 — ✓ 已完成(2026-08-31,报告落地不动手)
全库静态扫描出评估报告:docs/qt6-migration-assessment.md。**结论:可行,~3 天,无阻断项**——
必改仅 1 处 QString::SkipEmptyParts + QtSvgWidgets 模块拆分(svgitem),第三方库零 Qt 依赖(ChaiScript/
cereal/BurstLinker 全部与 Qt 版本无关),CMake 链路(Q4.2)正是 Qt6 硬前置已就绪。主要待办 = 装 Qt 6.5 LTS
+ 高 DPI 强制启用后的 UI 巡检(5.15 下未开高 DPI,Qt6 恒开,非 100% 缩放屏观感全变)。**建议不急**:
待需要新 Qt 特性/新机部署时按报告步骤执行,报告即实施说明书。

### Q4.5 Model 级禁用单测重启用 — □ 跟踪项(2026-09-28 建档)
src\test\test_model.cpp 中 16 个 Model 级用例以 `#if 0 // CD` 禁用(区域优先、分段颜色、四开关组合
确定性、T7 不变量、异常传播/池存活等),其行为当前由 run_ab.ps1 的 EXPECTED_DIFF 哨兵端到端覆盖。
禁用是逐块叠加的(同文件内多组 #if/#endif 配对),重启用前需先清理嵌套再逐用例核对断言口径;
重启用时 README 与本文件的用例数口径从 89/39 改回 105/39。

---

## 5. 推荐执行顺序(下次对话可直接引用)

```
✅ 已完成(第一批,基建+速效)
  Q4.1 单元测试(双变体门禁)→ Q4.3 矩阵扩展 → F3.5 分辨率七档

✅ 已完成(第二批,性能主攻)
  P1.2 核验否决(结论存档)→ P1.1 金字塔搜索落地(opt-in,1024 图 3.95x / 2048 图 3.71x)

✅ 已完成(第三批,质量主攻)
  A2.2 自适应步长 ──→ A2.3 alpha 微搜索(同步数 -7.5%/-9.1%;等时间口径结论见 §2,
  增强轨道基建:统一 bestHillClimbStateEnhanced + geobench 宏守卫修复 + run_ab fastExtra)

✅ 已完成(第五批,2026-08-30:F3.6 + A2.1)
  F3.6 导出倍率(qt 0019,PNG/序列图 Output Scale 1-8 默认 3)──→
  A2.1 误差图引导(lib 0014 + qt 0020;实测标准预算 -2~3%、低预算 -6.7%,15% 预估不成立,
  详见 §2 A2.1 与 benchmarks/report.md;测试基建 DSC 视口越界是堆损坏悬案根因,已修)

✅ 已完成(第六批,2026-08-30:F3.4 撤销重做)
  F3.4 撤销/重做(qt 0021,纯应用层;worker 批量重放 + signal_modelDidReplay 专用信号,
  undo floor=1;ShapeResult 不可赋值 → truncate 走区间构造+swap)

✅ 已完成(第七批,2026-08-30:F3.2 区域优先)
  F3.2 区域优先(lib 0015 + qt 0022;Ctrl+拖拽框选,区域内误差 ×10;标准预算区域内 -11.4%)

═══ 功能轨道收官(2026-08-30)═══
  F3.1 GIF 导出 / F3.2 区域优先 / F3.4 撤销重做 / F3.5 分辨率七档 / F3.6 导出倍率:全清
  → P1.1 落地、P1.2 否决存档、A2.1-A2.3 落地;GUI 面板四勾选框、撤销重做、区域框选全可用
  → 剩余可选项(无优先级承诺,按需挑,动手前查 §7 陷阱速查):
    P1.4 大对象内存带宽(2048+ 场景,1~2 天)
    P1.3 AVX2 误差内核(profile 后决定,预估 <15%)
    A2.5 新形状类型(opt-in,SHAPE_COUNT 联动)
    Q4.4 Qt6 迁移(评估已完成,报告即实施说明书,时机成熟再动手)

✅ 已完成(第八批,2026-08-30:A2.4 分段颜色)
  A2.4 分段颜色(lib 0016 + qt 0023;100 步 -30.4%/-56.2%,增强轨道质量收益最大;
    SVG 色带导出与位图逐像素对齐;重放/重做复用 segments 存档;线型旁路 + run_ab 24 例)

✅ 已完成(第九批,2026-08-31:P1.5 启动优化)
  P1.5 启动优化(qt 0024;网格铺满 2.7s→0.5s/-82%、首帧 -16%;顺带修复上游搜索补全恒空 bug)

✅ 已完成(第十批,2026-08-31:Q4.2 CMake 化)
  Q4.2 CMake 构建(qt 0025;并行链路与 qmake 共存,qrc/qm 预生成零自定义命令,app.manifest 解法)

✅ 已完成(第十一批,2026-08-31:Q4.4 Qt6 迁移评估)
  Q4.4 评估报告(docs/qt6-migration-assessment.md;结论可行 ~3 天无阻断项,建议时机成熟再动手)

✅ 已完成(第十二批,2026-08-31:F3.3 批处理增强 + P1.4/P1.3 实测盖棺)
  F3.3 批处理增强(qt 0026,纯应用层;完成感知 diff-connect + 队列持久化 + 自动导出 PNG/SVG
    + 任务栏进度 COM;QSystemTrayIcon 气泡裁剪;详见 §3 F3.3)
  P1.4 否决(池化实测负收益 -2.3%~-5.4%;原案 ~0.4% 配结构改动不成立;详见 §1)
  P1.3 否决(插桩实测 ~30% 未过 40% 门槛,三口径一致;详见 §1)

✅ 已完成(第十三批,2026-09-01:F3.7 导出性能优化 + F3.3 漏翻修复)
  F3.7 导出性能(qt 0027 + lib 0017;GIF/PNG 序列 O(N²)→O(N) 增量帧 + 帧数封顶 1000
    + 异步导出 + GIF 红蓝互换通道序修复 + SVG 命名空间修复 + GIF 控件默认值;
    用户 1052276 线卡死实测驱动;详见 §3 F3.7)
  F3.3 补充:runner 计数标签 Priority Regions 漏翻修复(并入 0026;三处 context 陷阱)
  → 原清单全部处置后新增功能项(后续可选方向:按需挑,动手前查 §7 陷阱速查):
    A2.5 新形状类型(SHAPE_COUNT/RNG 概率面联动,风险大于收益)
    Q4.4 Qt6 迁移(评估已完成,报告即实施说明书,时机成熟再动手)
    新方向:待定(可考虑 CLI 批处理模式、HTTP API、更多导出格式等应用层扩展)
```

每批结束:双门禁全绿(ctest + run_ab)+ 发布包同步 + 补丁重编号导出 + MEMORY 更新。

---

## 6. 关键技术资产位置(迭代时直接引用)

| 资产 | 路径 |
|---|---|
| 库改进源 | src\improved-lib\geometrize\ |
| 基线上游源 | src\baseline-lib\geometrize\(只读,对拍基准) |
| 基准器 | src\geobench\(main.cpp 双口径哈希 + --shape-bounds + --dump-final) |
| 对拍矩阵 | tools\run_ab.ps1(26 用例定义内嵌,11 bit-exact + 15 EXPECTED_DIFF;-BaselineExe 外部基线) |
| 补丁一致性校验 | tools\verify_patches.py + patches\regen\(规范全量补丁;G1 重放复现 / G2 归档新鲜度;历史拆分系列经实测不可顺序重放——行尾混杂/缺 hunk 头/同文件重复导出,详见 patches\regen\README) |
| 单元测试 | src\test\(doctest 双变体,CMake target 在 src\geobench\CMakeLists.txt,ctest 门禁) |
| 测试图 | src\testdata\images\(gen_test_images.py 可再生) |
| 库补丁 | patches\lib\0001 全量 + 按文件拆分(0012 金字塔/0013 增强轨道/0014 误差图引导/0015 区域优先/0016 分段颜色/0017 SVG 命名空间) |
| 应用补丁 | patches\qt\0001-0027(0026 批处理增强/0027 导出性能,累计 diff 含附注) |
| 等价性论证 | docs\bitwise-equivalence-notes.md |
| bug 分级 | docs\bugfix-triage.md |
| Qt 评估 | docs\qt-app-findings.md |
| Qt6 迁移评估 | docs\qt6-migration-assessment.md(Q4.4,2026-08-31) |
| CMake 构建 | src\improved-app\CMakeLists.txt + build_cmake.bat(qt 0025;产物 build\Release\) |
| 性能报告 | benchmarks\report.md(+ benchmarks\runs\ 每轮 CSV) |
| 发布包 | dist\Geometrize-Improved\(windeployqt 产物,exe 与 release 同步复制) |
| 构建脚本 | src\improved-app\build_qt.bat(D:\tmp\build_qt_all.bat 备份含环境) |

## 7. 已知陷阱速查(新会话最容易踩)

1. **偏好文件旧值持久化**:用户机 global_preferences.json 已有字段不会被新默认值覆盖——改默认值时必须考虑存量文件(线程上限问题即此坑);
2. ChaiScript 三选一:HEAD 编不过 / v6.1.0 编过运行崩 / **pin 2898ae6 可用**;
3. 库源码含中文 → 应用 pro 必须 -utf-8(geobench 的 CMake 已有,勿丢);
4. std::async→线程池已换,勿在 Model 外再开 async(种子语义会漂);
5. ShapeResult 成员 const 不可赋值,vector 操作只能 emplace_back/重建;
6. clone() 必须拷贝全部新增 std::function 成员(9 个形状文件 + shapefactory 双处);
7. PowerShell 脚本要 UTF-8 BOM;给 cmd 的 bat 要 CRLF;**bash 里调 ps1 用正斜杠路径**(`tools/run_ab.ps1`,反斜杠会被吞);
8. 跑长矩阵前先 `Stop-Process -Name geotest-*,geobench-*` 清残留,否则编译期 LNK1104;
9. **Qt 翻译双 context**:嵌套类 Impl 里 `tr()` 的 context 是带命名空间全名(`geometrize::dialog::LaunchWindow`),uic retranslateUi 用短名——同一文案两处用则 ts 两个 context 都要放条目;
10. **写测试的坑**:doctest 比较 shared_ptr 用 `.get()`(直接比较触发 stringification 编译错);DeterministicShapeCreator 的 gridSize 必须 >16(内部 `% (gridSize-16)`,16 会除零);变体分弋试 `GEOTEST_BASE/GEOTEST_FAST` 宏分流,勿"顺手统一"。
11. **编译产物瞬态异常**:0xC0000374 堆损坏若在增量编译后出现且源码回退无效,先 `cmake --build --clean-first` 干净重建再怀疑代码(第八批实测:回退全部新代码仍 18% 崩,干净重建后 220+ 次 0 复现,疑似 obj 不一致)。run_ab 门禁已内建防护(2026-09-28):单侧无输出自动重跑一次,仍无输出记 FAIL 继续跑完矩阵——此前会 InvokeMethodOnNull 中断整个矩阵。
12. **裸形状必须绑 rasterize**:`std::make_shared<Rectangle>(...)` 等不经 shapefactory 的形状,其 `rasterize` std::function 为空,drawShape 调用即 UB/崩溃——照 GUI `drawBackgroundRectangle` 先绑定。椭圆光栅化输出 y 不升序(从中心向两边),导出/统计侧须 stable_sort。
13. **CMake 链专用**:target_link_options 传含空格的链接选项(/MANIFESTDEPENDENCY)会被 VS 生成器拆成假输入文件(LNK1104)——用 .manifest 文件走源列表(app.manifest 先例);bat 里用 Python 写 Windows 路径必须 raw string(`\b`/`\5` 会被转义吃掉);CMake 版 exe 未经 windeployqt 启动会弹缺 DLL 错误框且进程挂着不退,**勿把 HasExited=False 误判为运行正常**(看 startup_timing.log 是否新增)。
14. **QPointer 在 Qt5 无 qHash**:`QSet<QPointer<T>>`/`QHash<QPointer<T>,V>` 编译报 qHash 无重载——受管窗口集合用 `QVector<QPointer<T>>` 线性查找(F3.3 先例);QPointer 作 connect lambda 捕获 + receiver 传宿主窗口,WA_DeleteOnClose 的 sender 销毁时 Qt 自动断连,回调不悬空;嵌套类非 QObject 的 Impl 里 `connect` 是全局五参函数直接可用。
15. **形状边界是排他上界**:`setup`/`mutate`/各 `rasterize` 一律把边界元组的 max 当排他上界消费(内部 `randomRange(xMin, xMax-1)`、`clamp` 到 `xMax-1`、y 过滤在 `[yMin, yMax)`),整幅画布即 `(0, 0, width, height)`。`mapShapeBoundsToImage` 上游返回闭区间 `size-1`,错配使最右列/最下行永不落画(C.1.4);新代码写边界时**别照抄那个 -1**,脚本模板里的 `xMax - 1` 才是对的。
16. **QGraphicsItem 的 DeviceCoordinateCache 吃全局 QPixmapCache 配额**(默认 10MB,约 5 个全画布图层):图层数超配额后每帧重新渲染而非命中缓存,帧耗时量级跳变(0.2ms→70ms)。给场景加 item 前先算图层数 × 单层设备像素;需要更多图层就 `QPixmapCache::setCacheLimit`(应用 main.cpp 已设 128MB)。诊断手法:离屏 `QGraphicsView::render` 计时 + 打印 item 数,见 F3.9。
