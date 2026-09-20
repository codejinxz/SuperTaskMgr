@echo off
rem ============================================================
rem  一键编译：生成 build\Release\SuperTaskMgr.exe
rem  双击运行即可；结束后自动跑一遍自测（stm_selftest）。
rem ============================================================
setlocal
echo [1/3] 编译中（Release，约 1-3 分钟）...
call "%~dp0scripts\build.bat" Release
if errorlevel 1 (
    echo.
    echo [失败] 编译未成功，请检查上方错误信息。
    pause
    exit /b 1
)
echo.
echo [2/3] 运行自测（176 项）...
"%~dp0build\Release\stm_selftest.exe" | findstr /C:"[" 
"%~dp0build\Release\stm_selftest.exe" --json >nul
if errorlevel 1 (
    echo [警告] 自测存在失败项，详见上方输出。
) else (
    echo [自测] 全部通过。
)
echo.
echo [3/3] 完成！程序在： %~dp0build\Release\SuperTaskMgr.exe
echo 提示：发布前请编辑 src\app\AboutInfo.h 填写版本号/发布时间/仓库地址后重新编译。
choice /C YN /M "立即启动程序吗"
if errorlevel 2 exit /b 0
start "" "%~dp0build\Release\SuperTaskMgr.exe"
exit /b 0
