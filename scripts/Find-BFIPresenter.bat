@echo off
rem Runs Find-BFIPresenter.ps1 from this folder, bypassing PowerShell's script policy and the
rem "downloaded from the internet" block for this run only. Extra arguments are passed on,
rem e.g.  Find-BFIPresenter.bat -DryRun
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Find-BFIPresenter.ps1" %*
echo.
pause
