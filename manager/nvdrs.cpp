// BFI Presenter Manager -- NVIDIA driver profile settings (G-SYNC, Vertical Sync) through NVAPI DRS.
// Copyright (C) 2026 artzox. SPDX-License-Identifier: GPL-3.0-or-later
//
// NVAPI is reached through nvapi64.dll's nvapi_QueryInterface; the structures, function ids and setting ids
// below are those of NVIDIA's public NVAPI SDK (github.com/NVIDIA/nvapi, MIT): nvapi.h, nvapi_interface.h,
// NvApiDriverSettings.h.
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <mutex>
#include "nvdrs.h"

namespace bfi {

std::string U8(const std::wstring &w);

namespace {

// NVAPI matches profiles best with the full path in the form c:/folder/app.exe: games whose profile names the
// exe with its folder (Burnout Paradise) or tells apart games by a file next to the exe (Dead Space 2008 vs
// the 2023 remake, same dead space.exe) are only found that way.
std::wstring NvPath(const std::wstring &path)
{
    std::wstring p = path;
    for (auto &c : p) { if (c == L'\\') c = L'/'; c = (wchar_t)towlower(c); }
    return p;
}

std::wstring FileName(const std::wstring &path)
{
    size_t k = path.find_last_of(L"\\/");
    std::wstring n = k == std::wstring::npos ? path : path.substr(k + 1);
    for (auto &c : n) c = (wchar_t)towlower(c);
    return n;
}

typedef uint32_t NvU32;
typedef uint64_t NvU64;
typedef int NvStatus;
typedef void *NvSession;
typedef void *NvProfile;
typedef uint16_t NvUStr[2048];

const NvStatus NV_OK = 0, NV_SETTING_NOT_FOUND = -160, NV_PROFILE_NOT_FOUND = -163, NV_PROFILE_NAME_IN_USE = -164,
               NV_EXECUTABLE_NOT_FOUND = -166, NV_INVALID_USER_PRIVILEGE = -137;

const NvU32 VSYNCMODE_ID = 0x00A879CF;
const NvU32 VRR_APP_OVERRIDE_ID = 0x10A879CF;
const NvU32 VRR_APP_OVERRIDE_REQUEST_STATE_ID = 0x10A879AC;

#pragma pack(push, 4)
struct NvBinary { NvU32 len; uint8_t data[4096]; };
struct NvSetting {
    NvU32 version;
    NvUStr settingName;
    NvU32 settingId;
    NvU32 settingType;       // 0 = DWORD
    NvU32 settingLocation;   // 0 current profile, 1 global, 2 base, 3 default
    NvU32 isCurrentPredefined;
    NvU32 isPredefinedValid;
    union { NvU32 u32; NvBinary bin; NvUStr wsz; NvU64 u64; } predefined;
    union { NvU32 u32; NvBinary bin; NvUStr wsz; NvU64 u64; } current;
};
#pragma pack(pop)
struct NvApplication {     // NVDRS_APPLICATION_V4
    NvU32 version;
    NvU32 isPredefined;
    NvUStr appName, userFriendlyName, launcher, fileInFolder;
    NvU32 flags;
    NvUStr commandLine;
};
struct NvProfileInfo {     // NVDRS_PROFILE_V1
    NvU32 version;
    NvUStr profileName;
    NvU32 gpuSupport;
    NvU32 isPredefined, numOfApps, numOfSettings;
};
inline NvU32 Ver(size_t size, NvU32 v) { return (NvU32)size | (v << 16); }

typedef void *(__cdecl *QueryFn)(NvU32);
struct Api {
    bool ok = false;
    NvStatus (__cdecl *Initialize)() = nullptr;
    NvStatus (__cdecl *CreateSession)(NvSession *) = nullptr;
    NvStatus (__cdecl *DestroySession)(NvSession) = nullptr;
    NvStatus (__cdecl *LoadSettings)(NvSession) = nullptr;
    NvStatus (__cdecl *SaveSettings)(NvSession) = nullptr;
    NvStatus (__cdecl *FindApplicationByName)(NvSession, NvUStr, NvProfile *, NvApplication *) = nullptr;
    NvStatus (__cdecl *GetSetting)(NvSession, NvProfile, NvU32, NvSetting *) = nullptr;
    NvStatus (__cdecl *SetSetting)(NvSession, NvProfile, NvSetting *) = nullptr;
    NvStatus (__cdecl *DeleteProfileSetting)(NvSession, NvProfile, NvU32) = nullptr;
    NvStatus (__cdecl *CreateProfile)(NvSession, NvProfileInfo *, NvProfile *) = nullptr;
    NvStatus (__cdecl *CreateApplication)(NvSession, NvProfile, NvApplication *) = nullptr;
    NvStatus (__cdecl *GetProfileInfo)(NvSession, NvProfile, NvProfileInfo *) = nullptr;
    NvStatus (__cdecl *GetBaseProfile)(NvSession, NvProfile *) = nullptr;
};

Api &GetApi()
{
    static Api api;
    static std::once_flag once;
    std::call_once(once, [] {
        HMODULE m = LoadLibraryW(sizeof(void *) == 8 ? L"nvapi64.dll" : L"nvapi.dll");
        if (!m) return;
        auto q = (QueryFn)GetProcAddress(m, "nvapi_QueryInterface");
        if (!q) return;
        auto get = [&](NvU32 id) { return q(id); };
        api.Initialize = (decltype(api.Initialize))get(0x0150E828);
        api.CreateSession = (decltype(api.CreateSession))get(0x0694D52E);
        api.DestroySession = (decltype(api.DestroySession))get(0xDAD9CFF8);
        api.LoadSettings = (decltype(api.LoadSettings))get(0x375DBD6B);
        api.SaveSettings = (decltype(api.SaveSettings))get(0xFCBC7E14);
        api.FindApplicationByName = (decltype(api.FindApplicationByName))get(0xEEE566B2);
        api.GetSetting = (decltype(api.GetSetting))get(0x73BF8338);
        api.SetSetting = (decltype(api.SetSetting))get(0x577DD202);
        api.DeleteProfileSetting = (decltype(api.DeleteProfileSetting))get(0xE4A26362);
        api.CreateProfile = (decltype(api.CreateProfile))get(0xCC176068);
        api.CreateApplication = (decltype(api.CreateApplication))get(0x4347A9DE);
        api.GetProfileInfo = (decltype(api.GetProfileInfo))get(0x61CD6FD6);
        api.GetBaseProfile = (decltype(api.GetBaseProfile))get(0xDA8466A0);
        api.ok = api.Initialize && api.CreateSession && api.DestroySession && api.LoadSettings && api.SaveSettings &&
                 api.FindApplicationByName && api.GetSetting && api.SetSetting && api.DeleteProfileSetting &&
                 api.CreateProfile && api.CreateApplication && api.GetProfileInfo && api.GetBaseProfile &&
                 api.Initialize() == NV_OK;
    });
    return api;
}

void ToNv(NvUStr dst, const std::wstring &s)
{
    memset(dst, 0, sizeof(NvUStr));
    for (size_t i = 0; i < s.size() && i < 2047; i++) dst[i] = (uint16_t)s[i];
}

std::wstring FromNv(const NvUStr s)
{
    std::wstring w;
    for (size_t i = 0; i < 2048 && s[i]; i++) w += (wchar_t)s[i];
    return w;
}

std::string Err(const char *what, NvStatus st)
{
    if (st == NV_INVALID_USER_PRIVILEGE) return std::string(what) + ": NVIDIA settings can only be changed with administrator rights.";
    return std::string(what) + " failed (NVAPI " + std::to_string(st) + ").";
}

struct Session {
    Api &a;
    NvSession s = nullptr;
    NvStatus st = -1;
    explicit Session(Api &api) : a(api)
    {
        st = a.CreateSession(&s);
        if (st == NV_OK) st = a.LoadSettings(s);
    }
    ~Session() { if (s) a.DestroySession(s); }
};

// Value in effect for the profile (own or inherited from the global profile); -1 when the driver default applies.
long long Read(Api &a, NvSession s, NvProfile p, NvU32 id, bool &own)
{
    NvSetting *st = new NvSetting();
    st->version = Ver(sizeof(NvSetting), 1);
    NvStatus r = a.GetSetting(s, p, id, st);
    long long v = -1;
    own = false;
    if (r == NV_OK) { v = st->current.u32; own = st->settingLocation == 0; }
    delete st;
    return v;
}

} // namespace

bool NvAvailable() { return GetApi().ok; }

std::string NvRead(const std::vector<std::wstring> &exeNames, std::vector<NvState> &out)
{
    out.assign(exeNames.size(), NvState());
    Api &a = GetApi();
    if (!a.ok) return "No NVIDIA driver found.";
    Session ses(a);
    if (ses.st != NV_OK) return Err("Reading NVIDIA settings", ses.st);
    NvProfile base = nullptr;
    a.GetBaseProfile(ses.s, &base);
    bool own;
    long long gVrr = base ? Read(a, ses.s, base, VRR_APP_OVERRIDE_ID, own) : -1;
    long long gVs = base ? Read(a, ses.s, base, VSYNCMODE_ID, own) : -1;
    NvApplication *app = new NvApplication();
    NvProfileInfo *pi = new NvProfileInfo();
    for (size_t i = 0; i < exeNames.size(); i++)
    {
        NvState &o = out[i];
        o.known = true;
        o.vrr = gVrr;
        o.vsync = gVs;
        memset(app, 0, sizeof(*app));
        app->version = Ver(sizeof(NvApplication), 4);
        NvUStr name;
        ToNv(name, NvPath(exeNames[i]));
        NvProfile p = nullptr;
        if (a.FindApplicationByName(ses.s, name, &p, app) != NV_OK || !p)
        {
            memset(app, 0, sizeof(*app));
            app->version = Ver(sizeof(NvApplication), 4);
            ToNv(name, FileName(exeNames[i]));
            if (a.FindApplicationByName(ses.s, name, &p, app) != NV_OK) p = nullptr;
        }
        if (!p) continue;
        memset(pi, 0, sizeof(*pi));
        pi->version = Ver(sizeof(NvProfileInfo), 1);
        if (a.GetProfileInfo(ses.s, p, pi) == NV_OK) o.profile = FromNv(pi->profileName);
        o.vrr = Read(a, ses.s, p, VRR_APP_OVERRIDE_ID, o.vrrOwn);
        o.vsync = Read(a, ses.s, p, VSYNCMODE_ID, o.vsyncOwn);
        if (!o.vrrOwn) o.vrr = gVrr;
        if (!o.vsyncOwn) o.vsync = gVs;
    }
    delete app;
    delete pi;
    return {};
}

std::string NvWrite(const std::wstring &exeName, const std::wstring &profileName, long long vrr, long long vsync)
{
    Api &a = GetApi();
    if (!a.ok) return "No NVIDIA driver found.";
    Session ses(a);
    if (ses.st != NV_OK) return Err("Opening the NVIDIA settings", ses.st);
    NvApplication *app = new NvApplication();
    app->version = Ver(sizeof(NvApplication), 4);
    NvUStr name;
    ToNv(name, NvPath(exeName));
    NvProfile p = nullptr;
    NvStatus r = a.FindApplicationByName(ses.s, name, &p, app);
    if (r != NV_OK || !p)
    {
        memset(app, 0, sizeof(*app));
        app->version = Ver(sizeof(NvApplication), 4);
        ToNv(name, FileName(exeName));
        r = a.FindApplicationByName(ses.s, name, &p, app);
    }
    std::string err;
    if (r != NV_OK || !p)
    {
        // no profile has this exe: make one, named after the game
        NvProfileInfo *pi = new NvProfileInfo();
        for (int attempt = 0; attempt < 3 && !p; attempt++)
        {
            memset(pi, 0, sizeof(*pi));
            pi->version = Ver(sizeof(NvProfileInfo), 1);
            std::wstring n = profileName.empty() ? FileName(exeName) : profileName;
            if (attempt == 1) n += L" (BFI Presenter)";
            if (attempt == 2) n = FileName(exeName);
            ToNv(pi->profileName, n);
            r = a.CreateProfile(ses.s, pi, &p);
            if (r != NV_OK && r != NV_PROFILE_NAME_IN_USE) break;
        }
        delete pi;
        if (!p) { delete app; return Err("Creating an NVIDIA profile", r); }
        memset(app, 0, sizeof(*app));
        app->version = Ver(sizeof(NvApplication), 4);
        ToNv(app->appName, FileName(exeName));
        r = a.CreateApplication(ses.s, p, app);
        if (r != NV_OK) { delete app; return Err("Adding the game to its NVIDIA profile", r); }
    }
    delete app;
    auto set = [&](NvU32 id, long long v) -> NvStatus {
        if (v == -2) return NV_OK;
        if (v == -1)
        {
            NvStatus d = a.DeleteProfileSetting(ses.s, p, id);
            return d == NV_SETTING_NOT_FOUND ? NV_OK : d;
        }
        NvSetting *st = new NvSetting();
        st->version = Ver(sizeof(NvSetting), 1);
        st->settingId = id;
        st->settingType = 0;
        st->current.u32 = (NvU32)v;
        NvStatus s = a.SetSetting(ses.s, p, st);
        delete st;
        return s;
    };
    // G-SYNC is two settings in the driver (the app override and its requested state); NVIDIA Profile
    // Inspector writes both, so do the same.
    if ((r = set(VRR_APP_OVERRIDE_ID, vrr)) != NV_OK) err = Err("Setting G-SYNC", r);
    else if ((r = set(VRR_APP_OVERRIDE_REQUEST_STATE_ID, vrr)) != NV_OK) err = Err("Setting G-SYNC", r);
    else if ((r = set(VSYNCMODE_ID, vsync)) != NV_OK) err = Err("Setting Vertical Sync", r);
    if (err.empty() && (r = a.SaveSettings(ses.s)) != NV_OK) err = Err("Saving the NVIDIA settings", r);
    return err;
}

bool NvNeedsAdmin(const std::string &err) { return err.find("administrator") != std::string::npos; }

std::string VrrText(long long v)
{
    switch (v)
    {
    case -1: return "default (on)";
    case kVrrAllow: return "on";
    case kVrrForceOff: return "off";
    case 2: return "disallowed";
    case 3: return "ULMB";
    case kVrrFixedRefresh: return "fixed refresh";
    default: return "?";
    }
}

std::string VsyncText(long long v)
{
    switch (v)
    {
    case -1: case kVsyncApp: return "app setting";
    case kVsyncForceOn: return "force on";
    case kVsyncForceOff: return "force off";
    case kVsyncFast: return "fast sync";
    case 0x32610244: return "1/2 refresh";
    case 0x71271021: return "1/3 refresh";
    case 0x13245256: return "1/4 refresh";
    default: return "?";
    }
}

} // namespace bfi
