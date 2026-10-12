@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set "BO3_ROOT=%~dp0.."
set "BO3_CMAKE=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "BO3_NINJA=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
set "PATH=%BO3_ROOT%\tools\llvm-20.1.8\bin;%PATH%"
"%BO3_CMAKE%" -S "%BO3_ROOT%\app" -B "%BO3_ROOT%\build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="%BO3_ROOT%\tools\llvm-20.1.8\bin\clang.exe" -DCMAKE_CXX_COMPILER="%BO3_ROOT%\tools\llvm-20.1.8\bin\clang++.exe" -DCMAKE_MAKE_PROGRAM="%BO3_NINJA%" -DREXSDK_DIR="%BO3_ROOT%\thirdparty\rexglue-sdk" -DCMAKE_C_FLAGS=-march=x86-64-v3 -DCMAKE_CXX_FLAGS=-march=x86-64-v3
if errorlevel 1 exit /b 1
"%BO3_CMAKE%" --build "%BO3_ROOT%\build" --target bo3_zm --parallel 8
exit /b %errorlevel%
