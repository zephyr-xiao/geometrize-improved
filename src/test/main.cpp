// geotest:Geometrize 双变体单元测试入口
// 同一份测试源码分别链接 baseline-lib / improved-lib,产出 geotest-base / geotest-fast;
// 共享 golden 用例双变体全 PASS 即函数级 bit-exact 门禁,分叉点用 GEOTEST_BASE/GEOTEST_FAST 宏分流。
//
// golden 采集:geotest-fast.exe --dump-golden 会打印全部 golden 常量(分数、扫描线序列等),
// 粘贴进 test_goldens.h 即完成更新;两变体的 golden 值必须一致(不一致 = 行为分叉,禁止直接采纳)。

#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"

#include <cstring>

// --dump-golden 模式:跳过断言执行,由各测试文件向 stdout 打印 golden 常量
// (doctest 无原生"仅收集"模式,用全局旗标让 golden 用例走打印分支)
bool g_dumpGolden = false;

int main(int argc, char** argv)
{
    for(int i = 1; i < argc; i++) {
        if(std::strcmp(argv[i], "--dump-golden") == 0) {
            g_dumpGolden = true;
            break;
        }
    }

    doctest::Context context{argc, argv};
    return context.run();
}
