@echo off
setlocal
rem Registers the BFI Presenter Vulkan layer (64-bit and 32-bit games) for the current Windows user (no admin needed).
rem Run as administrator, it also registers it for all users: needed when games run as administrator (User Account
rem Control off, or Steam / the game set to run as administrator) -- Windows' Vulkan loader then ignores per-user layers.
rem It only does something in games that have BFIPresenter.ini next to their .exe.
set "DEST=%LOCALAPPDATA%\BFIPresenter\Vulkan"
set "KEY=HKCU\SOFTWARE\Khronos\Vulkan\ImplicitLayers"
if not exist "%DEST%" mkdir "%DEST%"
copy /Y "%~dp0BFIPresenterVK.dll" "%DEST%\" >nul
if errorlevel 1 goto copyfail
copy /Y "%~dp0BFIPresenterVK32.dll" "%DEST%\" >nul
if errorlevel 1 goto copyfail
copy /Y "%~dp0VkLayer_BFI_presenter.json" "%DEST%\" >nul
copy /Y "%~dp0VkLayer_BFI_presenter32.json" "%DEST%\" >nul
rem Remove and add again so the layers are listed last, after ReShade: layers listed later sit closer to the driver.
reg delete "%KEY%" /v "%DEST%\VkLayer_BFI_presenter.json" /f >nul 2>&1
reg delete "%KEY%" /v "%DEST%\VkLayer_BFI_presenter32.json" /f >nul 2>&1
reg add "%KEY%" /v "%DEST%\VkLayer_BFI_presenter.json" /t REG_DWORD /d 0 /f >nul
if errorlevel 1 goto regfail
reg add "%KEY%" /v "%DEST%\VkLayer_BFI_presenter32.json" /t REG_DWORD /d 0 /f >nul
if errorlevel 1 goto regfail
set "ALL="
net session >nul 2>&1
if errorlevel 1 goto peruser
set "MKEY=HKLM\SOFTWARE\Khronos\Vulkan\ImplicitLayers"
reg delete "%MKEY%" /v "%DEST%\VkLayer_BFI_presenter.json" /f >nul 2>&1
reg delete "%MKEY%" /v "%DEST%\VkLayer_BFI_presenter32.json" /f >nul 2>&1
reg add "%MKEY%" /v "%DEST%\VkLayer_BFI_presenter.json" /t REG_DWORD /d 0 /f >nul
if errorlevel 1 goto regfail
reg add "%MKEY%" /v "%DEST%\VkLayer_BFI_presenter32.json" /t REG_DWORD /d 0 /f >nul
set "ALL= -- for this user and all users (it also loads in games run as administrator)"
:peruser
echo.
echo BFI Presenter Vulkan layer installed to %DEST% (64-bit and 32-bit)%ALL%
if not defined ALL echo If games run as administrator here (User Account Control off, Steam set to run as administrator), run this file again with right-click, Run as administrator.
echo.
echo Vulkan implicit layers, in load order (all machine-wide ones first, then per-user).
echo ReShade must be listed BEFORE BFI Presenter:
reg query "HKLM\SOFTWARE\Khronos\Vulkan\ImplicitLayers" 2>nul
reg query "%KEY%" 2>nul
echo.
echo To use it in a game: copy BFIPresenter.ini into the game folder, next to its .exe (the same file the DX12 add-on uses).
echo BFIPresenterVK.log appears there when the layer is active.
pause
exit /b 0
:copyfail
echo Could not copy the layer files -- close any running Vulkan game and try again.
pause
exit /b 1
:regfail
echo Could not register the layer in the registry.
pause
exit /b 1
