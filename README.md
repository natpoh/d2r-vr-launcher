# VR Mod Launcher (draft)

A shared launcher for the two Diablo II: Resurrected VR mods — **D2R VR** (BodyWalkVR) and **D2R PCVR** (bearstonem/d2r-pcvr) — and any
third mod with a manifest. It keeps each mod's package in its own folder, puts the **complete** setup of the chosen mod into the game
folder (plugin, ini, shaders, textures, ReShade configuration, an OpenXR loader beside the exe), keeps every mod's settings per game, and
starts the game with the chosen mod and, per start, the chosen OpenXR runtime.

**Status: a draft that is meant to move to its own repository.** The core is written and tested; there is no window yet.
Read [SPEC.md](SPEC.md) first.

```
cmake -S . -B build -G "Visual Studio 18 2026" -A x64         # any generator; C++17, no dependencies, no network
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure          # 9 ctest tests: 8 test programs and the schema check
python tools/demo.py build/Release/vrmodlauncher.exe <work folder>    # a walk through a FAKE game folder
```

| Folder | |
|---|---|
| `src/` | the library (`vrml`) and the command line (`vrmodlauncher`) |
| `schema/` | `vrmod-manifest.1.schema.json` — the manifest contract |
| `examples/` | the manifests of D2R VR 0.165.0 and D2R PCVR 0.3.13 |
| `tests/` | unit tests on a fake game folder, the manifest fixtures (`ok_*`, `bad_*`, `semantic_*`) |
| `tools/` | `check_schema.py` (schema ⇄ validator parity), `demo.py` |

Commands: `list | status | register <manifest> | unregister <id> | activate <id> | deactivate <id|group> | verify | recover | import <id> |
requirements <id> | launch <id> | deps verify <zip>` — see `vrmodlauncher` without arguments and SPEC.md §6.

Nothing here touches a real game folder unless you point `--game` at one; `activate` changes the files the manifests name and nothing else, and refuses while the game runs.
