// BFI Presenter Manager -- game discovery, detection and install actions (no UI).
// Copyright (C) 2026 artzox. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <vector>
#include <functional>

namespace bfi {

// UTF-8 <-> UTF-16
std::string  U8(const std::wstring &w);
std::wstring W(const std::string &s);
std::wstring Lower(std::wstring s);

enum Api : unsigned { ApiD3D9 = 1, ApiD3D10 = 2, ApiD3D11 = 4, ApiD3D12 = 8, ApiGL = 16, ApiVulkan = 32 };
std::string ApiText(unsigned apis);

// What BFI Presenter files a game needs.
enum class Route { Unknown, Addon64, Addon32, Vulkan, Unsupported };
const char *RouteText(Route r);

struct DllInfo {
    std::wstring path;
    std::string  product, description, version;
};

// One BFI Presenter release on disk (downloaded or a local folder).
struct Source {
    std::wstring dxDir;          // folder with BFIPresenter.addon64 / .addon32 / example ini
    std::wstring vkDir;          // folder with BFIPresenterVK.dll / json / ready ini
    std::string  dxVersion, vkVersion;
    std::string  label;          // "GitHub v1.0.2" or the folder
    bool ok() const { return !dxDir.empty() || !vkDir.empty(); }
};

struct Game {
    std::string  name, store;    // store: Steam, Epic, GOG, Xbox, Folder
    std::string  appid;          // Steam app id, if any
    std::wstring root;           // install folder
    std::vector<std::wstring> exes;   // candidates, best first
    std::wstring exe;            // chosen (best or the user's override)
    bool         exeOverride = false;
    int          bits = 0;       // 32, 64, 0 unknown
    unsigned     apis = 0;       // what the route is decided on: PCGamingWiki if known, else the executable
    unsigned     exeApis = 0;    // read from the executable and the DLLs next to it
    bool         pcgwKnown = false;
    unsigned     pcgwApis = 0;   // from PCGamingWiki
    std::string  pcgwText;       // e.g. "Direct3D 12, 11; Vulkan"
    // Detected next to the exe
    bool         reshade = false;
    DllInfo      reshadeDll;     // the ReShade DLL (any name: dxgi.dll, ReShade64.dll, ...)
    bool         optiscaler = false, dxvk = false, dgvoodoo = false, specialK = false;
    std::wstring addonPath;      // installed BFIPresenter.addon64/32, if any
    std::string  addonVersion;
    bool         addonWrongBits = false;
    bool         ini = false;    // BFIPresenter.ini next to the add-on or the exe
    std::wstring iniPath;
    Route        route = Route::Unknown;
    std::vector<std::string> notes;   // shown in the detail pane
    std::string  tested;         // matching line from the tested-games list
    bool         hidden = false;
};

struct VulkanLayerState {
    bool installed = false;      // registered for this user and the files exist
    std::string version;
    std::wstring dir;
    bool reshadeLayer = false;   // ReShade's own Vulkan layer is registered
    std::string reshadeVersion;
};

// ----- Settings (%LOCALAPPDATA%\BFIPresenter\manager.ini)
std::wstring DataDir();                       // %LOCALAPPDATA%\BFIPresenter
std::vector<std::wstring> LoadFolders();
void SaveFolders(const std::vector<std::wstring> &f);
std::wstring LoadSetting(const wchar_t *key, const wchar_t *def = L"");
void SaveSetting(const wchar_t *key, const std::wstring &v);
std::wstring RouteKey(const std::wstring &root);   // setting key for the per-game route choice
std::wstring LoadExeOverride(const std::wstring &root);
void SaveExeOverride(const std::wstring &root, const std::wstring &exe);

// ----- Discovery and detection
struct ScanOptions { bool steam = true, epic = true, gog = true, xbox = true; std::vector<std::wstring> folders; };
ScanOptions LoadScanOptions();
void SaveScanOptions(const ScanOptions &o);
struct LibraryInfo { bool steam = false, epic = false, gog = false, xbox = false; std::string steamText, epicText, gogText, xboxText; };
LibraryInfo FindLibraries();                    // which stores are installed (quick)
std::vector<Game> FindGames(const ScanOptions &opt,
                            const std::function<void(const std::string &)> &progress = nullptr);
// The game list is kept between runs: saved after each scan, loaded (and re-detected) at start.
void SaveGameList(const std::vector<Game> &games);
bool LoadGameList(std::vector<Game> &games);    // false if there is no saved list
void DetectAll(std::vector<Game> &games, const std::function<void(const std::string &)> &progress = nullptr);
// PCGamingWiki: graphics APIs per game (Steam by app id, others by page name). Blocking; caches results.
std::string FetchPcgw(std::vector<Game> &games, const std::function<void(const std::string &)> &progress = nullptr);
void Detect(Game &g);                          // (re)detects everything for g.exe
VulkanLayerState DetectVulkanLayer();
DllInfo ReadDllInfo(const std::wstring &path);
std::string BfiVersionOf(const std::wstring &file, bool vulkan);

// ----- Sources
Source SourceFromFolder(const std::wstring &dir);    // searches dir (2 levels) for the release files
int CompareVersions(const std::string &a, const std::string &b);   // <0, 0, >0 ("" is lowest)

// ----- Actions. Each returns "" on success or an error message; every replaced file is backed up and logged.
std::string InstallOrUpdate(Game &g, const Source &src);
std::string Remove(Game &g, bool alsoIni);
std::string Revert(Game &g);                  // puts back the files the last action on this game replaced
bool CanRevert(const Game &g);
std::string InstallVulkanLayer(const Source &src);
std::string UninstallVulkanLayer();
std::wstring BackupDir();
std::wstring LogPath();

// ----- GitHub releases
struct RemoteRelease { std::string tag, dxUrl, vkUrl, notesUrl; bool ok = false; std::string error; };
RemoteRelease FetchLatestRelease();
RemoteRelease ParseRelease(const std::string &json);           // blocking (call from a worker thread)
std::string DownloadRelease(const RemoteRelease &r, Source &out,
                            const std::function<void(const std::string &)> &progress);
Source CachedRelease();                        // newest release already downloaded, if any

// Open a folder or file in Explorer.
void OpenInExplorer(const std::wstring &path, bool select);

} // namespace bfi
