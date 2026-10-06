// geotest:Geometrize 双变体单元测试入口
// 同一份测试源码分别链接 baseline-lib / improved-lib,产出 geotest-base / geotest-fast;
// 共享 golden 用例双变体全 PASS 即函数级 bit-exact 门禁,分叉点用 GEOTEST_BASE/GEOTEST_FAST 宏分流。
// 端到端值级锁定由 tools\run_ab.ps1 + tools\goldens.csv 承载,测试侧不再维护独立的 golden 采集机制。

#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"

int main(int argc, char** argv)
{
    doctest::Context context{argc, argv};
    return context.run();
}
