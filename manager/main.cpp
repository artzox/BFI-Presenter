// BFI Presenter Manager -- finds games, installs and updates BFI Presenter (DX add-on and Vulkan layer).
// Copyright (C) 2026 artzox. SPDX-License-Identifier: GPL-3.0-or-later
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <shobjidl.h>
#include <d3d11.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <set>
#include <functional>
#include "imgui.h"
#include "imgui_internal.h"   // ImGuiItemFlags_MixedValue (tri-state checkbox)
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "core.h"
#include "nvdrs.h"

using namespace bfi;

#define MANAGER_VERSION "1.3.1"

// ------------------------------------------------------------------ D3D11 window plumbing

static ID3D11Device *g_dev = nullptr;
static ID3D11DeviceContext *g_ctx = nullptr;
static IDXGISwapChain *g_sc = nullptr;
static ID3D11RenderTargetView *g_rtv = nullptr;
static UINT g_resizeW = 0, g_resizeH = 0;
static float g_dpiScale = 1.0f;
static bool g_dpiChanged = false;

static void CreateRT()
{
    ID3D11Texture2D *bb = nullptr;
    g_sc->GetBuffer(0, IID_PPV_ARGS(&bb));
    if (bb) { g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv); bb->Release(); }
}
static void CleanupRT() { if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; } }

static bool CreateDevice(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, fl, 2, D3D11_SDK_VERSION,
                                               &sd, &g_sc, &g_dev, &got, &g_ctx);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, fl, 2, D3D11_SDK_VERSION,
                                           &sd, &g_sc, &g_dev, &got, &g_ctx);
    if (FAILED(hr)) return false;
    CreateRT();
    return true;
}

static void CleanupDevice()
{
    CleanupRT();
    if (g_sc) g_sc->Release();
    if (g_ctx) g_ctx->Release();
    if (g_dev) g_dev->Release();
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
    switch (msg)
    {
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) { g_resizeW = LOWORD(lp); g_resizeH = HIWORD(lp); }
        return 0;
    case WM_GETMINMAXINFO:
    {
        auto *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = (LONG)(900 * g_dpiScale);
        mm->ptMinTrackSize.y = (LONG)(560 * g_dpiScale);
        return 0;
    }
    case WM_DPICHANGED:
    {
        g_dpiScale = HIWORD(wp) / 96.0f;
        g_dpiChanged = true;
        const RECT *r = (const RECT *)lp;
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_CLOSE:
    {
        WINDOWPLACEMENT wp{ sizeof(wp) };
        if (GetWindowPlacement(hwnd, &wp))
        {
            const RECT &r = wp.rcNormalPosition;   // workspace coordinates: same as screen when the taskbar is at the bottom
            wchar_t b[96];
            swprintf(b, 96, L"%ld,%ld,%ld,%ld,%d", r.left, r.top, r.right, r.bottom, wp.showCmd == SW_SHOWMAXIMIZED ? 1 : 0);
            SaveSetting(L"Window", b);
        }
        break;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ------------------------------------------------------------------ app state

struct App {
    std::vector<Game> games;
    int sel = -1;                      // the game shown in the detail pane (and the Shift-range anchor)
    std::set<std::wstring> picked;     // ticked games, by install folder
    Source src;
    VulkanLayerState vk;
    ScanOptions opt;
    LibraryInfo libs;

    // background work
    std::mutex lock;
    std::thread worker;
    std::atomic<bool> busy{ false };
    std::string busyText;              // guarded by lock
    bool scanDone = false;             // guarded by lock
    std::vector<Game> scanned;         // guarded by lock
    std::string scanErr;               // guarded by lock
    bool lastFresh = false;
    bool releaseDone = false;          // guarded by lock
    Source fetched;                    // guarded by lock
    std::string remoteTag, remoteErr;  // guarded by lock

    // UI
    char search[128] = {};
    bool onlyRelevant = false;
    std::string msg;
    bool msgErr = false;
    bool removeIni = false;
    bool showLibraries = false;        // open the Libraries dialog this frame
    std::string latestTag;             // newest tag seen on GitHub this session
    bool scannedOnce = false, checkedOnce = false;   // GitHub is checked once, after the first scan

    // NVIDIA profiles
    bool nvOk = false;
    bool needAdmin = false;            // last NVIDIA change needs administrator rights
    // ReShade versions and downloads
    std::vector<std::string> rsVersions;   // guarded by lock while the worker fills it
    std::string rsVersion;                 // the version chosen in the UI
    bool rsDone = false;                   // guarded by lock: a download finished
    std::string rsErr, rsPendingVer;       // guarded by lock
    std::vector<std::wstring> rsPending;   // games to install ReShade in after the download
    // install question (NVIDIA settings)
    std::vector<std::wstring> instPending;
    bool openInstall = false;

    void SetMsg(const std::string &m, bool err) { msg = m; msgErr = err; needAdmin = false; }
};

static App g_app;
static void RestartAsAdmin();

// ------------------------------------------------------------------ batch jobs
// Installs, updates and removes over many games run a few per frame, so the window keeps drawing (a progress
// dialog with the current game) instead of freezing until the last one is done.
struct Job {
    bool active = false, cancel = false;
    std::string title;
    std::vector<std::wstring> roots;
    size_t next = 0;
    int ok = 0, fail = 0, nvFail = 0;
    std::string firstErr, nvErr, note, current;
    std::function<std::string(Game &, Job &)> fn;      // "" = done, else the error
    std::function<void(Job &)> finish;                 // after the last game (or Cancel)
    double started = 0.0;
};
static Job g_job;

static double NowSec() { return GetTickCount64() / 1000.0; }

static void StartJob(const std::string &title, std::vector<std::wstring> roots,
                     std::function<std::string(Game &, Job &)> fn, std::function<void(Job &)> finish)
{
    g_job = Job();
    g_job.active = true;
    g_job.title = title;
    g_job.roots = std::move(roots);
    g_job.fn = std::move(fn);
    g_job.finish = std::move(finish);
    g_job.started = NowSec();
}

// Runs games for up to ~40 ms per frame.
static void StepJob()
{
    if (!g_job.active) return;
    double t0 = NowSec();
    while (g_job.next < g_job.roots.size() && !g_job.cancel)
    {
        const std::wstring &root = g_job.roots[g_job.next++];
        for (auto &g : g_app.games)
            if (g.root == root)
            {
                g_job.current = g.name;
                std::string e = g_job.fn(g, g_job);
                if (e.empty()) g_job.ok++;
                else { g_job.fail++; if (g_job.firstErr.empty()) g_job.firstErr = g.name + ": " + e; }
                break;
            }
        if (NowSec() - t0 > 0.04) return;
    }
    g_job.active = false;
    if (g_job.finish) g_job.finish(g_job);
}

static void DrawJob()
{
    // only for jobs that take a moment; a single quick install shows no dialog
    if (!g_job.active)
    {
        if (ImGui::IsPopupOpen("Working##job") && ImGui::BeginPopupModal("Working##job", nullptr, ImGuiWindowFlags_NoTitleBar))
        {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        return;
    }
    if (g_job.roots.size() < 2 && NowSec() - g_job.started < 0.3) return;
    if (!ImGui::IsPopupOpen("Working##job")) ImGui::OpenPopup("Working##job");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 28, 0), ImGuiCond_Always);
    if (!ImGui::BeginPopupModal("Working##job", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove))
        return;
    size_t n = g_job.roots.size(), done = std::min(g_job.next, n);
    ImGui::TextUnformatted(g_job.title.c_str());
    char ov[64];
    snprintf(ov, sizeof(ov), "%zu of %zu", done, n);
    ImGui::ProgressBar(n ? (float)done / (float)n : 1.0f, ImVec2(-1, 0), ov);
    ImGui::TextDisabled("%s", g_job.current.c_str());
    ImGui::BeginDisabled(g_job.cancel);
    if (ImGui::Button(g_job.cancel ? "Stopping..." : "Stop")) g_job.cancel = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Stop finishes the current game; games already done stay done (Undo last change per game).");
    ImGui::PopTextWrapPos();
    ImGui::EndPopup();
}

static std::string JobSummary(const Job &j, const std::string &verb)
{
    std::string m = verb + " " + std::to_string(j.ok) + (j.ok == 1 ? " game" : " games");
    if (j.cancel && j.next < j.roots.size()) m += " (stopped, " + std::to_string(j.roots.size() - j.next) + " not done)";
    if (j.fail) m += ", failed " + std::to_string(j.fail) + ". " + j.firstErr;
    else m += ".";
    return m;
}
static void ReShadeVersionCombo(float width);

static void JoinWorker()
{
    if (g_app.worker.joinable()) g_app.worker.join();
}

static void NvFill(std::vector<Game> &games)
{
    if (!NvAvailable()) return;
    std::vector<std::wstring> names;
    for (auto &g : games) names.push_back(g.exe);
    std::vector<NvState> st;
    if (!NvRead(names, st).empty()) return;
    for (size_t i = 0; i < games.size(); i++)
    {
        Game &g = games[i];
        g.nvKnown = st[i].known;
        g.nvProfile = st[i].profile;
        g.nvVrr = st[i].vrr; g.nvVrrOwn = st[i].vrrOwn;
        g.nvVsync = st[i].vsync; g.nvVsyncOwn = st[i].vsyncOwn;
        g.nvGl = st[i].glPresent; g.nvGlOwn = st[i].glPresentOwn;
    }
}

static void NvFillOne(Game &g)
{
    std::vector<Game> one{ g };
    NvFill(one);
    g.nvKnown = one[0].nvKnown; g.nvProfile = one[0].nvProfile;
    g.nvVrr = one[0].nvVrr; g.nvVrrOwn = one[0].nvVrrOwn;
    g.nvVsync = one[0].nvVsync; g.nvVsyncOwn = one[0].nvVsyncOwn;
    g.nvGl = one[0].nvGl; g.nvGlOwn = one[0].nvGlOwn;
}

static std::wstring ExeName(const Game &g) { return g.exe.substr(g.exe.find_last_of(L'\\') + 1); }

// vrr / vsync: -2 leave, -1 use global, else the value. Returns "" or the error (needAdmin set when that's the cause).
static std::string NvSet(Game &g, long long vrr, long long vsync, long long gl = -2)
{
    std::string e = NvWrite(g.exe, W(g.name), vrr, vsync, gl);
    NvFillOne(g);
    return e;
}

static void SetBusy(const std::string &t)
{
    std::lock_guard<std::mutex> l(g_app.lock);
    g_app.busyText = t;
}

// fresh: search the libraries again; otherwise re-check the saved list (exe facts are cached).
static void StartScan(bool fresh)
{
    if (g_app.busy) return;
    JoinWorker();
    g_app.busy = true;
    g_app.lastFresh = fresh;
    SetBusy(fresh ? "Scanning game libraries..." : "Checking games...");
    ScanOptions opt = g_app.opt;
    std::vector<Game> current = g_app.games;
    g_app.worker = std::thread([opt, fresh, current]() mutable {
        std::vector<Game> games;
        if (fresh) games = FindGames(opt, SetBusy);
        else { games = std::move(current); DetectAll(games, SetBusy); }
        std::string err;
        if (LoadSetting(L"PCGamingWiki", L"1") == L"1")
        {
            err = FetchPcgw(games, SetBusy);
            DetectAll(games, SetBusy);
        }
        SaveGameList(games);
        NvFill(games);
        std::vector<std::string> rsv;
        {
            std::lock_guard<std::mutex> l(g_app.lock);
            rsv = g_app.rsVersions;
        }
        if (rsv.empty()) { SetBusy("Getting the list of ReShade versions..."); FetchReShadeVersions(rsv); }
        auto vk = DetectVulkanLayer();
        std::lock_guard<std::mutex> l(g_app.lock);
        if (!rsv.empty()) g_app.rsVersions = rsv;
        g_app.scanned = std::move(games);
        g_app.vk = vk;
        g_app.scanErr = err;
        g_app.scanDone = true;
        g_app.busy = false;
    });
}

static void StartReShade(const std::string &ver, const std::vector<std::wstring> &roots)
{
    if (g_app.busy || ver.empty()) return;
    JoinWorker();
    g_app.busy = true;
    g_app.rsPending = roots;
    SetBusy("Getting ReShade " + ver + "...");
    g_app.worker = std::thread([ver] {
        std::string e = EnsureReShade(ver, SetBusy);
        std::lock_guard<std::mutex> l(g_app.lock);
        g_app.rsErr = e;
        g_app.rsPendingVer = ver;
        g_app.rsDone = true;
        g_app.busy = false;
    });
}

static void StartFetch()
{
    if (g_app.busy) return;
    JoinWorker();
    g_app.busy = true;
    { std::lock_guard<std::mutex> l(g_app.lock); g_app.busyText = "Checking GitHub for the latest release..."; }
    std::string have = g_app.src.dxVersion;
    g_app.worker = std::thread([have] {
        RemoteRelease rr = FetchLatestRelease();
        Source s;
        std::string err = rr.error;
        if (rr.ok)
        {
            std::string tagV = rr.tag;
            // "v1.0.4", "v.1.0.4", "1.0.4": skip everything before the first digit
            while (!tagV.empty() && !isdigit((unsigned char)tagV[0])) tagV.erase(0, 1);
            Source cached = CachedRelease();
            if (cached.ok() && CompareVersions(cached.dxVersion.empty() ? cached.vkVersion : cached.dxVersion, tagV) >= 0)
                s = cached;
            else
                err = DownloadRelease(rr, s, [](const std::string &p) {
                    std::lock_guard<std::mutex> l(g_app.lock);
                    g_app.busyText = p;
                });
        }
        std::lock_guard<std::mutex> l(g_app.lock);
        g_app.fetched = s;
        g_app.remoteTag = rr.tag;
        g_app.remoteErr = err;
        g_app.releaseDone = true;
        g_app.busy = false;
    });
}

static void PollWorker()
{
    std::lock_guard<std::mutex> l(g_app.lock);
    if (g_app.scanDone)
    {
        g_app.scanDone = false;
        std::wstring selRoot = (g_app.sel >= 0 && g_app.sel < (int)g_app.games.size()) ? g_app.games[g_app.sel].root : L"";
        g_app.games = std::move(g_app.scanned);
        g_app.sel = -1;
        for (int i = 0; i < (int)g_app.games.size(); i++)
            if (g_app.games[i].root == selRoot) g_app.sel = i;
        g_app.SetMsg((g_app.lastFresh ? "Found " : "Checked ") + std::to_string(g_app.games.size()) + " games." +
                     (g_app.scanErr.empty() ? "" : " (" + g_app.scanErr + ")"), false);
        g_app.scannedOnce = true;
    }
    if (g_app.rsDone)
    {
        g_app.rsDone = false;
        if (!g_app.rsErr.empty()) g_app.SetMsg(g_app.rsErr, true);
        else
        {
            std::string ver = g_app.rsPendingVer;
            StartJob("Installing ReShade " + ver + " in " + std::to_string(g_app.rsPending.size()) + " games...", g_app.rsPending,
                     [ver](Game &g, Job &j) {
                         std::string n, e = InstallReShade(g, ver, &n);
                         if (!n.empty()) j.note = n;
                         return e;
                     },
                     [ver](Job &j) {
                         g_app.SetMsg("ReShade " + ver + ": " + JobSummary(j, "installed in") + (j.note.empty() ? "" : " " + j.note), j.fail != 0);
                     });
        }
        g_app.rsPending.clear();
    }
    if (g_app.releaseDone)
    {
        g_app.releaseDone = false;
        if (!g_app.remoteErr.empty()) g_app.SetMsg(g_app.remoteErr, true);
        else if (g_app.fetched.ok())
        {
            g_app.src = g_app.fetched;
            g_app.latestTag = g_app.remoteTag;
            SaveSetting(L"Source", L"");
            g_app.SetMsg("Latest release: " + g_app.remoteTag + ".", false);
        }
    }
}

// ------------------------------------------------------------------ helpers

static std::wstring PickFolder(HWND owner, const wchar_t *title)
{
    std::wstring out;
    IFileOpenDialog *d = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d))))
    {
        DWORD o = 0;
        d->GetOptions(&o);
        d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        d->SetTitle(title);
        if (SUCCEEDED(d->Show(owner)))
        {
            IShellItem *it = nullptr;
            if (SUCCEEDED(d->GetResult(&it)))
            {
                wchar_t *p = nullptr;
                if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { out = p; CoTaskMemFree(p); }
                it->Release();
            }
        }
        d->Release();
    }
    return out;
}

