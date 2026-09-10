@echo off
call "D:\Visual Studio2026\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 exit /b 1
cmake --preset windows-ninja-msvc
if errorlevel 1 exit /b 1
cmake --build --preset windows-ninja-msvc
