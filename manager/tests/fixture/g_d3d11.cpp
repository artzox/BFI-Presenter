#include <windows.h>
#include <d3d11.h>
int main(){ if (GetTickCount()==1) D3D11CreateDevice(0,D3D_DRIVER_TYPE_HARDWARE,0,0,0,0,7,0,0,0); return 0; }
