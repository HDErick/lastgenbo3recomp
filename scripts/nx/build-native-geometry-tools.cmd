@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set "BO3_ROOT=%~dp0..\.."
set "BO3_GEOMETRY_SOURCE=%BO3_ROOT%\tools\bo3-mw2-shader-translator\runtime\gpu\vulkan"
set "BO3_GEOMETRY_OUT=%BO3_ROOT%\build\native-shader-tools"
if not exist "%BO3_GEOMETRY_OUT%" mkdir "%BO3_GEOMETRY_OUT%"
"%BO3_ROOT%\tools\llvm-20.1.8\bin\clang++.exe" -std=c++20 -O2 -DNOMINMAX -I"%BO3_GEOMETRY_SOURCE%" "%BO3_ROOT%\nx\native\rectangle_shader_validate.cpp" "%BO3_ROOT%\nx\native\rectangle_shader.cpp" "%BO3_GEOMETRY_SOURCE%\spirv.cpp" -o "%BO3_GEOMETRY_OUT%\rectangle_shader_validate.exe"
if errorlevel 1 exit /b 1
"%BO3_ROOT%\tools\llvm-20.1.8\bin\clang++.exe" -std=c++20 -O2 "%BO3_ROOT%\nx\native\edram_clear_test.cpp" -o "%BO3_GEOMETRY_OUT%\edram_clear_test.exe"
exit /b %errorlevel%
