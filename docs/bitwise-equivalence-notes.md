# 位精确等价性论证(逐优化点)

本文档记录每个性能优化 patch 为什么能在"相同参数下输出与上游逐位一致"。总原则:

1. **整数运算重排安全**:误差统计与混色全是整数加法/乘法,整数加法满足结合律且各项绝对值有界(64 位累加不会溢出),因此向量化/分块重排不改变结果。
2. **浮点参与点唯一**:全管线只有 `computeColor` 的 `257.0f * 255.0f / alpha` 一处浮点,该表达式按 float 语义逐字保留。
3. **确定性根基**:种子在主线程 submit 循环里分配(`base + offset++`),futures 按 submit 序收割,min_element 用严格小于比较 —— 结果向量是 submit 序的函数,与线程完成顺序无关。
4. **RNG 消费次数是输出的一部分**:bestRandomState 的 off-by-one 怪癖(循环外 1 次 + `for(i=0;i<=n;i++)` 共 n+1 次 = n+2 次)必须原样保留。

## B1 Bitmap 访问器内联化

- 改动:`getPixel/setPixel/fill/getWidth/getHeight/copyData/getDataRef` 从 bitmap.cpp 移入 bitmap.h inline 定义;索引乘法从 32 位升级 uint64。
- 等价性:纯消函数调用开销,数值与访存序列不变。uint64 升级只在 width*y 超 int32 时有差异,那是原版的 UB(内存安全修复),常规图尺寸下两种写法数值恒等。

## B2 copyLines 行段 memcpy;drawLines/computeColor/differencePartial 裸指针化

- 等价性:公式、舍入、运算次序逐像素保持;`257.0f*255.0f/alpha` 原样保留。memcpy 语义与逐像素赋值一致(区域互不重叠,行内连续)。
- 同 y 相邻线段合并写入只减少写次数,内容恒等。

## B3 Circle 光栅化 O(r²)→O(r)

- 数学依据:对整数 x,x²+y²≤r² ⟺ |x| ≤ ⌊√(r²−y²)⌋。原实现每行取 xScan.front()/back(),因判定域关于 x=0 对称,front/back 恒为 [−half,+half] 与新式一致;y∈[−r,r] 时 r²−y²≥0 恒成立,"空行"分支不可达,行为一致。
- isqrt_floor_u64 全程 uint64 防 r² 溢出 int32。

## B4 scanlinesForPolygon 去 map/set

- 等价性:map 键升序 ↔ sort 后 y 升序;set 的 min/max ↔ 每 y 组内线性扫描取 min/max;输出 Scanline 向量按 y 升序一一对应。sort 是值排序,全序下确定。

## B5 Model::step 补丁保存/恢复

- 数据流:computeColor 在写之前读 target/current(不受影响);drawLines 只碰覆盖像素;differencePartial 需要 before/after 两张图的覆盖像素值 —— 补丁方案把覆盖区原值存进 undo buffer,before 读 undo、after 读现值,k 游标按同序遍历单调推进。
- 回滚安全:各扫描线区间互不相交,单趟写回即可恢复,m_lastScore 不变。
- 门控:仅当 energyFunction==nullptr && addShapePrecondition==nullptr 走快路径;自定义回调路径保留整图拷贝语义(GUI ChaiScript 可能读取任意全图像素)。

## B6 scratch 复用

- rasterize 加 out 参重载(null 时转发老路径);trimScanlines 加原地过滤版(float clamp → int32 clamp:float 能无损表示 ≤2²⁴ 的整数,数值恒等)。复用容器不改变值,只消除堆分配往返。

## B7 持久线程池替代 std::async

- 正确性要件:①任务体第一件事 seedRandomGenerator(base+offset++),pooled 线程残留 RNG 状态永不被读(commonutil.cpp 中 mt 只被 randomRange 消费,已核实);②offset 分配留在主线程 submit 循环内递增;③结果按下标归位保 submit 序收割,min_element 平局选 index 小者与上游一致;④pool 归 ModelImpl 所有,dtor join;hardware_concurrency()==0 fallback 4 保持。

## C Bug 修复(允许改变输出的路径)

