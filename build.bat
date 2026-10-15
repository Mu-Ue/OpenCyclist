@echo off
setlocal
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (
    echo Visual Studio with the C++ workload was not found.
    exit /b 1
)
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

rem Arguments, such as -DSTRAVA_CLIENT_ID=12345, go to CMake and stick for later builds.
set "CONFIGURE="
if not exist build\build.ninja set "CONFIGURE=1"
if not "%~1"=="" set "CONFIGURE=1"
if defined CONFIGURE cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release %* || exit /b 1
cmake --build build || exit /b 1
echo.
echo Built "build\OpenCyclist.exe"
