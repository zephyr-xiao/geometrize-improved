# Qt 应用层评估记录(只评估、未改码的项)

以下各项在本次改进中经评估后**有意不动代码**,记录现状、风险与后续改造路径。

## 1. worker 双向 BlockingQueuedConnection 栅栏

- 位置:`task/imagetask.cpp` 中 `signal_willStep/signal_didStep → ImageTask::modelWillStep/modelDidStep` 用 `Qt::BlockingQueuedConnection`
- 现状:每步开始前/结束后 worker 线程阻塞等待主线程完成对应槽(UI 状态标记、场景刷新等)。这是刻意的"UI 同步栅栏",保证 GUI 读 `getCurrent()` 时有一致窗口。
- 风险:主线程卡顿时吞吐直接下降;但取消栅栏会引入信号积压重排与数据竞争。
- 改造路径:若要解除,需要把"UI 读当前画面"改为 worker 侧双缓冲快照 + 主线程拉取,牵动整个步进协议,收益依赖 profile 证据,暂缓。

## 2. 脚本模式逐步克隆 ChaiScript 引擎

- 位置:`task/imagetask.cpp` stepModel 中 `make_shared<GeometrizerEngine>(m_geometrizer.getEngine()->get_state())`
- 现状:脚本模式开启时每步深拷贝引擎状态并重新 eval 全部脚本函数,防多线程共享引擎状态。克隆成本毫秒~几十毫秒级,相对每步 hill-climb(百毫秒级)占比可容忍。
- 结论:机制"贵而不险",默认脚本关闭时完全不走此路径。保留。

## 3. destroyTask 的 wait(1000)+terminate()

- 位置:`task/imagetask.cpp` 销毁任务路径
- 现状:超时后强杀工作线程,QObject 析构风险存在但爆炸半径小(仅悬挂场景触发)。
- 改造路径:合作式取消需要给核心库加深层取消令牌(step 循环内检查 atomic flag),属跨库 API 改动,超出本次"最小侵入"边界。

## 4. DataSlinger 分支(#ifdef DATASLINGER_INCLUDED)

- 现状:默认关闭的可选 WebSocket 推流,每形状一次 sendSvgShapeData。
- 结论:默认编译不含,无运行时代价;启用场景属实验性质,不动。

## 5. AVX2 手写 SIMD(核心库)

- 评估:B1/B2 落地后 MSVC /O2 已把整数循环自动向量化(SSE2 级),手写 AVX2 预估再收益 <15%。
- 方案存档(需要时启用):core_simd_avx2.cpp 单 TU `/arch:AVX2` + `__cpuid` 运行时分发 + 标量回退,对偶测试强制两 TU 一致。绝不全局开 `/arch:AVX2`(ChaiScript 全 TU 基线抬升 + 老机器崩溃面)。

## 已改码项摘要(详见 patches/qt)

| 项 | 文件 | 内容 |
|---|---|---|
| 步进节流 | dialog/imagetaskwindow.cpp | afterAppendShapes 置 dirty + 33ms 单发 timer 合帧,清空立即 flush |
| SVG item 缓存 | scene/imagetasksvgscene.cpp | NoCache → DeviceCoordinateCache |
| 定时器按需 | dialog/imagetaskwindow.cpp + imagetaskscriptingwidget.* | 无 timed-update 脚本不启动 100ms 轮询 |
| 加载链精简 | image/imageloader.cpp | 去多余 copy;Bitmap move 构造消二次深拷(lib 面) |
| 前条件竞争 | task/imagetask.cpp | 启用前条件脚本时强制本步 maxThreads=1 |
| 大图支持 | preferences/globalpreferences.cpp | 默认缩放阈值 256→1024 |
