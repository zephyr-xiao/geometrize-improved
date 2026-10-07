# Geometrize Improved — 迭代路线图

> 版本基准:2026-10-07 第十七次交付(**大图评估内核融合**:内置能量函数"混色→差分"融合为一遍只读扫描 + 混合查找表,4096 单步 1.77x,位精确)
> 下次会话:§4.6 仅剩有意搁置项(见该节第 7 条"仍有意未做");§5 尾部"后续可选方向"仍无优先级承诺,动手前查 §7 陷阱速查(26 条)
> 机器基准:i3-13100F(4C8T 全 P 核)/ RX 5700XT / Win11 / VS2022(MSVC 14.44)/ **Qt 6.8.3 LTS(msvc2022_64)**
> 使用约定:**双轨制**——纯性能优化维持 bit-exact 门禁(与上游逐位一致);算法增强做成独立开关(默认关),不破坏验证体系。暂自用,不排开源工程项。

---

## 0. 现状基线(迭代前必读)

| 项 | 状态 | 备注 |
|---|---|---|
| 核心库 | B1-B7 全部落地 + P1.1 金字塔(opt-in) + **P1.6 评估内核融合(第十七批,位精确)** | 内联/memcpy/isqrt 圆/扁平化 polygon/补丁快照/scratch 复用/持久线程池;金字塔=搜索启发式轨道;内核融合=融合为一遍只读扫描 + 4×256 混合查找表(4096 单步 1.77x,输出零变化) |
| 交付物 | `dist\Geometrize-Improved\` | 免安装绿色包(与 release exe 同步);第十四批起为纯 Qt6 包(无 Qt5/ANGLE 残留) |
| 端到端门禁 | `tools\run_ab.ps1` **26 用例矩阵** | 11 个 bit-exact + 15 个 EXPECTED_DIFF(边界修复/增强/区域/分段哨兵);`-BaselineExe` 支持外部基线;FAIL 自动留痕 .raw,单侧无输出自动重试一次(陷阱 #11) |
| 单测门禁 | `src\test\` doctest 双变体 | 111 用例 × fast / 39 × base(ctest);Model 级哨兵已全部启用(2026-09-29,原 16 个 `#if 0 // CD`);变体分叉宏分流锁定 |
| Qt 渲染侧门禁 | `tools\qt_render_ab.py` + `tools\qt_goldens.csv` + `tools\qt_render_ref\` | 第十四批新增:headless 脚本对拍(6 用例,覆盖位图/透明/多线程/边界/GIF/SVG 光栅化);升级 Qt 或改应用层渲染后 `check` 对冻结参考校验 |
| 应用层 | 撤销重做 + 区域优先框选 + 增强五勾选框 + 分段颜色 + GIF/PNG 导出参数化 + F3.5 分辨率下拉 + 全量中文化 + 启动优化(网格铺满 0.5s)+ 动态线程 + 批处理增强(F3.3)+ 导出 O(N) 化(F3.7)+ Qt6 迁移(第十四批)+ 审查遗留清理(第十五批)+ **序列导出告知与确认(第十六批)** | patches\qt 0001-0030 已归档 |
| 构建链 | **CMake 唯一维护链**(build_cmake.bat 一键含 windeployqt) | Qt6 迁移后 qmake/geometrize.pro **保留但冻结**(陷阱 #20);换 Qt 版本只改 bat 顶部 QT_DIR |
| 翻译 | 全中文(zh.ts 320 条,9 条技术串与技术原文一致被 -removeidentical 剔除)+ **Qt 标准按钮 qtbase_zh**(第十六批) | zh.ts 双 context(短名供 uic / 全名供嵌套类 tr);Qt 侧需 `qtbase_<lang>.qm` 才有 OK/Yes/No 中文(陷阱 #25) |
| 已知残留 | 偏好文件旧值持久化 | 全局偏好 JSON 里旧字段会被沿用,改默认值时注意用户机已有文件 |

**改动铁律**(每次迭代都适用):
1. 每个 patch 独立、可 revert,落地即跑**双门禁**(ctest 2/2 + run_ab 26/26,含 tools\goldens.csv 值级比对);改应用层渲染相关代码/升级 Qt 追加 `python tools\qt_render_ab.py check`;
2. 注释只写意图不写历史;
3. 上游怪癖是可观察行为,不得"顺手修":bestRandomState off-by-one(RNG 消费 n+2 次)、`257.0f*255.0f/alpha` 唯一浮点点、drawLines 的 RGB 预计算公式——ellipse_bounds_fork 用例会替你盯着的;
4. bat 必须 CRLF;应用构建走 CMake(VS 生成器自定位工具链,无需 vcvars);qrc 生成在 resources/ cwd 下跑;ChaiScript 保持 pin `2898ae6`;
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
- 遗留:custom energyFunction 时金字塔静默忽略(文档已声明);Qt UI 勾选框透传已落地(runner 面板 pyramidSearchCheckbox 与偏好双向绑定)

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
注(第十七批补充):该否决结论的**范围**是当时的四遍内核;第十七批融合+LUT 后,4096 大图上"混合+差分"合计已占 ~76%,其中逐像素算术仍是主导(见下节),若再议 SIMD 应以融合遍为目标而非旧的误差内核。

### P1.6 大图评估内核融合 + 混合查找表 — ✓ 已落地(2026-10-07,第十七批,位精确)
- **诊断口径**(4096² 测试图 × 默认预算 × 8 线程 × ellipse):上游原版 163.0 s/步、改进版(改动前)64.4 s/步;
  插桩定量:**每次候选评估平均扫描 161 万像素**,四遍扫描里"拷贝+混色"40.2%、"差分"35.1%——
  瓶颈是逐像素整数算术(融合前混合+差分约 50 条指令/像素),不是中间位图的内存往返(此结论推翻了本批的初始假设)。
- **实现**:`core::defaultEnergyFunctionFused`(core.h/core.cpp)——"混色→差分"两遍并为一遍只读扫描,
  混合值不写 scratch buffer 而逐像素现算;**逐像素混合改 4×256 项通道查找表**(表值用与 drawLines
  完全相同的表达式生成,含 uint32 回绕语义,位等价由构造保证);内置评估路径(经典+金字塔)切换,
  并省掉每线程每步 `Bitmap buffer{m_current}`/`halfBuffer` 的整图拷贝(库内已核实 buffer 无其他消费者)。
  `defaultEnergyFunction` 原样保留(脚本绑定 bindingscreator / 自定义能量函数 / 单测继续用)。
- **实测**:4096² 无金字塔 64.4 → 36.4 s/步(**1.77x**);4096² + 金字塔 14.3 → 8.5 s/步(1.69x);
  2048² 10.6 → 5.5 s/步(1.93x);4096² 相对上游 4.48x。逐像素:融合遍 12.6 → 6.1~7.1 ns/px。
  三条口径 STEP_FINGERPRINT 与改动前逐位相同;run_ab 26/26;新增单测(72 断言)锁定融合≡逐遍。
- 复现:`geobench-fast --input <4096.png> --steps 5 --threads 8 --types ellipse`(金字塔口径加 `--pyramid`)。
- 后续可选:融合遍 SIMD(6~7 ns/px 仍算术主导,预估 1.5~2x);增强轨道(分段颜色/自适应步长)同模式融合。

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

### F3.1 GIF/APNG 动图导出增强 — ✓ GIF 部分已落地(并入 F3.7,2026-09-01;APNG 未做)
原案"导出对话框加每 N 步一帧 + 帧率"已由 F3.7 一并兑现:frameStepSpinBox(每帧形状数 1-1000,默认 20)+ frameRateSpinBox(FPS 1-50,默认 20)+ 输出倍率默认 3→1(patches\qt 0027)。实现走增量帧渲染(IncrementalFrameRenderer,O(N)),优于原案"回放 shapes 序列重新渲染"的路线;shapes 以 shared_ptr 浅拷贝持有,内存同样可控。详见 §3 F3.7。

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
实际改动:导出面板 PNG 组加"Output Scale"QSpinBox(1-8,默认 3 对齐旧行为),saveRasterizedSVG/saveRasterizedSVGs 读控件值。零 lib 改动、零翻译新增(复用 GIF 组 "Output Scale" 条目)。已知限制:本条建批时的"QImage 上限 32767px(4096 源 ×8 越限)"记载经 Qt6 复现推翻(见陷阱 #23)——超限导出现在由第十五批的输出尺寸预检弹窗确定性拒绝。

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

**→ 已落地(第十四批,2026-10-03,Q4.4 收官)**:目标版本改 **6.8.3 LTS**(6.5 线已 EOL);
实际必改 = QSplashScreen 构造 1 处(SkipEmptyParts 有版本守卫,零改)+ CMake 模块迁移;另修
同步任务脚本路径死锁(上游 bug,见陷阱 #21)。验收 = 三道门禁全绿 + headless 脚本对拍
(Qt5 vs Qt6 严格项逐字节/逐像素一致,SVG 光栅化 0 像素差;Qt 图像缩放输入 1 LSB 属预期,
见陷阱 #22)+ 真机巡检。细节与复现步骤:docs/qt6-migration-report.md。

### Q4.5 Model 级禁用单测重启用 — ✅ 已完成(2026-09-29,审查轮)
16 个 Model 级用例(区域优先、分段颜色、四开关组合确定性、T7 不变量、异常传播/池存活、drawShape/reset 等)
已全部启用:删除 test_model.cpp 内全部 `#if 0 // CD` 配对(43 对,保留内层合法的 GEOTEST_FAST/BASE 宏分叉)。
启用后**立即暴露一条真实缺陷**:`step 异常传播` 用例给 16×16 的 Model 配了 32 格 creator,形状扫描线越界
(x=22 > 15),而库的裸指针落画路径不裁剪 → 写穿位图缓冲 → 堆损坏 0xC0000374(实测 15~30% 触发率)。
修复 = 落画入口统一裁剪(clipScanlinesToBitmap)+ 该用例改用 32×32 图;修复后 60 连跑 0 复现。
用例数口径:89/39 → **111/39**(含本轮新增的越界裁剪回归、scale 家族、rasterizeInto 等价用例)。
教训:禁用单测不是"暂时省事",而是把该覆盖的缺陷藏起来——本条正是靠复活的用例才被挖出来的。

