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
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "core.h"

using namespace bfi;

#define MANAGER_VERSION "1.0.0"

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
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ------------------------------------------------------------------ app state

struct App {
    std::vector<Game> games;
    int sel = -1;
    Source src;
    VulkanLayerState vk;
    std::vector<std::wstring> folders;

    // background work
    std::mutex lock;
    std::thread worker;
    std::atomic<bool> busy{ false };
    std::string busyText;              // guarded by lock
    bool scanDone = false;             // guarded by lock
    std::vector<Game> scanned;         // guarded by lock
    bool releaseDone = false;          // guarded by lock
    Source fetched;                    // guarded by lock
    std::string remoteTag, remoteErr;  // guarded by lock

    // UI
    char search[128] = {};
    bool onlyRelevant = false;
    std::string msg;
    bool msgErr = false;
    bool removeIni = false;
    bool showFolders = false;
    std::string latestTag;             // newest tag seen on GitHub this session
    bool scannedOnce = false, checkedOnce = false;   // GitHub is checked once, after the first scan

    void SetMsg(const std::string &m, bool err) { msg = m; msgErr = err; }
};

static App g_app;

static void JoinWorker()
{
    if (g_app.worker.joinable()) g_app.worker.join();
}

static void StartScan()
{
    if (g_app.busy) return;
    JoinWorker();
    g_app.busy = true;
    { std::lock_guard<std::mutex> l(g_app.lock); g_app.busyText = "Scanning game libraries..."; }
    auto folders = g_app.folders;
    g_app.worker = std::thread([folders] {
        auto games = FindGames(folders);
        auto vk = DetectVulkanLayer();
        std::lock_guard<std::mutex> l(g_app.lock);
        g_app.scanned = std::move(games);
        g_app.vk = vk;
        g_app.scanDone = true;
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
            if (!tagV.empty() && (tagV[0] == 'v' || tagV[0] == 'V')) tagV = tagV.substr(1);
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
        g_app.SetMsg("Found " + std::to_string(g_app.games.size()) + " games.", false);
        g_app.scannedOnce = true;
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
        ImGui::TextColored(kGrey, "(add-on %s, Vulkan layer %s)", g_app.src.dxVersion.empty() ? "-" : g_app.src.dxVersion.c_str(),
                           g_app.src.vkVersion.empty() ? "-" : g_app.src.vkVersion.c_str());
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
    if (ImGui::Button("Rescan")) StartScan();
    ImGui::SameLine();
    if (ImGui::Button("Game folders...")) g_app.showFolders = true;
    Tooltip("Steam, Epic, GOG and Xbox games are found automatically. Add folders for games installed elsewhere.");
    ImGui::SameLine();
    int nUpd = 0;
    for (auto &g : g_app.games) { St s = StatusOf(g); if ((s == St::Update || s == St::WrongBits) && g.route != Route::Vulkan) nUpd++; }
    bool vkOld = g_app.vk.installed && !g_app.src.vkVersion.empty() && CompareVersions(g_app.vk.version, g_app.src.vkVersion) < 0;
    ImGui::BeginDisabled(!g_app.src.ok() || (nUpd == 0 && !vkOld));
    char lbl[64];
    snprintf(lbl, 64, "Update all (%d%s)", nUpd, vkOld ? " + layer" : "");
    if (ImGui::Button(lbl))
    {
        int ok = 0, fail = 0;
        std::string firstErr;
        if (vkOld) { std::string e = InstallVulkanLayer(g_app.src); if (e.empty()) ok++; else { fail++; firstErr = e; } }
        for (auto &g : g_app.games)
        {
            St s = StatusOf(g);
            if ((s == St::Update || s == St::WrongBits) && g.route != Route::Vulkan)
            {
                std::string e = InstallOrUpdate(g, g_app.src);
                if (e.empty()) ok++; else { fail++; if (firstErr.empty()) firstErr = g.name + ": " + e; }
            }
        }
        g_app.vk = DetectVulkanLayer();
        if (fail) g_app.SetMsg("Updated " + std::to_string(ok) + ", failed " + std::to_string(fail) + ". " + firstErr, true);
        else g_app.SetMsg("Updated " + std::to_string(ok) + ".", false);
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    ImGui::InputTextWithHint("##search", "Search games", g_app.search, sizeof(g_app.search));
    ImGui::SameLine();
    ImGui::Checkbox("Only games with ReShade, BFI or a tested entry", &g_app.onlyRelevant);

    if (g_app.busy)
    {
        std::lock_guard<std::mutex> l(g_app.lock);
        ImGui::TextColored(kAccent, "%s", g_app.busyText.c_str());
    }
    else if (!g_app.msg.empty())
    {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(g_app.msgErr ? kRed : kGreen, "%s", g_app.msg.c_str());
        ImGui::PopTextWrapPos();
    }
    else ImGui::TextUnformatted("");
}

static void DrawTable(float height)
{
    ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                         ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("games", 7, tf, ImVec2(0, height))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Game", ImGuiTableColumnFlags_WidthStretch, 2.6f);
    ImGui::TableSetupColumn("Store", ImGuiTableColumnFlags_WidthStretch, 0.6f);
    ImGui::TableSetupColumn("Bits", ImGuiTableColumnFlags_WidthStretch, 0.35f);
    ImGui::TableSetupColumn("API", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("ReShade", ImGuiTableColumnFlags_WidthStretch, 0.7f);
    ImGui::TableSetupColumn("BFI Presenter", ImGuiTableColumnFlags_WidthStretch, 0.9f);
    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 1.3f);
    ImGui::TableHeadersRow();
    for (int i = 0; i < (int)g_app.games.size(); i++)
    {
        Game &g = g_app.games[i];
        if (!Matches(g, g_app.search) || (g_app.onlyRelevant && !Relevant(g))) continue;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID(i);
        if (ImGui::Selectable(g.name.c_str(), g_app.sel == i, ImGuiSelectableFlags_SpanAllColumns)) g_app.sel = i;
        ImGui::PopID();
        ImGui::TableNextColumn(); ImGui::TextColored(kGrey, "%s", g.store.c_str());
        ImGui::TableNextColumn(); if (g.bits) ImGui::Text("%d", g.bits); else ImGui::TextColored(kGrey, "?");
        ImGui::TableNextColumn(); ImGui::TextUnformatted(g.exe.empty() ? "" : ApiText(g.apis).c_str());
        ImGui::TableNextColumn();
        if (g.reshade) ImGui::Text("%s", g.reshadeDll.version.empty() ? "yes" : g.reshadeDll.version.c_str());
        else ImGui::TextColored(kGrey, "-");
        ImGui::TableNextColumn();
        if (g.route == Route::Vulkan) { if (g.ini) ImGui::Text("Vulkan (ini)"); else ImGui::TextColored(kGrey, "-"); }
        else if (!g.addonPath.empty()) ImGui::Text("%s%s", g.addonVersion.c_str(), g.addonWrongBits ? " (wrong bits)" : "");
        else ImGui::TextColored(kGrey, "-");
        ImGui::TableNextColumn(); StatusText(g);
    }
    ImGui::EndTable();
}

static void DrawActions(Game &g)
{
    St s = StatusOf(g);
    bool canInstall = g_app.src.ok() && !g.exe.empty() &&
                      ((g.route == Route::Vulkan && !g_app.src.vkDir.empty()) ||
                       ((g.route == Route::Addon64 || g.route == Route::Addon32) && g.reshade && !g_app.src.dxDir.empty()));
    const char *verb = (s == St::Update || s == St::WrongBits) ? "Update" : (s == St::Installed ? "Reinstall" : "Install");
    ImGui::BeginDisabled(!canInstall || (s == St::Installed && g.route == Route::Vulkan));
    if (ImGui::Button(verb)) DoAction(std::string(verb) + " done: " + g.name, InstallOrUpdate(g, g_app.src));
    ImGui::EndDisabled();
    if (!g_app.src.ok()) { ImGui::SameLine(); ImGui::TextColored(kGrey, "(pick a release first)"); }
    SameLineIfFits(ButtonW("Remove"));
    bool installed = !g.addonPath.empty() || g.ini;
    ImGui::BeginDisabled(!installed);
    if (ImGui::Button("Remove")) DoAction("Removed from " + g.name, Remove(g, g_app.removeIni));
    SameLineIfFits(ImGui::GetFrameHeight() + ButtonW("also BFIPresenter.ini"));
    ImGui::Checkbox("also BFIPresenter.ini", &g_app.removeIni);
    Tooltip("The ini holds this game's settings. For Vulkan games the ini is what turns the layer on, so Remove always deletes it there.");
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

static void DrawDetail(HWND)
{
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
        const char *items[] = { "Automatic", "DX add-on", "Vulkan layer" };
        std::wstring key = RouteKey(g.root);
        std::wstring v = LoadSetting(key.c_str(), L"");
        int r = v == L"dx" ? 1 : v == L"vulkan" ? 2 : 0;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
        if (ImGui::Combo("##route", &r, items, 3))
        {
            SaveSetting(key.c_str(), r == 1 ? L"dx" : r == 2 ? L"vulkan" : L"");
            Detect(g);
        }
        ImGui::SameLine();
        ImGui::TextColored(kAccent, "%s", RouteText(g.route));
    }

    // ReShade / other
    ImGui::TextColored(kGrey, "ReShade"); ImGui::SameLine(lab);
    if (g.reshade) ImGui::Text("%s  (%s)", g.reshadeDll.version.c_str(), U8(g.reshadeDll.path.substr(g.reshadeDll.path.find_last_of(L'\\') + 1)).c_str());
    else if (g.route == Route::Vulkan)
        ImGui::TextColored(kGrey, g_app.vk.reshadeLayer ? "through ReShade's Vulkan layer" : "optional for Vulkan games (ReShade's Vulkan layer isn't installed)");
    else ImGui::TextColored(kOrange, "not found next to the executable");
    std::string extra;
    if (g.optiscaler) extra += "OptiScaler  ";
    if (g.dxvk) extra += "DXVK  ";
    if (g.dgvoodoo) extra += "dgVoodoo  ";
    if (g.specialK) extra += "Special K  ";
    if (!extra.empty()) { ImGui::TextColored(kGrey, "Also found"); ImGui::SameLine(lab); ImGui::Text("%s", extra.c_str()); }
    ImGui::TextColored(kGrey, "BFI Presenter"); ImGui::SameLine(lab);
    if (g.route == Route::Vulkan) ImGui::Text(g.ini ? "BFIPresenter.ini present (the Vulkan layer is active in this game)" : "no BFIPresenter.ini next to the exe");
    else if (!g.addonPath.empty()) ImGui::Text("v%s  %s", g.addonVersion.c_str(), g.ini ? "(with BFIPresenter.ini)" : "");
    else ImGui::TextColored(kGrey, "not installed");
    if (!g.tested.empty())
    {
        ImGui::TextColored(kGrey, "Tested"); ImGui::SameLine(lab);
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(g.tested.c_str());
        ImGui::PopTextWrapPos();
    }
    for (auto &n : g.notes)
    {
        ImGui::TextColored(kYellow, "  *"); ImGui::SameLine(lab);
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(n.c_str());
        ImGui::PopTextWrapPos();
    }

}

static void DrawFolders(HWND hwnd)
{
    if (g_app.showFolders) { ImGui::OpenPopup("Game folders"); g_app.showFolders = false; }
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 40, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Game folders", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    ImGui::TextWrapped("Folders searched in addition to Steam, Epic, GOG and Xbox. Each sub-folder is taken as one game "
                       "(or the folder itself, if it has the game's .exe).");
    ImGui::Separator();
    int del = -1;
    for (int i = 0; i < (int)g_app.folders.size(); i++)
    {
        ImGui::PushID(i);
        if (ImGui::SmallButton("Remove")) del = i;
        ImGui::SameLine();
        ImGui::TextUnformatted(U8(g_app.folders[i]).c_str());
        ImGui::PopID();
    }
    if (g_app.folders.empty()) ImGui::TextColored(kGrey, "(none)");
    if (del >= 0) { g_app.folders.erase(g_app.folders.begin() + del); SaveFolders(g_app.folders); }
    ImGui::Separator();
    if (ImGui::Button("Add folder..."))
    {
        std::wstring d = PickFolder(hwnd, L"Folder with games");
        if (!d.empty()) { g_app.folders.push_back(d); SaveFolders(g_app.folders); }
    }
    ImGui::SameLine();
    if (ImGui::Button("Close and rescan")) { ImGui::CloseCurrentPopup(); StartScan(); }
    ImGui::SameLine();
    if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

static void DrawFrame(HWND hwnd)
{
    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus);
    DrawTop(hwnd);
    float avail = ImGui::GetContentRegionAvail().x;
    float tableW = std::max(avail * 0.56f, avail - ImGui::GetFontSize() * 34);
    ImGui::BeginChild("left", ImVec2(tableW, 0));
    DrawTable(ImGui::GetContentRegionAvail().y);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("detail", ImVec2(0, 0), ImGuiChildFlags_Borders);
    DrawDetail(hwnd);
    ImGui::EndChild();
    DrawFolders(hwnd);
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
        !io.Fonts->AddFontFromFileTTF(font.c_str(), 17.0f * scale, &cfg, io.Fonts->GetGlyphRangesDefault()))
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
    POINT pt{ 0, 0 };
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    g_dpiScale = ImGui_ImplWin32_GetDpiScaleForMonitor(mon);
    if (g_dpiScale < 1.0f) g_dpiScale = 1.0f;
    int w = (int)(1280 * g_dpiScale), h = (int)(820 * g_dpiScale);
    int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2, y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2;
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"BFI Presenter Manager " MANAGER_VERSION, WS_OVERLAPPEDWINDOW, x, y, w, h,
                              nullptr, nullptr, inst, nullptr);
    if (!CreateDevice(hwnd))
    {
        CleanupDevice();
        MessageBoxW(nullptr, L"Couldn't create a Direct3D 11 device.", L"BFI Presenter Manager", MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    SetupStyle(g_dpiScale);
    LoadFonts(g_dpiScale);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    // initial state
    g_app.folders = LoadFolders();
    std::wstring srcDir = LoadSetting(L"Source", L"");
    if (!srcDir.empty()) g_app.src = SourceFromFolder(srcDir);
    if (!g_app.src.ok()) g_app.src = CachedRelease();
    g_app.vk = DetectVulkanLayer();
    StartScan();

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
        if (!g_app.busy && !ImGui::IsAnyItemActive()) MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
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
