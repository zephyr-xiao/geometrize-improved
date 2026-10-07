# geobench 与测试的统一构建入口
# 两套库目标:
#   geometrize-base   = src/baseline-lib 上游快照(零修改)
#   geometrize-fast   = src/improved-lib 补丁后树(初始阶段内容与 baseline 相同)
# 同一份 main.cpp / 测试源码分别链接两库,产出 geobench-base / geobench-fast
cmake_minimum_required(VERSION 3.20)
project(geobench LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 函数式定义一套"库 + 可执行",避免重复代码
# DEFINE_NAME 打到可执行文件上(而非库):main.cpp 里有 fast 专属的 options 赋值
# (baseline 库无对应字段),以宏守卫隔离,保证 geobench-base 永远可重编
function(add_geometrize_variant VARIANT_NAME LIB_SRC_DIR OUT_SUFFIX DEFINE_NAME)
    # 库源码:递归收集 baseline/improved 树下全部 cpp,不含测试与工具
    # CONFIGURE_DEPENDS 保证补丁增删文件后重新 cmake 时自动刷新列表
    # 先规范化为绝对路径:下面的按文件编译选项依赖路径字符串与 SOURCES 完全一致
    get_filename_component(GEOMETRIZE_LIB_ROOT "${LIB_SRC_DIR}" ABSOLUTE)
    file(GLOB_RECURSE LIB_SOURCES CONFIGURE_DEPENDS "${GEOMETRIZE_LIB_ROOT}/*.cpp")
    list(LENGTH LIB_SOURCES LIB_SOURCE_COUNT)
    if(LIB_SOURCE_COUNT EQUAL 0)
        message(FATAL_ERROR "未在 ${LIB_SRC_DIR} 找到任何 .cpp 源文件")
    endif()
    add_library(geometrize-${VARIANT_NAME} STATIC ${LIB_SOURCES})
    target_include_directories(geometrize-${VARIANT_NAME} PUBLIC "${LIB_SRC_DIR}/geometrize")

    # AVX2 融合内核 TU:只有这一个文件按 AVX2 指令集编译,库其余部分保持基线指令集;
    # 运行时由 avx2EnergyKernelAvailable() 派发,不支持 AVX2 的机器不会执行该 TU 的任何指令。
    # baseline 树没有此文件(EXISTS 判空),非 x86 平台也不加指令集开关。
    set(GEOMETRIZE_SIMD_SRC "${GEOMETRIZE_LIB_ROOT}/geometrize/geometrize/core/energykernelsimd.cpp")
    if(EXISTS "${GEOMETRIZE_SIMD_SRC}" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64|X86|i.86|x86)$")
        if(MSVC)
            set_source_files_properties("${GEOMETRIZE_SIMD_SRC}" PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
        else()
            set_source_files_properties("${GEOMETRIZE_SIMD_SRC}" PROPERTIES COMPILE_OPTIONS "-mavx2")
        endif()
    endif()

    if(MSVC)
        # /utf-8:源码与执行字符集均为 UTF-8,否则中文注释在 GBK 代码页下被切分出假语法错误
        target_compile_options(geometrize-${VARIANT_NAME} PRIVATE /EHsc /W3 /permissive- /utf-8 /Zi /FS
            $<$<CONFIG:Release>:/O2>)
    else()
        target_compile_options(geometrize-${VARIANT_NAME} PRIVATE -O2 -Wall)
    endif()

    add_executable(geobench-${OUT_SUFFIX} main.cpp sha256.h stb_image.h)
    target_link_libraries(geobench-${OUT_SUFFIX} PRIVATE geometrize-${VARIANT_NAME})
    target_compile_definitions(geobench-${OUT_SUFFIX} PRIVATE ${DEFINE_NAME})
    if(MSVC)
        target_compile_options(geobench-${OUT_SUFFIX} PRIVATE /EHsc /W3 /utf-8 /Zi /FS
            $<$<CONFIG:Release>:/O2>)
    endif()
endfunction()

add_geometrize_variant(base "${CMAKE_CURRENT_SOURCE_DIR}/../baseline-lib" base GEOBENCH_BASE)
add_geometrize_variant(fast "${CMAKE_CURRENT_SOURCE_DIR}/../improved-lib" fast GEOBENCH_FAST)

# ---- 单元测试(geotest-base / geotest-fast)----
# 同一份测试源码链接两套库:共享 golden 用例双变体全 PASS = 函数级 bit-exact 门禁;
# 有意分叉点(Ellipse 非对称边界修复)用 GEOTEST_BASE / GEOTEST_FAST 宏分流,各自锁定行为。
# 源文件显式列出(不用 GLOB):新增用例必须显式登记,防"文件加了没编译"的假绿。
enable_testing()

set(GEOTEST_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/../test/main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../test/test_bitmap.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../test/test_scanline.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../test/test_rasterize.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../test/test_core.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../test/test_state.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../test/test_model.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../test/test_commonutil.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../test/test_exporter.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../test/test_shapemutator.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../test/test_errorweightmap.cpp"
)

function(add_geotest_variant VARIANT_NAME OUT_SUFFIX DEFINE_NAME)
    add_executable(geotest-${OUT_SUFFIX} ${GEOTEST_SOURCES})
    target_link_libraries(geotest-${OUT_SUFFIX} PRIVATE geometrize-${VARIANT_NAME})
    target_compile_definitions(geotest-${OUT_SUFFIX} PRIVATE ${DEFINE_NAME})
    target_include_directories(geotest-${OUT_SUFFIX} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../test")
    if(MSVC)
        target_compile_options(geotest-${OUT_SUFFIX} PRIVATE /EHsc /W3 /permissive- /utf-8 /Zi /FS
            $<$<CONFIG:Release>:/O2>)
    else()
        target_compile_options(geotest-${OUT_SUFFIX} PRIVATE -O2 -Wall)
    endif()
    if(MSVC)
        target_link_libraries(geotest-${OUT_SUFFIX} PRIVATE dbghelp)
    endif()
    if(MSVC)
        target_link_options(geotest-${OUT_SUFFIX} PRIVATE "/MAP$$<TARGET_FILE_DIR:geotest-${OUT_SUFFIX}>/geotest-${OUT_SUFFIX}.map")
    endif()
    add_test(NAME geotest-${OUT_SUFFIX} COMMAND geotest-${OUT_SUFFIX})
endfunction()

add_geotest_variant(base base GEOTEST_BASE)
add_geotest_variant(fast fast GEOTEST_FAST)