enum class St { NoExe, Unsupported, NeedsReShade, NotInstalled, WrongBits, Update, Installed, LayerMissing };

static St StatusOf(const Game &g)
{
    if (g.exe.empty()) return St::NoExe;
    if (g.route == Route::Unsupported) return St::Unsupported;
    if (g.route == Route::Vulkan)
    {
        if (!g.ini) return St::NotInstalled;
        if (!g_app.vk.installed) return St::LayerMissing;
        if (!g_app.src.vkVersion.empty() && CompareVersions(g_app.vk.version, g_app.src.vkVersion) < 0) return St::Update;
        return St::Installed;
    }
    if (g.route == Route::Shader)
    {
        if (!g.reshade) return St::NeedsReShade;
        if (g.shaderPath.empty() || !g.shaderOn) return St::NotInstalled;
        if (!g_app.src.shaderVersion.empty() && CompareVersions(g.shaderVersion, g_app.src.shaderVersion) < 0) return St::Update;
        return St::Installed;
    }
    if (g.addonWrongBits) return St::WrongBits;
    if (!g.addonPath.empty())
    {
        if (!g_app.src.dxVersion.empty() && CompareVersions(g.addonVersion, g_app.src.dxVersion) < 0) return St::Update;
        return St::Installed;
    }
    if (!g.reshade) return St::NeedsReShade;
    return St::NotInstalled;
}

static const ImVec4 kGreen(0.40f, 0.85f, 0.45f, 1), kYellow(0.95f, 0.80f, 0.30f, 1), kOrange(0.95f, 0.60f, 0.30f, 1),
                    kRed(0.95f, 0.40f, 0.40f, 1), kGrey(0.55f, 0.55f, 0.58f, 1), kText(0.90f, 0.90f, 0.92f, 1),
                    kAccent(0.35f, 0.62f, 0.95f, 1);

static void StatusText(const Game &g)
{
    switch (StatusOf(g))
    {
    case St::NoExe:        ImGui::TextColored(kGrey, "No game .exe"); break;
    case St::Unsupported:  ImGui::TextColored(kGrey, "Not supported"); break;
    case St::NeedsReShade: ImGui::TextColored(kOrange, "Needs ReShade"); break;
    case St::NotInstalled: ImGui::TextColored(kText, "Ready to install"); break;
    case St::WrongBits:    ImGui::TextColored(kRed, "Wrong add-on"); break;
    case St::Update:       ImGui::TextColored(kYellow, "Update available"); break;
    case St::LayerMissing: ImGui::TextColored(kOrange, "Vulkan layer missing"); break;
    case St::Installed:    ImGui::TextColored(kGreen, "Installed"); break;
    }
}

static bool Relevant(const Game &g)
{
    return g.reshade || !g.addonPath.empty() || g.ini || !g.tested.empty();
}

static bool Matches(const Game &g, const char *q)
{
    if (!q[0]) return true;
    std::string a = g.name, b = q;
    for (auto &c : a) c = (char)tolower((unsigned char)c);
    for (auto &c : b) c = (char)tolower((unsigned char)c);
    return a.find(b) != std::string::npos;
}

// SameLine() if a widget of width w still fits on this line, else start a new line.
static void SameLineIfFits(float w)
{
    float right = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + w;
    if (right <= ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x) ImGui::SameLine();
}

static float ButtonW(const char *label)
{
    return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2;
}

static void Tooltip(const char *t)
{
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32);
        ImGui::TextUnformatted(t);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

static void DoAction(const std::string &what, const std::string &err)
{
    if (err.empty()) g_app.SetMsg(what, false);
    else g_app.SetMsg(err, true);
    g_app.vk = DetectVulkanLayer();
}

