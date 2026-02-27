@echo off
setlocal

cd /d "%~dp0"

set "BUILD_DIR=build"
set "CONFIG=Debug"

if not "%~1"=="" set "CONFIG=%~1"
if not "%~2"=="" set "BUILD_DIR=%~2"

call setup.bat %BUILD_DIR%
if errorlevel 1 exit /b 1

call build.bat %CONFIG% myengine %BUILD_DIR%
if errorlevel 1 exit /b 1

echo [REBUILD] OK
exit /b 0
