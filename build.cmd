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

rem Locate Visual Studio 2022: try BuildTools, then Community, then vswhere probe
set "VSDIR=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
if not exist "%VSDIR%\VC\Tools\MSVC" set "VSDIR=C:\Program Files\Microsoft Visual Studio\2022\Community"
if not exist "%VSDIR%\VC\Tools\MSVC" set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSDIR%\VC\Tools\MSVC" if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`""%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath"`) do set "VSDIR=%%i"
if not exist "%VSDIR%\VC\Tools\MSVC" (
    echo [ERROR] No Visual Studio 2022 with MSVC v143 tools found.
    echo         Install VS 2022 BuildTools or Community with the C++ workload first.
    exit /b 1
)
echo [info] VSDIR=%VSDIR%
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
rem 注意：XAML 编译管线实际来自 WindowsAppSDK NuGet 包（vcxproj 里有手写胶水层），
rem 不依赖 VS 的 WindowsXaml 规则；目录缺失只影响提示，不阻塞构建。
if not exist "%XAMLDIR%" echo [WARN] VS WindowsXaml rules missing (build still works, see winui/UnlockerApp.vcxproj glue notes)

:have_xaml
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