| 修复 | 触发条件 | 影响 |
|---|---|---|
| Ellipse 光栅化笔误 `y1>=xMin`→`y1>=yMin` | 仅当启用 shapeBounds 且 yMin>0 | 默认全图 bounds 无变化;显式 bounds 用例归类 EXPECTED_DIFF |
| scale(Polyline) 重写 | 仅外部脚本直接调用 | 主循环只走 setup/mutate/rasterize,已核实 ChaiScript 绑定面未暴露 scale |
| scale(QuadraticBezier) 实现 TODO | 同上 | 同上 |
| commonutil 尾像素 `x<x2`→`<=` | 仅 scanlinesContainTransparentPixels API | 不在主循环;归 EXPECTED_DIFF 注明 |

## D 金字塔搜索轨道(opt-in,不进 bit-exact 门禁)

金字塔不是 bit-exact 优化——它是搜索启发式:hill-climb 的候选生成与变异评估在
半分辨率图(1/4 像素)上进行,step() 的全分辨率落画/接受判定原样保留。默认关,
开启后输出与上游不同属预期。本节论证的是**该模式自身的确定性**与**全分辨率轨道零扰动**。

### D.1 全分辨率轨道零扰动

- 评估入口走复制变体 `bestHillClimbStatePyramid`,与 `bestHillClimbState` 平行——bit-exact 路径一个字节未动;
- `pyramidSearch && !energyFunction` 才启用(自定义能量函数语义按全分辨率位图约定,静默忽略金字塔);
- 默认 false 时 `Model::step` 行为与先前版本逐位一致(run_ab 12/12 全 PASS 验证)。

### D.2 opt-in 模式自身确定性(同输入 + 同 seed + 同线程数 → 同输出)

1. **整数降采样**:`downsampleHalf` 2×2 box 求和 `(sum + count/2)/count`,count∈{4,2,1},
   纯整数零浮点——不新增浮点参与点,守住"全管线唯一浮点点"不变量;
2. **RNG 流不变**:金字塔路径的 shapeCreator/setup/mutate 调用次数与顺序与全分辨率路径
   完全一致(评估分辨率不影响 RNG 消费),两模式生成同一候选序列,可逐候选对照;
3. **每任务私有 buffer**:半分辨率 buffer 每任务拷贝自共享只读的 m_currentHalf
   (共享 buffer 是数据竞争,ROADMAP 草案已修正);
4. **懒重建时序**:m_currentHalf 由脏标记驱动(m_halfDirty),接受落画/reset/drawShape
   置脏,下一次 getHillClimbState 提交前重建——futures 全 join 后主线程才动 m_current,
   重建与任务并发窗口天然错开;拒绝路径 restoreFromUndo 不置脏(m_current 未变);
5. **尺度自洽**:爬山内全部评估共用同一常数 baseline(全分辨率 lastScore 配半分辨率
   rgbaCount),步内排序自洽;min_element 跨任务比较同为半分辨率分数;step() 落画后
   m_lastScore 保持全分辨率口径,跨步无污染;
6. **奇数尺寸**:half = ceil(W/2)×ceil(H/2),floor 坐标映射 x → x/2 下恰好覆盖无盲区。

### D.3 scanline 投影语义(语义锁定)

`downscaleScanlines`:相邻行对 (2k,2k+1) 合并为半分辨率行 k,x 范围取并后 floor 除 2
(保守超覆盖——搜索启发式允许,最终判定在全分辨率);同行相交区间就地合并
(能量函数按像素遍历,重复覆盖会被重复累加);trim 到半分辨率边界。

### D.4 实测(2026-08-29,i3-13100F 8 线程)

| 图 | 模式 | 时间 | 同步数质量(diffFull,越低越好) |
|---|---|---|---|
| 1024²×30 步 | off | 20.5s | 0.1129 |
| 1024²×30 步 | pyramid | 5.2s(**3.95x**) | 0.1143(+1.2%) |
| 2048²×20 步 | off | 160.6s | 0.1373 |
| 2048²×20 步 | pyramid | 43.3s(**3.71x**) | 0.1402(+2.1%) |

确定性验证:1024/2048 两跑 FINAL_SHA256 与 STEP_FINGERPRINT 均一致;on/off SHA 不同(分叉存在)。

## E 增强爬山轨道(第三批:A2.2 自适应步长 / A2.3 alpha 搜索;opt-in)

统一入口 `bestHillClimbStateEnhanced`(core.cpp),HillClimbEnhancements 结构体控制开关,
与金字塔半分辨率评估可组合。经典/金字塔路径零参与(默认关时调度不进增强函数)。

### E.1 A2.2 自适应步长的确定性论证

