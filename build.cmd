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
set "XAMLDIR=%VSDIR%\MSBuild\Microsoft\WindowsXaml\v17.0"
set "CMAKE=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not exist "%CMAKE%" set "CMAKE=cmake"

set "PY="
if exist "%USERPROFILE%\.workbuddy\binaries\python\versions\3.13.12\python.exe" set "PY=%USERPROFILE%\.workbuddy\binaries\python\versions\3.13.12\python.exe"

if /i "%~1"=="winui" goto winui

echo [1/2] Configure...
"%CMAKE%" -S . -B build -G "Visual Studio 17 2022" -A x64 -DUNLOCKER_BUILD_APP=ON -DUNLOCKER_BUILD_WINUI=OFF
if errorlevel 1 exit /b 1
if defined PY "%CMAKE%" -S . -B build -DPython3_EXECUTABLE="%PY%"

echo [2/2] Build Release...
"%CMAKE%" --build build --config Release
if errorlevel 1 exit /b 1

echo.
echo Done: build\Release\
dir /b build\Release\*.exe build\Release\*.dll 2>nul
exit /b 0

:winui
rem NOTE: the XAML compile pipeline comes from the WindowsAppSDK NuGet package (the vcxproj has a
rem hand-written glue layer); it does not rely on VS WindowsXaml rules. A missing dir only warns.
rem IMPORTANT: keep this file ASCII-only. Under a GBK console, a UTF-8 Chinese comment can swallow
rem the trailing CR into a DBCS pair, which splits the line and runs its tail as a command.
if not exist "%XAMLDIR%" echo [WARN] VS WindowsXaml rules missing (build still works, see winui/UnlockerApp.vcxproj glue notes)

:have_xaml
rem nuget.exe is an 8 MB binary and is not committed; download it on demand. Offline: drop it in.
if exist "tools\nuget\nuget.exe" goto have_nuget
echo [0/5] Downloading nuget.exe...
if not exist "tools\nuget" mkdir "tools\nuget"
powershell -NoProfile -ExecutionPolicy Bypass -Command "Invoke-WebRequest -Uri 'https://dist.nuget.org/win-x86-commandline/latest/nuget.exe' -OutFile 'tools\nuget\nuget.exe'"
if not exist "tools\nuget\nuget.exe" (
    echo [ERROR] Failed to download nuget.exe. Put it at tools\nuget\nuget.exe manually and retry.
    exit /b 1
)

:have_nuget
if exist "winui\packages\Microsoft.WindowsAppSDK.1.7.251107005" goto have_wasdk
echo [1/5] Restore Microsoft.WindowsAppSDK...
tools\nuget\nuget.exe install Microsoft.WindowsAppSDK -Version 1.7.251107005 -OutputDirectory winui\packages -NonInteractive
if errorlevel 1 exit /b 1

:have_wasdk
if exist "winui\packages\Microsoft.Windows.CppWinRT.3.0.260818.1" goto have_cppwinrt
echo [2/5] Restore Microsoft.Windows.CppWinRT...
tools\nuget\nuget.exe install Microsoft.Windows.CppWinRT -OutputDirectory winui\packages -NonInteractive
if errorlevel 1 exit /b 1

:have_cppwinrt
echo [3/5] Configure...
"%CMAKE%" -S . -B build -G "Visual Studio 17 2022" -A x64 -DUNLOCKER_BUILD_APP=ON -DUNLOCKER_BUILD_WINUI=ON
if errorlevel 1 exit /b 1
if defined PY "%CMAKE%" -S . -B build -DPython3_EXECUTABLE="%PY%"

echo [4/5] Build WinUI...
"%CMAKE%" --build build --config Release --target winui
if errorlevel 1 exit /b 1

echo [5/5] Verify output...
if defined PY (
    "%PY%" tools\check-winui-output.py winui\build-winui\x64\Release
    if errorlevel 1 exit /b 1
) else (
    echo [WARN] Python not found, skip output verification.
)

echo.
echo Done: winui\build-winui\x64\Release\Unlocker.exe
exit /b 0
