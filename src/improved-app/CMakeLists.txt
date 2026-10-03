# Geometrize Qt 应用 CMake 构建(Q4.2;Qt6 迁移后为唯一在维护的构建链路)
# geometrize.pro(qmake)保留但不再维护:Qt6 下 qmake 已被 CMake 取代,本文件失败可回退 Qt5 链路重建。
# 生成物全部落在 build/(out-of-source),不触碰 qmake 在源树的 obj/qt_gen/debug/release。
# 用法见 build_cmake.bat。

cmake_minimum_required(VERSION 3.20)
project(Geometrize LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt 自动化:moc(Q_OBJECT 头 46 个)/ uic(22 个 .ui,源码 include "ui_*.h" 驱动)/ rcc(148 个 .qrc)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Qt6 6.8.3 LTS(msvc2022_64):CMAKE_PREFIX_PATH 由 build_cmake.bat 传入
# SvgWidgets 是 Qt6 拆出的模块(QGraphicsSvgItem/QSvgWidget 从 QtSvg 移入),必须显式链接
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Svg SvgWidgets Network Concurrent)

# ---- 源码收集(与各 .pri 的 $$files(..., true) 递归等价)----
# 应用源(geometrize/sources.pri)。排除 dataslinger/:可选 WebSocket 推流,
# qmake 下靠 ifdef DATASLINGER_INCLUDED 变空 TU,这里直接不编(所有引用点都在 ifdef 内)
file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/geometrize/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/geometrize/*.h")
list(FILTER APP_SOURCES EXCLUDE REGEX "/dataslinger/")

# 算法库副本(lib/geometrize/geometrize/geometrize.pri)
file(GLOB_RECURSE LIB_GEOMETRIZE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/geometrize/geometrize/geometrize/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/geometrize/geometrize/geometrize/*.h")

# BurstLinker GIF 编码(lib/burstlinker/burstlinker.pri)
file(GLOB_RECURSE LIB_BURSTLINKER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/burstlinker/burstlinker/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/burstlinker/burstlinker/src/*.h")

# 资源(resources/resources.pri $$files(*.qrc)):qrc 已由 scripts/generate_geometrize_qrcs.py
# 预生成在 resources/ 源树(148 个);仅新增/删除模板时需重跑该脚本,然后重新 configure
# (CONFIGURE_DEPENDS 会自动刷新文件列表)。qm 同理已预生成在 resources/translations/,零 lrelease。
file(GLOB GEOMETRIZE_QRCS "${CMAKE_CURRENT_SOURCE_DIR}/resources/*.qrc")

# UI 表单(dialog/*.ui,AUTOUIC 由 include 驱动,列出仅为 VS 工程可见)
file(GLOB GEOMETRIZE_FORMS "${CMAKE_CURRENT_SOURCE_DIR}/geometrize/dialog/*.ui")

add_executable(Geometrize WIN32
    ${APP_SOURCES}
    ${LIB_GEOMETRIZE_SOURCES}
    ${LIB_BURSTLINKER_SOURCES}
    ${GEOMETRIZE_QRCS}
    ${GEOMETRIZE_FORMS}
    resources/app_icons/winicon.rc
    resources/app_icons/app.manifest)
# WIN32 = GUI 子系统;WinMain→main 转发由 Qt6::Core 按 WIN32_EXECUTABLE 属性自动链接
# (Qt6CoreTargets 的 INTERFACE_LINK_LIBRARIES 里带 Qt6::EntryPointPrivate 生成式)
# app.manifest:Common-Controls SxS 依赖,MSVC 工具链把它当 linker input 自动合并进内嵌清单
# (等价于 qmake 的 /MANIFESTDEPENDENCY;target_link_options 传该选项在 VS 生成器下会被空格拆散)

# include 目录(对齐 qmake INCPATH)
target_include_directories(Geometrize PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/geometrize"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/cereal/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/chaiscript/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/geometrize/geometrize"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/burstlinker/burstlinker/src")

target_link_libraries(Geometrize PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg Qt6::SvgWidgets Qt6::Network Qt6::Concurrent)

# 任务栏进度条(TaskQueueWindow 批处理进度):Win32 原生 COM ITaskbarList3
if(WIN32)
    target_link_libraries(Geometrize PRIVATE ole32 uuid)
endif()

if(MSVC)
    # /utf-8:中文注释按 UTF-8 解析(同 pro);/bigobj:ChaiScript TU 节数;其余对齐 qmake mkspec
    # /permissive- 是 qmake 没有的加强项,若 ChaiScript 编译报错,第一件事去掉它
    target_compile_options(Geometrize PRIVATE
        /EHsc /W3 /permissive- /utf-8 /bigobj /Zi /FS /Zc:__cplusplus
        $<$<CONFIG:Release>:/O2>)
    # qmake win32-msvc mkspec 默认定义;NDEBUG(Release)与 QT_NO_DEBUG(非 Debug)由 CMake/Qt 配置包自动加
    target_compile_definitions(Geometrize PRIVATE
        UNICODE _UNICODE _ENABLE_EXTENDED_ALIGNED_STORAGE)
endif()