1. **RNG 流不变**:步长缩放只改 randomRange 的区间(`-S>>stepShift, S>>stepShift`)不改
   draw 次数——T12 锁定(shift=0 与 shift=2 各 K 次变异后下一次 randomRange 值相同);
2. **shift=0 逐字等价**:默认参数下所有调用点与原实现取值分布相同(T9 平行序列全等),
   经典路径(shapemutator 被 mutate() 无参调用)输出不受注入影响——run_ab 全矩阵兜底;
3. **状态机无跨候选状态**:stepShift/acceptRun 是 hillClimbScratchEnhanced 局部变量,
   种子的纯函数;不挂 Shape(否则 undo 回滚克隆会污染状态机);
4. **参数**:N=8(连拒升档)/M=4(连受降档)/maxShift=3(×1/8 封顶),定值不外露调参面。

### E.2 A2.3 alpha 搜索的确定性与回写

1. **档位规范化**:升序去重 ∪ {用户 alpha}(用户档恒可达);平局取先评估者(低档);
2. **零 RNG**:穷举只多次调用能量函数,不消费随机数——增强路径与经典路径候选序列同位;
3. **胜者回写通道**:穷举胜者写 State::m_alpha → bestState 拷贝携带 → step() 落画改用
   `it->m_alpha`(model.cpp)。经典路径下 m_alpha 恒等于外部 alpha(**不变量,双变体用例
   T7 锁定;注:T7 现暂以 #if 0 // CD 禁用,见 test_model.cpp——该不变量同时由 run_ab
   逐位对拍矩阵承载**),故该改写在经典/金字塔路径输出逐位不变;alpha 搜索路径下 m_alpha 是搜出的
   胜者档,自然传导到 computeColor/ShapeResult.color.a/SVG fill-opacity/对拍指纹;
4. **buffer 无串染**:每档求值前 copyLines 重写覆盖区,档位间评估互不污染;
5. **已知语义巧合**(非 bug):默认三档 {64,128,192} 在"自适应步长开"的形状序列上胜者
   恒为用户 alpha——组合模式与 adaptive-only 输出逐位相同(指纹实证)。宽档位
   (--alpha-tiers 32..224)可恢复分叉。含义:组合使用时默认三档是无效付费,应配宽档位。

### E.3 geobench 双变体宏守卫

main.cpp 由 base/fast 两库共享,fast 专属 options 赋值(pyramid/enhancements)包
`#ifdef GEOBENCH_FAST`(CMake 打到可执行文件 compile definitions)。无守卫时 geobench-base
重编必炸(baseline 头文件无对应字段)——这是金字塔批次遗留的潜伏断裂,第三批修复。

## F. A2.1 误差图引导的确定性论证(2026-08-30)

1. **重建纯函数性**:`ErrorWeightMap::rebuild(target, current)` 是两张位图的纯整数函数——逐像素 L1 差按 16×16 块累加、整数缩放(除数 k = totalRaw/INT32_MAX+1,超加性保证 totalScaled < INT32_MAX)、inclusive 前缀和。无浮点、无时间依赖、无迭代顺序歧义。
2. **重建时序**:主线程在 submit 循环前(futures 全 join 后)重建,任务期间只读——与半分辨率缓存同一提交前槽位,无数据竞争。
3. **采样序固定**:sampleCenter = 1 次 CDF 二分 roll(upper_bound)+ 2 次块内抖动,每次调用恰好 3 draw;全零误差时 0 draw 且返回 false(精确均匀退化)。ε 判定恒 1 roll——引导消耗与误差分布无关,只与候选数有关。
4. **RNG 流声明**:errorGuide 开启时每候选 +1 roll(ε 判定)、引导成功再 +3(采样),与经典/增强路径**不再流对齐**——这是有意分叉(算法增强轨道),输出不同属预期;轨道自身同 seed 同输入同输出。
5. **跨线程数语义**:任务内消耗 seed 相关 RNG(采样)→ 不同任务(seed 不同)采样中心不同 → maxThreads 改变候选池 → 全局最优可变。B7 的"线程数无关"承诺仅对任务内无 RNG 消耗的轨道成立,测试用例已按此收敛断言范围(各自确定性,不跨比)。
6. **组合语义**:引导采样坐标恒为全分辨率(与形状坐标一致),金字塔半分辨率评估不受影响——四开关全开组合的确定性有专用用例锁定(注:该用例现暂以 #if 0 // CD 禁用,见 test_model.cpp「增强全组合」;组合分叉仍由 run_ab 哨兵 error_guide_combo 锁定)。
