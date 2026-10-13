@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set "BO3_ROOT=%~dp0..\.."
set "BO3_MW2_SOURCE=%BO3_ROOT%\tools\mw2-recompiled-reference\runtime\gpu\vulkan"
set "BO3_SHADER_OUT=%BO3_ROOT%\build\native-shader-tools"
set "BO3_TRANSLATOR_NAME=mw2_translate"
if /i "%~1"=="bo3" (
  set "BO3_MW2_SOURCE=%BO3_ROOT%\tools\bo3-mw2-shader-translator\runtime\gpu\vulkan"
  set "BO3_TRANSLATOR_NAME=mw2_bo3_translate"
)
if not exist "%BO3_SHADER_OUT%" mkdir "%BO3_SHADER_OUT%"
"%BO3_ROOT%\tools\llvm-20.1.8\bin\clang++.exe" -std=c++20 -O2 -DNOMINMAX -I"%BO3_MW2_SOURCE%" "%BO3_ROOT%\nx\shader-tools\mw2_translate.cpp" "%BO3_MW2_SOURCE%\shader_translator.cpp" "%BO3_MW2_SOURCE%\spirv.cpp" -o "%BO3_SHADER_OUT%\%BO3_TRANSLATOR_NAME%.exe"
exit /b %errorlevel%
