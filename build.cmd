@echo off
setlocal
cd /d "%~dp0"

rem Clear duplicated proxy vars (HTTP_PROXY / http_proxy): the MSBuild CL task throws MSB6001 otherwise
set "HTTP_PROXY="
set "HTTPS_PROXY="
set "http_proxy="
set "https_proxy="
set "ALL_PROXY="
set "all_proxy="

rem Locate Visual Studio 2022: try BuildTools, then Community, then a vswhere probe.
rem NOTE: %ProgramFiles(x86)% expands to a path containing a closing paren. Using it directly
rem inside an if/for compound statement makes cmd close that statement early and abort with
rem "\Microsoft was unexpected at this time" -- which only happens where neither hardcoded
rem path exists (i.e. on CI runners, not on a dev box). Delayed expansion (!var!) is expanded
rem after parsing, so it is safe there.
setlocal EnableDelayedExpansion
set "VSDIR=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
if not exist "%VSDIR%\VC\Tools\MSVC" set "VSDIR=C:\Program Files\Microsoft Visual Studio\2022\Community"
if exist "%VSDIR%\VC\Tools\MSVC" goto have_vs

set "VSWHERE=!ProgramFiles(x86)!\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" goto no_vs
for /f "usebackq delims=" %%i in (`""!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath"`) do set "VSDIR=%%i"

:have_vs
if not exist "%VSDIR%\VC\Tools\MSVC" goto no_vs
echo [info] VSDIR=%VSDIR%
goto vs_ready

:no_vs
echo [ERROR] No Visual Studio 2022 with MSVC v143 tools found.
echo         Install VS 2022 BuildTools or Community with the C++ workload first.
exit /b 1

:vs_ready
set "CMAKE=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not exist "%CMAKE%" set "CMAKE=cmake"

set "PY="
if exist "%USERPROFILE%\.workbuddy\binaries\python\versions\3.13.12\python.exe" set "PY=%USERPROFILE%\.workbuddy\binaries\python\versions\3.13.12\python.exe"

if /i "%~1"=="single" goto single

echo [1/2] Configure...
"%CMAKE%" -S . -B build -G "Visual Studio 17 2022" -A x64 -DUNLOCKER_BUILD_APP=ON
if errorlevel 1 exit /b 1
if defined PY "%CMAKE%" -S . -B build -DPython3_EXECUTABLE="%PY%"

echo [2/2] Build Release...
"%CMAKE%" --build build --config Release
if errorlevel 1 exit /b 1

echo.
echo Done: build\Release\
dir /b build\Release\*.exe build\Release\*.dll 2>nul
exit /b 0

:single
rem Single-file build: static CRT so the exe needs no VC++ Redistributable.
rem Keep this file ASCII-only (see the note above about DBCS consoles).
echo [1/2] Configure (static CRT)...
"%CMAKE%" -S . -B build-single -G "Visual Studio 17 2022" -A x64 -DUNLOCKER_BUILD_APP=ON -DUNLOCKER_STATIC_CRT=ON
if errorlevel 1 exit /b 1
if defined PY "%CMAKE%" -S . -B build-single -DPython3_EXECUTABLE="%PY%"

echo [2/2] Build Release...
"%CMAKE%" --build build-single --config Release --target Unlocker
if errorlevel 1 exit /b 1

echo.
echo Done: build-single\Release\Unlocker.exe ^(no VC++ runtime needed^)
exit /b 0
