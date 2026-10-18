@echo off
setlocal
set "DEST=%LOCALAPPDATA%\BFIPresenter\Vulkan"
reg delete "HKCU\SOFTWARE\Khronos\Vulkan\ImplicitLayers" /v "%DEST%\VkLayer_BFI_presenter.json" /f
reg delete "HKCU\SOFTWARE\Khronos\Vulkan\ImplicitLayers" /v "%DEST%\VkLayer_BFI_presenter32.json" /f 2>nul
set "MKEY=HKLM\SOFTWARE\Khronos\Vulkan\ImplicitLayers"
reg query "%MKEY%" /v "%DEST%\VkLayer_BFI_presenter.json" >nul 2>&1
if errorlevel 1 goto files
net session >nul 2>&1
if errorlevel 1 goto noadmin
reg delete "%MKEY%" /v "%DEST%\VkLayer_BFI_presenter.json" /f >nul 2>&1
reg delete "%MKEY%" /v "%DEST%\VkLayer_BFI_presenter32.json" /f >nul 2>&1
goto files
:noadmin
echo The layer is also registered for all users: run this file as administrator to remove that too.
pause
exit /b 1
:files
del /Q "%DEST%\BFIPresenterVK.dll" "%DEST%\VkLayer_BFI_presenter.json" "%DEST%\BFIPresenterVK32.dll" "%DEST%\VkLayer_BFI_presenter32.json" 2>nul
echo BFI Presenter Vulkan layer removed.
pause
