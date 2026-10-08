@echo off
setlocal
if not exist build mkdir build
for /f "delims=" %%G in ('where g++') do if not defined GXX set "GXX=%%G"
if not defined GXX (
  echo Error: g++ was not found on PATH.
  exit /b 1
)
for %%I in ("%GXX%") do set "GCC_BIN=%%~dpI"

g++ -O2 -std=c++11 -fopenmp business_analysis.cpp -o build\business_analysis.exe 2>build\build.log
if not errorlevel 1 goto run_program

if not exist "%GCC_BIN%pthreadGC-3.dll" goto build_failed
if not exist "%GCC_BIN%dlltool.exe" goto build_failed
if not exist "%GCC_BIN%objdump.exe" goto build_failed
if not exist build\openmp mkdir build\openmp
powershell -NoProfile -ExecutionPolicy Bypass -Command "$dll = '%GCC_BIN%pthreadGC-3.dll'; $symbols = & '%GCC_BIN%objdump.exe' -p $dll | ForEach-Object { if ($_ -match '^\s*\[\s*\d+\]\s+(pthread_[A-Za-z0-9_]+)$') { $matches[1] } }; @('LIBRARY pthreadGC-3.dll','EXPORTS') + $symbols | Set-Content -LiteralPath 'build\openmp\pthread_import.def' -Encoding ASCII"
if errorlevel 1 goto build_failed
"%GCC_BIN%dlltool.exe" -d build\openmp\pthread_import.def -D pthreadGC-3.dll -l build\openmp\libpthread.a
if errorlevel 1 goto build_failed
g++ -O2 -std=c++11 -fopenmp -Lbuild\openmp business_analysis.cpp -o build\business_analysis.exe 2>build\build.log
if errorlevel 1 goto build_failed

:run_program
set "PATH=%GCC_BIN%;%PATH%"
build\business_analysis.exe %*
exit /b %errorlevel%

:build_failed
type build\build.log
echo Error: C++ build failed. Install a GCC distribution with OpenMP and its pthread linker library.
exit /b 1
