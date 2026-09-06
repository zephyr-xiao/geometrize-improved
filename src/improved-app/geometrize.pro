QT += core gui network svg concurrent

greaterThan(QT_MAJOR_VERSION, 5) {
    QT += svgwidgets
}

TARGET = Geometrize
TEMPLATE = app

# Put intermediate files in separate folders to keep the build directory tidy
OBJECTS_DIR = obj
MOC_DIR = qt_gen
RCC_DIR = qt_gen
UI_DIR = qt_gen

# Enable C++17 features
CONFIG += c++17

# Disable assertions in release builds (for gcc, clang etc builds)
CONFIG(release, debug|release): DEFINES += NDEBUG

# Flag for ChaiScript linking
# Note that mingw32 builds may not work with the compiled-in stdlib because of a "too many sections error" with Chaiscript
msvc:QMAKE_CXXFLAGS += -bigobj

# 改进版核心库源码含中文注释,MSVC 须显式按 UTF-8 解析(否则 GBK 代码页下注释被切碎产生假语法错误)
msvc:QMAKE_CXXFLAGS += -utf-8

# 任务栏进度条(TaskQueueWindow 批处理进度):Win32 原生 COM ITaskbarList3
win32:LIBS += -lole32 -luuid

# Options for the Mac AppStore version of Geometrize
macx {
    include($$PWD/osx/geometrize_osx.pri)
}

# Some versions of gcc have linker issues like:
# //lib/x86_64-linux-gnu/libdl.so.2: error adding symbols: DSO missing from command line
linux {
    *-g++* {
        LIBS += -ldl
    }
}

# Include library dependencies
INCLUDEPATH += $$PWD/lib/cereal/include \
    $$PWD/lib/chaiscript/include

include($$PWD/lib/geometrize/geometrize/geometrize.pri)
include($$PWD/lib/burstlinker/burstlinker.pri)

# Optional support for WebSocket connections for sending and receiving
# images/data. Uses Boost, see https://github.com/Tw1ddle/dataslinger-lib

#CONFIG += DATASLINGER_INCLUDED
#DEFINES += DATASLINGER_INCLUDED

DATASLINGER_INCLUDED {
    # Include Boost - you will need to set your own path
    INCLUDEPATH += C:/Development/boost_1_68_0/
    DEPENDPATH += C:/Development/boost_1_68_0/

    # Include Boost libs
    LIBS += "-LC:/Development/boost_1_68_0/stage/x64/lib/"

    include($$PWD/lib/dataslinger/dataslinger.pri)
}

# Include the Geometrize headers, sources, UI forms
include($$PWD/geometrize/sources.pri)

# Generate localization TS files and then binary QM files, which it puts into the resources folder
include($$PWD/translations/translations.pri)

# Include resources that Geometrize requires, such as icons, images and scripts
# Note this must come last, as resource files are automatically generated
include($$PWD/resources/resources.pri)
