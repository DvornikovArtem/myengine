@echo off
setlocal EnableExtensions DisableDelayedExpansion

cd /d "%~dp0"

set "GENERATOR="
set "VS_VERSION="
set "VS_INSTALLATION="
set "ARCH=x64"
set "BUILD_DIR=build"

if not "%~1"=="" set "BUILD_DIR=%~1"

call :find_visual_studio
if errorlevel 1 exit /b 1

set "CACHED_GENERATOR="
if exist "%BUILD_DIR%\CMakeCache.txt" (
  for /f "tokens=1,* delims==" %%A in ('findstr /b /c:"CMAKE_GENERATOR:INTERNAL=" "%BUILD_DIR%\CMakeCache.txt" 2^>nul') do set "CACHED_GENERATOR=%%B"
)

if defined CACHED_GENERATOR if /i not "%CACHED_GENERATOR%"=="%GENERATOR%" (
  echo [SETUP] FAILED: build directory "%BUILD_DIR%" uses generator "%CACHED_GENERATOR%".
  echo [SETUP] Selected generator is "%GENERATOR%".
  echo [SETUP] Use a new build directory or remove the old cache manually; the directory was not changed.
  exit /b 1
)

echo [SETUP] Source: %cd%
echo [SETUP] Build dir: %BUILD_DIR%
echo [SETUP] Visual Studio: %VS_INSTALLATION%
echo [SETUP] Generator: %GENERATOR% (%ARCH%)

cmake -S . -B "%BUILD_DIR%" -G "%GENERATOR%" -A %ARCH%
if errorlevel 1 (
  echo [SETUP] FAILED
  exit /b 1
)

echo [SETUP] OK
exit /b 0

:find_visual_studio
set "VSWHERE="
set "VS18_INSTALLATION="
set "VS17_INSTALLATION="

rem Prefer the installed vswhere, then a copy available on PATH.
for %%P in (
  "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
  "%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
) do if not defined VSWHERE if exist "%%~P" set "VSWHERE=%%~P"
if not defined VSWHERE (
  for /f "delims=" %%P in ('where.exe vswhere 2^>nul') do if not defined VSWHERE set "VSWHERE=%%P"
)

if defined VSWHERE (
  for /f "usebackq delims=" %%P in (`"%VSWHERE%" -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -version "[18.0,19.0)" -latest -property installationPath 2^>nul`) do if not defined VS18_INSTALLATION if exist "%%P\VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt" set "VS18_INSTALLATION=%%P"
  if not defined VS18_INSTALLATION for /f "usebackq delims=" %%P in (`"%VSWHERE%" -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -version "[17.0,18.0)" -latest -property installationPath 2^>nul`) do if not defined VS17_INSTALLATION if exist "%%P\VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt" set "VS17_INSTALLATION=%%P"
)

rem vswhere is not included on every machine. Check standard edition paths as a fallback.
if not defined VS18_INSTALLATION for %%P in (
  "%ProgramFiles%\Microsoft Visual Studio\18\Community"
  "%ProgramFiles%\Microsoft Visual Studio\18\Professional"
  "%ProgramFiles%\Microsoft Visual Studio\18\Enterprise"
  "%ProgramFiles%\Microsoft Visual Studio\18\BuildTools"
  "%ProgramFiles(x86)%\Microsoft Visual Studio\18\Community"
  "%ProgramFiles(x86)%\Microsoft Visual Studio\18\Professional"
  "%ProgramFiles(x86)%\Microsoft Visual Studio\18\Enterprise"
  "%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools"
  "%ProgramFiles%\Microsoft Visual Studio\2026\Community"
  "%ProgramFiles%\Microsoft Visual Studio\2026\Professional"
  "%ProgramFiles%\Microsoft Visual Studio\2026\Enterprise"
  "%ProgramFiles%\Microsoft Visual Studio\2026\BuildTools"
  "%ProgramFiles(x86)%\Microsoft Visual Studio\2026\Community"
  "%ProgramFiles(x86)%\Microsoft Visual Studio\2026\Professional"
  "%ProgramFiles(x86)%\Microsoft Visual Studio\2026\Enterprise"
  "%ProgramFiles(x86)%\Microsoft Visual Studio\2026\BuildTools"
) do if not defined VS18_INSTALLATION if exist "%%~P\VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt" set "VS18_INSTALLATION=%%~P"

if not defined VS17_INSTALLATION for %%P in (
  "%ProgramFiles%\Microsoft Visual Studio\2022\Community"
  "%ProgramFiles%\Microsoft Visual Studio\2022\Professional"
  "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise"
  "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools"
  "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\Community"
  "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\Professional"
  "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\Enterprise"
  "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools"
  "%ProgramFiles%\Microsoft Visual Studio\17\Community"
  "%ProgramFiles%\Microsoft Visual Studio\17\Professional"
  "%ProgramFiles%\Microsoft Visual Studio\17\Enterprise"
  "%ProgramFiles%\Microsoft Visual Studio\17\BuildTools"
  "%ProgramFiles(x86)%\Microsoft Visual Studio\17\Community"
  "%ProgramFiles(x86)%\Microsoft Visual Studio\17\Professional"
  "%ProgramFiles(x86)%\Microsoft Visual Studio\17\Enterprise"
  "%ProgramFiles(x86)%\Microsoft Visual Studio\17\BuildTools"
) do if not defined VS17_INSTALLATION if exist "%%~P\VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt" set "VS17_INSTALLATION=%%~P"

if defined VS18_INSTALLATION (
  set "VS_VERSION=18"
  set "VS_INSTALLATION=%VS18_INSTALLATION%"
  set "GENERATOR=Visual Studio 18 2026"
  goto :eof
)
if defined VS17_INSTALLATION (
  set "VS_VERSION=17"
  set "VS_INSTALLATION=%VS17_INSTALLATION%"
  set "GENERATOR=Visual Studio 17 2022"
  goto :eof
)

echo [SETUP] FAILED: Visual Studio 18 2026 or Visual Studio 17 2022 with C++ tools was not found.
echo [SETUP] Install the Desktop development with C++ workload, then run setup.bat again.
exit /b 1
