cmake_minimum_required(VERSION 3.20)
project(svgscene_bench LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 与主工程同一套 Qt(Qt 6.8.3 LTS msvc2022_64);Qt 路径经 -DCMAKE_PREFIX_PATH 传入。
# Qt6 把 QGraphicsSvgItem 从 QtSvg 移到 QtSvgWidgets 模块,必须显式链(陷阱 #19)。
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Svg SvgWidgets)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# 复用 geobench 已构建的改进版静态库(先构建 geobench,见本目录 README)。
# 做成 cache 变量:换构建目录/配置时用 -DGEOMETRIZE_FAST_LIB=... 覆盖,不必改本文件。
set(GEOMETRIZE_FAST_LIB "${REPO_ROOT}/src/geobench/build/Release/geometrize-fast.lib"
    CACHE FILEPATH "geometrize-fast 静态库路径(默认取 geobench 的 Release 构建产物)")

add_executable(svgscene_bench main.cpp)
target_include_directories(svgscene_bench PRIVATE "${REPO_ROOT}/src/improved-lib/geometrize")
target_link_libraries(svgscene_bench PRIVATE Qt6::Widgets Qt6::Svg Qt6::SvgWidgets
    "${GEOMETRIZE_FAST_LIB}")
if(MSVC)
    target_compile_options(svgscene_bench PRIVATE /EHsc /utf-8 /O2)
endif()
