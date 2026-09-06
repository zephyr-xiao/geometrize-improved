@echo off
rem Qt 应用 CMake 构建脚本(VS 生成器,无需 vcvars64:MSBuild 自定位工具链)
rem 用法:双击或命令行执行;产物在 build\Release\Geometrize.exe
set PATH=C:\Program Files\CMake\bin;D:\Qt\5.15.2\msvc2019_64\bin;%PATH%
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=D:/Qt/5.15.2/msvc2019_64 %*
if errorlevel 1 exit /b 1
cmake --build build --config Release --parallel
if errorlevel 1 exit /b 1
rem 部署 Qt 运行时 DLL 到输出目录(否则 exe 双击缺 Qt5*.dll)
windeployqt --no-compiler-runtime build\Release\Geometrize.exe
