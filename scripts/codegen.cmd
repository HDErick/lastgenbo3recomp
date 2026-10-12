@echo off
setlocal
set "BO3_ROOT=%~dp0.."
"%BO3_ROOT%\tools\rexglue-bootstrap\rexglue.exe" codegen "%BO3_ROOT%\app\bo3_zm_manifest.toml"
exit /b %errorlevel%
