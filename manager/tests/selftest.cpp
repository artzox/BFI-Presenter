// Self-test for the manager's core (detection and install actions) on a fake game library.
// Build: tests/build_selftest.sh; run: wine selftest.exe <fixture dir> <release dir>
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <cstdio>
#include "../core.h"

using namespace bfi;

static int g_fail = 0;
#define CHECK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); else { printf("  FAIL " __VA_ARGS__); g_fail++; } printf("\n"); } while (0)

static Game *Find(std::vector<Game> &gs, const char *name)
{
    for (auto &g : gs) if (g.name == name) return &g;
    return nullptr;
}

static void Dump(const Game &g)
{
    printf("%-28s %-6s bits=%-2d api=%-14s route=%-20s reshade=%s %s addon=%s ini=%d exe=%s\n", g.name.c_str(), g.store.c_str(),
           g.bits, ApiText(g.apis).c_str(), RouteText(g.route), g.reshade ? U8(g.reshadeDll.path.substr(g.reshadeDll.path.find_last_of(L'\\') + 1)).c_str() : "-",
           g.reshadeDll.version.c_str(), g.addonVersion.c_str(), g.ini, U8(g.exe.substr(g.root.size())).c_str());
    for (auto &n : g.notes) printf("      note: %s\n", n.c_str());
    if (!g.tested.empty()) printf("      tested: %s\n", g.tested.c_str());
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 3) { printf("usage: selftest <games dir> <release dir>\n"); return 2; }
    std::wstring games = argv[1], rel = argv[2];
    ScanOptions o;
    o.steam = o.epic = o.gog = o.xbox = false;
    o.folders = { games };
    auto all = FindGames(o);
    printf("found %zu games\n", all.size());
    for (auto &g : all) Dump(g);

    Game *wf = Find(all, "Wreckfest"), *aw = Find(all, "Alan Wake"), *op = Find(all, "OptiGame"), *rd = Find(all, "Red Dead Redemption 2"),
         *h2 = Find(all, "Heretic II"), *nr = Find(all, "NoReShade"), *ue = Find(all, "UEGame");
    CHECK(wf && aw && op && rd && h2 && nr && ue, "all fixture games found");
    if (g_fail) return 1;
    CHECK(wf->bits == 64 && (wf->apis & ApiD3D11) && wf->route == Route::Addon64 && wf->reshade, "Wreckfest: 64-bit D3D11, DX add-on, ReShade");
    CHECK(wf->tested.find("DX11GamePresent") != std::string::npos, "Wreckfest: tested-games note");
    CHECK(aw->bits == 32 && (aw->apis & ApiD3D9) && aw->route == Route::Addon32, "Alan Wake: 32-bit D3D9 -> addon32");
    CHECK(op->reshade && op->optiscaler && op->reshadeDll.path.find(L"ReShade64.dll") != std::wstring::npos && op->reshadeDll.version == "6.4.1",
          "OptiGame: ReShade64.dll next to OptiScaler's dxgi.dll, version 6.4.1");
    CHECK(rd->route == Route::Vulkan && (rd->apis & ApiVulkan), "RDR2: Vulkan route");
    CHECK(h2->bits == 32 && (h2->apis & ApiGL) && h2->route == Route::Addon32 && h2->reshade, "Heretic II: 32-bit OpenGL, addon32");
    CHECK(!nr->reshade, "NoReShade: no ReShade");
    CHECK(ue->exe.find(L"Shipping") != std::wstring::npos && ue->reshade, "UEGame: picks the Shipping exe with ReShade");
    Game *de = Find(all, "DOOM Eternal"), *hd = Find(all, "Hades II"), *bp = Find(all, "Burnout Paradise Remastered"), *pf = Find(all, "PerfGame");
    CHECK(de && de->exe.find(L"DOOMEternalx64vk.exe") != std::wstring::npos, "DOOM Eternal: DOOMEternalx64vk.exe, not idTechLauncher");
    CHECK(hd && hd->exe.find(L"Ship\\Hades2.exe") != std::wstring::npos, "Hades II: Ship\\Hades2.exe, not F10.exe");
    CHECK(bp && bp->exe.find(L"BurnoutPR.exe") != std::wstring::npos, "Burnout: BurnoutPR.exe, not the trial");
    CHECK(pf && !(pf->apis & ApiD3D9) && (pf->apis & ApiVulkan) && (pf->apis & ApiD3D12), "D3DPERF-only d3d9 import ignored: %s", pf ? ApiText(pf->apis).c_str() : "-");

    Source src = SourceFromFolder(rel);
    printf("source dx=%s vk=%s\n", src.dxVersion.c_str(), src.vkVersion.c_str());
    CHECK(!src.dxDir.empty() && !src.vkDir.empty() && !src.dxVersion.empty(), "release folder recognised");

    // install, update, revert, remove
    std::string e = InstallOrUpdate(*wf, src);
    CHECK(e.empty() && wf->addonVersion == src.dxVersion, "Wreckfest install (%s)", e.c_str());
    e = InstallOrUpdate(*aw, src);
    CHECK(e.empty() && wf->addonPath.size() && aw->addonPath.find(L"addon32") != std::wstring::npos && !aw->addonWrongBits, "Alan Wake install addon32 (%s)", e.c_str());
    e = InstallOrUpdate(*nr, src);
    CHECK(!e.empty(), "NoReShade install refused: %s", e.c_str());

    // older add-on in OptiGame, then update and revert twice
    {
        std::wstring old = op->exe.substr(0, op->exe.find_last_of(L'\\')) + L"\\BFIPresenter.addon64";
        FILE *f = _wfopen(old.c_str(), L"wb");
        fputs("xx BFI Presenter v1.0.1 xx", f);
        fclose(f);
        Detect(*op);
        CHECK(op->addonVersion == "1.0.1", "OptiGame: old add-on 1.0.1 detected");
        e = InstallOrUpdate(*op, src);
        CHECK(e.empty() && op->addonVersion == src.dxVersion, "OptiGame updated to %s (%s)", op->addonVersion.c_str(), e.c_str());
        CHECK(CanRevert(*op), "OptiGame can revert");
        e = Revert(*op);
        CHECK(e.empty() && op->addonVersion == "1.0.1", "OptiGame reverted to %s (%s)", op->addonVersion.c_str(), e.c_str());
        CHECK(!CanRevert(*op), "OptiGame: nothing more to revert");
        e = Remove(*op, false);
        CHECK(e.empty() && op->addonPath.empty(), "OptiGame removed");
        e = Revert(*op);
        CHECK(e.empty() && op->addonVersion == "1.0.1", "OptiGame remove reverted (%s)", op->addonVersion.c_str());
    }
    // wrong bits: put addon64 into the 32-bit Heretic II, install replaces it
    {
        std::wstring d = h2->exe.substr(0, h2->exe.find_last_of(L'\\'));
        CopyFileW((src.dxDir + L"\\BFIPresenter.addon64").c_str(), (d + L"\\BFIPresenter.addon64").c_str(), FALSE);
        Detect(*h2);
        CHECK(h2->addonWrongBits, "Heretic II: wrong-bit add-on detected");
        e = InstallOrUpdate(*h2, src);
        CHECK(e.empty() && !h2->addonWrongBits && GetFileAttributesW((d + L"\\BFIPresenter.addon64").c_str()) == INVALID_FILE_ATTRIBUTES,
              "Heretic II: addon32 installed, addon64 removed (%s)", e.c_str());
    }
    // Vulkan route
    e = InstallOrUpdate(*rd, src);
    VulkanLayerState vs = DetectVulkanLayer();
    CHECK(e.empty() && rd->ini && vs.installed && vs.version == src.vkVersion, "RDR2: layer %s installed and ini copied (%s)", vs.version.c_str(), e.c_str());
    e = Remove(*rd, false);
    CHECK(e.empty() && !rd->ini, "RDR2: ini removed");
    UninstallVulkanLayer();
    CHECK(!DetectVulkanLayer().installed, "layer unregistered");

    printf(g_fail ? "\n%d FAILED\n" : "\nall passed\n", g_fail);
    return g_fail ? 1 : 0;
}
