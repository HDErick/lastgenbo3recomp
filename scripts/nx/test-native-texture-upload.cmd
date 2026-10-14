@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set "BO3_ROOT=%~dp0..\.."
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build "%BO3_ROOT%\build\pc-vulkan" --target bo3_native_texture_upload_test bo3_native_resolve_ownership_test bo3_native_color_resolve_test bo3_native_pipeline_state_test bo3_native_texture_watch_test bo3_native_physical_access_test bo3_native_texture_residency_test
if errorlevel 1 exit /b 1
"%BO3_ROOT%\build\pc-vulkan\bo3_native_texture_upload_test.exe"
if errorlevel 1 exit /b 1
"%BO3_ROOT%\build\pc-vulkan\bo3_native_resolve_ownership_test.exe"
if errorlevel 1 exit /b 1
"%BO3_ROOT%\build\pc-vulkan\bo3_native_color_resolve_test.exe"
if errorlevel 1 exit /b 1
"%BO3_ROOT%\build\pc-vulkan\bo3_native_pipeline_state_test.exe"
if errorlevel 1 exit /b 1
"%BO3_ROOT%\build\pc-vulkan\bo3_native_texture_watch_test.exe"
if errorlevel 1 exit /b 1
"%BO3_ROOT%\build\pc-vulkan\bo3_native_physical_access_test.exe"
if errorlevel 1 exit /b 1
"%BO3_ROOT%\build\pc-vulkan\bo3_native_texture_residency_test.exe"
exit /b %errorlevel%
