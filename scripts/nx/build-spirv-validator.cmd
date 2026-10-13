@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set "BO3_ROOT=%~dp0..\.."
set "BO3_ROOT=%BO3_ROOT:\=/%"
set "BO3_CMAKE=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "BO3_NINJA=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
set "BO3_PYTHON=C:\Users\Erick\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe"
"%BO3_CMAKE%" -S "%BO3_ROOT%\tools\nx-sdk\thirdparty\spirv-tools" -B "%BO3_ROOT%\build\native-spirv-tools" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="%BO3_ROOT%\tools\llvm-20.1.8\bin\clang.exe" -DCMAKE_CXX_COMPILER="%BO3_ROOT%\tools\llvm-20.1.8\bin\clang++.exe" -DCMAKE_MAKE_PROGRAM="%BO3_NINJA%" -DSPIRV-Headers_SOURCE_DIR="%BO3_ROOT%/tools/nx-sdk/thirdparty/spirv-headers" -DPython3_EXECUTABLE="%BO3_PYTHON%" -DSPIRV_SKIP_TESTS=ON -DSPIRV_SKIP_EXECUTABLES=OFF -DSPIRV_WERROR=OFF
if errorlevel 1 exit /b 1
"%BO3_CMAKE%" --build "%BO3_ROOT%\build\native-spirv-tools" --target spirv-val --parallel 8
exit /b %errorlevel%