---

## 4.6 审查轮遗留项 — □ 跟踪项(2026-09-29 建档)

2026-09-29 六路并行深审的 P0/P1 与大部分 P2 已修(见 README 审查轮修复清单),以下为**有意未做**的项,
按性质分组,接手时按需挑:

**A. 修它要动 bit-exact 或需产品决策**
1. C.1.4 仍有 5 处按闭区间消费形状边界(`shapemutator` 的 Line/Polyline setup 起点、RotatedRectangle 的
   setup/mutate、Triangle 的 mutate):其余 20+ 处用 `xMax-1`,这 5 处用 `xMax`,导致 fix 关闭时
   triangle/rrect 顶点可落到最后一列而 rasterize 裁到 `xMax-1`(点阵与矢量视图 1px 不一致)。
   统一成 `xMax-1` 会改变输出、破坏 bit-exact 门禁,故需与 `fixShapeBoundsOffByOne` 同轨设计后再动。
   (应用侧已固定开启修复开关,此项对日常使用零影响,维持搁置)

**B. 工具链与门禁的已知口径缺口(已在报告里声明,未改行为)**
2. `verify_patches.py` 的行尾归一使"纯行尾变更"对 G1/G2 完全不可见(设计取舍;bat 必须 CRLF 的约定靠人守);
3. `BIN_EXT` 增删会让归档 diff 的 echo 行全变 → G2 全量报漂移(fail-closed,但会误以为"补丁全废");
4. ~~`test_goldens.h` / `--dump-golden` 死代码占位~~ — 第十五批已删除(全库无消费方;端到端值级锁定由
   `tools/goldens.csv` 承载);
