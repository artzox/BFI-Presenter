// BFI Presenter Manager -- NVIDIA driver profile settings (G-SYNC, Vertical Sync) through NVAPI DRS.
// Copyright (C) 2026 artzox. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <vector>

namespace bfi {

// Setting values as NVIDIA Profile Inspector stores them
enum : int { kVrrAllow = 0, kVrrForceOff = 1, kVrrFixedRefresh = 4 };
enum : unsigned { kVsyncApp = 0x60925292, kVsyncForceOff = 0x08416747, kVsyncForceOn = 0x47814940, kVsyncFast = 0x18888888 };
// Vulkan/OpenGL present method (OGL_CPL_PREFER_DXPRESENT)
enum : int { kGlPresentNative = 0, kGlPresentLayered = 1, kGlPresentAuto = 2 };

struct NvState {
    bool         known = false;       // the driver answered for this game
    std::wstring profile;             // its profile ("" if the exe isn't in any profile)
    long long    vrr = -1;            // G-SYNC (VRR_APP_OVERRIDE) value in effect, -1 = driver default
    bool         vrrOwn = false;      // set in the game's own profile (else global / default)
    long long    vsync = -1;          // Vertical Sync value in effect, -1 = driver default
    bool         vsyncOwn = false;
    long long    glPresent = -1;       // Vulkan/OpenGL present method in effect, -1 = driver default (Auto)
    bool         glPresentOwn = false;
};

bool NvAvailable();                                        // an NVIDIA driver with NVAPI is present
std::string NvRead(const std::vector<std::wstring> &exePaths, std::vector<NvState> &out);   // full paths
// vrr / vsync / glPresent: -2 = leave as is, -1 = remove from the game's profile (use global), otherwise the value.
// Creates a profile named profileName for the exe if it has none.
std::string NvWrite(const std::wstring &exePath, const std::wstring &profileName, long long vrr, long long vsync, long long glPresent = -2);
bool NvNeedsAdmin(const std::string &err);                 // the error means "run as administrator"

std::string VrrText(long long v);
std::string VsyncText(long long v);
std::string GlPresentText(long long v);

} // namespace bfi
