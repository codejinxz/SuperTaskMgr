@echo off
rem 重新生成应用 Logo 资产（assets/app.ico 与 assets/logo_256.png）。
rem 用法：scripts\make_logo.bat
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 exit /b 1
cd /d "%~dp0.."
cl /nologo /O2 /EHsc /utf-8 /Fe:tools\gen_logo.exe tools\gen_logo.cpp
if errorlevel 1 exit /b 1
tools\gen_logo.exe assets
endlocal
