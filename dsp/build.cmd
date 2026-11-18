@echo off
rem Native build + tests with the Visual Studio Build Tools (no CMake on PATH needed).
setlocal
set VS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set CMAKE=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
cd /d "%~dp0"
"%CMAKE%" -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release >nul || exit /b 1
"%CMAKE%" --build build || exit /b 1
build\test_leveler.exe
