#include <windows.h>
#include <d3d9.h>
// Imports d3d9.dll only for D3DPERF markers and loads Vulkan / Direct3D 12 at run time (like RDR2).
int main(){ if (GetTickCount()==1) { D3DPERF_BeginEvent(0, L"x"); LoadLibraryA("vulkan-1.dll"); LoadLibraryA("d3d12.dll"); } return 0; }
