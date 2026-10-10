@echo off
rem Build the DX add-on with Visual Studio's x64 compiler.
rem Run from "x64 Native Tools Command Prompt for VS" in this folder. Output: BFIPresenter.addon64
cl /nologo /O2 /EHsc /std:c++17 /W3 /LD /I ..\third_party\reshade\include bfi_presenter.cpp /Fe:BFIPresenter.addon64 /link d3d12.lib dxgi.lib user32.lib advapi32.lib
