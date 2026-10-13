// BFI Presenter Manager -- game discovery, detection and install actions (no UI).
// Copyright (C) 2026 artzox. SPDX-License-Identifier: GPL-3.0-or-later
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <winhttp.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <mutex>
#include <sstream>
#include <string_view>
#include <functional>
#include <memory>
#include "core.h"

namespace fs = std::filesystem;

namespace bfi {

bool ZipExtract(const std::string &data, const std::string &name, std::string &out);   // unzip.cpp

// ------------------------------------------------------------------ strings

std::string U8(const std::wstring &w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring W(const std::string &s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::wstring Lower(std::wstring s)
{
    if (!s.empty()) CharLowerBuffW(s.data(), (DWORD)s.size());
    return s;
}

static std::string LowerA(std::string s)
{
    for (auto &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

static std::string Trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::string ApiText(unsigned a)
{
    std::string s;
    auto add = [&](unsigned f, const char *t) { if (a & f) { if (!s.empty()) s += " / "; s += t; } };
    add(ApiD3D12, "D3D12"); add(ApiD3D11, "D3D11"); add(ApiD3D10, "D3D10"); add(ApiD3D9, "D3D9");
    add(ApiGL, "OpenGL"); add(ApiVulkan, "Vulkan");
    return s.empty() ? "?" : s;
}

const char *RouteText(Route r)
{
    switch (r)
    {
    case Route::Addon64:     return "DX add-on (64-bit)";
    case Route::Addon32:     return "DX add-on (32-bit)";
    case Route::Vulkan:      return "Vulkan layer";
    case Route::Unsupported: return "Not supported";
    default:                 return "Unknown";
    }
}

// ------------------------------------------------------------------ files

static bool Exists(const std::wstring &p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool DirExists(const std::wstring &p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring DirOf(const std::wstring &p)
{
    size_t k = p.find_last_of(L"\\/");
    return k == std::wstring::npos ? std::wstring() : p.substr(0, k);
}

static std::wstring NameOf(const std::wstring &p)
{
    size_t k = p.find_last_of(L"\\/");
    return k == std::wstring::npos ? p : p.substr(k + 1);
}

static std::wstring Join(const std::wstring &a, const std::wstring &b)
{
    if (a.empty()) return b;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

static std::vector<char> ReadBytes(const std::wstring &path, size_t maxBytes)
{
    std::vector<char> v;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return v;
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    size_t n = (size_t)std::min<long long>(sz.QuadPart, (long long)maxBytes);
    v.resize(n);
    size_t got = 0;
    while (got < n)
    {
        DWORD r = 0, want = (DWORD)std::min<size_t>(n - got, 1u << 24);
        if (!ReadFile(h, v.data() + got, want, &r, nullptr) || r == 0) break;
        got += r;
    }
    v.resize(got);
    CloseHandle(h);
    return v;
}

static long long FileSize(const std::wstring &p)
{
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return -1;
    return ((long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
}

static unsigned long long FileTime(const std::wstring &p)
{
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return 0;
    return ((unsigned long long)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
}

static bool SameContent(const std::wstring &a, const std::wstring &b)
{
    if (FileSize(a) != FileSize(b)) return false;
    return ReadBytes(a, 1u << 30) == ReadBytes(b, 1u << 30);
}

static std::string ReadText(const std::wstring &p)
{
    auto v = ReadBytes(p, 256u << 20);
    return std::string(v.begin(), v.end());
}

// Read-only view of a whole file; pages are read only when touched.
struct Mapped {
    HANDLE file = INVALID_HANDLE_VALUE, map = nullptr;
    const char *data = nullptr;
    size_t size = 0;
    explicit Mapped(const std::wstring &p)
    {
        file = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER sz{};
        GetFileSizeEx(file, &sz);
        if (sz.QuadPart == 0) return;
        map = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (map) data = (const char *)MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0);
        if (data) size = (size_t)sz.QuadPart;
    }
    ~Mapped()
    {
        if (data) UnmapViewOfFile(data);
        if (map) CloseHandle(map);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
    std::string_view view() const { return std::string_view(data ? data : "", size); }
};

// ------------------------------------------------------------------ settings

std::wstring DataDir()
{
    wchar_t buf[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring d = (n && n < MAX_PATH) ? std::wstring(buf) : std::wstring(L".");
    d = Join(d, L"BFIPresenter");
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

static std::wstring SettingsPath() { return Join(DataDir(), L"manager.ini"); }

std::wstring LoadSetting(const wchar_t *key, const wchar_t *def)
{
    wchar_t buf[4096]{};
    GetPrivateProfileStringW(L"Manager", key, def, buf, 4096, SettingsPath().c_str());
    return buf;
}

void SaveSetting(const wchar_t *key, const std::wstring &v)
{
    WritePrivateProfileStringW(L"Manager", key, v.c_str(), SettingsPath().c_str());
}

std::vector<std::wstring> LoadFolders()
{
    std::vector<std::wstring> f;
    int n = GetPrivateProfileIntW(L"Folders", L"Count", 0, SettingsPath().c_str());
    for (int i = 0; i < n; i++)
    {
        wchar_t buf[4096]{};
        GetPrivateProfileStringW(L"Folders", (L"Folder" + std::to_wstring(i + 1)).c_str(), L"", buf, 4096, SettingsPath().c_str());
        if (buf[0]) f.push_back(buf);
    }
    return f;
}

void SaveFolders(const std::vector<std::wstring> &f)
{
    WritePrivateProfileSectionW(L"Folders", L"\0", SettingsPath().c_str());
    WritePrivateProfileStringW(L"Folders", L"Count", std::to_wstring(f.size()).c_str(), SettingsPath().c_str());
    for (size_t i = 0; i < f.size(); i++)
        WritePrivateProfileStringW(L"Folders", (L"Folder" + std::to_wstring(i + 1)).c_str(), f[i].c_str(), SettingsPath().c_str());
}

// ini keys can't hold '=' or start with '[' or ';'; paths never do, but keep the key short and safe anyway.
static std::wstring PathKey(const std::wstring &root)
{
    unsigned long long h = 1469598103934665603ull;
    for (wchar_t c : Lower(root)) { h ^= (unsigned)c; h *= 1099511628211ull; }
    wchar_t b[32];
    swprintf(b, 32, L"%016llx", h);
    return b;
}

std::wstring RouteKey(const std::wstring &root) { return L"Route_" + PathKey(root); }

std::wstring LoadExeOverride(const std::wstring &root)
{
    wchar_t buf[4096]{};
    GetPrivateProfileStringW(L"Exe", PathKey(root).c_str(), L"", buf, 4096, SettingsPath().c_str());
    return buf;
}

void SaveExeOverride(const std::wstring &root, const std::wstring &exe)
{
    WritePrivateProfileStringW(L"Exe", PathKey(root).c_str(), exe.empty() ? nullptr : exe.c_str(), SettingsPath().c_str());
}

// ------------------------------------------------------------------ registry

static std::wstring RegString(HKEY root, const wchar_t *sub, const wchar_t *val, REGSAM extra = 0)
{
    wchar_t buf[2048]{};
    DWORD sz = sizeof(buf);
    if (RegGetValueW(root, sub, val, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | extra, nullptr, buf, &sz) == ERROR_SUCCESS)
        return buf;
    return {};
}

static std::vector<std::wstring> RegSubkeys(HKEY root, const wchar_t *sub)
{
    std::vector<std::wstring> out;
    HKEY k;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) return out;
    wchar_t name[512];
    for (DWORD i = 0;; i++)
    {
        DWORD n = 512;
        if (RegEnumKeyExW(k, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        out.push_back(name);
    }
    RegCloseKey(k);
    return out;
}

static std::vector<std::wstring> RegValueNames(HKEY root, const wchar_t *sub)
{
    std::vector<std::wstring> out;
    HKEY k;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) return out;
    std::vector<wchar_t> name(32768);
    for (DWORD i = 0;; i++)
    {
        DWORD n = (DWORD)name.size();
        if (RegEnumValueW(k, i, name.data(), &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        out.emplace_back(name.data(), n);
    }
    RegCloseKey(k);
    return out;
}

// ------------------------------------------------------------------ version info

// Reads a VS_VERSIONINFO string straight from the file: some loaders (and Wine) answer GetFileVersionInfo for
// a file named like a system DLL (dxgi.dll, d3d9.dll) with the system DLL's details instead of the file's own.
static std::string RawVersionString(std::string_view f, const char *key)
{
    std::string k;
    for (const char *p = key; *p; p++) { k += *p; k += '\0'; }
    k += std::string(2, '\0');
    size_t pos = f.size();
    std::string_view fv = f;
    while (pos > 0)
    {
        size_t at = fv.rfind(std::string_view(k.data(), k.size()), pos - 1);
        if (at == std::string_view::npos || at < 6) break;
        pos = at;
        auto w = [&](size_t o) { return (unsigned)(unsigned char)f[o] | ((unsigned)(unsigned char)f[o + 1] << 8); };
        unsigned len = w(at - 6), vlen = w(at - 4), type = w(at - 2);
        if (type != 1 || vlen == 0 || vlen > 256 || len < k.size()) continue;
        size_t v = at + k.size();
        while (v + 1 < f.size() && f[v] == 0 && f[v + 1] == 0 && v < at + k.size() + 4) v += 2;
        std::wstring out;
        for (unsigned i = 0; i < vlen && v + 1 < f.size(); i++, v += 2)
        {
            wchar_t c = (wchar_t)w(v);
            if (!c) break;
            out += c;
        }
        return Trim(U8(out));
    }
    return {};
}

DllInfo ReadDllInfo(const std::wstring &path)
{
    DllInfo d;
    d.path = path;
    {
        Mapped m(path);
        std::string_view f = m.view();
        d.product = RawVersionString(f, "ProductName");
        d.description = RawVersionString(f, "FileDescription");
        d.version = RawVersionString(f, "ProductVersion");
        if (d.version.empty()) d.version = RawVersionString(f, "FileVersion");
        if (!d.product.empty() || !d.description.empty()) return d;
    }
    DWORD dummy = 0, sz = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (!sz) return d;
    std::vector<char> buf(sz);
    if (!GetFileVersionInfoW(path.c_str(), 0, sz, buf.data())) return d;
    struct LangCp { WORD lang, cp; } *tr = nullptr;
    UINT trLen = 0;
    std::vector<std::wstring> prefixes;
    if (VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", (void **)&tr, &trLen) && tr)
        for (UINT i = 0; i < trLen / sizeof(LangCp); i++)
        {
            wchar_t p[64];
            swprintf(p, 64, L"\\StringFileInfo\\%04x%04x\\", tr[i].lang, tr[i].cp);
            prefixes.push_back(p);
        }
    prefixes.push_back(L"\\StringFileInfo\\040904b0\\");
    prefixes.push_back(L"\\StringFileInfo\\040904e4\\");
    auto q = [&](const wchar_t *field) -> std::string {
        for (auto &p : prefixes)
        {
            wchar_t *v = nullptr;
            UINT n = 0;
            if (VerQueryValueW(buf.data(), (p + field).c_str(), (void **)&v, &n) && v && n > 1) return Trim(U8(std::wstring(v)));
        }
        return {};
    };
    d.product = q(L"ProductName");
    d.description = q(L"FileDescription");
    d.version = q(L"ProductVersion");
    if (d.version.empty()) d.version = q(L"FileVersion");
    if (d.version.empty())
    {
        VS_FIXEDFILEINFO *fi = nullptr;
        UINT n = 0;
        if (VerQueryValueW(buf.data(), L"\\", (void **)&fi, &n) && fi)
        {
            char v[64];
            snprintf(v, 64, "%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS));
            d.version = v;
        }
    }
    return d;
}

static bool InfoHas(const DllInfo &d, const char *what)
{
    std::string w = LowerA(what);
    return LowerA(d.product).find(w) != std::string::npos || LowerA(d.description).find(w) != std::string::npos;
}

// "6.4.1.1924" -> "6.4.1"; ReShade puts the build after the third part
static std::string ShortVersion(const std::string &v)
{
    std::string out;
    int dots = 0;
    for (char c : v)
    {
        if (c == '.' && ++dots == 3) break;
        if (c == ',' ) { if (++dots == 3) break; out += '.'; continue; }
        if (c == ' ') continue;
        out += c;
    }
    return out;
}

int CompareVersions(const std::string &a, const std::string &b)
{
    if (a == b) return 0;
    auto parts = [](const std::string &s) {
        std::vector<int> v;
        int cur = -1;
        for (char c : s)
        {
            if (c >= '0' && c <= '9') cur = (cur < 0 ? 0 : cur * 10) + (c - '0');
            else if (c == '.') { v.push_back(cur < 0 ? 0 : cur); cur = -1; }
            else break;
        }
        if (cur >= 0) v.push_back(cur);
        return v;
    };
    auto pa = parts(a), pb = parts(b);
    if (pa.empty() || pb.empty()) return pa.empty() ? (pb.empty() ? 0 : -1) : 1;
    for (size_t i = 0; i < std::max(pa.size(), pb.size()); i++)
    {
        int x = i < pa.size() ? pa[i] : 0, y = i < pb.size() ? pb[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

// The DX add-on carries "BFI Presenter v1.0.2"; the Vulkan layer "BFI Presenter (Vulkan layer) v1.0.2".
std::string BfiVersionOf(const std::wstring &file, bool vulkan)
{
    auto v = ReadBytes(file, 64u << 20);
    if (v.empty()) return {};
    const char *key = vulkan ? "BFI Presenter (Vulkan layer) v" : "BFI Presenter v";
    size_t kl = strlen(key);
    auto it = std::search(v.begin(), v.end(), key, key + kl);
    if (it == v.end()) return vulkan ? "older" : "older than 0.72";
    std::string out;
    for (auto p = it + kl; p != v.end() && ((*p >= '0' && *p <= '9') || *p == '.'); ++p) out += *p;
    while (!out.empty() && out.back() == '.') out.pop_back();
    return out;
}

// ------------------------------------------------------------------ PE analysis

struct PeInfo { int bits = 0; std::set<std::string> imports; bool ok = false; bool d3d9PerfOnly = false; };
// Many Direct3D 11/12 games import d3d9.dll only for the D3DPERF_* profiler markers: that isn't Direct3D 9.

static PeInfo ReadPe(std::string_view f)
{
    PeInfo pi;
    auto rd16 = [&](size_t o) -> unsigned { return o + 2 <= f.size() ? (unsigned)(unsigned char)f[o] | ((unsigned)(unsigned char)f[o + 1] << 8) : 0; };
    auto rd32 = [&](size_t o) -> unsigned { return o + 4 <= f.size() ? rd16(o) | (rd16(o + 2) << 16) : 0; };
    if (f.size() < 0x40 || f[0] != 'M' || f[1] != 'Z') return pi;
    size_t pe = rd32(0x3c);
    if (pe + 24 > f.size() || memcmp(f.data() + pe, "PE\0\0", 4) != 0) return pi;
    unsigned machine = rd16(pe + 4), nsec = rd16(pe + 6), optSize = rd16(pe + 20);
    pi.bits = machine == 0x14c ? 32 : (machine == 0x8664 || machine == 0xaa64) ? 64 : 0;
    size_t opt = pe + 24;
    unsigned magic = rd16(opt);
    size_t dd = opt + (magic == 0x20b ? 112 : 96);
    unsigned nDirs = rd32(opt + (magic == 0x20b ? 108 : 92));
    size_t sec = opt + optSize;
    auto rva2off = [&](unsigned rva) -> size_t {
        for (unsigned i = 0; i < nsec; i++)
        {
            size_t s = sec + i * 40;
            unsigned va = rd32(s + 12), vsz = rd32(s + 8), raw = rd32(s + 20), rsz = rd32(s + 16);
            unsigned span = std::max(vsz, rsz);
            if (rva >= va && rva < va + span) return (size_t)raw + (rva - va);
        }
        return (size_t)-1;
    };
    auto readName = [&](unsigned rva) -> std::string {
        size_t o = rva2off(rva);
        std::string s;
        while (o < f.size() && f[o] && s.size() < 260) s += f[o++];
        return LowerA(s);
    };
    // d3d9.dll: does it import anything besides D3DPERF_* markers?
    auto onlyPerf = [&](unsigned intRva) -> bool {
        size_t o = rva2off(intRva);
        unsigned step = pi.bits == 64 ? 8 : 4;
        bool any = false;
        for (int i = 0; o != (size_t)-1 && o + step <= f.size() && i < 4096; i++, o += step)
        {
            unsigned lo = rd32(o), hi = step == 8 ? rd32(o + 4) : 0;
            if (!lo && !hi) break;
            bool ordinal = step == 8 ? (hi & 0x80000000u) != 0 : (lo & 0x80000000u) != 0;
            if (ordinal) return false;
            std::string fn = readName(lo + 2);
            if (fn.rfind("d3dperf_", 0) != 0) return false;
            any = true;
        }
        return any;
    };
    auto add = [&](const std::string &dll, unsigned intRva) {
        if (dll == "d3d9.dll" && intRva && onlyPerf(intRva)) { pi.d3d9PerfOnly = true; return; }
        pi.imports.insert(dll);
    };
    // 1 = import table (20-byte descriptors, name table at +0, name at +12),
    // 13 = delay imports (32-byte descriptors, name at +4, name table at +16)
    if (nDirs > 1)
    {
        size_t o = rva2off(rd32(dd + 8));
        for (int i = 0; o != (size_t)-1 && o + 20 <= f.size() && i < 512; i++, o += 20)
        {
            unsigned name = rd32(o + 12);
            if (!name) break;
            unsigned intRva = rd32(o);
            if (!intRva) intRva = rd32(o + 16);
            add(readName(name), intRva);
        }
    }
    if (nDirs > 13)
    {
        size_t o = rva2off(rd32(dd + 13 * 8));
        for (int i = 0; o != (size_t)-1 && o + 32 <= f.size() && i < 512; i++, o += 32)
        {
            unsigned name = rd32(o + 4);
            if (!name) break;
            add(readName(name), rd32(o + 16));
        }
    }
    pi.ok = true;
    return pi;
}

static unsigned ApisFromImports(const std::set<std::string> &im)
{
    unsigned a = 0;
    if (im.count("d3d12.dll")) a |= ApiD3D12;
    if (im.count("d3d11.dll")) a |= ApiD3D11;
    if (im.count("d3d10.dll") || im.count("d3d10_1.dll")) a |= ApiD3D10;
    if (im.count("d3d9.dll")) a |= ApiD3D9;
    if (im.count("opengl32.dll")) a |= ApiGL;
    if (im.count("vulkan-1.dll")) a |= ApiVulkan;
    return a;
}

// Engines load their API at run time; look for the DLL names in the binary (ASCII and UTF-16).
static bool FindNoCase(std::string_view hay, const std::string &needle)
{
    auto eq = [](char a, char b) { return tolower((unsigned char)a) == tolower((unsigned char)b); };
    struct H { size_t operator()(char c) const { return (size_t)tolower((unsigned char)c); } };
    std::boyer_moore_horspool_searcher<std::string::const_iterator, H, decltype(eq)> srch(needle.begin(), needle.end(), H(), eq);
    return std::search(hay.begin(), hay.end(), srch) != hay.end();
}

static unsigned ApisFromStrings(std::string_view f)
{
    static const struct { const char *n; unsigned a; } k[] = {
        { "d3d12.dll", ApiD3D12 }, { "d3d11.dll", ApiD3D11 }, { "d3d9.dll", ApiD3D9 },
        { "opengl32.dll", ApiGL }, { "vulkan-1.dll", ApiVulkan }, { "d3d10.dll", ApiD3D10 } };
    unsigned a = 0;
    for (auto &e : k)
    {
        std::string n = e.n, wn;
        for (char c : n) { wn += c; wn += '\0'; }
        if (FindNoCase(f, n) || FindNoCase(f, wn)) a |= e.a;
    }
    return a;
}

// Cache: scanning a 100 MB Unreal executable takes a moment; keep the result keyed by size and date.
struct ExeFacts { int bits = 0; unsigned apis = 0; };
static std::map<std::wstring, std::pair<std::wstring, ExeFacts>> g_cache;
static bool g_cacheLoaded = false;
static std::mutex g_cacheLock;
static std::wstring CachePath() { return Join(DataDir(), L"exe_cache.txt"); }

static void LoadCache()
{
    if (g_cacheLoaded) return;
    g_cacheLoaded = true;
    std::ifstream in{fs::path(CachePath())};
    std::string line8;
    while (std::getline(in, line8))
    {
        std::wstring line = W(line8);
        size_t a = line.find(L'\t'), b = line.find(L'\t', a + 1), c = line.find(L'\t', b + 1);
        if (a == std::wstring::npos || b == std::wstring::npos || c == std::wstring::npos) continue;
        ExeFacts f;
        f.bits = _wtoi(line.substr(b + 1, c - b - 1).c_str());
        f.apis = (unsigned)_wtoi(line.substr(c + 1).c_str());
        g_cache[line.substr(0, a)] = { line.substr(a + 1, b - a - 1), f };
    }
}

static void SaveCache()
{
    std::lock_guard<std::mutex> l(g_cacheLock);
    std::ofstream out{fs::path(CachePath()), std::ios::trunc | std::ios::binary};
    for (auto &e : g_cache)
        out << U8(e.first) << '\t' << U8(e.second.first) << '\t' << e.second.second.bits << '\t' << e.second.second.apis << '\n';
}

static ExeFacts AnalyzeExe(const std::wstring &exe)
{
    std::wstring key = Lower(exe);
    std::wstring stamp = std::to_wstring(FileSize(exe)) + L"_" + std::to_wstring(FileTime(exe));
    {
        std::lock_guard<std::mutex> l(g_cacheLock);
        LoadCache();
        auto it = g_cache.find(key);
        if (it != g_cache.end() && it->second.first == stamp) return it->second.second;
    }

    ExeFacts r;
    Mapped f(exe);
    PeInfo pi = ReadPe(f.view());
    r.bits = pi.bits;
    r.apis = ApisFromImports(pi.imports);
    const unsigned strong = ApiD3D9 | ApiD3D11 | ApiD3D12 | ApiGL | ApiVulkan;
    std::wstring dir = DirOf(exe);
    if (Exists(Join(dir, L"UnityPlayer.dll")))
    {
        // Unity links opengl32 and loads Direct3D at run time; Direct3D 11 is its Windows default
        // (PCGamingWiki, when it knows the game, says otherwise).
        r.apis = ApiD3D11;
    }
    else if (!(r.apis & strong))
    {
        // Engines that keep the renderer in a DLL (Northlight: Control, Alan Wake 2)
        static const wchar_t *skip[] = { L"dxgi", L"d3d9", L"d3d10", L"d3d11", L"d3d12", L"opengl32", L"vulkan-1", L"dinput8",
            L"version", L"winmm", L"dbghelp", L"reshade", L"optiscaler", L"d3dcompiler", L"dxcompiler", L"dxil", L"nvngx",
            L"sl.", L"amd_", L"ffx", L"libxess", L"xess", L"nvapi", L"gfsdk", L"steam_api", L"eossdk", L"galaxy", L"discord",
            L"bink", L"fmod", L"wwise", L"physx", L"openal", L"dstorage", L"nvidia", L"igxess", L"renderdoc" };
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(Join(dir, L"*.dll").c_str(), &fd);
        int n = 0;
        if (h != INVALID_HANDLE_VALUE)
        {
            do
            {
                std::wstring l = Lower(fd.cFileName);
                bool bad = false;
                for (auto k : skip) if (l.rfind(k, 0) == 0) bad = true;
                if (bad) continue;
                Mapped m(Join(dir, fd.cFileName));
                r.apis |= ApisFromImports(ReadPe(m.view()).imports);
            } while (++n < 300 && FindNextFileW(h, &fd));
            FindClose(h);
        }
        if (!(r.apis & strong))
        {
            unsigned str = ApisFromStrings(f.view());
            if (pi.d3d9PerfOnly) str &= ~ApiD3D9;   // the import table's own "d3d9.dll"
            r.apis |= str;
        }
    }
    std::lock_guard<std::mutex> l(g_cacheLock);
    g_cache[key] = { stamp, r };
    return r;
}

// ------------------------------------------------------------------ exe choice

static bool SkipExe(const std::wstring &nameLower)
{
    static const wchar_t *bad[] = {
        L"unins", L"crash", L"report", L"setup", L"redist", L"vcredist", L"dxsetup", L"directx", L"dotnet",
        L"easyanticheat", L"eac_", L"battleye", L"beservice", L"unitycrashhandler", L"cefprocess", L"cefsharp",
        L"notification", L"overlay", L"updater", L"update.exe", L"installer", L"install.exe", L"uploader",
        L"helper", L"touchup", L"activation", L"physx", L"oalinst", L"vc_redist", L"ue4prereq", L"ueprereq",
        L"dowser", L"quicksfv", L"7z", L"python", L"java", L"node.exe", L"steamerrorreporter", L"ffmpeg",
        L"epiconlineservices", L"eossdk", L"galaxy", L"benchmark", L"server", L"editor", L"modmanager" };
    for (auto b : bad)
        if (nameLower.find(b) != std::wstring::npos) return true;
    return false;
}

static const wchar_t *kReShadeNames[] = {
    L"dxgi.dll", L"d3d9.dll", L"d3d10.dll", L"d3d11.dll", L"d3d12.dll", L"opengl32.dll", L"dinput8.dll",
    L"ReShade64.dll", L"ReShade32.dll", L"ReShade.dll", L"version.dll", L"winmm.dll", L"dbghelp.dll" };

static void CollectExes(const std::wstring &dir, int depth, std::vector<std::wstring> &out)
{
    if (depth > 5 || out.size() > 200) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(Join(dir, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::wstring> sub;
    do
    {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        std::wstring l = Lower(n);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            static const wchar_t *skipDirs[] = { L"_commonredist", L"redist", L"redistributables", L"directx", L"support",
                L"__installer", L"installers", L"_redist", L"easyanticheat", L"battleye", L"dotnet", L"vcredist",
                L"shadercache", L"saves", L"logs", L"screenshots", L"movies", L"content", L"paks", L"data", L"engine" };
            bool skip = false;
            for (auto s : skipDirs) if (l == s) skip = true;
            // Unreal: Engine\Binaries holds tools only; the game's own Binaries are under <Game>\Binaries
            if (!skip) sub.push_back(Join(dir, n));
        }
        else if (l.size() > 4 && l.compare(l.size() - 4, 4, L".exe") == 0 && !SkipExe(l))
            out.push_back(Join(dir, n));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    for (auto &s : sub) CollectExes(s, depth + 1, out);
}

static bool HasReShadeNextTo(const std::wstring &exe)
{
    std::wstring d = DirOf(exe);
    if (Exists(Join(d, L"ReShade.ini")) || Exists(Join(d, L"ReShade64.dll")) || Exists(Join(d, L"ReShade32.dll"))) return true;
    for (auto n : kReShadeNames)
        if (Exists(Join(d, n)) && InfoHas(ReadDllInfo(Join(d, n)), "ReShade")) return true;
    return false;
}

// Words of a game name for matching executables: lower case, ™ and punctuation dropped,
// Roman numerals as digits ("Hades II" -> hades, 2), filler words left out.
static std::vector<std::string> NameTokens(const std::string &name)
{
    static const char *filler[] = { "the", "of", "a", "an", "and", "edition", "remastered", "remaster", "definitive",
                                    "goty", "enhanced", "complete", "game", "year", "ultimate", "deluxe", "directors", "cut" };
    static const struct { const char *r, *d; } roman[] = { { "ii", "2" }, { "iii", "3" }, { "iv", "4" }, { "vi", "6" },
                                                           { "vii", "7" }, { "viii", "8" }, { "ix", "9" } };
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] {
        if (cur.empty()) return;
        for (auto &rn : roman) if (cur == rn.r) cur = rn.d;
        bool skip = false;
        for (auto f : filler) if (cur == f) skip = true;
        if (!skip) out.push_back(cur);
        cur.clear();
    };
    for (unsigned char c : name)
    {
        if (c < 128 && isalnum(c)) cur += (char)tolower(c);
        else flush();
    }
    flush();
    return out;
}

static int NameScore(const std::wstring &exe, const std::string &gameName, const std::wstring &root)
{
    std::string stem;
    for (wchar_t c : Lower(NameOf(exe)))
    {
        if (c == L'.') break;
        if (c < 128 && iswalnum(c)) stem += (char)c;
    }
    if (stem.empty()) return 0;
    int best = 0;
    for (const std::string &nm : { gameName, U8(NameOf(root)) })
    {
        auto tok = NameTokens(nm);
        std::string all;
        for (auto &t : tok) all += t;
        int sc = 0;
        if (all.size() >= 4 && (stem.find(all) != std::string::npos || (stem.size() >= 4 && all.find(stem) != std::string::npos)))
            sc = 250;
        else
        {
            for (auto &t : tok)
                if ((t.size() >= 4 || (t.size() >= 1 && isdigit((unsigned char)t[0]))) && stem.find(t) != std::string::npos)
                    sc += t.size() >= 4 ? 70 : 20;
            // initials: "Red Dead Redemption 2" -> rdr2
            std::string ini;
            for (auto &t : tok) ini += isdigit((unsigned char)t[0]) ? t : t.substr(0, 1);
            if (ini.size() >= 3 && stem.rfind(ini, 0) == 0) sc = std::max(sc, 200);
            sc = std::min(sc, 220);
        }
        best = std::max(best, sc);
    }
    return best;
}

static std::vector<std::wstring> RankExes(std::vector<std::wstring> exes, const std::string &gameName, const std::wstring &root)
{
    bool hasShipping = false;
    for (auto &e : exes) if (Lower(NameOf(e)).find(L"shipping") != std::wstring::npos) hasShipping = true;
    std::string gl = LowerA(gameName);
    std::vector<std::pair<double, std::wstring>> scored;
    for (auto &e : exes)
    {
        std::wstring n = Lower(NameOf(e));
        double mb = (double)FileSize(e) / (1 << 20);
        double s = std::min(mb, 300.0) / 3.0;                 // bigger is usually the game, but only a hint
        s += NameScore(e, gameName, root);                     // the game's name in the exe name counts most
        if (n.find(L"shipping") != std::wstring::npos) s += 200;
        if (n.find(L"launcher") != std::wstring::npos || n.find(L"bootstrap") != std::wstring::npos) s -= 150;
        if ((n.find(L"trial") != std::wstring::npos && gl.find("trial") == std::string::npos) ||
            (n.find(L"demo") != std::wstring::npos && gl.find("demo") == std::string::npos)) s -= 150;
        if (n.find(L"win32") != std::wstring::npos || n.find(L"x86") != std::wstring::npos) s -= 5;
        if (hasShipping && n.find(L"shipping") == std::wstring::npos) s -= 150;   // Unreal's small bootstrap exe
        if (Exists(Join(DirOf(e), L"BFIPresenter.addon64")) || Exists(Join(DirOf(e), L"BFIPresenter.addon32"))) s += 600;
        if (HasReShadeNextTo(e)) s += 400;
        scored.push_back({ s, e });
    }
    std::stable_sort(scored.begin(), scored.end(), [](auto &a, auto &b) { return a.first > b.first; });
    std::vector<std::wstring> out;
    for (auto &p : scored) out.push_back(p.second);
    return out;
}

// ------------------------------------------------------------------ tested games list

struct TestedEntry { const char *name, *status, *note; };
static const TestedEntry kTested[] = {
#include "tested_games.inc"
};

static std::string NormName(const std::string &s)
{
    std::string out;
    bool paren = false;
    for (unsigned char c : s)
    {
        if (c == '(') { paren = true; continue; }
        if (c == ')') { paren = false; continue; }
        if (paren) continue;
        if (isalnum(c)) out += (char)tolower(c);
    }
    return out;
}

static std::string TestedFor(const std::string &game)
{
    std::string g = NormName(game);
    if (g.empty()) return {};
    for (auto &t : kTested)
    {
        std::string n = NormName(t.name);
        if (n == g || (n.size() >= 6 && g.size() >= 6 && (g.rfind(n, 0) == 0 || n.rfind(g, 0) == 0) &&
                       std::max(n.size(), g.size()) - std::min(n.size(), g.size()) <= 12 &&
                       // "Wreckfest" must not match "Wreckfest 2"
                       !isdigit((unsigned char)(g.size() > n.size() ? g[n.size()] : n[g.size()]))))
        {
            std::string s = std::string(t.status);
            if (t.note && *t.note) s += " -- " + std::string(t.note);
            return s;
        }
    }
    return {};
}

// ------------------------------------------------------------------ PCGamingWiki

static bool HttpGet(const std::wstring &url, std::string &body, std::string &err,
                    const std::function<void(size_t, size_t)> &prog = nullptr, const std::wstring &extraHeaders = L"");
static std::string JsonString(const std::string &json, const std::string &key);

struct PcgwEntry { long long when = 0; bool found = false; unsigned apis = 0; std::string text; };
static std::map<std::string, PcgwEntry> g_pcgw;
static bool g_pcgwLoaded = false;
static std::mutex g_pcgwLock;
static std::wstring PcgwPath() { return Join(DataDir(), L"pcgw_cache.txt"); }

static std::string PcgwKey(const Game &g)
{
    if (!g.appid.empty()) return "steam:" + g.appid;
    return "name:" + LowerA(g.name);
}

static void PcgwLoad()
{
    if (g_pcgwLoaded) return;
    g_pcgwLoaded = true;
    std::ifstream in{fs::path(PcgwPath())};
    std::string line;
    while (std::getline(in, line))
    {
        std::vector<std::string> c;
        size_t a = 0, b;
        while ((b = line.find('\t', a)) != std::string::npos) { c.push_back(line.substr(a, b - a)); a = b + 1; }
        c.push_back(line.substr(a));
        if (c.size() < 5) continue;
        PcgwEntry e;
        e.when = atoll(c[1].c_str());
        e.found = c[2] == "1";
        e.apis = (unsigned)atoi(c[3].c_str());
        e.text = c[4];
        g_pcgw[c[0]] = e;
    }
}

static void PcgwSave()
{
    std::ofstream out{fs::path(PcgwPath()), std::ios::trunc | std::ios::binary};
    for (auto &e : g_pcgw)
        out << e.first << '\t' << e.second.when << '\t' << (e.second.found ? 1 : 0) << '\t' << e.second.apis << '\t' << e.second.text << '\n';
}

static void PcgwApply(Game &g)
{
    std::lock_guard<std::mutex> l(g_pcgwLock);
    PcgwLoad();
    g.pcgwKnown = false; g.pcgwApis = 0; g.pcgwText.clear();
    auto it = g_pcgw.find(PcgwKey(g));
    if (it == g_pcgw.end() || !it->second.found) return;
    g.pcgwKnown = true;
    g.pcgwApis = it->second.apis;
    g.pcgwText = it->second.text;
}

static std::string UrlEncode(const std::string &s)
{
    std::string o;
    char b[4];
    for (unsigned char c : s)
    {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { snprintf(b, 4, "%%%02X", c); o += b; }
    }
    return o;
}

// "12, 11" -> D3D12 | D3D11; any Vulkan or OpenGL version -> that API
static void PcgwParseRow(const std::string &d3d, const std::string &vk, const std::string &gl, unsigned &apis, std::string &text)
{
    apis = 0;
    std::vector<int> v;
    int cur = -1;
    for (char c : d3d + ",")
    {
        if (isdigit((unsigned char)c)) cur = (cur < 0 ? 0 : cur * 10) + (c - '0');
        else if (cur >= 0) { v.push_back(cur); cur = -1; if (c == '.') { /* 11.1 etc: skip the minor part */ } }
    }
    std::string t;
    for (int x : v)
    {
        if (x == 12) apis |= ApiD3D12;
        else if (x == 11) apis |= ApiD3D11;
        else if (x == 10) apis |= ApiD3D10;
        else if (x == 9) apis |= ApiD3D9;
        else if (x == 1 || x == 0) continue;
        if (x >= 2) t += (t.empty() ? "Direct3D " : ", ") + std::to_string(x);
    }
    if (!vk.empty() && LowerA(vk) != "false") { apis |= ApiVulkan; t += (t.empty() ? "" : "; ") + std::string("Vulkan"); }
    if (!gl.empty() && LowerA(gl) != "false") { apis |= ApiGL; t += (t.empty() ? "" : "; ") + std::string("OpenGL ") + gl; }
    text = t;
}

std::string FetchPcgw(std::vector<Game> &games, const std::function<void(const std::string &)> &progress)
{
    std::vector<Game *> want;
    long long now = (long long)time(nullptr);
    {
        std::lock_guard<std::mutex> l(g_pcgwLock);
        PcgwLoad();
        for (auto &g : games)
        {
            auto it = g_pcgw.find(PcgwKey(g));
            if (it != g_pcgw.end() && now - it->second.when < (it->second.found ? 30 : 7) * 86400LL) continue;
            want.push_back(&g);
        }
    }
    if (want.empty()) return {};
    std::string err;
    const size_t batch = 25;
    for (size_t i = 0; i < want.size(); i += batch)
    {
        if (progress) progress("Looking up graphics APIs on PCGamingWiki (" + std::to_string(i) + "/" + std::to_string(want.size()) + ")");
        std::string where;
        for (size_t k = i; k < std::min(want.size(), i + batch); k++)
        {
            Game &g = *want[k];
            if (!where.empty()) where += " OR ";
            if (!g.appid.empty()) where += "Infobox_game.Steam_AppID HOLDS \"" + g.appid + "\"";
            else
            {
                std::string n;
                for (char c : g.name) { if (c == '"' || c == '\\') n += '\\'; n += c; }
                where += "Infobox_game._pageName=\"" + n + "\"";
            }
        }
        std::string url = "https://www.pcgamingwiki.com/w/api.php?action=cargoquery&format=json&limit=500"
                          "&tables=Infobox_game,API&join_on=Infobox_game._pageName%3DAPI._pageName"
                          "&fields=Infobox_game._pageName%3DPage,Infobox_game.Steam_AppID%3DAppid,"
                          "API.Direct3D_versions%3DD3D,API.Vulkan_versions%3DVK,API.OpenGL_versions%3DGL"
                          "&where=" + UrlEncode(where);
        std::string body, e;
        if (!HttpGet(W(url), body, e)) { err = "PCGamingWiki: " + e; break; }
        std::lock_guard<std::mutex> l(g_pcgwLock);
        std::set<std::string> hit;
        for (size_t p = 0; (p = body.find("\"title\"", p)) != std::string::npos; p += 7)
        {
            size_t end = body.find('}', p);
            std::string obj = body.substr(p, end == std::string::npos ? std::string::npos : end - p);
            std::string page = JsonString(obj, "Page"), ids = JsonString(obj, "Appid");
            PcgwEntry en;
            en.when = now;
            en.found = true;
            PcgwParseRow(JsonString(obj, "D3D"), JsonString(obj, "VK"), JsonString(obj, "GL"), en.apis, en.text);
            for (size_t k = i; k < std::min(want.size(), i + batch); k++)
            {
                Game &g = *want[k];
                bool match = false;
                if (!g.appid.empty())
                {
                    std::string list = "," + ids + ",";
                    for (auto &c : list) if (c == ' ') c = ',';
                    match = list.find("," + g.appid + ",") != std::string::npos;
                }
                else match = LowerA(page) == LowerA(g.name);
                if (match) { g_pcgw[PcgwKey(g)] = en; hit.insert(PcgwKey(g)); }
            }
        }
        for (size_t k = i; k < std::min(want.size(), i + batch); k++)
            if (!hit.count(PcgwKey(*want[k]))) { PcgwEntry en; en.when = now; g_pcgw[PcgwKey(*want[k])] = en; }
    }
    {
        std::lock_guard<std::mutex> l(g_pcgwLock);
        PcgwSave();
    }
    return err;
}

// ------------------------------------------------------------------ detection

void Detect(Game &g)
{
    g.notes.clear();
    g.reshade = g.optiscaler = g.dxvk = g.dgvoodoo = g.specialK = false;
    g.reshadeDll = {};
    g.addonPath.clear(); g.addonVersion.clear(); g.addonWrongBits = false;
    g.ini = false; g.iniPath.clear();
    g.bits = 0; g.apis = 0;
    g.route = Route::Unknown;
    if (g.exe.empty())
    {
        g.notes.push_back("No game executable found in this folder.");
        return;
    }
    ExeFacts ef = AnalyzeExe(g.exe);
    g.bits = ef.bits;
    g.exeApis = ef.apis;
    PcgwApply(g);
    g.apis = (g.pcgwKnown && g.pcgwApis) ? g.pcgwApis : g.exeApis;
    g.tested = TestedFor(g.name);
    std::wstring dir = DirOf(g.exe);

    for (auto n : kReShadeNames)
    {
        std::wstring p = Join(dir, n);
        if (!Exists(p)) continue;
        DllInfo d = ReadDllInfo(p);
        if (InfoHas(d, "ReShade"))
        {
            if (!g.reshade) { g.reshade = true; g.reshadeDll = d; g.reshadeDll.version = ShortVersion(d.version); }
        }
        else if (InfoHas(d, "OptiScaler")) g.optiscaler = true;
        else if (InfoHas(d, "Special K")) g.specialK = true;
        else if (InfoHas(d, "dgVoodoo")) g.dgvoodoo = true;
    }
    if (Exists(Join(dir, L"OptiScaler.ini")) || Exists(Join(dir, L"OptiScaler.dll"))) g.optiscaler = true;
    if (Exists(Join(dir, L"dgVoodoo.conf"))) g.dgvoodoo = true;
    for (auto n : { L"d3d11.dll", L"dxgi.dll", L"d3d9.dll", L"d3d10core.dll" })
    {
        std::wstring p = Join(dir, n);
        if (!Exists(p) || (g.reshade && Lower(g.reshadeDll.path) == Lower(p))) continue;
        DllInfo d = ReadDllInfo(p);
        if (InfoHas(d, "ReShade") || InfoHas(d, "OptiScaler") || InfoHas(d, "dgVoodoo") || InfoHas(d, "Special K")) continue;
        Mapped m(p);
        if (FindNoCase(m.view(), "dxvk")) { g.dxvk = true; break; }
    }
    if (g.dgvoodoo) g.apis = (g.apis & ~(ApiD3D9 | ApiD3D10)) | ApiD3D11;   // dgVoodoo hands ReShade a D3D11 device

    // Where ReShade looks for add-ons: [ADDON] AddonPath in ReShade.ini, else next to ReShade's DLL.
    std::wstring addonDir = g.reshade ? DirOf(g.reshadeDll.path) : dir;
    std::wstring rsIni = Join(addonDir, L"ReShade.ini");
    if (Exists(rsIni))
    {
        wchar_t buf[2048]{};
        GetPrivateProfileStringW(L"ADDON", L"AddonPath", L"", buf, 2048, rsIni.c_str());
        std::wstring ap = buf;
        if (!ap.empty())
        {
            if (ap.size() > 1 && ap[0] == L'.' ) ap = Join(addonDir, ap.substr(ap[1] == L'\\' || ap[1] == L'/' ? 2 : 1));
            else if (ap.find(L':') == std::wstring::npos && ap[0] != L'\\') ap = Join(addonDir, ap);
            if (DirExists(ap)) { addonDir = ap; g.notes.push_back("ReShade.ini sets an add-on folder: " + U8(ap)); }
        }
    }
    for (auto n : { L"BFIPresenter.addon64", L"BFIPresenter.addon32", L"BFIPresenter.addon" })
    {
        std::wstring p = Join(addonDir, n);
        if (!Exists(p)) continue;
        if (g.addonPath.empty()) { g.addonPath = p; g.addonVersion = BfiVersionOf(p, false); }
        bool is64 = Lower(std::wstring(n)) == L"bfipresenter.addon64";
        if ((is64 && g.bits == 32) || (!is64 && g.bits == 64)) g.addonWrongBits = true;
    }
    for (auto d : { addonDir, dir })
    {
        std::wstring p = Join(d, L"BFIPresenter.ini");
        if (Exists(p)) { g.ini = true; g.iniPath = p; break; }
    }

    // Route
    std::wstring ro = LoadSetting(RouteKey(g.root).c_str(), L"");
    unsigned dxApis = g.apis & (ApiD3D9 | ApiD3D11 | ApiD3D12 | ApiGL);
    if (ro == L"vulkan") g.route = Route::Vulkan;
    else if (ro == L"dx") g.route = g.bits == 32 ? Route::Addon32 : Route::Addon64;
    else if (g.dxvk) g.route = Route::Vulkan;
    else if ((g.apis & ApiVulkan) && g.tested.find("(Vulkan") != std::string::npos) g.route = Route::Vulkan;
    else if (dxApis) g.route = g.bits == 32 ? Route::Addon32 : Route::Addon64;
    else if (g.apis & ApiVulkan) g.route = Route::Vulkan;
    else if (g.apis & ApiD3D10) g.route = Route::Unsupported;
    else g.route = g.bits == 32 ? Route::Addon32 : g.bits == 64 ? Route::Addon64 : Route::Unknown;

    if (g.bits == 0) g.notes.push_back("Couldn't read the executable's header; pick the game's .exe below.");
    if (!g.apis) g.notes.push_back("The graphics API wasn't recognised from the executable; the route below is a guess.");
    if (g.pcgwKnown && g.pcgwApis && g.exeApis && !(g.pcgwApis & g.exeApis & (ApiD3D9 | ApiD3D11 | ApiD3D12 | ApiGL | ApiVulkan)))
        g.notes.push_back("PCGamingWiki lists " + ApiText(g.pcgwApis) + "; the executable looked like " + ApiText(g.exeApis) +
                          ". The route follows PCGamingWiki.");
    if ((g.apis & ApiD3D12) && (g.apis & ApiVulkan) && !g.dxvk)
        g.notes.push_back("The game supports both Direct3D 12 and Vulkan: the route follows the API you run it with (change it below).");
    if (g.route == Route::Unsupported) g.notes.push_back("Direct3D 10 games aren't supported (try DXVK and the Vulkan layer).");
    if ((g.route == Route::Addon64 || g.route == Route::Addon32) && !g.reshade)
        g.notes.push_back("ReShade isn't installed for this game. Install ReShade (with full add-on support) first: reshade.me");
    if ((g.route == Route::Addon64 || g.route == Route::Addon32) && g.reshade && (g.apis & ApiD3D9) &&
        !(g.apis & (ApiD3D11 | ApiD3D12)) && CompareVersions(g.reshadeDll.version, "6.8") < 0)
        g.notes.push_back("Direct3D 9 needs ReShade 6.8 or newer (installed: " + g.reshadeDll.version + ").");
    if (g.reshade && Lower(NameOf(g.reshadeDll.path)).rfind(L"reshade", 0) == 0)
        g.notes.push_back("ReShade is loaded as " + U8(NameOf(g.reshadeDll.path)) + (g.optiscaler ? " by OptiScaler." : " by another loader."));
    if (g.optiscaler) g.notes.push_back("OptiScaler is installed. Resident Evil 4 hung its GPU with OptiScaler and the presenter.");
    if (g.dxvk && g.bits == 32) g.notes.push_back("DXVK in a 32-bit game: the 32-bit add-on without DXVK usually works better (Twelve Minutes, Alan Wake flickered through DXVK).");
    if (g.dgvoodoo) g.notes.push_back("dgVoodoo is installed: ReShade sees a Direct3D 11 game; use the DX add-on.");
    if (g.apis & ApiGL && !(g.apis & (ApiD3D11 | ApiD3D12 | ApiD3D9)) && !g.dgvoodoo)
        g.notes.push_back("OpenGL needs an NVIDIA GPU. Old OpenGL games may need the NVIDIA profile setting Vulkan/OpenGL present method = Prefer layered on DXGI swapchain.");
    if (g.addonWrongBits) g.notes.push_back("An add-on for the wrong bitness is installed; Install replaces it with the right one.");
}

// ------------------------------------------------------------------ discovery

static Game *AddGame(std::vector<Game> &out, std::set<std::wstring> &seen, const std::string &name,
                     const std::string &store, const std::wstring &root)
{
    if (root.empty() || !DirExists(root)) return nullptr;
    std::wstring key = Lower(root);
    while (!key.empty() && (key.back() == L'\\' || key.back() == L'/')) key.pop_back();
    if (seen.count(key)) return nullptr;
    seen.insert(key);
    Game g;
    g.name = name.empty() ? U8(NameOf(root)) : name;
    g.store = store;
    g.root = root;
    out.push_back(g);
    return &out.back();
}

static std::string VdfValue(const std::string &text, const std::string &key)
{
    std::string k = "\"" + key + "\"";
    size_t p = LowerA(text).find(LowerA(k));
    if (p == std::string::npos) return {};
    size_t a = text.find('"', p + k.size());
    if (a == std::string::npos) return {};
    size_t b = text.find('"', a + 1);
    std::string v = text.substr(a + 1, b - a - 1);
    std::string out;
    for (size_t i = 0; i < v.size(); i++)
    {
        if (v[i] == '\\' && i + 1 < v.size() && v[i + 1] == '\\') { out += '\\'; i++; }
        else out += v[i];
    }
    return out;
}

static void FindSteam(std::vector<Game> &out, std::set<std::wstring> &seen)
{
    std::wstring steam = RegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (steam.empty()) steam = RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
    if (steam.empty()) return;
    for (auto &c : steam) if (c == L'/') c = L'\\';
    std::vector<std::wstring> libs = { steam };
    std::string vdf = ReadText(Join(steam, L"steamapps\\libraryfolders.vdf"));
    for (size_t p = 0; (p = LowerA(vdf).find("\"path\"", p)) != std::string::npos; p += 6)
    {
        std::string rest = vdf.substr(p);
        std::string v = VdfValue(rest, "path");
        if (!v.empty()) libs.push_back(W(v));
    }
    std::set<std::wstring> libSeen;
    for (auto &lib : libs)
    {
        if (!libSeen.insert(Lower(lib)).second) continue;
        std::wstring apps = Join(lib, L"steamapps");
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(Join(apps, L"appmanifest_*.acf").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do
        {
            std::string acf = ReadText(Join(apps, fd.cFileName));
            std::string name = VdfValue(acf, "name"), dir = VdfValue(acf, "installdir");
            std::string ln = LowerA(name);
            if (dir.empty() || ln.find("redistributable") != std::string::npos || ln.find("proton") == 0 ||
                ln.find("steamvr") == 0 || ln.find("steam linux runtime") == 0 || ln.find("wallpaper engine") == 0)
                continue;
            Game *g = AddGame(out, seen, name, "Steam", Join(Join(apps, L"common"), W(dir)));
            if (g)
            {
                g->appid = VdfValue(acf, "appid");
                if (g->appid.empty())
                {
                    std::wstring f = fd.cFileName;   // appmanifest_<id>.acf
                    g->appid = U8(f.substr(12, f.size() - 16));
                }
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

static std::string JsonString(const std::string &json, const std::string &key)
{
    size_t p = json.find("\"" + key + "\"");
    if (p == std::string::npos) return {};
    p = json.find(':', p);
    if (p == std::string::npos) return {};
    p = json.find('"', p);
    if (p == std::string::npos) return {};
    std::string out;
    for (size_t i = p + 1; i < json.size() && json[i] != '"'; i++)
    {
        if (json[i] == '\\' && i + 1 < json.size())
        {
            char c = json[++i];
            out += c == 'n' ? '\n' : c == 't' ? '\t' : c;
        }
        else out += json[i];
    }
    return out;
}

static void FindEpic(std::vector<Game> &out, std::set<std::wstring> &seen)
{
    std::wstring dir = L"C:\\ProgramData\\Epic\\EpicGamesLauncher\\Data\\Manifests";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(Join(dir, L"*.item").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
    {
        std::string j = ReadText(Join(dir, fd.cFileName));
        std::string name = JsonString(j, "DisplayName"), loc = JsonString(j, "InstallLocation");
        if (j.find("\"bIsIncompleteInstall\": true") != std::string::npos) continue;
        if (LowerA(JsonString(j, "AppCategories")).find("engines") != std::string::npos) continue;
        AddGame(out, seen, name, "Epic", W(loc));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void FindGog(std::vector<Game> &out, std::set<std::wstring> &seen)
{
    const wchar_t *base = L"SOFTWARE\\WOW6432Node\\GOG.com\\Games";
    for (auto &id : RegSubkeys(HKEY_LOCAL_MACHINE, base))
    {
        std::wstring k = std::wstring(base) + L"\\" + id;
        std::wstring name = RegString(HKEY_LOCAL_MACHINE, k.c_str(), L"gameName");
        std::wstring path = RegString(HKEY_LOCAL_MACHINE, k.c_str(), L"path");
        if (RegString(HKEY_LOCAL_MACHINE, k.c_str(), L"dependsOn").size()) continue;   // DLC
        AddGame(out, seen, U8(name), "GOG", path);
    }
}

static void FindXbox(std::vector<Game> &out, std::set<std::wstring> &seen)
{
    DWORD drives = GetLogicalDrives();
    for (int d = 0; d < 26; d++)
    {
        if (!(drives & (1u << d))) continue;
        wchar_t root[] = { (wchar_t)(L'A' + d), L':', L'\\', 0 };
        if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
        std::wstring xb = std::wstring(root) + L"XboxGames";
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(Join(xb, L"*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do
        {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L".." || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || Lower(n) == L"gamesave") continue;
            std::wstring c = Join(Join(xb, n), L"Content");
            AddGame(out, seen, U8(n), "Xbox", DirExists(c) ? c : Join(xb, n));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

static bool DirHasExe(const std::wstring &dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(Join(dir, L"*.exe").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool any = false;
    do { if (!SkipExe(Lower(fd.cFileName))) any = true; } while (!any && FindNextFileW(h, &fd));
    FindClose(h);
    return any;
}

static void FindInFolder(const std::wstring &folder, std::vector<Game> &out, std::set<std::wstring> &seen)
{
    if (!DirExists(folder)) return;
    if (DirHasExe(folder)) { AddGame(out, seen, U8(NameOf(folder)), "Folder", folder); return; }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(Join(folder, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
    {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L".." || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::wstring l = Lower(n);
        if (l == L"steamapps" || l == L"$recycle.bin" || l == L"system volume information") continue;
        AddGame(out, seen, U8(n), "Folder", Join(folder, n));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

ScanOptions LoadScanOptions()
{
    ScanOptions o;
    auto get = [](const wchar_t *k) { return GetPrivateProfileIntW(L"Libraries", k, 1, SettingsPath().c_str()) != 0; };
    o.steam = get(L"Steam"); o.epic = get(L"Epic"); o.gog = get(L"GOG"); o.xbox = get(L"Xbox");
    o.folders = LoadFolders();
    return o;
}

void SaveScanOptions(const ScanOptions &o)
{
    auto put = [](const wchar_t *k, bool v) { WritePrivateProfileStringW(L"Libraries", k, v ? L"1" : L"0", SettingsPath().c_str()); };
    put(L"Steam", o.steam); put(L"Epic", o.epic); put(L"GOG", o.gog); put(L"Xbox", o.xbox);
    SaveFolders(o.folders);
}

LibraryInfo FindLibraries()
{
    LibraryInfo li;
    std::wstring steam = RegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (steam.empty()) steam = RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
    for (auto &c : steam) if (c == L'/') c = L'\\';
    if (!steam.empty() && DirExists(steam))
    {
        std::string vdf = ReadText(Join(steam, L"steamapps\\libraryfolders.vdf"));
        int libs = 0;
        for (size_t p = 0; (p = LowerA(vdf).find("\"path\"", p)) != std::string::npos; p += 6) libs++;
        li.steam = true;
        li.steamText = U8(steam) + (libs > 1 ? " and " + std::to_string(libs - 1) + " more librar" + (libs > 2 ? "ies" : "y") : "");
    }
    else li.steamText = "not installed";
    {
        int n = 0;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(L"C:\\ProgramData\\Epic\\EpicGamesLauncher\\Data\\Manifests\\*.item", &fd);
        if (h != INVALID_HANDLE_VALUE) { do n++; while (FindNextFileW(h, &fd)); FindClose(h); }
        li.epic = n > 0;
        li.epicText = n ? std::to_string(n) + " installed item" + (n > 1 ? "s" : "") : "no installed games found";
    }
    {
        size_t n = RegSubkeys(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\GOG.com\\Games").size();
        li.gog = n > 0;
        li.gogText = n ? std::to_string(n) + " game" + (n > 1 ? "s" : "") + " registered" : "no installed games found";
    }
    {
        std::string drives;
        DWORD mask = GetLogicalDrives();
        for (int d = 0; d < 26; d++)
        {
            if (!(mask & (1u << d))) continue;
            wchar_t root[] = { (wchar_t)(L'A' + d), L':', L'\\', 0 };
            if (GetDriveTypeW(root) == DRIVE_FIXED && DirExists(std::wstring(root) + L"XboxGames"))
                drives += (drives.empty() ? "" : ", ") + std::string(1, (char)('A' + d)) + ":";
        }
        li.xbox = !drives.empty();
        li.xboxText = drives.empty() ? "no XboxGames folder found" : "XboxGames on " + drives;
    }
    return li;
}

std::vector<Game> FindGames(const ScanOptions &opt, const std::function<void(const std::string &)> &progress)
{
    std::vector<Game> out;
    out.reserve(4096);   // AddGame hands out pointers into the vector
    std::set<std::wstring> seen;
    if (progress) progress("Reading game libraries...");
    if (opt.steam) FindSteam(out, seen);
    if (opt.epic) FindEpic(out, seen);
    if (opt.gog) FindGog(out, seen);
    if (opt.xbox) FindXbox(out, seen);
    for (auto &f : opt.folders) FindInFolder(f, out, seen);
    int i = 0;
    for (auto &g : out)
    {
        if (progress) progress("Reading " + g.name + " (" + std::to_string(++i) + " of " + std::to_string(out.size()) + ")");
        std::vector<std::wstring> exes;
        CollectExes(g.root, 0, exes);
        g.exes = RankExes(exes, g.name, g.root);
        std::wstring ov = LoadExeOverride(g.root);
        if (!ov.empty() && Exists(ov)) { g.exe = ov; g.exeOverride = true; }
        else if (!g.exes.empty()) g.exe = g.exes[0];
        Detect(g);
    }
    SaveCache();
    // folders without a game executable (tools, redistributables, empty installs) can't be acted on
    out.erase(std::remove_if(out.begin(), out.end(), [](const Game &g) { return g.exe.empty(); }), out.end());
    std::sort(out.begin(), out.end(), [](const Game &a, const Game &b) { return LowerA(a.name) < LowerA(b.name); });
    return out;
}

static std::wstring ListPath() { return Join(DataDir(), L"games.txt"); }

void SaveGameList(const std::vector<Game> &games)
{
    std::ofstream o{fs::path(ListPath()), std::ios::trunc | std::ios::binary};
    for (auto &g : games)
    {
        o << g.store << '\t' << g.appid << '\t' << g.name << '\t' << U8(g.root) << '\t';
        for (size_t k = 0; k < g.exes.size(); k++) o << (k ? "|" : "") << U8(g.exes[k]);
        o << '\n';
    }
}

bool LoadGameList(std::vector<Game> &games)
{
    std::ifstream in{fs::path(ListPath())};
    if (!in) return false;
    games.clear();
    std::string line;
    while (std::getline(in, line))
    {
        std::vector<std::string> c;
        size_t a = 0, b;
        while ((b = line.find('\t', a)) != std::string::npos) { c.push_back(line.substr(a, b - a)); a = b + 1; }
        c.push_back(line.substr(a));
        if (c.size() < 5) continue;
        Game g;
        g.store = c[0]; g.appid = c[1]; g.name = c[2]; g.root = W(c[3]);
        for (size_t p = 0, q; p < c[4].size(); p = q + 1)
        {
            q = c[4].find('|', p);
            if (q == std::string::npos) q = c[4].size();
            std::wstring e = W(c[4].substr(p, q - p));
            if (Exists(e)) g.exes.push_back(e);
        }
        if (!DirExists(g.root) || g.exes.empty()) continue;   // uninstalled since
        std::wstring ov = LoadExeOverride(g.root);
        if (!ov.empty() && Exists(ov)) { g.exe = ov; g.exeOverride = true; }
        else g.exe = g.exes[0];
        games.push_back(g);
    }
    return true;
}

void DetectAll(std::vector<Game> &games, const std::function<void(const std::string &)> &progress)
{
    int i = 0;
    for (auto &g : games)
    {
        if (progress && (++i % 10) == 0) progress("Checking games (" + std::to_string(i) + " of " + std::to_string(games.size()) + ")");
        Detect(g);
    }
    SaveCache();
}

VulkanLayerState DetectVulkanLayer()
{
    VulkanLayerState s;
    for (auto &v : RegValueNames(HKEY_CURRENT_USER, L"SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers"))
    {
        std::wstring l = Lower(v);
        if (l.find(L"vklayer_bfi_presenter.json") != std::wstring::npos && Exists(v))
        {
            s.dir = DirOf(v);
            std::wstring dll = Join(s.dir, L"BFIPresenterVK.dll");
            s.installed = Exists(dll);
            if (s.installed) s.version = BfiVersionOf(dll, true);
        }
    }
    for (HKEY root : { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER })
        for (auto &v : RegValueNames(root, L"SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers"))
            if (Lower(v).find(L"reshade") != std::wstring::npos && Exists(v))
            {
                s.reshadeLayer = true;
                std::wstring dll = Join(DirOf(v), L"ReShade64.dll");
                if (Exists(dll)) s.reshadeVersion = ShortVersion(ReadDllInfo(dll).version);
            }
    return s;
}

// ------------------------------------------------------------------ sources

static std::wstring FindFileUnder(const std::wstring &dir, const wchar_t *name, int depth)
{
    if (Exists(Join(dir, name))) return dir;
    if (depth <= 0) return {};
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(Join(dir, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::wstring found;
    do
    {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L".." || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        found = FindFileUnder(Join(dir, n), name, depth - 1);
    } while (found.empty() && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

Source SourceFromFolder(const std::wstring &dir)
{
    Source s;
    s.dxDir = FindFileUnder(dir, L"BFIPresenter.addon64", 2);
    if (s.dxDir.empty()) s.dxDir = FindFileUnder(dir, L"BFIPresenter.addon32", 2);
    s.vkDir = FindFileUnder(dir, L"BFIPresenterVK.dll", 2);
    if (!s.dxDir.empty())
    {
        std::wstring a = Join(s.dxDir, L"BFIPresenter.addon64");
        if (!Exists(a)) a = Join(s.dxDir, L"BFIPresenter.addon32");
        s.dxVersion = BfiVersionOf(a, false);
    }
    if (!s.vkDir.empty())
    {
        // The layer needs its manifests too; a folder with only the DLL (a hand-made test folder) can still
        // serve the DX add-on, but not a Vulkan layer install.
        std::string missing;
        for (auto n : { L"BFIPresenterVK.dll", L"BFIPresenterVK32.dll", L"VkLayer_BFI_presenter.json",
                        L"VkLayer_BFI_presenter32.json", L"BFIPresenter.ini" })
            if (!Exists(Join(s.vkDir, n))) missing += (missing.empty() ? "" : ", ") + U8(n);
        if (missing.empty()) s.vkVersion = BfiVersionOf(Join(s.vkDir, L"BFIPresenterVK.dll"), true);
        else { s.vkMissing = missing; s.vkDir.clear(); }
    }
    s.label = U8(dir);
    return s;
}

// ------------------------------------------------------------------ backups and log

std::wstring BackupDir() { std::wstring d = Join(DataDir(), L"backups"); CreateDirectoryW(d.c_str(), nullptr); return d; }
std::wstring LogPath() { return Join(DataDir(), L"manager_log.csv"); }

static std::string Now(bool forFile)
{
    time_t t = time(nullptr);
    tm lt{};
    localtime_s(&lt, &t);
    char b[32];
    strftime(b, 32, forFile ? "%Y%m%d-%H%M%S" : "%Y-%m-%d %H:%M:%S", &lt);
    return b;
}

static void Log(const std::string &action, const std::string &game, const std::wstring &file,
                const std::string &oldV, const std::string &newV)
{
    bool fresh = !Exists(LogPath());
    std::ofstream o(fs::path(LogPath()), std::ios::app | std::ios::binary);
    if (fresh) o << "time,action,game,file,old_version,new_version\r\n";
    auto q = [](std::string s) { std::string r = "\""; for (char c : s) { if (c == '"') r += '"'; r += c; } return r + "\""; };
    o << Now(false) << "," << action << "," << q(game) << "," << q(U8(file)) << "," << q(oldV) << "," << q(newV) << "\r\n";
}

static std::wstring GameBackupRoot(const Game &g)
{
    std::wstring safe;
    for (wchar_t c : W(g.name)) safe += (iswalnum(c) ? c : L'_');
    if (safe.size() > 40) safe.resize(40);
    std::wstring d = Join(BackupDir(), safe + L"_" + PathKey(g.root).substr(0, 8));
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

// One "transaction" per action: every file it touches is listed in manifest.txt
// (destination \t backup copy, or \t* when the file didn't exist before).
struct Txn {
    std::wstring dir;
    std::ofstream manifest;
    int n = 0;
    explicit Txn(const Game &g)
    {
        dir = Join(GameBackupRoot(g), W(Now(true)));
        for (int i = 2; DirExists(dir); i++) dir = Join(GameBackupRoot(g), W(Now(true)) + L"_" + std::to_wstring(i));
        CreateDirectoryW(dir.c_str(), nullptr);
        manifest.open(fs::path(Join(dir, L"manifest.txt")), std::ios::binary);
    }
    // call before changing or deleting dest
    bool Keep(const std::wstring &dest)
    {
        if (Exists(dest))
        {
            std::wstring b = Join(dir, std::to_wstring(n++) + L"_" + NameOf(dest));
            if (!CopyFileW(dest.c_str(), b.c_str(), FALSE)) return false;
            manifest << U8(dest) << '\t' << U8(b) << '\n';
        }
        else manifest << U8(dest) << "\t*\n";
        manifest.flush();
        return true;
    }
    void Finish()
    {
        manifest.close();
        if (n == 0)
        {
            // nothing to put back but new files: still keep it so Revert can delete them
            std::ifstream in{fs::path(Join(dir, L"manifest.txt"))};
            if (in.peek() == EOF) { in.close(); DeleteFileW(Join(dir, L"manifest.txt").c_str()); RemoveDirectoryW(dir.c_str()); }
        }
    }
};

static std::string LastError(const char *what, const std::wstring &p)
{
    DWORD e = GetLastError();
    wchar_t *m = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, e, 0, (wchar_t *)&m, 0, nullptr);
    std::string msg = m ? Trim(U8(m)) : std::to_string(e);
    if (m) LocalFree(m);
    std::string extra = (e == ERROR_SHARING_VIOLATION || e == ERROR_ACCESS_DENIED) ? " Close the game and try again." : "";
    return std::string(what) + " " + U8(p) + ": " + msg + extra;
}

static std::string CopyIn(Txn &t, const std::wstring &src, const std::wstring &dest)
{
    if (!Exists(src)) return "Missing in the release: " + U8(src);
    if (Exists(dest) && SameContent(src, dest)) return {};
    if (!t.Keep(dest)) return LastError("Couldn't back up", dest);
    if (!CopyFileW(src.c_str(), dest.c_str(), FALSE)) return LastError("Couldn't copy to", dest);
    return {};
}

static std::string DeleteKept(Txn &t, const std::wstring &dest)
{
    if (!Exists(dest)) return {};
    if (!t.Keep(dest)) return LastError("Couldn't back up", dest);
    if (!DeleteFileW(dest.c_str())) return LastError("Couldn't delete", dest);
    return {};
}

std::string InstallOrUpdate(Game &g, const Source &src)
{
    std::string err;
    if (g.route == Route::Addon64 || g.route == Route::Addon32)
    {
        if (src.dxDir.empty()) return "The selected release has no DX add-on.";
        if (!g.reshade) return "Install ReShade (with full add-on support) for this game first.";
        bool is64 = g.route == Route::Addon64;
        std::wstring dir = g.addonPath.empty() ? DirOf(g.reshadeDll.path) : DirOf(g.addonPath);
        std::wstring name = is64 ? L"BFIPresenter.addon64" : L"BFIPresenter.addon32";
        std::wstring other = Join(dir, is64 ? L"BFIPresenter.addon32" : L"BFIPresenter.addon64");
        std::string oldV = g.addonVersion;
        Txn t(g);
        err = CopyIn(t, Join(src.dxDir, name), Join(dir, name));
        if (err.empty()) err = DeleteKept(t, other);
        if (err.empty()) err = DeleteKept(t, Join(dir, L"BFIPresenter.addon"));
        t.Finish();
        if (err.empty()) Log(oldV.empty() ? "install" : "update", g.name, Join(dir, name), oldV, src.dxVersion);
    }
    else if (g.route == Route::Vulkan)
    {
        if (src.vkDir.empty()) return "The selected release has no Vulkan layer.";
        VulkanLayerState vs = DetectVulkanLayer();
        if (!vs.installed || CompareVersions(vs.version, src.vkVersion) < 0)
        {
            err = InstallVulkanLayer(src);
            if (!err.empty()) return err;
        }
        if (!g.ini)
        {
            Txn t(g);
            std::wstring dest = Join(DirOf(g.exe), L"BFIPresenter.ini");
            err = CopyIn(t, Join(src.vkDir, L"BFIPresenter.ini"), dest);
            t.Finish();
            if (err.empty()) Log("install", g.name, dest, "", src.vkVersion);
        }
    }
    else return "Nothing to install: the game's route is " + std::string(RouteText(g.route)) + ".";
    Detect(g);
    return err;
}

std::string Remove(Game &g, bool alsoIni)
{
    Txn t(g);
    std::string err;
    std::string oldV = g.addonVersion;
    if (!g.addonPath.empty())
    {
        std::wstring dir = DirOf(g.addonPath);
        for (auto n : { L"BFIPresenter.addon64", L"BFIPresenter.addon32", L"BFIPresenter.addon" })
            if (err.empty()) err = DeleteKept(t, Join(dir, n));
    }
    if (err.empty() && g.ini && (alsoIni || g.route == Route::Vulkan)) err = DeleteKept(t, g.iniPath);
    t.Finish();
    if (err.empty()) Log("remove", g.name, g.addonPath.empty() ? g.iniPath : g.addonPath, oldV, "");
    Detect(g);
    return err;
}

static std::wstring LastTxn(const Game &g)
{
    std::wstring root = GameBackupRoot(g), best;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(Join(root, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    do
    {
        std::wstring n = fd.cFileName;
        if (n != L"." && n != L".." && (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            Exists(Join(Join(root, n), L"manifest.txt")) && n > best)
            best = n;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return best.empty() ? best : Join(root, best);
}

bool CanRevert(const Game &g) { return !LastTxn(g).empty(); }

std::string Revert(Game &g)
{
    std::wstring dir = LastTxn(g);
    if (dir.empty()) return "Nothing to revert for this game.";
    std::ifstream in{fs::path(Join(dir, L"manifest.txt"))};
    std::vector<std::pair<std::wstring, std::wstring>> items;
    std::string line8;
    while (std::getline(in, line8))
    {
        std::wstring line = W(line8);
        size_t t = line.find(L'\t');
        if (t != std::wstring::npos) items.push_back({ line.substr(0, t), line.substr(t + 1) });
    }
    in.close();
    std::string oldV = g.addonVersion;
    for (auto it = items.rbegin(); it != items.rend(); ++it)
    {
        if (it->second == L"*")
        {
            if (Exists(it->first) && !DeleteFileW(it->first.c_str())) return LastError("Couldn't delete", it->first);
        }
        else if (!CopyFileW(it->second.c_str(), it->first.c_str(), FALSE)) return LastError("Couldn't restore", it->first);
    }
    // done: drop this backup so the next Revert steps back one more
    for (auto &i : items) if (i.second != L"*") DeleteFileW(i.second.c_str());
    DeleteFileW(Join(dir, L"manifest.txt").c_str());
    RemoveDirectoryW(dir.c_str());
    Detect(g);
    Log("revert", g.name, items.empty() ? L"" : items[0].first, oldV, g.addonVersion);
    return {};
}

// ------------------------------------------------------------------ Vulkan layer

static const wchar_t *kVkKey = L"SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers";

std::string InstallVulkanLayer(const Source &src)
{
    if (src.vkDir.empty()) return "The selected release has no Vulkan layer.";
    std::wstring dest = Join(DataDir(), L"Vulkan");
    CreateDirectoryW(dest.c_str(), nullptr);
    Game pseudo;
    pseudo.name = "Vulkan layer";
    pseudo.root = dest;
    std::string oldV = DetectVulkanLayer().version;
    Txn t(pseudo);
    std::string err;
    for (auto n : { L"BFIPresenterVK.dll", L"BFIPresenterVK32.dll", L"VkLayer_BFI_presenter.json", L"VkLayer_BFI_presenter32.json" })
        if (err.empty()) err = CopyIn(t, Join(src.vkDir, n), Join(dest, n));
    t.Finish();
    if (!err.empty()) return err;
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kVkKey, 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return "Couldn't open the Vulkan layer registry key.";
    // delete and add again so the layer is listed last, after ReShade (closer to the driver)
    for (auto n : { L"VkLayer_BFI_presenter.json", L"VkLayer_BFI_presenter32.json" })
    {
        std::wstring v = Join(dest, n);
        RegDeleteValueW(k, v.c_str());
        DWORD zero = 0;
        RegSetValueExW(k, v.c_str(), 0, REG_DWORD, (const BYTE *)&zero, sizeof(zero));
    }
    RegCloseKey(k);
    Log(oldV.empty() ? "install" : "update", "Vulkan layer", Join(dest, L"BFIPresenterVK.dll"), oldV, src.vkVersion);
    return {};
}

std::string UninstallVulkanLayer()
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kVkKey, 0, KEY_READ | KEY_WRITE, &k) == ERROR_SUCCESS)
    {
        for (auto &v : RegValueNames(HKEY_CURRENT_USER, kVkKey))
            if (Lower(v).find(L"vklayer_bfi_presenter") != std::wstring::npos) RegDeleteValueW(k, v.c_str());
        RegCloseKey(k);
    }
    Log("remove", "Vulkan layer", Join(DataDir(), L"Vulkan"), DetectVulkanLayer().version, "");
    return {};
}

// ------------------------------------------------------------------ GitHub

static bool HttpGet(const std::wstring &url, std::string &body, std::string &err,
                    const std::function<void(size_t, size_t)> &prog, const std::wstring &extraHeaders)
{
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256]{}, path[4096]{};
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 4096;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) { err = "Bad URL"; return false; }
    HINTERNET s = WinHttpOpen(L"BFIPresenterManager", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!s) s = WinHttpOpen(L"BFIPresenterManager", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    HINTERNET c = s ? WinHttpConnect(s, host, uc.nPort, 0) : nullptr;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, nullptr, nullptr, nullptr,
                                         uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    bool ok = false;
    if (r)
    {
        std::wstring hdrs = L"Accept: application/vnd.github+json, application/octet-stream\r\n" + extraHeaders;
        const wchar_t *hdr = hdrs.c_str();
        if (WinHttpSendRequest(r, hdr, (DWORD)-1, nullptr, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr))
        {
            DWORD status = 0, sz = sizeof(status);
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &sz, nullptr);
            wchar_t lenBuf[32]{};
            DWORD lsz = sizeof(lenBuf);
            size_t total = 0;
            if (WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH, nullptr, lenBuf, &lsz, nullptr)) total = (size_t)_wtoi64(lenBuf);
            for (;;)
            {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(r, &avail) || avail == 0) break;
                size_t old = body.size();
                body.resize(old + avail);
                DWORD got = 0;
                if (!WinHttpReadData(r, body.data() + old, avail, &got)) break;
                body.resize(old + got);
                if (prog) prog(body.size(), total);
            }
            ok = status == 200;
            if (!ok) err = "HTTP " + std::to_string(status);
        }
        else err = "Couldn't reach " + U8(host) + " (error " + std::to_string(GetLastError()) + ")";
    }
    else err = "Couldn't connect";
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return ok;
}

RemoteRelease ParseRelease(const std::string &body)
{
    RemoteRelease rr;
    rr.tag = JsonString(body, "tag_name");
    rr.notesUrl = JsonString(body, "html_url");
    for (size_t p = 0; (p = body.find("\"browser_download_url\"", p)) != std::string::npos; p += 10)
    {
        std::string u = JsonString(body.substr(p), "browser_download_url");
        std::string l = LowerA(u);
        if (l.size() < 4 || l.compare(l.size() - 4, 4, ".zip") != 0) continue;
        if (l.find("bfipresenter-dx") != std::string::npos) rr.dxUrl = u;
        else if (l.find("vulkan") != std::string::npos) rr.vkUrl = u;
    }
    rr.ok = !rr.tag.empty() && (!rr.dxUrl.empty() || !rr.vkUrl.empty());
    if (!rr.ok) rr.error = "The latest release has no BFI Presenter zips.";
    return rr;
}

RemoteRelease FetchLatestRelease()
{
    RemoteRelease rr;
    std::string body;
    if (!HttpGet(L"https://api.github.com/repos/artzox/BFI-Presenter/releases/latest", body, rr.error))
    {
        if (rr.error == "HTTP 404") rr.error = "No published release on GitHub yet (or the repository isn't public).";
        else if (rr.error == "HTTP 403" || rr.error == "HTTP 429")
            rr.error = "GitHub refused the request (" + rr.error + "), usually its hourly limit for anonymous checks. Try again later.";
        else rr.error = "Couldn't check GitHub: " + rr.error;
        return rr;
    }
    return ParseRelease(body);
}

static std::string Extract(const std::wstring &zip, const std::wstring &dir)
{
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring tar = Join(sys, L"tar.exe");
    std::wstring cmd;
    if (Exists(tar)) cmd = L"\"" + tar + L"\" -xf \"" + zip + L"\" -C \"" + dir + L"\"";
    else cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"Expand-Archive -Force -LiteralPath '" + zip +
               L"' -DestinationPath '" + dir + L"'\"";
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return "Couldn't start the unzip tool.";
    WaitForSingleObject(pi.hProcess, 120000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code == 0 ? std::string() : "Unzipping " + U8(NameOf(zip)) + " failed.";
}

std::string DownloadRelease(const RemoteRelease &r, Source &out, const std::function<void(const std::string &)> &progress)
{
    std::wstring rel = Join(DataDir(), L"releases");
    CreateDirectoryW(rel.c_str(), nullptr);
    std::wstring dir = Join(rel, W(r.tag));
    CreateDirectoryW(dir.c_str(), nullptr);
    for (auto &u : { r.dxUrl, r.vkUrl })
    {
        if (u.empty()) continue;
        std::string name = u.substr(u.find_last_of('/') + 1), body, err;
        if (progress) progress("Downloading " + name);
        if (!HttpGet(W(u), body, err, [&](size_t got, size_t total) {
                if (progress && total) progress("Downloading " + name + " (" + std::to_string(got * 100 / total) + " %)");
            }))
            return "Download of " + name + " failed: " + err;
        std::wstring zip = Join(dir, W(name));
        std::ofstream(fs::path(zip), std::ios::binary).write(body.data(), (std::streamsize)body.size());
        if (progress) progress("Unpacking " + name);
        std::string e = Extract(zip, dir);
        if (!e.empty()) return e;
    }
    out = SourceFromFolder(dir);
    out.label = "GitHub " + r.tag;
    return out.ok() ? std::string() : "The downloaded release has no BFI Presenter files.";
}

Source CachedRelease()
{
    std::wstring rel = Join(DataDir(), L"releases");
    Source best;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(Join(rel, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return best;
    do
    {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L".." || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        Source s = SourceFromFolder(Join(rel, n));
        if (!s.ok()) continue;
        std::string v = s.dxVersion.empty() ? s.vkVersion : s.dxVersion;
        std::string bv = best.dxVersion.empty() ? best.vkVersion : best.dxVersion;
        if (!best.ok() || CompareVersions(v, bv) > 0) { best = s; best.label = "GitHub " + U8(n); }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return best;
}

// ------------------------------------------------------------------ ReShade

std::string FetchReShadeVersions(std::vector<std::string> &out)
{
    out.clear();
    std::string body, err;
    if (!HttpGet(L"https://api.github.com/repos/crosire/reshade/tags?per_page=100", body, err))
        return "Couldn't get the list of ReShade versions: " + err;
    for (size_t p = 0; (p = body.find("\"name\"", p)) != std::string::npos; p += 6)
    {
        std::string n = JsonString(body.substr(p, 80), "name");
        if (n.size() > 1 && (n[0] == 'v' || n[0] == 'V')) n = n.substr(1);
        bool ok = !n.empty() && isdigit((unsigned char)n[0]) && n.find_first_not_of("0123456789.") == std::string::npos;
        if (ok && CompareVersions(n, "5.0") >= 0 && std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
    }
    std::sort(out.begin(), out.end(), [](const std::string &a, const std::string &b) { return CompareVersions(a, b) > 0; });
    if (out.empty()) return "No ReShade versions found.";
    return {};
}

std::wstring ReShadeCacheDir(const std::string &version)
{
    std::wstring d = Join(DataDir(), L"reshade");
    CreateDirectoryW(d.c_str(), nullptr);
    d = Join(d, W(version));
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

std::vector<std::string> CachedReShadeVersions()
{
    std::vector<std::string> out;
    std::wstring root = Join(DataDir(), L"reshade");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(Join(root, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do
    {
        std::wstring n = fd.cFileName;
        if (n != L"." && n != L".." && (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            Exists(Join(Join(root, n), L"ReShade64.dll")))
            out.push_back(U8(n));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const std::string &a, const std::string &b) { return CompareVersions(a, b) > 0; });
    return out;
}

// The add-on build of ReShade's setup carries ReShade32.dll and ReShade64.dll in a zip appended to the exe.
std::string EnsureReShade(const std::string &version, const std::function<void(const std::string &)> &progress)
{
    std::wstring dir = ReShadeCacheDir(version);
    if (Exists(Join(dir, L"ReShade64.dll")) && Exists(Join(dir, L"ReShade32.dll"))) return {};
    std::wstring setup = Join(dir, W("ReShade_Setup_" + version + "_Addon.exe"));
    if (!Exists(setup))
    {
        std::string body, err;
        std::wstring url = W("https://reshade.me/downloads/ReShade_Setup_" + version + "_Addon.exe");
        if (progress) progress("Downloading ReShade " + version + " (with add-on support)");
        if (!HttpGet(url, body, err, [&](size_t got, size_t total) {
                if (progress && total) progress("Downloading ReShade " + version + " (" + std::to_string(got * 100 / total) + " %)");
            }, L"Referer: https://reshade.me/\r\n"))
            return "Download of ReShade " + version + " failed: " + err;
        if (body.size() < 100000 || body[0] != 'M' || body[1] != 'Z') return "reshade.me didn't send the ReShade " + version + " setup.";
        std::ofstream(fs::path(setup), std::ios::binary).write(body.data(), (std::streamsize)body.size());
    }
    if (progress) progress("Unpacking ReShade " + version);
    {
        std::string all = ReadText(setup), dll;
        for (auto n : { "ReShade64.dll", "ReShade32.dll" })
            if (ZipExtract(all, n, dll)) std::ofstream(fs::path(Join(dir, W(n))), std::ios::binary).write(dll.data(), (std::streamsize)dll.size());
    }
    if (!Exists(Join(dir, L"ReShade64.dll"))) Extract(setup, dir);
    if (!Exists(Join(dir, L"ReShade64.dll")))
    {
        // tar didn't take the exe: let .NET's zip reader try a copy named .zip
        std::wstring zip = Join(dir, L"setup.zip");
        CopyFileW(setup.c_str(), zip.c_str(), FALSE);
        Extract(zip, dir);
        DeleteFileW(zip.c_str());
    }
    if (!Exists(Join(dir, L"ReShade64.dll")) || !InfoHas(ReadDllInfo(Join(dir, L"ReShade64.dll")), "ReShade"))
        return "Couldn't unpack ReShade from its setup. Install it with the setup from reshade.me instead.";
    return {};
}

std::string InstallReShade(Game &g, const std::string &version, std::string *note)
{
    if (g.exe.empty()) return "No game executable.";
    if (g.route == Route::Vulkan)
        return "Vulkan games: ReShade installs as a system-wide Vulkan layer; use the setup from reshade.me for those.";
    std::wstring dir = DirOf(g.exe);
    std::wstring cache = ReShadeCacheDir(version);
    std::wstring src = Join(cache, g.bits == 32 ? L"ReShade32.dll" : L"ReShade64.dll");
    if (!Exists(src)) return "ReShade " + version + " isn't downloaded yet.";
    std::wstring dest;
    std::string oldV;
    if (g.reshade) { dest = g.reshadeDll.path; oldV = g.reshadeDll.version; }    // change version: same file
    else
    {
        std::wstring name;
        if (g.optiscaler) name = g.bits == 32 ? L"ReShade32.dll" : L"ReShade64.dll";   // OptiScaler loads it (LoadReshade=true)
        else if ((g.apis & ApiD3D9) && !(g.apis & (ApiD3D11 | ApiD3D12))) name = L"d3d9.dll";
        else if ((g.apis & ApiGL) && !(g.apis & (ApiD3D9 | ApiD3D11 | ApiD3D12))) name = L"opengl32.dll";
        else name = L"dxgi.dll";
        dest = Join(dir, name);
        if (Exists(dest))
        {
            DllInfo d = ReadDllInfo(dest);
            std::string what = d.product.empty() ? std::string("another program") : d.product;
            return U8(name) + " in the game folder belongs to " + what + ". Remove it first, or install ReShade with its own setup.";
        }
    }
    Txn t(g);
    std::string err = CopyIn(t, src, dest);
    t.Finish();
    if (!err.empty()) return err;
    Log(oldV.empty() ? "install ReShade" : "change ReShade", g.name, dest, oldV, version);
    if (g.optiscaler && !g.reshade && note)
        *note = "ReShade copied as " + U8(NameOf(dest)) + ". Set LoadReshade=true in OptiScaler.ini so OptiScaler loads it.";
    Detect(g);
    return {};
}

void OpenInExplorer(const std::wstring &path, bool select)
{
    if (select)
    {
        std::wstring args = L"/select,\"" + path + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }
    else ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

} // namespace bfi
