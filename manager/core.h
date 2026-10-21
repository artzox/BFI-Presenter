// BFI Presenter Manager -- game discovery, detection and install actions (no UI).
// Copyright (C) 2026 artzox. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <vector>
#include <functional>

#define MANAGER_VERSION "1.3.6"
#define MANAGER_VERSION_W L"1.3.6"

namespace bfi {

// UTF-8 <-> UTF-16
std::string  U8(const std::wstring &w);
std::wstring W(const std::string &s);
std::wstring Lower(std::wstring s);

enum Api : unsigned { ApiD3D9 = 1, ApiD3D10 = 2, ApiD3D11 = 4, ApiD3D12 = 8, ApiGL = 16, ApiVulkan = 32 };
std::string ApiText(unsigned apis);

// What BFI Presenter files a game needs.
enum class Route { Unknown, Addon64, Addon32, Vulkan, Shader, Unsupported };
inline bool IsAddonRoute(Route r) { return r == Route::Addon64 || r == Route::Addon32; }
inline bool UsesReShadeRoute(Route r) { return IsAddonRoute(r) || r == Route::Shader; }   // needs ReShade next to the game
extern const wchar_t *const kShaderFile;           // BFIPresenter_Shader.fx

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
    std::string  vkMissing;      // Vulkan layer files missing from the folder (then vkDir is empty)
    std::wstring shaderFile;     // BFIPresenter_Shader.fx (the shader version), if the release has it
    std::string  shaderVersion;
    bool ok() const { return !dxDir.empty() || !vkDir.empty(); }
};

struct Game {
    std::string  name, store;    // store: Steam, Epic, GOG, Xbox, Battle.net, Folder
    std::string  appid;          // Steam app id, if any
    bool         vrOnly = false; // found while scanning: plays only in a VR headset
    bool         online = false; // Steam lists it as multiplayer / online (PvP, co-op, MMO)
    std::string  antiCheat;      // anti-cheat found in the game's folder ("Easy Anti-Cheat", "BattlEye", ...)
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
    std::wstring shaderPath;     // installed BFIPresenter_Shader.fx, if any
    std::string  shaderVersion;
    bool         shaderOn = false;          // enabled in the active ReShade preset
    std::wstring shaderDir, presetPath;     // where ReShade looks for effects / its active preset
    bool         shaderRecommended = false; // the tested-games list recommends the shader for this game
    bool         reshadeVkIni = false;      // ReShade.ini next to the exe: ReShade's Vulkan layer is on for this game
    bool         ini = false;    // BFIPresenter.ini next to the add-on or the exe
    std::wstring iniPath;
    Route        route = Route::Unknown;
    std::vector<std::string> notes;   // shown in the detail pane
    std::string  tested;         // matching line from the tested-games list
    // NVIDIA profile (filled by the app from nvdrs)
    bool         nvKnown = false;
    std::wstring nvProfile;
    long long    nvVrr = -1, nvVsync = -1;
    bool         nvVrrOwn = false, nvVsyncOwn = false;
    long long    nvGl = -1;      // Vulkan/OpenGL present method
    bool         nvGlOwn = false;
    bool         hidden = false;
};

struct VulkanLayerState {
    bool installed = false;      // registered (for this user or all users) and the files exist
    bool user = false;           // registered for this Windows user (HKCU)
    bool machine = false;        // registered for all users (HKLM): also loads in games run as administrator
    std::string version;
    std::wstring dir;
    bool reshadeLayer = false;   // ReShade's own Vulkan layer is registered
    bool reshadeAfter = false;   // ReShade is listed after BFI Presenter (wrong order: its effects run on every refresh)
    std::string reshadeVersion;
    std::string elevatedWhy;     // why games here likely run as administrator ("" = no sign of it)
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
struct ScanOptions {
    bool steam = true, epic = true, gog = true, xbox = true, bnet = true;
    bool skipVr = true;          // leave out VR-only games (BFI needs a monitor or TV)
    std::vector<std::wstring> folders;
};
ScanOptions LoadScanOptions();
void SaveScanOptions(const ScanOptions &o);
struct LibraryInfo {
    bool steam = false, epic = false, gog = false, xbox = false, bnet = false;
    std::string steamText, epicText, gogText, xboxText, bnetText;
    std::vector<std::string> steamLibs;          // every Steam library folder (the first is Steam's own)
};
// Online or anti-cheat: ReShade with add-ons in such a game can count as cheating (a ban is possible).
inline bool OnlineRisk(const Game &g) { return g.online || !g.antiCheat.empty(); }
LibraryInfo FindLibraries();                    // which stores are installed (quick)
std::vector<Game> FindGames(const ScanOptions &opt,
                            const std::function<void(const std::string &)> &progress = nullptr,
                            int *vrSkipped = nullptr);    // how many VR-only games were left out
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
std::string ShaderVersionOf(const std::wstring &file);

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
// All-users registration (HKLM). Needs administrator rights: the Manager runs itself elevated
// with --vk-machine add|remove <dir> for it.
std::string RegisterVulkanLayerMachine(bool add, const std::wstring &dir);
bool IsElevated();
bool ExeRunsAsAdmin(const std::wstring &exe);    // "Run this program as an administrator" set for it
std::wstring BackupDir();
std::wstring LogPath();

// ----- GitHub releases
struct RemoteRelease { std::string tag, dxUrl, vkUrl, notesUrl, mgrUrl, mgrVersion; bool ok = false; std::string error; };
RemoteRelease FetchLatestRelease();
RemoteRelease ParseRelease(const std::string &json);           // blocking (call from a worker thread)
std::string DownloadRelease(const RemoteRelease &r, Source &out,
                            const std::function<void(const std::string &)> &progress);
Source CachedRelease();                        // newest release already downloaded, if any
// Downloads the Manager zip of r and puts its exe in place of the running one (which is renamed to .old).
// "" = done: start the new exe and quit.
std::string UpdateManagerExe(const RemoteRelease &r, const std::function<void(const std::string &)> &progress);
void CleanupOldManagerExe();                   // removes the .old exe left by an update

// ----- ReShade (the add-on build from reshade.me)
std::string FetchReShadeVersions(std::vector<std::string> &out);     // newest first, from GitHub tags
std::vector<std::string> CachedReShadeVersions();                    // already downloaded
std::string EnsureReShade(const std::string &version, const std::function<void(const std::string &)> &progress);
// Installs (no ReShade yet) or changes the version of (ReShade present) ReShade for the game. "" = done;
// *note gets a hint that isn't an error (OptiScaler).
std::string InstallReShade(Game &g, const std::string &version, std::string *note = nullptr);
// Vulkan games: turns ReShade's own (system-wide) Vulkan layer on or off for the game, the way ReShade's setup
// does: ReShade.ini next to the exe (what the layer looks for) and the exe in ReShadeApps.ini (the setup's list).
std::string SetReShadeVulkan(Game &g, bool on, std::string *note = nullptr);

// Open a folder or file in Explorer.
void OpenInExplorer(const std::wstring &path, bool select);

} // namespace bfi
