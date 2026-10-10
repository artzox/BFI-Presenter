# Third-party headers

Vendored so both parts build without extra downloads. Only headers are included; nothing here is linked as a library.

| Folder | What | Licence |
|---|---|---|
| `reshade/include/reshade*.hpp` | ReShade add-on API headers (6.4), © Patrick Mours | BSD-3-Clause OR MIT |
| `reshade/include/imgui.h`, `imconfig.h` | Dear ImGui 1.91.8-docking headers, © Omar Cornut — the version ReShade 6.4's overlay API expects | MIT |
| `vulkan-headers/` | Khronos Vulkan headers (`vulkan/`, `vk_video/`) | Apache-2.0 (see each file's SPDX line) |
| `mingw-shim/` | Two one-line headers (`Windows.h`, `Unknwn.h`) that forward to MinGW's lower-case names, for building on case-sensitive file systems | — |

**Local change:** in the ReShade headers, event callbacks are cast with `reinterpret_cast<void *>` instead of `static_cast<void *>` so they compile with GCC/MinGW. Behaviour is unchanged.
