# Third-party headers

Vendored so every part builds without extra downloads. The add-on and the Vulkan layer use headers only; the manager app compiles Dear ImGui from `imgui/`.

| Folder | What | Licence |
|---|---|---|
| `reshade/include/reshade*.hpp` | ReShade add-on API headers (6.4), © Patrick Mours | BSD-3-Clause OR MIT |
| `reshade/include/imgui.h`, `imconfig.h` | Dear ImGui 1.91.8-docking headers, © Omar Cornut — the version ReShade 6.4's overlay API expects | MIT |
| `vulkan-headers/` | Khronos Vulkan headers (`vulkan/`, `vk_video/`) | Apache-2.0 (see each file's SPDX line) |
| `imgui/` | Dear ImGui 1.91.9 sources and the Win32 / Direct3D 11 backends, © Omar Cornut — compiled into the manager app (`manager/`) | MIT (`imgui/LICENSE.txt`) |
| `mingw-shim/` | Two one-line headers (`Windows.h`, `Unknwn.h`) that forward to MinGW's lower-case names, for building on case-sensitive file systems | — |

**Local change:** in the ReShade headers, event callbacks are cast with `reinterpret_cast<void *>` instead of `static_cast<void *>` so they compile with GCC/MinGW. Behaviour is unchanged.
