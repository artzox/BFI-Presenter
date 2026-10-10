@echo off
setlocal
set "DEST=%LOCALAPPDATA%\BFIPresenter\Vulkan"
reg delete "HKCU\SOFTWARE\Khronos\Vulkan\ImplicitLayers" /v "%DEST%\VkLayer_BFI_presenter.json" /f
reg delete "HKCU\SOFTWARE\Khronos\Vulkan\ImplicitLayers" /v "%DEST%\VkLayer_BFI_presenter32.json" /f 2>nul
del /Q "%DEST%\BFIPresenterVK.dll" "%DEST%\VkLayer_BFI_presenter.json" "%DEST%\BFIPresenterVK32.dll" "%DEST%\VkLayer_BFI_presenter32.json" 2>nul
echo BFI Presenter Vulkan layer removed.
pause
