@echo off
rem Qt 应用构建脚本:MSVC x64 环境 + qmake + nmake
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
set PATH=D:\Qt\5.15.2\msvc2019_64\bin;%PATH%
qmake geometrize.pro "CONFIG+=release" "CONFIG+=warn_off"
nmake /NOLOGO