// ------------------------------------------------------------------ UI

static void DrawTop(HWND hwnd)
{
    // Release
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kGrey, "Release");
    ImGui::SameLine(ImGui::CalcTextSize("Vulkan layer").x + ImGui::GetFontSize() * 1.5f);
    if (g_app.src.ok())
    {
        ImGui::Text("%s", g_app.src.label.c_str());
        ImGui::SameLine();
        ImGui::TextColored(kGrey, "(add-on %s, Vulkan layer %s%s%s)", g_app.src.dxVersion.empty() ? "-" : g_app.src.dxVersion.c_str(),
                           g_app.src.vkVersion.empty() ? "-" : g_app.src.vkVersion.c_str(),
                           g_app.src.shaderVersion.empty() ? "" : ", shader ", g_app.src.shaderVersion.c_str());
        if (!g_app.src.vkMissing.empty())
        {
            ImGui::SameLine();
            ImGui::TextColored(kOrange, "(no Vulkan layer: missing %s)", g_app.src.vkMissing.c_str());
            Tooltip("The DX add-on installs from this folder. To install or update the Vulkan layer, use the folder of the "
                    "extracted Vulkan zip (BFIPresenterVK), or a folder with both zips extracted into it.");
        }
    }
    else ImGui::TextColored(kOrange, "None yet: check GitHub or pick an extracted release folder");
    ImGui::SameLine();
    ImGui::BeginDisabled(g_app.busy);
    if (ImGui::Button("Check GitHub")) StartFetch();
    Tooltip("Downloads the latest BFI Presenter release from GitHub (artzox/BFI-Presenter) if it is newer than the one here.");
    ImGui::SameLine();
    if (ImGui::Button("Use folder..."))
    {
        std::wstring d = PickFolder(hwnd, L"Folder with an extracted BFI Presenter release (or test build)");
        if (!d.empty())
        {
            Source s = SourceFromFolder(d);
            if (s.ok()) { g_app.src = s; SaveSetting(L"Source", d); g_app.SetMsg("Using " + s.label, false); }
            else g_app.SetMsg("No BFI Presenter files (BFIPresenter.addon64 or BFIPresenterVK.dll) in that folder.", true);
        }
    }
    Tooltip("Use an extracted release or a test build from a folder instead of GitHub.");
    ImGui::EndDisabled();

    // Vulkan layer
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kGrey, "Vulkan layer");
    ImGui::SameLine(ImGui::CalcTextSize("Vulkan layer").x + ImGui::GetFontSize() * 1.5f);
    if (g_app.vk.installed)
    {
        bool old = !g_app.src.vkVersion.empty() && CompareVersions(g_app.vk.version, g_app.src.vkVersion) < 0;
        ImGui::TextColored(old ? kYellow : kGreen, "Installed v%s%s", g_app.vk.version.c_str(), old ? " (update available)" : "");
    }
    else ImGui::TextColored(kGrey, "Not installed (only needed for Vulkan and DXVK games)");
    ImGui::SameLine();
    ImGui::TextColored(kGrey, g_app.vk.reshadeLayer ? "  ReShade Vulkan layer: yes%s%s" : "  ReShade Vulkan layer: no%s%s",
                       g_app.vk.reshadeVersion.empty() ? "" : " v", g_app.vk.reshadeVersion.c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(g_app.busy || g_app.src.vkDir.empty());
    if (ImGui::Button(g_app.vk.installed ? "Update layer" : "Install layer"))
        DoAction("Vulkan layer installed for your Windows user.", InstallVulkanLayer(g_app.src));
    ImGui::EndDisabled();
    if (g_app.vk.installed)
    {
        ImGui::SameLine();
        if (ImGui::Button("Remove layer")) DoAction("Vulkan layer removed.", UninstallVulkanLayer());
    }

    ImGui::Separator();

    // Toolbar
    ImGui::BeginDisabled(g_app.busy);
    if (ImGui::Button("Rescan")) StartScan(true);
    Tooltip("Searches the selected libraries again (new or removed games). Games already known are checked again quickly.");
    ImGui::SameLine();
    if (ImGui::Button("Libraries...")) g_app.showLibraries = true;
    Tooltip("Choose which stores to search and add folders with other games.");
    ImGui::SameLine();
    int nUpd = 0;
    for (auto &g : g_app.games) { St s = StatusOf(g); if ((s == St::Update || s == St::WrongBits) && g.route != Route::Vulkan) nUpd++; }
    bool vkOld = g_app.vk.installed && !g_app.src.vkVersion.empty() && CompareVersions(g_app.vk.version, g_app.src.vkVersion) < 0;
    ImGui::BeginDisabled(!g_app.src.ok() || (nUpd == 0 && !vkOld));
    char lbl[64];
    snprintf(lbl, 64, "Update outdated (%d%s)", nUpd, vkOld ? " + layer" : "");
    bool clicked = ImGui::Button(lbl);
    Tooltip("Updates every game whose BFI Presenter add-on is older than the selected release (or built for the wrong "
            "bitness), and the Vulkan layer if it is older. No ticks needed; for a chosen set of games, tick them and "
            "use the buttons on the right.");
    if (clicked)
    {
        std::string layerErr;
        if (vkOld) layerErr = InstallVulkanLayer(g_app.src);
        std::vector<std::wstring> roots;
        for (auto &g : g_app.games)
        {
            St s = StatusOf(g);
            if ((s == St::Update || s == St::WrongBits) && g.route != Route::Vulkan) roots.push_back(g.root);
        }
        StartJob("Updating BFI Presenter in " + std::to_string(roots.size()) + " games...", roots,
                 [](Game &g, Job &) { return InstallOrUpdate(g, g_app.src); },
                 [vkOld, layerErr](Job &j) {
                     g_app.vk = DetectVulkanLayer();
                     std::string m = JobSummary(j, "Updated");
                     if (vkOld) m += layerErr.empty() ? " Vulkan layer updated." : " Vulkan layer: " + layerErr;
                     g_app.SetMsg(m, j.fail || !layerErr.empty());
                 });
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    ImGui::InputTextWithHint("##search", "Search games", g_app.search, sizeof(g_app.search));
    if (g_app.search[0])
    {
        ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
        if (ImGui::Button("x##clearsearch")) g_app.search[0] = 0;
        Tooltip("Clear the search");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Only games with ReShade, BFI or a tested entry", &g_app.onlyRelevant);

    if (g_app.busy)
    {
        std::lock_guard<std::mutex> l(g_app.lock);
        ImGui::TextColored(kAccent, "%s", g_app.busyText.c_str());
    }
    else if (!g_app.msg.empty())
    {
        if (g_app.needAdmin)
        {
            if (ImGui::SmallButton("Restart as administrator")) RestartAsAdmin();
            ImGui::SameLine();
        }
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(g_app.msgErr ? kRed : kGreen, "%s", g_app.msg.c_str());
        ImGui::PopTextWrapPos();
    }
    else ImGui::TextUnformatted("");
}

static void DrawTable(float height)
{
    // Columns keep their own width (drag the header borders; remembered in manager_ui.ini) and the table scrolls
    // sideways when the pane is narrower than all of them, instead of squeezing the headers to "St..." / "G-...".
    ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
                         ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable |
                         ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Sortable;
    const float em = ImGui::GetFontSize();
    if (!ImGui::BeginTable("games", 10, tf, ImVec2(0, height))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("##pick", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize | ImGuiTableColumnFlags_NoSort, ImGui::GetFrameHeight());
    ImGui::TableSetupColumn("Game", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_NoHide, em * 15.0f);
    ImGui::TableSetupColumn("Store", ImGuiTableColumnFlags_WidthFixed, em * 4.0f);
    ImGui::TableSetupColumn("Bits", ImGuiTableColumnFlags_WidthFixed, em * 3.2f);
    ImGui::TableSetupColumn("API", ImGuiTableColumnFlags_WidthFixed, em * 8.0f);
    ImGui::TableSetupColumn("ReShade", ImGuiTableColumnFlags_WidthFixed, em * 5.5f);
    ImGui::TableSetupColumn("BFI Presenter", ImGuiTableColumnFlags_WidthFixed, em * 8.5f);
    ImGui::TableSetupColumn("G-SYNC", ImGuiTableColumnFlags_WidthFixed, em * 4.5f);
    ImGui::TableSetupColumn("VSync", ImGuiTableColumnFlags_WidthFixed, em * 6.0f);
    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoHide, em * 8.0f);
    std::vector<int> vis;
    for (int i = 0; i < (int)g_app.games.size(); i++)
        if (Matches(g_app.games[i], g_app.search) && (!g_app.onlyRelevant || Relevant(g_app.games[i]))) vis.push_back(i);
    // Sorting: click a header (again to reverse). The order is applied to the rows shown, the list itself stays.
    static int sortCol = 1;
    static bool sortAsc = true;
    if (ImGuiTableSortSpecs *ss = ImGui::TableGetSortSpecs())
        if (ss->SpecsCount > 0)
        {
            sortCol = ss->Specs[0].ColumnIndex;
            sortAsc = ss->Specs[0].SortDirection != ImGuiSortDirection_Descending;
        }
    {
        auto low = [](std::string s) { for (auto &c : s) c = (char)tolower((unsigned char)c); return s; };
        auto key = [&](const Game &g) -> std::string {
            char b[32];
            switch (sortCol)
            {
            case 2: return low(g.store);
            case 3: snprintf(b, 32, "%02d", g.bits); return b;
            case 4: return ApiText(g.apis);
            case 5: return g.reshade ? g.reshadeDll.version : std::string();
            case 6: return g.route == Route::Vulkan ? (g.ini ? "vulkan" : "") : g.route == Route::Shader ? g.shaderVersion : g.addonVersion;
            case 7: return g.nvKnown ? std::string(g.nvVrrOwn ? "0" : "1") + VrrText(g.nvVrr) : "2";
            case 8: return g.nvKnown ? std::string(g.nvVsyncOwn ? "0" : "1") + VsyncText(g.nvVsync) : "2";
            case 9: snprintf(b, 32, "%02d", (int)StatusOf(g)); return b;
            default: return low(g.name);
            }
        };
        auto ver = sortCol == 5 || sortCol == 6;
        std::stable_sort(vis.begin(), vis.end(), [&](int a, int b) {
            const Game &x = g_app.games[a], &y = g_app.games[b];
            std::string kx = key(x), ky = key(y);
            int c = ver ? CompareVersions(kx, ky) : kx.compare(ky);
            if (c == 0) return low(x.name) < low(y.name);
            return sortAsc ? c < 0 : c > 0;
        });
    }
    auto picked = [](int i) { return g_app.picked.count(g_app.games[i].root) != 0; };
    auto setPicked = [](int i, bool on) { if (on) g_app.picked.insert(g_app.games[i].root); else g_app.picked.erase(g_app.games[i].root); };
    auto pickRange = [&](int fromIdx, int toIdx, bool add) {
        int a = -1, b = -1;
        for (int k = 0; k < (int)vis.size(); k++) { if (vis[k] == fromIdx) a = k; if (vis[k] == toIdx) b = k; }
        if (b < 0) return;
        if (a < 0) a = b;
        if (!add) g_app.picked.clear();
        for (int k = std::min(a, b); k <= std::max(a, b); k++) setPicked(vis[k], true);
    };

    // header: a checkbox that ticks or clears every game shown
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    ImGui::TableSetColumnIndex(0);
    {
        int n = 0;
        for (int i : vis) if (picked(i)) n++;
        bool all = !vis.empty() && n == (int)vis.size();
        bool mixed = n > 0 && !all;
        if (mixed) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
        if (ImGui::Checkbox("##all", &all)) for (int i : vis) setPicked(i, all);
        if (mixed) ImGui::PopItemFlag();     // pop what was pushed, whatever the click changed
        Tooltip("Tick or clear all games shown (Ctrl+A ticks all, Esc clears)");
    }
    for (int c = 1; c < 10; c++) { ImGui::TableSetColumnIndex(c); ImGui::TableHeader(ImGui::TableGetColumnName(c)); }

    // Keyboard: Up/Down/PageUp/PageDown/Home/End move the selection itself (Shift extends the ticks),
    // Ctrl+A ticks every game shown, Esc clears the ticks.
    bool scrollToSel = false;
    if (!vis.empty() && !ImGui::GetIO().WantTextInput && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
    {
        int pos = -1;
        for (int k = 0; k < (int)vis.size(); k++) if (vis[k] == g_app.sel) pos = k;
        int page = std::max(1, (int)(height / ImGui::GetFrameHeightWithSpacing()) - 2), np = pos;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) np = pos < 0 ? 0 : pos + 1;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) np = pos < 0 ? 0 : pos - 1;
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) np = pos < 0 ? 0 : pos + page;
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) np = pos < 0 ? 0 : pos - page;
        if (ImGui::IsKeyPressed(ImGuiKey_Home)) np = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_End)) np = (int)vis.size() - 1;
        if (np != pos)
        {
            np = std::max(0, std::min(np, (int)vis.size() - 1));
            int anchor = g_app.sel;
            g_app.sel = vis[np];
            scrollToSel = true;
            if (ImGui::GetIO().KeyShift) pickRange(anchor < 0 ? vis[np] : anchor, vis[np], true);
        }
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A)) for (int i : vis) setPicked(i, true);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) g_app.picked.clear();
    }

    for (int i : vis)
    {
        Game &g = g_app.games[i];
        ImGui::TableNextRow();
        ImGui::PushID(i);
        ImGui::TableNextColumn();
        bool pk = picked(i);
        if (ImGui::Checkbox("##p", &pk)) setPicked(i, pk);
        ImGui::TableNextColumn();
        if (ImGui::Selectable(g.name.c_str(), g_app.sel == i || pk, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
        {
            ImGuiIO &io = ImGui::GetIO();
            if (io.KeyShift && g_app.sel >= 0) pickRange(g_app.sel, i, io.KeyCtrl);
            else if (io.KeyCtrl) setPicked(i, !pk);
            else if (!g_app.picked.empty()) g_app.picked.clear();     // a plain click shows one game
            g_app.sel = i;
        }
        if (scrollToSel && g_app.sel == i) ImGui::SetScrollHereY(0.5f);
        ImGui::PopID();
        ImGui::TableNextColumn(); ImGui::TextColored(kGrey, "%s", g.store.c_str());
        ImGui::TableNextColumn(); if (g.bits) ImGui::Text("%d", g.bits); else ImGui::TextColored(kGrey, "?");
        ImGui::TableNextColumn(); ImGui::TextUnformatted(g.exe.empty() ? "" : ApiText(g.apis).c_str());
        ImGui::TableNextColumn();
        if (g.reshade) ImGui::Text("%s", g.reshadeDll.version.empty() ? "yes" : g.reshadeDll.version.c_str());
        else if (g.route == Route::Vulkan && g.reshadeVkIni && g_app.vk.reshadeLayer) ImGui::Text("Vulkan %s", g_app.vk.reshadeVersion.c_str());
        else ImGui::TextColored(kGrey, "-");
        ImGui::TableNextColumn();
        if (g.route == Route::Vulkan) { if (g.ini) ImGui::Text("Vulkan (ini)"); else ImGui::TextColored(kGrey, "-"); }
        else if (g.route == Route::Shader && !g.shaderPath.empty()) ImGui::Text("shader %s%s", g.shaderVersion.c_str(), g.shaderOn ? "" : " (off)");
        else if (!g.addonPath.empty()) ImGui::Text("%s%s", g.addonVersion.c_str(), g.addonWrongBits ? " (wrong bits)" : "");
        else ImGui::TextColored(kGrey, "-");
        ImGui::TableNextColumn();
        if (!g.nvKnown) ImGui::TextColored(kGrey, "-");
        else ImGui::TextColored(g.nvVrrOwn ? (g.nvVrr == kVrrForceOff || g.nvVrr == kVrrFixedRefresh ? kGreen : kText) : kGrey, "%s", VrrText(g.nvVrr).c_str());
        ImGui::TableNextColumn();
        if (!g.nvKnown) ImGui::TextColored(kGrey, "-");
        else ImGui::TextColored(g.nvVsyncOwn ? (g.nvVsync == kVsyncForceOff || g.nvVsync == kVsyncFast ? kRed : kText) : kGrey, "%s", VsyncText(g.nvVsync).c_str());
        ImGui::TableNextColumn(); StatusText(g);
    }
    ImGui::EndTable();
}

static void RequestInstall(const std::vector<Game *> &list);

static void DrawActions(Game &g)
{
    St s = StatusOf(g);
    bool canInstall = g_app.src.ok() && !g.exe.empty() &&
                      ((g.route == Route::Vulkan && !g_app.src.vkDir.empty()) ||
                       (g.route == Route::Shader && g.reshade && !g_app.src.shaderFile.empty()) ||
                       ((g.route == Route::Addon64 || g.route == Route::Addon32) && g.reshade && !g_app.src.dxDir.empty()));
    const char *verb = (s == St::Update || s == St::WrongBits) ? "Update" : (s == St::Installed ? "Reinstall" : "Install");
    ImGui::BeginDisabled(!canInstall || (s == St::Installed && g.route == Route::Vulkan));
    if (ImGui::Button(verb)) RequestInstall({ &g });
    ImGui::EndDisabled();
    if (!g_app.src.ok()) { ImGui::SameLine(); ImGui::TextColored(kGrey, "(pick a release first)"); }
    SameLineIfFits(ButtonW("Remove"));
    bool installed = !g.addonPath.empty() || g.ini || !g.shaderPath.empty();
    ImGui::BeginDisabled(!installed);
    if (ImGui::Button("Remove")) DoAction("Removed from " + g.name, Remove(g, g_app.removeIni));
    SameLineIfFits(ImGui::GetFrameHeight() + ButtonW("Remove deletes the ini too"));
    ImGui::Checkbox("Remove deletes the ini too", &g_app.removeIni);
    Tooltip("For Remove only. The ini holds this game's settings; the add-on writes it on the game's first start, so installing never needs it. "
            "For Vulkan games the ini is what turns the layer on, so Remove always deletes it there.");
    ImGui::EndDisabled();
    SameLineIfFits(ButtonW("Undo last change"));
    ImGui::BeginDisabled(!CanRevert(g));
    if (ImGui::Button("Undo last change")) DoAction("Reverted the last change in " + g.name, Revert(g));
    Tooltip("Puts back the files the last install, update or remove in this game replaced. Click again to go one more step back.");
    ImGui::EndDisabled();
    SameLineIfFits(ButtonW("Open ini"));
    ImGui::BeginDisabled(!g.ini);
    if (ImGui::Button("Open ini")) OpenInExplorer(g.iniPath, false);
    ImGui::EndDisabled();
}

static bool CanInstall(const Game &g)
{
    return g_app.src.ok() && !g.exe.empty() &&
           ((g.route == Route::Vulkan && !g_app.src.vkDir.empty()) ||
            (g.route == Route::Shader && g.reshade && !g_app.src.shaderFile.empty()) ||
            ((g.route == Route::Addon64 || g.route == Route::Addon32) && g.reshade && !g_app.src.dxDir.empty()));
}

// What "Install / update" would do for a ticked game ("" = it will be installed or updated).
static std::string BatchSkip(const Game &g)
{
    St s = StatusOf(g);
    if (s == St::Installed) return "up to date";
    if (s == St::NoExe) return "no executable";
    if (s == St::Unsupported) return "not supported";
    if (s == St::NeedsReShade) return "needs ReShade";
    if (!CanInstall(g)) return g_app.src.ok() ? "not in this release" : "pick a release first";
    return {};
}

static bool IsGlAddon(const Game &g) { return (g.apis & ApiGL) && (g.route == Route::Addon64 || g.route == Route::Addon32); }

// Install or update, then (if chosen) set G-SYNC off / Vertical Sync Force on in each game's NVIDIA profile,
// and for OpenGL games the Vulkan/OpenGL present method to Prefer layered on DXGI swapchain.
static void RunInstall(const std::vector<Game *> &list, bool nvGsync, bool nvVsync, bool nvGl, bool rsVk = false)
{
    std::vector<std::wstring> roots;
    for (auto *g : list) roots.push_back(g->root);
    std::string one = list.size() == 1 ? list[0]->name : "";
    StartJob("Installing BFI Presenter in " + std::to_string(roots.size()) + " games...", roots,
             [nvGsync, nvVsync, nvGl, rsVk](Game &g, Job &j) {
                 std::string e = InstallOrUpdate(g, g_app.src);
                 if (!e.empty()) return e;
                 // Vulkan games: ReShade's Vulkan layer for the settings window, unless the tested list says the game
                 // doesn't start with ReShade (DOOM Eternal)
                 if (rsVk && g.route == Route::Vulkan && g_app.vk.reshadeLayer && !g.reshadeVkIni &&
                     g.tested.find("without ReShade") == std::string::npos)
                     SetReShadeVulkan(g, true, nullptr);
                 const bool gl = nvGl && IsGlAddon(g);
                 if (g_app.nvOk && (nvGsync || nvVsync || gl))
                 {
                     std::string ne = NvSet(g, nvGsync ? kVrrForceOff : -2, nvVsync ? kVsyncForceOn : -2, gl ? kGlPresentLayered : -2);
                     if (!ne.empty()) { j.nvFail++; if (j.nvErr.empty()) j.nvErr = ne; }
                 }
                 return std::string();
             },
             [one, nvGsync, nvVsync, nvGl](Job &j) {
                 g_app.vk = DetectVulkanLayer();
                 std::string m = (!one.empty() && j.ok == 1) ? "Installed in " + one + "." : JobSummary(j, "Installed or updated");
                 if (!one.empty() && j.fail) m = "Failed: " + j.firstErr;
                 if (j.nvFail) m += " NVIDIA settings not changed: " + j.nvErr;
                 else if (j.ok && (nvGsync || nvVsync) && g_app.nvOk)
                     m += std::string(" NVIDIA:") + (nvGsync ? " G-SYNC off" : "") + (nvVsync ? " VSync force on" : "") + (nvGl ? " (OpenGL: present layered on DXGI)" : "") + ".";
                 g_app.SetMsg(m, j.fail || j.nvFail);
                 if (j.nvFail && NvNeedsAdmin(j.nvErr)) g_app.needAdmin = true;
             });
}

static void RequestInstall(const std::vector<Game *> &list)
{
    if (list.empty()) return;
    bool ask = g_app.nvOk && LoadSetting(L"NvAskOnInstall", L"1") == L"1";
    if (ask)
    {
        g_app.instPending.clear();
        for (auto *g : list) g_app.instPending.push_back(g->root);
        g_app.openInstall = true;
        return;
    }
    RunInstall(list, g_app.nvOk && LoadSetting(L"NvGsyncOff", L"1") == L"1" && LoadSetting(L"NvAskOnInstall", L"1") == L"0",
               g_app.nvOk && LoadSetting(L"NvVsyncOn", L"1") == L"1" && LoadSetting(L"NvAskOnInstall", L"1") == L"0",
               g_app.nvOk && LoadSetting(L"NvGlLayered", L"1") == L"1" && LoadSetting(L"NvAskOnInstall", L"1") == L"0");
}

static void DrawInstallQuestion()
{
    static bool open = false;
    if (g_app.openInstall) { g_app.openInstall = false; ImGui::OpenPopup("Install BFI Presenter"); open = true; }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 34, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Install BFI Presenter", &open, ImGuiWindowFlags_NoSavedSettings)) return;
    std::vector<Game *> list;
    for (auto &root : g_app.instPending)
        for (auto &g : g_app.games) if (g.root == root) list.push_back(&g);
    if (list.size() == 1) ImGui::TextWrapped("Install BFI Presenter in %s.", list[0]->name.c_str());
    else ImGui::TextWrapped("Install or update BFI Presenter in %d games.", (int)list.size());
    ImGui::Spacing();
    ImGui::TextUnformatted("Also change these NVIDIA settings in the game's profile:");
    static bool gs = true, vs = true, gl = true, askAgain = true, init = false;
    if (!init)
    {
        init = true;
        gs = LoadSetting(L"NvGsyncOff", L"1") == L"1";
        vs = LoadSetting(L"NvVsyncOn", L"1") == L"1";
        gl = LoadSetting(L"NvGlLayered", L"1") == L"1";
    }
    bool anyGl = false, anyVk = false;
    for (auto *g : list) { if (IsGlAddon(*g)) anyGl = true; if (g->route == Route::Vulkan && !g->reshadeVkIni) anyVk = true; }
    static bool rsVk = true;
    ImGui::Checkbox("G-SYNC off for the game (fixed 120 Hz)", &gs);
    Tooltip("With G-SYNC on, VRR can take over from the presenter's steady presents and the strobe flickers or drops "
            "below 120 Hz (Control, Undying). Same as NVIDIA Profile Inspector: G-SYNC = Force off.");
    ImGui::Checkbox("Vertical Sync: Force on", &vs);
    Tooltip("Games that ignore VSync otherwise tear or run unpaced (Resonance). Never use Force off or Fast Sync with BFI.");
    if (anyGl)
    {
        ImGui::Checkbox("OpenGL: present method Prefer layered on DXGI swapchain", &gl);
        Tooltip("Older OpenGL games (Need for Speed: Porsche Unleashed, DOOM 3 BFG) block the presenter -- it runs at 20-30 fps -- "
                "until their NVIDIA profile has Vulkan/OpenGL present method = Prefer layered on DXGI swapchain.");
    }
    if (anyVk && g_app.vk.reshadeLayer)
    {
        ImGui::Checkbox("Vulkan games: turn on ReShade (the BFI Presenter settings window in the game)", &rsVk);
        Tooltip("Writes a ReShade.ini next to the game's exe, so ReShade's Vulkan layer loads there. Without it, settings are "
                "changed in BFIPresenter.ini (picked up while the game runs) and with the hotkeys. Skipped for games the "
                "tested list runs without ReShade (DOOM Eternal).");
    }
    ImGui::Spacing();
    ImGui::Checkbox("Ask every time", &askAgain);
    Tooltip("Off: the choices above are used for every install without asking. Turn it back on under Libraries...");
    ImGui::Separator();
    if (ImGui::Button("Install"))
    {
        SaveSetting(L"NvGsyncOff", gs ? L"1" : L"0");
        SaveSetting(L"NvVsyncOn", vs ? L"1" : L"0");
        if (anyGl) SaveSetting(L"NvGlLayered", gl ? L"1" : L"0");
        SaveSetting(L"NvAskOnInstall", askAgain ? L"1" : L"0");
        ImGui::CloseCurrentPopup();
        RunInstall(list, gs, vs, anyGl && gl, anyVk && g_app.vk.reshadeLayer && rsVk);
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

static void DrawBatch()
{
    std::vector<Game *> sel;
    for (auto &g : g_app.games) if (g_app.picked.count(g.root)) sel.push_back(&g);
    int nInst = 0, nRem = 0;
    for (auto *g : sel) { if (BatchSkip(*g).empty()) nInst++; if (!g->addonPath.empty() || g->ini || !g->shaderPath.empty()) nRem++; }

    ImGui::Text("%d games ticked", (int)sel.size());
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) { g_app.picked.clear(); return; }
    ImGui::Spacing();
    char lbl[64];
    snprintf(lbl, 64, "Install / update (%d)", nInst);
    ImGui::BeginDisabled(nInst == 0 || g_app.busy);
    if (ImGui::Button(lbl))
    {
        std::vector<Game *> todo;
        for (auto *g : sel) if (BatchSkip(*g).empty()) todo.push_back(g);
        RequestInstall(todo);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    snprintf(lbl, 64, "Remove (%d)", nRem);
    ImGui::BeginDisabled(nRem == 0 || g_app.busy);
    if (ImGui::Button(lbl)) ImGui::OpenPopup("Remove ticked");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Checkbox("Remove deletes the ini too", &g_app.removeIni);
    if (ImGui::BeginPopupModal("Remove ticked", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::Text("Remove BFI Presenter from %d games%s?", nRem, g_app.removeIni ? ", with their BFIPresenter.ini" : "");
        ImGui::TextColored(kGrey, "Every removed file is backed up; Undo last change in each game puts it back.");
        if (ImGui::Button("Remove"))
        {
            std::vector<std::wstring> roots;
            for (auto *g : sel) if (!g->addonPath.empty() || g->ini || !g->shaderPath.empty()) roots.push_back(g->root);
            bool alsoIni = g_app.removeIni;
            ImGui::CloseCurrentPopup();
            StartJob("Removing BFI Presenter from " + std::to_string(roots.size()) + " games...", roots,
                     [alsoIni](Game &g, Job &) { return Remove(g, alsoIni); },
                     [](Job &j) { g_app.SetMsg(JobSummary(j, "Removed from"), j.fail != 0); });
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ReShade for the ticked games that use the add-on
    {
        std::vector<std::wstring> need, change;
        for (auto *g : sel)
            if (UsesReShadeRoute(g->route))
            {
                if (!g->reshade) need.push_back(g->root);
                else if (CompareVersions(g->reshadeDll.version, g_app.rsVersion) != 0) change.push_back(g->root);
            }
        ImGui::Spacing();
        ReShadeVersionCombo(ImGui::GetFontSize() * 11);
        ImGui::SameLine();
        snprintf(lbl, 64, "Install ReShade (%d)", (int)need.size());
        ImGui::BeginDisabled(need.empty() || g_app.busy || g_app.rsVersion.empty());
        if (ImGui::Button(lbl)) StartReShade(g_app.rsVersion, need);
        ImGui::EndDisabled();
        Tooltip("Installs the chosen ReShade version in the ticked games that don't have ReShade yet.");
        ImGui::SameLine();
        snprintf(lbl, 64, "Change version (%d)", (int)change.size());
        ImGui::BeginDisabled(change.empty() || g_app.busy || g_app.rsVersion.empty());
        if (ImGui::Button(lbl)) StartReShade(g_app.rsVersion, change);
        ImGui::EndDisabled();
        Tooltip("Switches the ticked games that have another ReShade version to the chosen one (backed up first).");
    }

    // NVIDIA settings for the ticked games
    if (g_app.nvOk)
    {
        static int gi = 2, vi = 2;
        const char *gItems[] = { "leave", "use global", "on", "off (fixed refresh)" };
        const char *vItems[] = { "leave", "use global", "3D application setting", "force on" };
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kGrey, "G-SYNC");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        ImGui::Combo("##bg", &gi, gItems, 4);
        ImGui::SameLine();
        ImGui::TextColored(kGrey, "VSync");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9);
        ImGui::Combo("##bv", &vi, vItems, 4);
        ImGui::SameLine();
        snprintf(lbl, 64, "Apply (%d)", (int)sel.size());
        ImGui::BeginDisabled(gi == 0 && vi == 0);
        if (ImGui::Button(lbl))
        {
            long long vrr = gi == 0 ? -2 : gi == 1 ? -1 : gi == 2 ? kVrrAllow : kVrrForceOff;
            long long vs = vi == 0 ? -2 : vi == 1 ? -1 : vi == 2 ? kVsyncApp : kVsyncForceOn;
            int ok = 0, fail = 0;
            std::string firstErr;
            for (auto *g : sel)
            {
                if (g->exe.empty()) continue;
                std::string e = NvWrite(g->exe, W(g->name), vrr, vs);
                if (e.empty()) ok++; else { fail++; if (firstErr.empty()) firstErr = e; }
                if (fail && NvNeedsAdmin(firstErr)) break;
            }
            std::vector<Game> copy;
            for (auto *g : sel) copy.push_back(*g);
            NvFill(copy);
            for (size_t k = 0; k < sel.size(); k++) *sel[k] = copy[k];
            g_app.SetMsg("NVIDIA settings changed for " + std::to_string(ok) + " game" + (ok == 1 ? "" : "s") +
                         (fail ? ". " + firstErr : "."), fail != 0);
            if (fail && NvNeedsAdmin(firstErr)) g_app.needAdmin = true;
        }
        ImGui::EndDisabled();
    }
    ImGui::Separator();
    if (ImGui::BeginTable("batch", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(0, 0)))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Game", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Install as", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Install / update", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableHeadersRow();
        for (auto *g : sel)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(g->name.c_str());
            ImGui::TableNextColumn(); ImGui::TextColored(kGrey, "%s", RouteText(g->route));
            ImGui::TableNextColumn();
            std::string skip = BatchSkip(*g);
            if (skip.empty()) ImGui::TextColored(kGreen, StatusOf(*g) == St::NotInstalled ? "install" : "update");
            else ImGui::TextColored(skip == "up to date" ? kGrey : kOrange, "skip: %s", skip.c_str());
        }
        ImGui::EndTable();
    }
}

static void RestartAsAdmin()
{
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    if ((INT_PTR)ShellExecuteW(nullptr, L"runas", self, nullptr, nullptr, SW_SHOWNORMAL) > 32) PostQuitMessage(0);
}

// The ReShade version used for installs (newest by default); shared by the detail pane and the batch pane.
static void ReShadeVersionCombo(float width)
{
    std::vector<std::string> list;
    {
        std::lock_guard<std::mutex> l(g_app.lock);
        list = g_app.rsVersions;
    }
    for (auto &v : CachedReShadeVersions()) if (std::find(list.begin(), list.end(), v) == list.end()) list.push_back(v);
    std::sort(list.begin(), list.end(), [](const std::string &a, const std::string &b) { return CompareVersions(a, b) > 0; });
    if (g_app.rsVersion.empty())
    {
        std::string saved = U8(LoadSetting(L"ReShadeVersion", L""));
        g_app.rsVersion = !saved.empty() ? saved : (list.empty() ? "" : list[0]);
    }
    ImGui::SetNextItemWidth(width);
    if (ImGui::BeginCombo("##rsver", g_app.rsVersion.empty() ? "no versions" : ("ReShade " + g_app.rsVersion).c_str()))
    {
        for (size_t i = 0; i < list.size(); i++)
        {
            std::string label = list[i] + (i == 0 ? "  (newest)" : "");
            if (ImGui::Selectable(label.c_str(), list[i] == g_app.rsVersion))
            {
                g_app.rsVersion = list[i];
                SaveSetting(L"ReShadeVersion", W(list[i]));
            }
        }
        ImGui::EndCombo();
    }
    Tooltip("ReShade version to install (the build with full add-on support, downloaded from reshade.me). "
            "Some games only start with an older one. Direct3D 9 games need 6.8 or newer for BFI Presenter.");
}

static void NvidiaControls(Game &g, float lab)
{
    if (!g_app.nvOk) { ImGui::TextColored(kGrey, "NVIDIA"); ImGui::SameLine(lab); ImGui::TextColored(kGrey, "no NVIDIA driver found"); return; }
    ImGui::TextColored(kGrey, "NVIDIA profile"); ImGui::SameLine(lab);
    if (!g.nvProfile.empty()) ImGui::TextUnformatted(U8(g.nvProfile).c_str());
    else ImGui::TextColored(kGrey, "none yet (made on the first change, named after the game)");

    const char *gItems[] = { "Use global setting", "On (G-SYNC allowed)", "Off (fixed refresh)" };
    int gi = !g.nvVrrOwn ? 0 : (g.nvVrr == kVrrForceOff || g.nvVrr == kVrrFixedRefresh || g.nvVrr == 2) ? 2 : 1;
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kGrey, "G-SYNC"); ImGui::SameLine(lab);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 11);
    if (ImGui::Combo("##gsync", &gi, gItems, 3))
    {
        std::string e = NvSet(g, gi == 0 ? -1 : gi == 1 ? kVrrAllow : kVrrForceOff, -2);
        if (e.empty()) g_app.SetMsg("G-SYNC for " + g.name + ": " + std::string(gItems[gi]) + ".", false);
        else { g_app.SetMsg(e, true); g_app.needAdmin = NvNeedsAdmin(e); }
    }
    ImGui::SameLine();
    ImGui::TextColored(kGrey, "now: %s%s", VrrText(g.nvVrr).c_str(), g.nvVrrOwn ? "" : " (global)");

    const char *vItems[] = { "Use global setting", "Use the 3D application setting", "Force on", "Force off", "Fast Sync" };
    const long long vVals[] = { -1, kVsyncApp, kVsyncForceOn, kVsyncForceOff, kVsyncFast };
    int vi = 0;
    if (g.nvVsyncOwn) for (int k = 1; k < 5; k++) if (g.nvVsync == vVals[k]) vi = k;
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kGrey, "Vertical Sync"); ImGui::SameLine(lab);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 11);
    if (ImGui::Combo("##vsync", &vi, vItems, 5))
    {
        std::string e = NvSet(g, -2, vVals[vi]);
        if (e.empty()) g_app.SetMsg("Vertical Sync for " + g.name + ": " + std::string(vItems[vi]) + ".", false);
        else { g_app.SetMsg(e, true); g_app.needAdmin = NvNeedsAdmin(e); }
    }
    ImGui::SameLine();
    ImGui::TextColored(g.nvVsync == kVsyncForceOff || g.nvVsync == kVsyncFast ? kRed : kGrey, "now: %s%s",
                       VsyncText(g.nvVsync).c_str(), g.nvVsyncOwn ? "" : " (global)");
    if (g.nvVsync == kVsyncForceOff || g.nvVsync == kVsyncFast)
    {
        ImGui::TextColored(kYellow, "  *"); ImGui::SameLine(lab);
        ImGui::TextWrapped("Force off and Fast Sync break the strobe's cadence: use the 3D application setting or Force on.");
    }
    if (!(g.apis & ApiGL)) return;
    const char *oItems[] = { "Use global setting", "Auto", "Prefer layered on DXGI swapchain", "Prefer native" };
    const long long oVals[] = { -1, kGlPresentAuto, kGlPresentLayered, kGlPresentNative };
    int oi = 0;
    if (g.nvGlOwn) for (int k = 1; k < 4; k++) if (g.nvGl == oVals[k]) oi = k;
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kGrey, "OpenGL"); ImGui::SameLine(lab);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 11);
    if (ImGui::Combo("##glpresent", &oi, oItems, 4))
    {
        std::string e = NvSet(g, -2, -2, oVals[oi]);
        if (e.empty()) g_app.SetMsg("Vulkan/OpenGL present method for " + g.name + ": " + std::string(oItems[oi]) + ".", false);
        else { g_app.SetMsg(e, true); g_app.needAdmin = NvNeedsAdmin(e); }
    }
    Tooltip("NVIDIA's Vulkan/OpenGL present method. Older OpenGL games (Need for Speed: Porsche Unleashed, DOOM 3 BFG) "
            "block the presenter until it is Prefer layered on DXGI swapchain.");
    ImGui::SameLine();
    ImGui::TextColored(g.nvGl == kGlPresentLayered ? kGreen : kGrey, "now: %s%s", GlPresentText(g.nvGl).c_str(), g.nvGlOwn ? "" : " (global)");
}

