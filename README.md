# Modern Minimap Framework

A round HUD minimap for Skyrim SE/AE (SKSE plugin). Towns show as straight-down photos, the wilderness as your own terrain LOD, interiors as a room outline from the navmesh. Includes an in-game town photo bake, so the photos can match your own load order.

Author: itsPigeon2000. License: GPL-3.0-or-later (see LICENSE), as required by CommonLibSSE NG.

## Building

1. Visual Studio 2022 (C++ desktop workload) with vcpkg, CMake and Ninja.
2. Put [CommonLibSSE NG](https://github.com/alandtse/CommonLibSSE-NG) 13.1.0 (tag v13.1.0, unmodified) in `lib/commonlibsse-ng`.
3. Run `build.ps1`. The plugin is written to `build/release/ModernMinimap.dll`.

## Layout

| Folder | What it holds |
|---|---|
| `src/PhotoBake` | In-game town photo bake |
| `src/Minimap` | Drawing the minimap on the HUD |
| `src/Terrain` | Reading terrain LOD meshes |
| `src/Interiors` | Room outline for interiors |
| `src/Icons` | Markers, doors, quest targets, actors |
| `src/Settings` | The ini and the SKSE Menu Framework page |
| `tests` | Offline checks (ModernMinimapTests.exe) |

`include/SKSEMenuFramework.h` is the SKSE Menu Framework API header by Thiago099 (MIT).
