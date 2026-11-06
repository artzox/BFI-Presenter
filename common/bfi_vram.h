// BFI Presenter -- the game's video memory use and budget, shared by the DX add-on and the Vulkan layer.
//
// Read from the kernel (D3DKMTQueryVideoMemoryInfo) for the GPU with the most dedicated video memory, per process, in MB.
// DXGI is used once to find that GPU and released at once: a DXGI adapter kept for the session crashed RPCS3 -- the code
// behind it had been unloaded by the time the per-second check called it. The kernel handle has no such code behind it.
#pragma once
#ifdef _WIN32
#include <windows.h>
#include <dxgi1_4.h>

namespace bfi_vram
{
struct KmtQueryVideoMemoryInfo
{
    HANDLE   hProcess;
    UINT     hAdapter;
    UINT     MemorySegmentGroup;   // 0 local
    UINT64   Budget, CurrentUsage, CurrentReservation, AvailableForReservation;
    UINT     PhysicalAdapterIndex;
};
struct KmtOpenAdapterFromLuid { LUID AdapterLuid; UINT hAdapter; };
typedef LONG (APIENTRY *PFN_Open)(KmtOpenAdapterFromLuid *);
typedef LONG (APIENTRY *PFN_Query)(KmtQueryVideoMemoryInfo *);

// The first call finds the GPU; *unavailable (if given) is set true once when that fails, for the caller to log.
// Afterwards only the kernel query runs. Called from one helper thread.
inline bool Query(double &use, double &budget, bool *unavailable = nullptr)
{
    static PFN_Query query = nullptr; static UINT adapter = 0; static bool tried = false;
    if (!tried)
    {
        tried = true;
        LUID luid = {}; bool found = false;
        typedef HRESULT (WINAPI *PFN_CF1)(REFIID, void **);
        HMODULE h = LoadLibraryA("dxgi.dll");
        PFN_CF1 cf = h ? (PFN_CF1)(void *)GetProcAddress(h, "CreateDXGIFactory1") : nullptr;
        IDXGIFactory1 *fac = nullptr;
        if (cf && SUCCEEDED(cf(__uuidof(IDXGIFactory1), (void **)&fac)) && fac)
        {
            IDXGIAdapter1 *a = nullptr; SIZE_T best = 0;
            for (UINT k = 0; fac->EnumAdapters1(k, &a) == S_OK; k++)
            {
                DXGI_ADAPTER_DESC1 d;
                if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.DedicatedVideoMemory > best)
                {
                    best = d.DedicatedVideoMemory; luid = d.AdapterLuid; found = true;
                }
                a->Release();
            }
            fac->Release();
        }
        HMODULE g = GetModuleHandleA("gdi32.dll");
        if (!g) g = LoadLibraryA("gdi32.dll");
        PFN_Open open = g ? (PFN_Open)(void *)GetProcAddress(g, "D3DKMTOpenAdapterFromLuid") : nullptr;
        PFN_Query q = g ? (PFN_Query)(void *)GetProcAddress(g, "D3DKMTQueryVideoMemoryInfo") : nullptr;
        KmtOpenAdapterFromLuid oa = {};
        oa.AdapterLuid = luid;
        if (found && open && q && open(&oa) == 0 && oa.hAdapter) { adapter = oa.hAdapter; query = q; }
        else if (unavailable) *unavailable = true;
    }
    if (!query) return false;
    KmtQueryVideoMemoryInfo mi = {};
    mi.hProcess = nullptr;          // the current process
    mi.hAdapter = adapter;
    mi.MemorySegmentGroup = 0;
    if (query(&mi) != 0) return false;
    use = mi.CurrentUsage / 1048576.0; budget = mi.Budget / 1048576.0;
    return true;
}
}
#endif
