@echo off
setlocal

cd /d "%~dp0"

set "BUILD_DIR=build"
set "CONFIG=Debug"
set "TARGET=myengine"

if not "%~1"=="" set "CONFIG=%~1"
if not "%~2"=="" set "TARGET=%~2"
if not "%~3"=="" set "BUILD_DIR=%~3"

echo [BUILD] Dir: %BUILD_DIR%
echo [BUILD] Config: %CONFIG%
echo [BUILD] Target: %TARGET%

cmake --build "%BUILD_DIR%" --config %CONFIG% --target %TARGET%
if errorlevel 1 (
  echo [BUILD] FAILED
  exit /b 1
)

echo [BUILD] OK
exit /b 0
