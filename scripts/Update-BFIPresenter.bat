@echo off
rem Runs Update-BFIPresenter.ps1 from this folder, bypassing PowerShell's script policy and the
rem "downloaded from the internet" block for this run only. Extra arguments are passed on,
rem e.g.  Update-BFIPresenter.bat -DryRun
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Update-BFIPresenter.ps1" %*
echo.
pause
