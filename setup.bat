@echo off
setlocal

cd /d "%~dp0"

set "GENERATOR=Visual Studio 17 2022"
set "ARCH=x64"
set "BUILD_DIR=build"

if not "%~1"=="" set "BUILD_DIR=%~1"

echo [SETUP] Source: %cd%
echo [SETUP] Build dir: %BUILD_DIR%
echo [SETUP] Generator: %GENERATOR% (%ARCH%)

cmake -S . -B "%BUILD_DIR%" -G "%GENERATOR%" -A %ARCH%
if errorlevel 1 (
  echo [SETUP] FAILED
  exit /b 1
)

echo [SETUP] OK
exit /b 0