static void DrawDetail(HWND)
{
    if (g_app.picked.size() > 1) { DrawBatch(); return; }
    if (g_app.sel < 0 || g_app.sel >= (int)g_app.games.size())
    {
        ImGui::TextColored(kGrey, "Select a game to see what it needs and to install, update or remove BFI Presenter.");
        return;
    }
    Game &g = g_app.games[g_app.sel];
    float lab = ImGui::CalcTextSize("BFI Presenter").x + ImGui::GetFontSize() * 1.2f;
    ImGui::Text("%s", g.name.c_str());
    ImGui::SameLine();
    ImGui::TextColored(kGrey, "(%s)", g.store.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Open folder")) OpenInExplorer(g.exe.empty() ? g.root : g.exe, !g.exe.empty());
    ImGui::Spacing();
    DrawActions(g);
    ImGui::Separator();

    // exe
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kGrey, "Executable"); ImGui::SameLine(lab);
    ImGui::SetNextItemWidth(-1);
    std::string cur = g.exe.empty() ? std::string("(none)") : U8(g.exe.substr(std::min(g.root.size() + 1, g.exe.size())));
    if (ImGui::BeginCombo("##exe", cur.c_str()))
    {
        for (auto &e : g.exes)
        {
            std::string n = U8(e.substr(std::min(g.root.size() + 1, e.size())));
            if (ImGui::Selectable(n.c_str(), e == g.exe))
            {
                g.exe = e;
                g.exeOverride = e != g.exes[0];
                SaveExeOverride(g.root, g.exeOverride ? e : L"");
                Detect(g);
            }
        }
        ImGui::EndCombo();
    }

    // route
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kGrey, "Install as"); ImGui::SameLine(lab);
    {
        const char *items[] = { "Automatic", "DX add-on", "Vulkan layer", "BFI shader" };
        std::wstring key = RouteKey(g.root);
        std::wstring v = LoadSetting(key.c_str(), L"");
        int r = v == L"dx" ? 1 : v == L"vulkan" ? 2 : v == L"shader" ? 3 : 0;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
        if (ImGui::Combo("##route", &r, items, 4))
        {
            SaveSetting(key.c_str(), r == 1 ? L"dx" : r == 2 ? L"vulkan" : r == 3 ? L"shader" : L"");
            Detect(g);
        }
        Tooltip("BFI shader: the strobe as a ReShade effect, clocked by the game's own frames. Only for games that hold a "
                "rock-steady frame rate equal to the refresh rate and whose engine stutters with the presenter "
                "(Moons of Madness). Everywhere else the DX add-on is better: with the shader a late frame flashes.");
        ImGui::SameLine();
        ImGui::TextColored(kAccent, "%s", RouteText(g.route));
    }

    // API sources
    ImGui::TextColored(kGrey, "Graphics API"); ImGui::SameLine(lab);
    {
        std::string t;
        if (g.pcgwKnown && !g.pcgwText.empty()) t = "PCGamingWiki: " + g.pcgwText;
        if (g.exeApis) t += (t.empty() ? "" : "   |   ") + std::string("executable: ") + ApiText(g.exeApis);
        if (t.empty()) t = "unknown";
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(t.c_str());
        ImGui::PopTextWrapPos();
    }

    // ReShade / other
    ImGui::TextColored(kGrey, "ReShade"); ImGui::SameLine(lab);
    if (g.reshade) ImGui::Text("%s  (%s)", g.reshadeDll.version.c_str(), U8(g.reshadeDll.path.substr(g.reshadeDll.path.find_last_of(L'\\') + 1)).c_str());
    else if (g.route == Route::Vulkan)
    {
        ImGui::PushTextWrapPos(0);
        if (!g_app.vk.reshadeLayer)
            ImGui::TextColored(kGrey, "not installed. Optional: ReShade gives the BFI Presenter settings window in the game. Run ReShade's "
                                      "setup once with its Vulkan option (for any game); then it can be turned on here per game.");
        else
        {
            ImGui::Text("%s", g.reshadeVkIni ? "on (ReShade's Vulkan layer, ReShade.ini next to the exe)" : "off for this game (ReShade's Vulkan layer is installed)");
            ImGui::SameLine();
            if (ImGui::SmallButton(g.reshadeVkIni ? "Turn off##rsvk" : "Turn on##rsvk"))
            {
                std::string note, e = SetReShadeVulkan(g, !g.reshadeVkIni, &note);
                if (e.empty()) g_app.SetMsg(std::string("ReShade ") + (g.reshadeVkIni ? "on" : "off") + " in " + g.name + "." + (note.empty() ? "" : " " + note), false);
                else g_app.SetMsg(e, true);
            }
            Tooltip("Vulkan games use ReShade's system-wide Vulkan layer, which loads in a game that has a ReShade.ini next to its exe. "
                    "Turn on writes that file (and, with administrator rights, lists the game in ReShade's own setup); Turn off removes it (backed up, Undo works). "
                    "With ReShade on, the BFI Presenter (Vulkan) tab in its overlay changes the settings live.");
        }
        ImGui::PopTextWrapPos();
    }
    else ImGui::TextColored(kOrange, "not found next to the executable");
    if (UsesReShadeRoute(g.route))
    {
        ImGui::Dummy(ImVec2(lab - ImGui::GetStyle().ItemSpacing.x, 0)); ImGui::SameLine(lab);
        ReShadeVersionCombo(ImGui::GetFontSize() * 11);
        ImGui::SameLine();
        bool same = g.reshade && CompareVersions(g.reshadeDll.version, g_app.rsVersion) == 0;
        std::string b = g.reshade ? (same ? "Reinstall ReShade" : "Change to " + g_app.rsVersion) : "Install ReShade";
        b += "##reshade";
        ImGui::BeginDisabled(g_app.busy || g_app.rsVersion.empty());
        if (ImGui::Button(b.c_str())) StartReShade(g_app.rsVersion, { g.root });
        ImGui::EndDisabled();
        Tooltip(g.reshade ? "Replaces the ReShade DLL with the chosen version, keeping its name. The old one is backed up (Undo last change)."
                          : "Downloads ReShade with add-on support from reshade.me and puts it next to the game's exe "
                            "(dxgi.dll; d3d9.dll or opengl32.dll for those games; ReShade64.dll next to OptiScaler).");
    }
    std::string extra;
    if (g.optiscaler) extra += "OptiScaler  ";
    if (g.dxvk) extra += "DXVK  ";
    if (g.dgvoodoo) extra += "dgVoodoo  ";
    if (g.specialK) extra += "Special K  ";
    if (!extra.empty()) { ImGui::TextColored(kGrey, "Also found"); ImGui::SameLine(lab); ImGui::Text("%s", extra.c_str()); }
    ImGui::TextColored(kGrey, "BFI Presenter"); ImGui::SameLine(lab);
    if (g.route == Route::Vulkan) ImGui::Text(g.ini ? "BFIPresenter.ini present (the Vulkan layer is active in this game)" : "no BFIPresenter.ini next to the exe");
    else if (g.route == Route::Shader)
    {
        ImGui::PushTextWrapPos(0);
        if (g.shaderPath.empty()) ImGui::TextColored(kGrey, "shader not installed");
        else ImGui::Text("shader v%s, %s", g.shaderVersion.c_str(), g.shaderOn ? "on in the ReShade preset (last in the list)" : "installed but off in the ReShade preset");
        if (!g.addonPath.empty()) ImGui::TextColored(kOrange, "The add-on is installed too: installing the shader removes it (never use both).");
        ImGui::PopTextWrapPos();
    }
    else if (!g.addonPath.empty()) ImGui::Text("v%s  %s", g.addonVersion.c_str(), g.ini ? "(with BFIPresenter.ini)" : "");
    else ImGui::TextColored(kGrey, "not installed");
    if (!g.tested.empty())
    {
        ImGui::TextColored(kGrey, "Tested"); ImGui::SameLine(lab);
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(g.tested.c_str());
        ImGui::PopTextWrapPos();
    }
    if (g.shaderRecommended && g.route != Route::Shader)
    {
        ImGui::TextColored(kYellow, "  *"); ImGui::SameLine(lab);
        ImGui::PushTextWrapPos(0);
        ImGui::TextWrapped("The tested-games list recommends the BFI shader for this game: Install as -> BFI shader.");
        ImGui::PopTextWrapPos();
    }
    ImGui::Spacing();
    NvidiaControls(g, lab);
    ImGui::Spacing();
    for (auto &n : g.notes)
    {
        ImGui::TextColored(kYellow, "  *"); ImGui::SameLine(lab);
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(n.c_str());
        ImGui::PopTextWrapPos();
    }

}

