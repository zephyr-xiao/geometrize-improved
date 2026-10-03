@echo off
rem Qt 应用 CMake 构建脚本(VS 生成器,无需 vcvars64:MSBuild 自定位工具链)
rem 用法:双击或命令行执行;产物在 build\Release\Geometrize.exe
rem Qt6 迁移后本链路是唯一在维护的构建链(geometrize.pro/qmake 保留但不再维护);升 Qt 版本只改下面一行
set QT_DIR=D:\Qt\6.8.3\msvc2022_64
set PATH=C:\Program Files\CMake\bin;%QT_DIR%\bin;%PATH%
rem %QT_DIR:\=/% 是 cmd 变量替换:把反斜杠换成斜杠,避免 CMake 参数里的转义歧义
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=%QT_DIR:\=/% %*
if errorlevel 1 exit /b 1
cmake --build build --config Release --parallel
if errorlevel 1 exit /b 1
rem 部署 Qt 运行时 DLL 到输出目录(否则 exe 双击缺 Qt6*.dll)
rem --no-translations:应用自身翻译已编进 qrc,不需要 Qt 自带 qt_*.qm
windeployqt --no-compiler-runtime --no-translations build\Release\Geometrize.exe
