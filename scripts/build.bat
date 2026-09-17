@echo off
rem Official build entry. Usage: scripts\build.bat [Debug|Release]  (default Release)
rem Notes: we deliberately do NOT call vcvarsall.bat. The Visual Studio generator
rem (MSBuild) locates the MSVC toolset itself, which is robust when launched from
rem non-developer shells (git-bash etc).
setlocal
set "CONFIG=%~1"
if "%CONFIG%"=="" set CONFIG=Release

set "CMAKE_EXE=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not exist "%CMAKE_EXE%" (
    for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.CMake -find "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" ^|^| echo MISSING`) do set "CMAKE_EXE=%%i"
)
if not exist "%CMAKE_EXE%" (
    echo CMake not found ^(VS2022 with CMake component required^).
    exit /b 1
)

"%CMAKE_EXE%" -S "%~dp0.." -B "%~dp0..\build" -A x64 || exit /b 1
"%CMAKE_EXE%" --build "%~dp0..\build" --config %CONFIG% -- /m /v:m || exit /b 1
exit /b 0
