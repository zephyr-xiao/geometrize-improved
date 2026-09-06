# patches/regen — 规范全量补丁(权威可重放集)

本目录的补丁由 `tools/verify_patches.py` 从工作树现生成,是"apply 到基线副本即得改进树"承诺的**权威载体**:

- `geometrize-lib-full.diff`:基线 = `src/baseline-lib/geometrize`,目标 = `src/improved-lib/geometrize`;
- `geometrize-app-full.diff`:基线 = `upstream/geometrize-app`,目标 = `src/improved-app`。

## 生成与重放方式

- 兄弟目录布局(a/ b/)下 `diff -ruN` 生成,`patch -p1` 应用;新文件/删除文件均为标准 /dev/null 形式;
- 两侧统一 EOL 归一(LF)后 diff:门禁是**内容级**等价,不受工作树 CRLF/LF 混杂影响(改进树现存
  24 个纯 LF 文件,与树约定 CRLF 不一致,已在上游诊断中如实记录,不影响本补丁语义);
- `---/+++` 行的时间戳被剥除、`-x` 排除参数排序,保证逐字节确定性(可 commit 后逐字节比对)。

## 双门禁(verify_patches.py)

- **G1 重放复现**:fresh 基线副本 + 本补丁 == 工作树(逐文件零漂移,行尾无关);
- **G2 归档新鲜度**:已提交的本补丁与按当前工作树现生成的内容逐字节一致——
  不一致即工作树有改动未重新导出补丁,逐文件列出(合入前门禁,lib 侧失败 exit 1)。
- app 侧为报告口径:构建产物(obj/qt_gen/release/build)、第三方 pin(lib/chaiscript 等 4 个 vendored
  仓库)、二进制文件(不进 diff)与上游仓库元数据(README/LICENSE/CI 配置等)豁免,漂移只告警。

## 为什么不用 patches/lib、patches/qt 的拆分系列

历史拆分归档(编号补丁)经实测**无法按编号顺序重放到 fresh clone**,与 README 原有表述不符:

1. 行尾混杂:各补丁文件 LF/CRLF 逐文件不一,与 CRLF 基线树上下文不匹配;
2. 格式缺陷:新文件补丁缺 `@@` hunk 头(0014/0015 部分),部分补丁是无文件头的裸 `@@` hunk(0015 部分);
3. 语义问题:各批次对同一文件重复导出(如 0002 与 0012 都把 bitmap.h 从基线变到终态),
   内容互相重叠,"顺序重放"语义不成立(应为"每文件取最新",且无导出工具存档该语义)。

拆分系列保留作**历史参考**(可读的按特性 diff,附注完整),重放/校验一律以本目录为准。