5. `--dump-final` 已改 .part 两段写 + rename(第十五批);`gen_test_images.py` 已补 `tree_under_clouds.png`
   上游快照复制(第十五批);`sha256.h` 已补 `<algorithm>`(第十五批);
   `svgscene-bench` 链接硬编码 Release `.lib`、内存列是推算值(未计"单件多渲染"的 renderer 内存)——仍未做;
6. `src/test` 不在补丁门禁覆盖范围内(改测试不受 G1/G2 约束;ctest 计数与 run_ab 值级锁定可部分兜底)。

**C. 应用层未修的 P2/P3**
7. 第十五批(2026-10-06)已清:16 区域上限无 UI 反馈、导出命名只靠内存计数器(重启重跑覆盖旧导出)、
   `taskqueuewindow` 列表项回调里 `delete item`(信号栈内销毁 sender)、GIF 输出尺寸超限无护栏、
   脚本下拉 `operator[]` 副作用——详见 §5 第十五批。
   **仍有意未做**:后台导出不可取消;导出侧缺"segments 与扫描线不齐时退单色"的回退(库侧 `drawShape` 有);
   导出后台线程的 bool 返回值仍未接 UI(尺寸类失败已被预检弹窗拦截,其余如磁盘写失败仍静默);
   撤销重放按需拷贝量 O(N²)(共享 shared_ptr,无形状深拷贝)。

**D. 验证状态**
8. ✅ 已真机回归(2026-10-01,用户实测):撤销/重做、批处理队列、GIF/PNG 导出交互路径逐项点过,未报异常。
   区域框选(Ctrl+拖拽)同期实测**发现真缺陷**并修复——不可选 item 收不到 release,见提交 7cfbd22 与陷阱 #17。
9. ✅ Qt6 迁移真机回归已通过(2026-10-03,用户实测):启动/模板网格、运行(默认+增强勾选)、撤销重做、
   区域框选、脚本控制台、导出 PNG/序列/SVG/GIF、批处理队列、偏好/语言、任务栏进度、关闭全项无异常。
   对应验收证据见 docs/qt6-migration-report.md(脚本对拍 + 三道门禁 + 交付包 check)。
10. ✅ 第十五批真机回归已通过(2026-10-07,用户实测):①"另存为"到工作区外、③批处理两轮导出不互相
   覆盖/队列项移除不崩、④脚本下拉与 16 区域上限提示——全过;②因测试图仅 1280×1706(×8=559MB,
   未达 2GB 上限)**未真正触发护栏**,第十六批改用 4096×4096 测试图补测**已通过**(见下条)。
   回归同期发现并修复:序列导出无提示直写目录(用户桌面被铺满)——见 §5 第十六批与陷阱 #25。
   本地 24 提交已推送、仓库已转 public(2026-10-07,用户确认)。
11. ✅ 第十六批真机回归已通过(2026-10-07,用户实测):尺寸护栏补测(4096×4096 图 + 输出倍率 8,
   弹出"导出尺寸超限"拒绝而非崩溃)、序列导出确认框(每步一张/张数/目标目录/默认"否")、
   Qt 标准按钮中文化("是/否"、"确定/取消")及其余交互项——全项无问题。
12. ⏳ 第十七批大图提速待用户体感确认(2026-10-07):内核融合为**位精确**改动(指纹/门禁证据充分,
   见 §1 P1.6),用户侧只需体感核对——4096 图同参数下单步应约为原来的 1/1.7;勾选"金字塔搜索(快速)"
   后合计约为原来的 1/7.6。若体感不符(例如仍明显卡顿),回报具体操作路径与耗时。

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

