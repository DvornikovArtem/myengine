@echo off
setlocal

cd /d "%~dp0"

set "BUILD_DIR=build"
set "CONFIG=Debug"
set "EXE=%BUILD_DIR%\app\%CONFIG%\myengine.exe"

if not "%~1"=="" set "CONFIG=%~1"
if not "%~2"=="" set "BUILD_DIR=%~2"
set "EXE=%BUILD_DIR%\app\%CONFIG%\myengine.exe"

if not exist "%EXE%" (
  echo [RUN] Executable not found: %EXE%
  call build.bat %CONFIG% myengine %BUILD_DIR%
  if errorlevel 1 exit /b 1
)

echo [RUN] Starting %EXE%
pushd "%BUILD_DIR%\app\%CONFIG%"
myengine.exe
set "EXIT_CODE=%ERRORLEVEL%"
popd

echo [RUN] Exit code: %EXIT_CODE%
exit /b %EXIT_CODE%