static void DrawLibraries(HWND hwnd)
{
    static bool open = false;
    if (g_app.showLibraries)
    {
        g_app.showLibraries = false;
        g_app.libs = FindLibraries();
        ImGui::OpenPopup("Libraries");
        open = true;
    }
    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 42, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Libraries", &open, ImGuiWindowFlags_NoSavedSettings)) return;
    ScanOptions &o = g_app.opt;
    LibraryInfo &li = g_app.libs;
    ImGui::TextWrapped("Choose where to look for games, then Scan. The list is kept, so this is needed only once and when "
                       "you install games somewhere new.");
    ImGui::Spacing();
    auto lib = [](const char *name, bool *on, bool found, const std::string &text) {
        ImGui::BeginDisabled(!found);
        if (!found) *on = false;
        ImGui::Checkbox(name, on);
        ImGui::EndDisabled();
        ImGui::SameLine(ImGui::GetFontSize() * 9);
        ImGui::TextColored(found ? kGrey : ImVec4(0.45f, 0.45f, 0.48f, 1), "%s", text.c_str());
    };
    lib("Steam", &o.steam, li.steam, li.steamText);
    lib("Epic Games", &o.epic, li.epic, li.epicText);
    lib("GOG", &o.gog, li.gog, li.gogText);
    lib("Xbox", &o.xbox, li.xbox, li.xboxText);
    {
        bool pw = LoadSetting(L"PCGamingWiki", L"1") == L"1";
        if (ImGui::Checkbox("Look up each game's graphics APIs on PCGamingWiki", &pw)) SaveSetting(L"PCGamingWiki", pw ? L"1" : L"0");
        bool ask = LoadSetting(L"NvAskOnInstall", L"1") == L"1";
        if (ImGui::Checkbox("Ask about G-SYNC and Vertical Sync when installing", &ask)) SaveSetting(L"NvAskOnInstall", ask ? L"1" : L"0");
        Tooltip("Asks pcgamingwiki.com which APIs a game supports (Steam games by app id, others by name). More reliable than "
                "reading the executable, for example for games that load Vulkan or Direct3D 12 at run time. Results are cached.");
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Other folders");
    ImGui::PushStyleColor(ImGuiCol_Text, kGrey);
    ImGui::TextWrapped("Each sub-folder is taken as one game (or the folder itself, if it has the game's .exe).");
    ImGui::PopStyleColor();
    int del = -1;
    for (int i = 0; i < (int)o.folders.size(); i++)
    {
        ImGui::PushID(i);
        if (ImGui::SmallButton("Remove")) del = i;
        ImGui::SameLine();
        ImGui::TextUnformatted(U8(o.folders[i]).c_str());
        ImGui::PopID();
    }
    if (o.folders.empty()) ImGui::TextColored(kGrey, "(none)");
    if (del >= 0) o.folders.erase(o.folders.begin() + del);
    if (ImGui::Button("Add folder..."))
    {
        std::wstring d = PickFolder(hwnd, L"Folder with games");
        if (!d.empty()) o.folders.push_back(d);
    }
    ImGui::Spacing();
    ImGui::Separator();
    bool any = o.steam || o.epic || o.gog || o.xbox || !o.folders.empty();
    ImGui::BeginDisabled(!any || g_app.busy);
    if (ImGui::Button("Scan"))
    {
        SaveScanOptions(o);
        ImGui::CloseCurrentPopup();
        StartScan(true);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) { g_app.opt = LoadScanOptions(); ImGui::CloseCurrentPopup(); }
    if (!open) g_app.opt = LoadScanOptions();     // closed with the X
    ImGui::EndPopup();
}

static void DrawFrame(HWND hwnd)
{
    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    // (no NoSavedSettings: the game table's column widths, order and sort are kept in manager_ui.ini)
    ImGui::Begin("main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);
    DrawTop(hwnd);
    // Table | splitter | detail pane. Drag the splitter to give either side more room; the share is remembered.
    static float split = -1.0f;
    if (split < 0.0f) { split = (float)_wtof(LoadSetting(L"Split", L"0.6").c_str()); if (split < 0.2f || split > 0.9f) split = 0.6f; }
    float avail = ImGui::GetContentRegionAvail().x;
    const float sw = std::max(6.0f, ImGui::GetFontSize() * 0.4f);
    float tableW = std::clamp(avail * split, ImGui::GetFontSize() * 12, avail - sw - ImGui::GetFontSize() * 16);
    ImGui::BeginChild("left", ImVec2(tableW, 0));
    DrawTable(ImGui::GetContentRegionAvail().y);
    ImGui::EndChild();
    ImGui::SameLine(0, 0);
    ImGui::InvisibleButton("##split", ImVec2(sw, ImGui::GetContentRegionAvail().y));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActive())
    {
        split = std::clamp((tableW + ImGui::GetIO().MouseDelta.x) / std::max(avail, 1.0f), 0.2f, 0.9f);
        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2((a.x + b.x) * 0.5f - 1, a.y), ImVec2((a.x + b.x) * 0.5f + 1, b.y),
                                                  ImGui::GetColorU32(ImGuiCol_SeparatorActive));
    }
    if (ImGui::IsItemDeactivated()) SaveSetting(L"Split", std::to_wstring(split));
    ImGui::SameLine(0, 0);
    ImGui::BeginChild("detail", ImVec2(0, 0), ImGuiChildFlags_Borders);
    DrawDetail(hwnd);
    ImGui::EndChild();
    DrawLibraries(hwnd);
    DrawInstallQuestion();
    DrawJob();
    ImGui::End();
}