✅ 已完成(第十四批,2026-10-03:Qt6 迁移落地 + 同步任务死锁修复 + Qt 渲染侧对拍工具)
  Q4.4 迁移落地(Qt 6.8.3 LTS msvc2022_64 单轨切换;CMake/bat 迁移 + QSplashScreen 构造适配;
    qmake/.pro 保留但冻结;dist 换纯 Qt6 包;详见 docs/qt6-migration-report.md)
  上游 bug 修复:同步任务(SynchronousImageTask)脚本路径死锁——worker→task 回传连接与
    DirectConnection 同型化(GUI 路径语义不变;陷阱 #21)
  验收基建入库:tools/qt_render_ab.py + 6 用例 + 冻结参考(qt_goldens.csv/qt_render_ref/),
    对拍结论:严格项逐字节/逐像素一致、SVG 光栅化 0 像素差、Qt 缩放输入 1 LSB 属预期(陷阱 #22)
  → 后续可选方向(未变):A2.5 新形状类型;新方向待定(CLI 批处理/HTTP API/更多导出格式)

✅ 已完成(第十五批,2026-10-06:应用层审查遗留清理,§4.6 C 组大头清账)
  必做:GIF 导出尺寸护栏——真实故障模式与旧记载不符,复现程序实证(.tmp_batch15/gif_overflow_repro,
    守护页法):①Qt6/64 位 QImage **无固定单边上限**(32768 宽构造成功、scaled(32768,32768) 4.29GB
    也能成功,"32767 上限"是 Qt5 时代假设,旧记载">8191 触发窄化"作废);②内存压力下 scaled 返回
    空图 → makeImageData 分配 0 像素 → BurstLinker 量化器按 init 尺寸读 → **堆越界读 0xC0000005**;
    ③GIF 头部宽高 uint16 超 65535 **静默截断**(70000→4464)。防线 = addFrame 空图检查 +
    exportGIF 尺寸护栏(每边 ≤65535、宽*高*4 ≤2GB)+ 帧失败接线(删半截文件返回 false)+
    导出面板三入口预检弹窗;qt_render_ab GIF 用例字节级一致,正常路径零扰动
  必做:批处理导出命名防覆盖(taskqueuewindow:纯内存计数器 → "内存 ∨ 磁盘现状"跳首个空闲名,
    本批要写的扩展名任一被占即让位;重启重跑同名批次不再覆盖旧导出)
  必做:队列移除回调延迟删除(原代码在 itemWidget 自己的信号栈内 delete item = 销毁 sender;
    QListWidgetItem 非 QObject 无 deleteLater → QTimer::singleShot(0, q, ...) 挪出信号栈)
  顺手:16 区域上限状态栏提示、脚本下拉 operator[] 读副作用改 find、test_goldens.h +
    --dump-golden 死机制删除、geobench --dump-final 改 .part 两段写 + rename、sha256.h 补
    <algorithm>、gen_test_images.py 补 tree_under_clouds.png 上游快照复制(与现副本逐字节一致)
  翻译:3 条新 tr(zh.ts + qm,lrelease 315/315 finished)
  门禁:四道全绿(ctest 2/2、run_ab 26/26、verify_patches 重导出 PASS、qt_render_ab check);
    dist 已同步(exe SHA-256 e0b930f4…与 build 产物一致);代码审查无 P0/P1
  → 后续可选方向(未变):A2.5 新形状类型;新方向待定(CLI 批处理/HTTP API/更多导出格式);
    §4.6 仅剩有意搁置项

✅ 已完成(第十六批,2026-10-07:序列导出告知与确认,真机回归反馈驱动)
  根因:中文界面 Save Image/Save Images 同译"保存图像",序列按钮会把渲染过程每步各存一张 PNG
    (最多 1000 张,超过均匀抽稀)直接铺进所选目录,目录选择框标题也只有"保存图像",全程无提示
    无确认——用户真机回归时把整批序列图存到了桌面。
  二次确认(imagetaskexportwidget):选目录后、写盘前弹确认框(标题"导出序列图像"),写明"每步一张"
    + 实际张数 + 目标目录;默认按钮与 Esc 都落在"否";0 形状不弹"将写入 0 张"空确认(导出器本就
    静默失败,保持既有行为);进度框文案带张数。
  张数单一真源(imageexporter):新增 exportedFrameCount(shapeCount) 与共享常量 maxSequenceFrames,
    写盘循环与确认框共用(stride=ceil(N/1000),取帧数=N/stride+(N%stride!=0))。
  文案:.ui 加按钮 tooltip;zh.ts 按钮→"保存序列图像"、目录框标题→"选择序列图像导出目录"、确认框
    标题/正文(两变体)/进度文案共 5 条新增;英文 source 字符串未动(其他 28 个语言不失效)。
  Qt 标准按钮中文化:补入 Qt 6.8.3 的 qtbase_zh.qm/qtbase_zh_CN.qm(resources + qrc),确认框显示
    "是/否"、其余对话框"确定/取消"同步转中文——根因与判据见陷阱 #25。
  门禁:ctest 2/2、run_ab 26/26、verify_patches 重导出 PASS;qt_render_ab 未跑(纯对话框/资源改动)。
  真机:Computer Use 冒烟——28 形状 → 弹窗报"28 张" → 目录落盘 28 个 PNG 一一对应;Esc/"否"
    零写盘;尺寸护栏弹窗(第十五批 #2 补测)与确认框/中文化按钮等全部项**已由用户复核通过**
    (2026-10-07,见 §4.6 D.11;测试图留在 D:\tmp\geometrize-limit-test\)。
  → 后续可选方向(未变):A2.5 新形状类型;新方向待定(CLI 批处理/HTTP API/更多导出格式)

✅ 已完成(第十七批,2026-10-07:大图评估内核融合,位精确)
  触发:真机反馈 4096 画布"单步几十秒";诊断(4096²/8 线程/默认预算):上游 163.0 s/步、
    改进版 64.4 s/步、每次候选评估平均 161 万像素,插桩定量"拷贝+混色 40.2% / 差分 35.1%"——
    瓶颈是逐像素算术而非内存往返(推翻本批初始假设,见 §1 P1.6)。
  实现:defaultEnergyFunctionFused(混色→差分融合为一遍只读扫描)+ 4×256 混合查找表(同式生成,
    含 uint32 回绕)+ 内置路径省掉 scratch 整图拷贝;defaultEnergyFunction 保留给脚本/自定义路径。
  实测:4096² 64.4→36.4 s/步(1.77x)、+金字塔 14.3→8.5 s/步(1.69x)、2048² 1.93x、
    相对上游 4.48x;三条口径 STEP_FINGERPRINT 与改动前逐位相同。
  门禁:ctest 2/2(新增融合一致性单测 72 断言)、run_ab 26/26(golden 全 OK)、verify_patches 重导出 PASS;
    镜像同步进 improved-app/lib 并重建交付包(dist 已同步 + fix_integrity_label)。
  → 后续可选项:融合遍 SIMD(预估 1.5~2x);增强轨道同模式融合;大图默认提示金字塔(RUN 面板)
```

每批结束:双门禁全绿(ctest + run_ab;改应用层渲染相关代码或升级 Qt 追加 qt_render_ab check)+ 发布包同步 + 补丁重编号导出 + MEMORY 更新。

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
| 库补丁 | patches\lib\0001 全量 + 按文件拆分(0012 金字塔/0013 增强轨道/0014 误差图引导/0015 区域优先/0016 分段颜色/0017 SVG 命名空间/0018 评估内核融合) |
| 应用补丁 | patches\qt\0001-0028(0026 批处理增强/0027 导出性能/0028 Qt6 迁移,累计 diff 含附注) |
| Qt 渲染侧对拍 | tools\qt_render_ab.py(+ tools\qt_render_ab\cases\ 用例、tools\qt_goldens.csv 严格项哈希、tools\qt_render_ref\ 光栅化参考图;用法见 tools\qt_render_ab\README.md) |
| 等价性论证 | docs\bitwise-equivalence-notes.md |
| bug 分级 | docs\bugfix-triage.md |
| Qt 评估 | docs\qt-app-findings.md |
| Qt6 迁移评估 | docs\qt6-migration-assessment.md(Q4.4,2026-08-31;已落地,见下) |
| Qt6 迁移报告 | docs\qt6-migration-report.md(第十四批实施+验收证据+换 Qt 版本复现步骤) |
| CMake 构建 | src\improved-app\CMakeLists.txt + build_cmake.bat(qt 0025/0028;产物 build\Release\;换 Qt 版本改 bat 顶部 QT_DIR) |
| 性能报告 | benchmarks\report.md(+ benchmarks\runs\ 每轮 CSV) |
| 发布包 | dist\Geometrize-Improved\(windeployqt 产物,exe 与 release 同步复制;第十四批起纯 Qt6) |
| 完整性标签修复 | tools\fix_integrity_label.py(重建 exe 后必须跑,否则双击另存为报"没有权限";见陷阱 #24) |
| 构建脚本 | src\improved-app\build_qt.bat(qmake 链路,qt 后冻结;D:\tmp\build_qt_all.bat 备份含环境) |

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
10. **写测试的坑**:doctest 比较 shared_ptr 用 `.get()`(直接比较触发 stringification 编译错);DeterministicShapeCreator 的 gridSize 必须 >16(内部 `% (gridSize-16)`,16 会除零),**且必须与 Model 的图尺寸一致**——creator 的坐标可达 gridSize-1、rasterize 也按 gridSize 界绑定,配小图就会产出越界扫描线(2026-09-29 实测:16×16 图配 32 格 → 堆损坏 0xC0000374);变体分叉用 `GEOTEST_BASE/GEOTEST_FAST` 宏分流,勿"顺手统一"。另:中文用例名无法用 `-tc=` 过滤(MSVC 按 936 代码页转 argv),需要按用例定位时用 `-s` 打印断言流或临时插桩。
11. **0xC0000374 堆损坏**:2026-09-29 审查轮已定位到真实根因——**宿主自定义 shapeCreator 产出越界扫描线时,库的裸指针落画路径写穿位图缓冲**(16×16 图配 32 格 creator 即触发,15~30% 概率)。修复:落画入口 `clipScanlinesToBitmap` 统一裁剪 + 三个 undo 助手与 rasterize 三件套补边界自守。**此前"疑似 obj 不一致"的归因不成立**(干净重建后仍复现,直到修掉越界写才归零)。定位手法(可复用):给 6 处 `getDataRefMut` 写入点插越界校验(越界即 fprintf+abort),比等堆报告快得多;Debug 构建下 `_CrtSetDbgFlag(_CRTDBG_CHECK_ALWAYS_DF)` 太慢(3 分钟跑不到第 3 个用例),不实用。run_ab 的门禁策略已改:**单侧无输出重试非零即按 FAIL 计**(旧行为把偶发故障洗成 PASS,正是它掩盖了本条)。
12. **裸形状必须绑 rasterize**:`std::make_shared<Rectangle>(...)` 等不经 shapefactory 的形状,其 `rasterize` std::function 为空,drawShape 调用即 UB/崩溃——照 GUI `drawBackgroundRectangle` 先绑定。椭圆光栅化输出 y 不升序(从中心向两边),导出/统计侧须 stable_sort。
13. **CMake 链专用**:target_link_options 传含空格的链接选项(/MANIFESTDEPENDENCY)会被 VS 生成器拆成假输入文件(LNK1104)——用 .manifest 文件走源列表(app.manifest 先例);bat 里用 Python 写 Windows 路径必须 raw string(`\b`/`\5` 会被转义吃掉);CMake 版 exe 未经 windeployqt 启动会弹缺 DLL 错误框且进程挂着不退,**勿把 HasExited=False 误判为运行正常**(看 startup_timing.log 是否新增)。
14. **QPointer 在 Qt5 无 qHash**:`QSet<QPointer<T>>`/`QHash<QPointer<T>,V>` 编译报 qHash 无重载——受管窗口集合用 `QVector<QPointer<T>>` 线性查找(F3.3 先例);QPointer 作 connect lambda 捕获 + receiver 传宿主窗口,WA_DeleteOnClose 的 sender 销毁时 Qt 自动断连,回调不悬空;嵌套类非 QObject 的 Impl 里 `connect` 是全局五参函数直接可用,**`QTimer::singleShot(ms, context, fn)` 的 context 同理必须传 `q`(Impl 传 `this` 直接编译失败)**(第十五批先例)。
15. **形状边界是排他上界**:`setup`/`mutate`/各 `rasterize` 一律把边界元组的 max 当排他上界消费(内部 `randomRange(xMin, xMax-1)`、`clamp` 到 `xMax-1`、y 过滤在 `[yMin, yMax)`),整幅画布即 `(0, 0, width, height)`。`mapShapeBoundsToImage` 上游返回闭区间 `size-1`,错配使最右列/最下行永不落画(C.1.4);新代码写边界时**别照抄那个 -1**,脚本模板里的 `xMax - 1` 才是对的。
16. **QGraphicsItem 的 DeviceCoordinateCache 吃全局 QPixmapCache 配额**(默认 10MB,约 5 个全画布图层):图层数超配额后每帧重新渲染而非命中缓存,帧耗时量级跳变(0.2ms→70ms)。给场景加 item 前先算图层数 × 单层设备像素;需要更多图层就 `QPixmapCache::setCacheLimit`(应用 main.cpp 已设 128MB)。诊断手法:离屏 `QGraphicsView::render` 计时 + 打印 item 数,见 F3.9。另:`QGraphicsSvgItem` 自带 `maximumCacheSize`(默认 1024×768),设备矩形超上限时 QGraphicsScene 会**整体旁路**设备坐标缓存 —— 分块 SVG item 的 boundingRect 是整幅画布,所以视口/缩放一大缓存就失效,`QPixmapCache` 调多大都没用(需 `setMaximumCacheSize`)。
17. **不可选/不可移动的 QGraphicsItem 收不到 release**:`QGraphicsItem::mousePressEvent` 对 `ItemIsSelectable == false` 的 item 走 `event->ignore()`,场景于是**不把它设为 mouse grabber**;图像视图开着 `ScrollHandDrag`,后续 move/release 全被视图接管 —— item 的 release 信号永不触发。表现极具迷惑性:**按下有反应(信号已发)、松手什么都不发生**。凡"Ctrl+拖拽/框选"这类靠 item 的 press+release 配对的交互,必须在 press 里显式 `event->accept()`(2026-10-01 区域框选就是这么坏的:README 记了功能、代码看着完整、纯静态审查也判"能用")。定位手法:item 的 press/release 与窗口侧处理器各插一条 stderr 日志,跑一次就能看到"只有 press 没有 release";反过来,"日志里看不到 release"本身就是该 bug 的症状,别误判成测试工具不可靠。
18. **改完应用必须同步 dist**:用户实际运行 `dist\Geometrize-Improved\Geometrize.exe`(README「直接使用」指它),它**不会**随 `build_cmake.bat` 自动更新。只重建 `src\improved-app\build\Release` 就交给用户复测 = 他测的是旧包(2026-09-30 实测:dist 停在 9-22,用户报"新修的功能没生效"其实是旧构建)。让用户复测前先 `cp src\improved-app\build\Release\Geometrize.exe dist\Geometrize-Improved\` 并比对哈希;根目录那个 `Geometrize.exe` 是更旧的遗留物,别误用。另:应用资源(模板/脚本/翻译)全部编进 exe,同步 exe 即可。
19. **Qt6 构建适配要点(迁移后新会话最易踩)**:
    - `QGraphicsSvgItem`/`QSvgWidget` 在 Qt6 移入 **QtSvgWidgets** 模块(CMake 必须 `find_package(... SvgWidgets)` + 链 `Qt6::SvgWidgets`;头文件名不变);
    - `QSplashScreen` **没有 `QWidget*` 父参重载**了(默认构造 + `setPixmap`);
    - WinMain→main 转发**无需手工 EntryPoint**:`Qt6::Core` 的 INTERFACE 里带 `WIN32_EXECUTABLE → Qt6::EntryPointPrivate` 生成式,`add_executable(... WIN32 ...)` 自动吃到;
    - windeployqt(Qt6):部署 `styles/qmodernwindowsstyle.dll`(取代 Qt5 的 windowsvista)与 `tls/`(取代 `bearer/`),默认**不部署** opengl32sw(raster widgets 不需要);`--no-translations` 可再瘦身(应用翻译已编进 qrc)。注意 **qtsvg 是 archive 不是 module,qtimageformats 才是 module**(webp/tiff/tga/icns 靠它,aqt 安装命令加 `-m qtimageformats`);
    - 单轨切换后**旧 Qt5 残留必须清掉**(Qt5*.dll / bearer / libEGL / libGLESv2 / qwindowsvistastyle),否则包内两套 Qt 混放,排障时极易看错。
20. **qmake 链路已冻结**:Qt6 后应用构建只维护 `CMakeLists.txt` + `build_cmake.bat`(换 Qt 版本只改 bat 顶部 `QT_DIR`);`geometrize.pro` 保留但不再跟随改动**,改应用新增文件时别再往 .pri/.pro 里补**(CMake 用 GLOB 递归收集,自动跟上)。
21. **同步任务脚本路径死锁(上游 bug,已于第十四批修复)**:`ImageTask` 用 `Qt::DirectConnection`(SynchronousImageTask)时,worker→task 的 `signal_willStep/didStep/didReplay` 若用 `BlockingQueuedConnection` = **同线程阻塞等自己** → 控制台/脚本模式必死锁;**GUI 的 QueuedConnection 路径正常,所以这个 bug 潜伏很久**(上游自带示例 `imagejob.chai` 一样挂)。修法 = 回传连接与入向连接同型。定位手法:进程不退但 **CPU≈0 且无窗口标题**(与"脚本错误"区分:后者会弹「脚本评估失败」模态框、`MainWindowTitle` 有标题,且该模态框自带事件循环会一直挂着等点击——headless 跑脚本必须 try/catch + 带超时强杀)。
22. **Qt 图像平滑缩放的跨版本差异(Qt5→Qt6 实测)**:`QImage` 平滑缩放在 5.15→6.8 间有 **1 个灰阶的取整差异**(512→256 实测 62.5% 像素差 1 LSB),该差异沿形状链混沌放大后最终输出不再逐位一致——**对拍矩阵必须把"过 Qt 缩放"的输入从严格项剥离**(tools\qt_render_ab 用例 06 专门量化记录);应用默认处理分辨率上限 1024,超过即走该路径。性质属 Qt 实现变更,不是缺陷、也不该"修回"。
23. **Qt6/64 位 QImage 无固定单边尺寸上限(第十五批复现实证,"32767 上限"是 Qt5 时代假设)**:32768 宽构造成功、`scaled(32768,32768)`(4.29GB)在本机也能成功——不能指望 Qt 替你拒绝超限导出。后果链:内存压力下 `scaled` 返回**空图** → `makeImageData` 按 0×0 分配 → BurstLinker 量化器按 `init` 时的尺寸读缓冲 → **堆越界读**(守护页实测 0xC0000005);GIF 头部宽高是 uint16,>65535 静默截断(mod 65536,70000→4464)。凡按用户倍率放大输出的路径,先用 `宽*高*4 ≤ 2GB`(uint64 乘法)与 GIF 每边 ≤65535 做确定性预检(第十五批已在导出面板三入口 + gifexporter 落防线),消费 scaled 结果前必须补空图检查。
24. **工作区内新建/更新的 exe 会继承 Low 完整性标签 → 双击运行时另存为全位置报「没有权限」(2026-10-06 定位,机器环境层面)**:本机工作区根被 dsh(DeepSeek Harness,本机 0.2.0-rc.2)的 Windows 沙箱盖了一条**可继承的 Low 强制完整性标签**(`Mandatory Label\Low Mandatory Level:(OI)(CI)(NW)`,其提交 `d5ad3baeb5`;工作区 ACL 里多出的 `S-1-4-…`"未知账户"是 dsh 的能力 SID,非恶意软件——公开记录见其讨论 #7735)。该目录树内**每个新建/更新的文件**都会物化 Low 标签,而由 Low 文件启动的进程按 Windows MIC 规则**以低完整性运行**——低完整性进程写不了任何普通目录,症状即:双击 Geometrize 后另存为时 Windows 原生对话框误报「你没有权限在此位置中保存文件…改为保存到图片文件夹?」(桌面/图片/D:\tmp 全拒,唯独工作区内可写——工作区本身带了配套的 Low 写许可)。**判据**:`icacls <exe>` 看是否含 `Mandatory Label\...Low`;对照实验 = 同会话 python/notepad 写 D:\tmp 正常而该 exe 写不了(应用内探针 `createDirectory`/`writeStringToFile` 全返回 false,注意此类写入**静默失败不抛异常**)。**修法**:`python tools\fix_integrity_label.py` 把交付入口/构建产物显式重置为 Medium(显式标签压过继承标签,**无需管理员**),**每次重建 exe 后都要再跑**;dsh 上游修复(f6698853f3)只豁免授权根**顶层**启动器,深层路径 exe(如本项目 dist)不受益,仍需本脚本兜底。排查已排除:应用代码/启动上下文/ACL/只读位/Defender CFA/火绒(3 个 db 含 4MB WAL 全扫,无 Geometrize 拦截记录)/完美世界 MessageTransfer.sys/AppCompat shim 与 AppInit·AppCertDlls 注入点/IFEO。
    **2026-10-06 追查 dsh 本体后的补充**:① **语义实证**——写操作的强制完整性规则是「进程 IL ≥ 对象标签」,policy 位(NO_WRITE_UP)**不能放开写**:Medium(policy 0) 与 Medium(NW) 同样拒绝低完整性进程写入;因此被重置为 Medium 的文件,**dsh 沙箱子进程无法再覆盖**(沙箱内构建需覆盖这些 exe 时,改在沙箱外跑,或临时 `icacls <文件> /setintegritylevel Low`)。② 已建**工作区级自动修复**:计划任务 `dsh-low-integrity-autofix`(每 15 分钟,脚本 `scripts\dsh-label-autofix\autofix.py`)把继承 Low 的启动文件自动重置为 Medium——新构建/新文件自愈,不必再手工跑本项目的 `tools\fix_integrity_label.py`(保留作单项目手动兜底)。③ 根源 = 本地 dsh **0.2.0-rc.2** 的沙箱后端 `@deepseek-ai/dsh-sandbox-windows-acl`(`restrictTokenIntegrity` 令牌降 Low + `buildLowLabelAcl` 给授权根盖 OI|CI Low 标签 + 对 world 拒绝 FILE_DELETE_CHILD,一次授权全树传播);该版本**不含**上游"顶层启动器豁免",且其 README 明说常驻标签不回收。
25. **Qt6 标准按钮中文要靠 `qtbase_<lang>.qm`,旧的 `qt_<lang>.qm` 不够(第十六批实测)**:应用按 `qt_`/`qtbase_` 两个前缀 × locale 降级链加载 Qt 翻译(localization.cpp);资源里中文只有 Qt5 时代的 `qt_zh.qm` 整目录,**没有 `QPlatformTheme` 上下文**,而 Qt6 的标准按钮文案(OK/Yes/No/Cancel)恰好查它 → `QMessageBox` 按钮全回退英文(实测确认框显示 "Yes/No")。注意 Qt6 自带的 `qt_zh_CN.qm` 只是 **99 字节伞目录**,真正内容在 `qtbase_zh_CN.qm`。**判据**:`lconvert -i <qm> -o x.ts` 后 grep `<name>QPlatformTheme</name>`,没有即命中此坑。**修法**:把 `D:\Qt\6.8.3\msvc2022_64\translations\qtbase_zh_CN.qm` 存为 `resources\translations\qt\qtbase_zh.qm`(+`qtbase_zh_CN.qm`,对齐其余 20 个语言的 `qtbase_<lang>.qm` 惯例),再重跑 `scripts\generate_geometrize_qrcs.py` —— 该脚本**必须在 `resources\` 目录下运行**(内部用相对路径,在别处跑会 FileNotFoundError:'templates/templates');重建后确认框即显示"是/否"、其余对话框"确定/取消"。残余缺口:`qtbase_zh_TW.qm` 未补(繁体中文的标准按钮仍是英文)。
26. **`qt_render_ab check` 的用例 06 依赖应用全局偏好(处理分辨率上限)——参考是在阈值 256 下冻结的(第十七批实测)**:case 06 走 `convertImageToBitmapWithDownscaling(loadImage(gradnoise_512.png))`,缩不缩放由持久化的 `global_preferences.json`(`%APPDATA%\Sam Twidale\Geometrize\global_preferences.json`,字段 `imageTaskImageResizeThreshold`)决定:阈值 256(上游默认,参考冻结时的环境)→ 输出 256×256 与参考逐字节一致;阈值 1024(本分支默认)→ 512 源图不再缩放 → check 报 `[DIFF] 06_image_scaling.qtinput.png`(实测 74.68% 字节不同、最大差 253,极易误判成回归)。**判据**:DIFF 只出现在 06、且差异是"整幅不同"而非 1 LSB 级;先看 `%APPDATA%` 里那个 JSON 的阈值。**跑 check 前**把阈值设回 256(或改用 `accept` 在目标环境下重采参考);注意 GUI 会话改过分辨率(如做护栏测试时设 4096)会持久生效,不仅让 check 误报,也让之后每个任务都按该分辨率处理。另:陷阱 #22 的"1 LSB 级"差异与本案是两回事,不要混为一谈。
