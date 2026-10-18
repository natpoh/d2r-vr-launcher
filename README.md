# D2R VR Launcher

A launcher for the two Diablo II: Resurrected VR mods — **D2R VR Authentic** (BodyWalkVR) and **D2R VR Unleashed** (bearstonem's d2r-pcvr) —
and any other D2RLoader mod. Both VR mods are **D2RLoader mods** (`<game>\mods\D2R-VR`, `<game>\mods\D2R-PCVR`): D2RLoader starts one mod per game start
(`D2RLoader.exe -mod <name>`), so **choosing the mod is the whole switch and the launcher moves no files**. It finds the game (Steam, Battle.net, or the folder you
pick), checks D2RLoader, lists the mods installed in that game, and starts the one you choose with the OpenXR runtime you choose for that start (the system's setting is never changed).

Read [docs/MOD_MANIFEST.md](docs/MOD_MANIFEST.md) first: it is the contract between the launcher and a mod's installer (`<game>\mods\<Name>\vrmod-manifest.json`).
No registration step: a mod's installer writes its manifest into its own mod folder; the launcher scans. The two VR mods have built-in profiles, so an install
without a manifest still shows up.

```
cmake -S . -B build -G "Visual Studio 18 2026" -A x64         # C++17, no third-party code (WinHTTP and ShellExecute are Windows'); the network is used only by `releases` / `install` and the window's install view and update check
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure         # no test touches the internet or a real game folder
vrmodlauncher mods --game "<game folder>"                     # what the launcher sees there
vrmodlauncher launch d2r-pcvr --game "<game folder>" --runtime vdxr [--dry-run]
vrmodlauncher releases [--game "<game folder>"] [--json]       # the newest D2R VR Authentic / Unleashed, and whether an update is available
vrmodlauncher install loader|authentic|unleashed --game "<game folder>" [--dry-run] [--no-run] [--accept-unverified]
installer\build_launcher_installer.ps1 -BuildDir build\Release  # D2R_VR_Launcher_Setup_<version>.exe, per user, no UAC
```

| Folder | |
|---|---|
| `src/` | the library (`vrml`): `mods` (discovery, D2RLoader check, runtimes, a start, settings), `launch`, `manifest`, `game`, `process`; `net`, `zip`, `install`, `releases`, `setup_run`, `install_run` (installing from the launcher; `install_run` runs the three pieces one after the other for the window and `--install-run`); the command line (`vrmodlauncher`); the window (`D2RVRLauncher`: `gui_model`, `gui_install_model`, `gui_window`, `gui_install_view`) |
| `docs/MOD_MANIFEST.md` | the contract for a mod's installer |
| `docs/INSTALL_PATH.md` | installing D2RLoader and the two mods from the launcher: sources, proof, rights, what is missing |
| `schema/`, `examples/d2rloader-mods/` | the manifest schema; the manifests of the two mods (also the launcher's built-in profiles) |
| `installer/` | the launcher's own Inno Setup script, its build script and a test that runs it for real in a scratch folder |
| `tests/` | unit tests on a fake game folder, the manifest fixtures (`ok_*`, `bad_*`, `semantic_*`) |
| `tools/` | `check_schema.py` (schema ⇄ validator parity) and the tools of the older swap design |

## Installing from the launcher: `releases` and `install`

The launcher can install or update the three pieces itself: **D2RLoader** (the pinned 1.3.1-beta ZIP, checked against the two reviewed archive hashes and the three reviewed file
hashes, unpacked by the launcher's own ZIP reader, with backups, the player's `d2rloader.toml` and `mods\` kept, and a rollback if a move fails), **D2R VR Authentic** and **D2R VR Unleashed**
(each mod's own Setup, downloaded over https from its own host, proven by a published SHA-256 when there is one, started - elevated for Authentic - and followed by a look at whether the mod is
now in that game folder). `--dry-run` says what would be downloaded from where and what it needs, and stops. Where each piece comes from, how it is proven, what needs administrator rights,
and what is still missing: [docs/INSTALL_PATH.md](docs/INSTALL_PATH.md). The code: `src/net` (WinHTTP, https only, host list, size cap, cancel), `zip`, `install`, `releases`, `setup_run`, `cli_install`;
tests: `zip`, `net`, `install`, `releases`, `setup_run`, `cli_install` and `tools/install_smoke.py` (the real binary against a local web server). The window does the same through its install view (below).

## The older design: swapping files (still in the code)

Before D2RLoader's mod mode the two mods put the same file names into the same places of the game folder, and the launcher swapped the complete setup of one for
the other in a transaction (`activate`, `deactivate`, `verify`, `recover`, `import`, `import-settings`, `register`; [SPEC.md](SPEC.md)). That engine still works,
is tested (`engine_test`, `tools/e2e_switch.py` on a copy of a real install) and is what a mod without a `d2rloader_mod` manifest would use; the window does not use it.

Nothing here touches a real game folder unless you point `--game` at one; a start refuses while the game runs.

## The window: `D2RVRLauncher.exe`

Plain Win32 and GDI+ (nothing to install; icon, pictures, manifest and version are in the exe). Built as the CMake target `d2rvrlauncher` on Windows.
It lists the D2RLoader mods of a game folder as cards (D2R VR Authentic and D2R VR Unleashed in the order they were installed on this computer - the older of the creation times of
`mods\<folder>` and its `modinfo.json`, so an update does not move a mod; mods that are not installed follow the installed ones; equal or unreadable times: Unleashed before Authentic;
then any other D2RLoader mod; `vrmodlauncher mods` prints the same order), starts the chosen one with the
OpenXR runtime picked for that start (never changing the system setting), and offers the mod's own settings (a big button after Play, named `settings_app.label` or "<mod name> Settings"),
Logs from the mod's manifest; **Uninstall** is a button on the card of each installed D2R VR mod (0.3.1: it runs the mod's own uninstaller after a question that names the mod, never while the game runs; Unleashed's without `/SILENT` so its own question shows; a copy with no uninstaller, e.g. from a ZIP, gets the words for removing it by hand; a D2RLoader mod the launcher does not know has no button; the launcher never deletes a mod folder). What it decides is in
`src/gui_model.*` (tested without a window in `tests/gui_model_test.cpp`); `src/gui_window.cpp` only shows it; `src/gui_main.cpp` is `--play` and `--install-run`.

**Installing and updating (0.3.0).** "Install / update mods..." opens the **install view** (`src/gui_install_view.cpp`, its decisions in `src/gui_install_model.*`): D2RLoader, D2R VR Authentic and D2R VR Unleashed, each
with what is installed, what is newest (asked on a worker thread: "checking..." never freezes the window), where it comes from and how it is proven, and a box that is ticked when the piece is missing or outdated.
*Install selected* downloads, checks the hash and installs in turn (D2RLoader first; the two mods through their own Setups, which show their own wizards), with a bar, a step line and a log; *Cancel* stops at the next
safe point (a Setup wizard that is open is the player's to finish or close). A release without a published checksum (today Authentic's) is installed only after a question whose default is No. A card whose mod has a newer
release shows an amber "Update available: 0.167.0 -> 0.168.0" and an **Update** button (**Install** when the mod is not installed); the check is one request per source once per start, silent on failure, and the box
"Check for updates when the launcher starts" (on by default) turns it off. On a first run with no VR mod the view is offered once; the launcher's own Setup offers it on its finish page. All of it, with the pictures
`docs/screens/install_view_100pct.png`, `install_view_result_100pct.png` and `launcher_update_badges_100pct.png`: [docs/INSTALL_PATH.md](docs/INSTALL_PATH.md), "The window".

```
D2RVRLauncher.exe [--game DIR] [--home DIR] [--select MOD_ID] [--dry-run]
D2RVRLauncher.exe --install-view [--install-tick loader,authentic,unleashed]    the window with the install view opened right away (the card's Install / Update ticks one piece)
D2RVRLauncher.exe --play [--dry-run] --game DIR     no window: do Play once, print the outcome, exit 0 or the refusal's code (10 game running, 11 not installed / no D2RLoader,
                                                    24 runtime, 25 not a game); also written to <home>\logs\gui_last.txt
D2RVRLauncher.exe --screenshot FILE.png             render the window (with --install-view: the view) to a PNG and exit   (test aids: --dpi N, --size WxH, --no-search, --processes LIST,
                                                    --no-update-check, --shot-now)
# test aids that reach a web server: only with VRML_ALLOW_HTTP_HOST=127.0.0.1:PORT (a loopback test server), else exit 2
D2RVRLauncher.exe --install-run loader,authentic,unleashed --game DIR [--accept-unverified] [--pin-file F]     no window: what Install selected does; one line per piece, exit 0 or the first failure's code
```

The window stays open after a start unless "Close the launcher when the game starts" is ticked (remembered in settings.json). `python tools/ui_smoke.py <exe> <work folder>`
starts the exe against a fake game folder and drives it (screenshots at 96 to 192 dpi, dry runs, a real start of a stand-in mod, Enter, close-on-start); `python tools/install_view_smoke.py --exe <exe> --fake-setup <fake_setup.exe> --work <folder>`
does the same for the install view and the update badges against a local web server and stand-in Setups (no internet); screenshots: `docs/screens/`.
