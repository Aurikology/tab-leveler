@echo off
rem Builds dsp\out\leveler.wasm (standalone, no JS glue) and checks it against the native build.
rem Needs the emsdk at C:\dev\emsdk (or EMSDK set) and build.cmd run first for parity_native.exe.
setlocal
if "%EMSDK%"=="" set EMSDK=C:\dev\emsdk
call "%EMSDK%\emsdk_env.bat" >nul 2>&1 || exit /b 1
cd /d "%~dp0"
if not exist out mkdir out
call em++ -O3 -std=c++17 -Iinclude src\leveler.cpp src\wasm_api.cpp -o out\leveler.wasm ^
  --no-entry -sSTANDALONE_WASM -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=1MB -fno-exceptions ^
  -sEXPORTED_FUNCTIONS=_tl_create,_tl_destroy,_tl_buffer,_tl_process,_tl_set_target,_tl_gain_db,_tl_limiter_db,_tl_input_momentary,_tl_input_short_term,_tl_latency_frames,_tl_reset,_malloc,_free || exit /b 1
for %%A in (out\leveler.wasm) do echo leveler.wasm %%~zA bytes
build\parity_native.exe out\native.f32 || exit /b 1
node tests\parity.mjs out\leveler.wasm out\native.f32
