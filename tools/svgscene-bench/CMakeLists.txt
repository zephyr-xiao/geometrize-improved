cmake_minimum_required(VERSION 3.20)
project(svgscene_bench LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 与主工程同一套 Qt(Qt 5.15.2 msvc2019_64);Qt 路径经 -DCMAKE_PREFIX_PATH 传入
find_package(Qt5 5.15 REQUIRED COMPONENTS Core Gui Widgets Svg)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(svgscene_bench main.cpp)
target_include_directories(svgscene_bench PRIVATE "${REPO_ROOT}/src/improved-lib/geometrize")
# 复用 geobench 已构建的改进版库(先构建 geobench,见 tools/svgscene-bench/README.md)
target_link_libraries(svgscene_bench PRIVATE Qt5::Widgets Qt5::Svg
    "${REPO_ROOT}/src/geobench/build/Release/geometrize-fast.lib")
if(MSVC)
    target_compile_options(svgscene_bench PRIVATE /EHsc /utf-8 /O2)
endif()
