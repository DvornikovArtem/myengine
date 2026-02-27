@echo off
setlocal

cd /d "%~dp0"

set "BUILD_DIR=build"
if not "%~1"=="" set "BUILD_DIR=%~1"

if exist "%BUILD_DIR%" (
  echo [CLEAN] Removing %BUILD_DIR%
  rmdir /s /q "%BUILD_DIR%"
)

if exist "%BUILD_DIR%" (
  echo [CLEAN] FAILED
  exit /b 1
)

echo [CLEAN] OK
exit /b 0