static void SetupStyle(float scale)
{
    ImGuiStyle &st = ImGui::GetStyle();
    st = ImGuiStyle();
    ImGui::StyleColorsDark();
    st.WindowRounding = 0; st.FrameRounding = 4; st.GrabRounding = 4; st.ChildRounding = 4; st.PopupRounding = 4;
    st.FramePadding = ImVec2(8, 4); st.ItemSpacing = ImVec2(8, 6); st.WindowPadding = ImVec2(12, 10);
    ImVec4 *c = st.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.09f, 0.10f, 1);
    c[ImGuiCol_ChildBg] = ImVec4(0.11f, 0.11f, 0.12f, 1);
    c[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.12f, 0.13f, 1);
    c[ImGuiCol_FrameBg] = ImVec4(0.17f, 0.17f, 0.19f, 1);
    c[ImGuiCol_Button] = ImVec4(0.20f, 0.36f, 0.58f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.26f, 0.45f, 0.72f, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(0.18f, 0.32f, 0.52f, 1);
    c[ImGuiCol_Header] = ImVec4(0.20f, 0.36f, 0.58f, 0.55f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.26f, 0.45f, 0.72f, 0.45f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.025f);
    st.ScaleAllSizes(scale);
}

// Latin with accents, Greek, Cyrillic, punctuation and symbols such as ™ and ® that game names use
static const ImWchar kRanges[] = { 0x0020, 0x024F, 0x0370, 0x03FF, 0x0400, 0x052F, 0x2000, 0x206F, 0x20A0, 0x20CF,
                                   0x2100, 0x214F, 0x2190, 0x21FF, 0x2460, 0x24FF, 0 };

