@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set "BO3_ROOT=%~dp0..\.."
set "BO3_SHADER_SOURCE=%BO3_ROOT%\tools\bo3-shader-translator\shaders"
set "BO3_SHADER_OUT=%BO3_ROOT%\build\native-shader-tools"
if not exist "%BO3_SHADER_OUT%" mkdir "%BO3_SHADER_OUT%"
"%BO3_ROOT%\tools\llvm-20.1.8\bin\clang++.exe" -std=c++20 -O2 -Wno-switch -Wno-unused-variable -Wno-null-arithmetic -fms-extensions -DNOMINMAX -DFMT_HEADER_ONLY -DXXH_INLINE_ALL -DMASSEFFECT_RECOMP -include "%BO3_SHADER_SOURCE%\XenosRecomp\pch_min.h" -I"%BO3_SHADER_SOURCE%" -I"%BO3_SHADER_SOURCE%\XenosRecomp" -I"%BO3_ROOT%\tools\nx-sdk\thirdparty\fmt\include" -I"%BO3_ROOT%\tools\nx-sdk\thirdparty\xxHash" "%BO3_SHADER_SOURCE%\tools\masseffect_hlsl.cpp" "%BO3_SHADER_SOURCE%\XenosRecomp\shader_recompiler.cpp" -o "%BO3_SHADER_OUT%\xenos_hlsl.exe"
exit /b %errorlevel%