static void LoadFonts(float scale)
{
    ImGuiIO &io = ImGui::GetIO();
    io.Fonts->Clear();
    wchar_t win[MAX_PATH];
    GetWindowsDirectoryW(win, MAX_PATH);
    std::string font = U8(std::wstring(win) + L"\\Fonts\\segoeui.ttf");
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    if (GetFileAttributesA(font.c_str()) == INVALID_FILE_ATTRIBUTES ||
        !io.Fonts->AddFontFromFileTTF(font.c_str(), 17.0f * scale, &cfg, kRanges))
    {
        ImFontConfig d;
        d.SizePixels = 13.0f * scale;
        io.Fonts->AddFontDefault(&d);
    }
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0, 0, inst, LoadIconW(inst, MAKEINTRESOURCEW(1)), LoadCursor(nullptr, IDC_ARROW),
                       nullptr, nullptr, L"BFIPresenterManager", nullptr };
    RegisterClassExW(&wc);
    // Where the window was last time, if that is still on a monitor; else centred on the monitor with the mouse.
    RECT saved{};
    bool maximized = false, haveSaved = false;
    {
        std::wstring ws = LoadSetting(L"Window", L"");
        int a, b, c, d, m;
        if (swscanf(ws.c_str(), L"%d,%d,%d,%d,%d", &a, &b, &c, &d, &m) == 5 && c - a > 200 && d - b > 150)
        {
            saved = { a, b, c, d };
            maximized = m != 0;
            haveSaved = MonitorFromRect(&saved, MONITOR_DEFAULTTONULL) != nullptr;
        }
    }
    POINT pt{};
    GetCursorPos(&pt);
    HMONITOR mon = haveSaved ? MonitorFromRect(&saved, MONITOR_DEFAULTTONEAREST) : MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    g_dpiScale = ImGui_ImplWin32_GetDpiScaleForMonitor(mon);
    if (g_dpiScale < 1.0f) g_dpiScale = 1.0f;
    const RECT &wa = mi.rcWork;
    int waW = wa.right - wa.left, waH = wa.bottom - wa.top;
    int w, h, x, y;
    if (haveSaved) { w = saved.right - saved.left; h = saved.bottom - saved.top; x = saved.left; y = saved.top; }
    else { w = (int)(1280 * g_dpiScale); h = (int)(820 * g_dpiScale); x = 0; y = 0; }
    // never larger than the work area, and never with the title bar off screen
    w = std::min(w, waW * 95 / 100);
    h = std::min(h, waH * 92 / 100);
    if (!haveSaved) { x = wa.left + (waW - w) / 2; y = wa.top + (waH - h) / 2; }
    x = std::max((int)wa.left, std::min(x, (int)wa.right - w));
    y = std::max((int)wa.top, std::min(y, (int)wa.bottom - h));
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"BFI Presenter Manager " MANAGER_VERSION, WS_OVERLAPPEDWINDOW, x, y, w, h,
                              nullptr, nullptr, inst, nullptr);
    if (!CreateDevice(hwnd))
    {
        CleanupDevice();
        MessageBoxW(nullptr, L"Couldn't create a Direct3D 11 device.", L"BFI Presenter Manager", MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, maximized ? SW_SHOWMAXIMIZED : SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    // ImGui's own settings file: only the game table's columns (widths, order, hidden, sort) are worth keeping
    static std::string uiIni = U8(DataDir() + L"\\manager_ui.ini");
    io.IniFilename = uiIni.c_str();
    io.ConfigDebugHighlightIdConflicts = false;     // never show ImGui's programmer popup to users
    SetupStyle(g_dpiScale);
    LoadFonts(g_dpiScale);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    // initial state
    g_app.opt = LoadScanOptions();
    std::wstring srcDir = LoadSetting(L"Source", L"");
    if (!srcDir.empty()) g_app.src = SourceFromFolder(srcDir);
    if (!g_app.src.ok()) g_app.src = CachedRelease();
    g_app.vk = DetectVulkanLayer();
    g_app.nvOk = NvAvailable();
    if (LoadGameList(g_app.games)) StartScan(false);      // the saved list, re-checked in the background
    else g_app.showLibraries = true;                      // first start: choose libraries, then scan

    bool done = false;
    while (!done)
    {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&m);
            DispatchMessageW(&m);
            if (m.message == WM_QUIT) done = true;
        }
        if (done) break;
        if (IsIconic(hwnd)) { Sleep(50); continue; }
        if (g_resizeW)
        {
            CleanupRT();
            g_sc->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeW = g_resizeH = 0;
            CreateRT();
        }
        if (g_dpiChanged)
        {
            g_dpiChanged = false;
            SetupStyle(g_dpiScale);
            LoadFonts(g_dpiScale);
            ImGui_ImplDX11_InvalidateDeviceObjects();
        }
        PollWorker();
        StepJob();
        // Using a downloaded release (not a folder picked by hand): look for a newer one once per session.
        if (g_app.scannedOnce && !g_app.checkedOnce && !g_app.busy)
        {
            g_app.checkedOnce = true;
            if (LoadSetting(L"Source", L"").empty() && LoadSetting(L"AutoCheck", L"1") == L"1") StartFetch();
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawFrame(hwnd);
        ImGui::Render();
        const float clear[4] = { 0.09f, 0.09f, 0.10f, 1 };
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_sc->Present(1, 0);
        if (!g_app.busy && !g_job.active && !ImGui::IsAnyItemActive()) MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
    }
    JoinWorker();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDevice();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, inst);
    CoUninitialize();
    return 0;
}
