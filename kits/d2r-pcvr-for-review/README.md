# D2R VR Launcher: a small file for D2R VR Unleashed (for review)

Hello bearstonem,

this folder is a proposal, not a request: please read it, change anything you like, or say no. Nothing here touches your repository or
your players' machines unless you decide to take it.

## What the D2R VR Launcher is

A small Windows launcher for the two Diablo II: Resurrected VR mods that are D2RLoader mods: **D2R VR Authentic** (`mods\D2R-VR`)
and your **D2R VR Unleashed** (`mods\D2R-PCVR`). The player picks a mod and an OpenXR runtime (the system's, SteamVR, Virtual Desktop,
or a runtime file) in one window and presses Play. Choosing a mod is just `D2RLoader.exe -mod <name>`: the launcher moves no files,
changes no settings and never changes the system's active OpenXR runtime (the choice is handed to the game it starts as
`XR_RUNTIME_JSON`, for that start only). It finds mods by looking at `<game>\mods\*\`.

## The one thing your installer would add

One file, written next to the rest of your mod, inside your own mod folder:

```
<game>\mods\D2R-PCVR\vrmod-manifest.json        (+ launcher_icon.png only if you ever want a picture)
```

For the mod itself nothing else: no registry key, no Steam change, nothing outside `mods\D2R-PCVR`, nothing in another mod's folder
(carrying the launcher is a separate, optional step, described below). It goes with the mod:
`install.py` journals it like every other file, so a restore or removal takes it away. Your mod does not need the launcher and the
launcher does not need the file: **without it the launcher already lists your mod** with a built-in profile made from the same data
(so installs from before and ZIP installs show up too). The file only lets you own the values: name, version, logs, update source.
Keys that start with `x-` are notes the launcher ignores.

`vrmod-manifest.json` here was made from your files at `a0a4f5f` (v0.3.19) by a script that only reads them with `git show`
(mod_identity.py, launch_windows.py, prepare_dev_scenarios.py, game_install.py, install.py, the Inno script, your evidence file for the
D2RLoader hashes, your docs); it stops if one of them stops saying what it said. `x-source` names the commit; **`x-unverified` lists
everything we could not check** (we ran none of your code, no game, no headset).

Read again at v0.3.19 (we first looked at v0.3.17), for this file:
- the installer, plugin and build script agree on 0.3.19, and the manifest's version follows them (the proposal adds a test and a line to
  step 2 of `docs/RELEASING.md` so it is not forgotten);
- the installer now also prepares the Arcane Sanctuary sky from the player's own game (`prepare_arcane_sky.py`). It writes into
  `mods\D2R-PCVR` like the rest, so "nothing outside your mod folder" still holds, and the manifest is one more journaled file beside it;
- your wrapper has a developer flag, `--frame-dev` (`Launch-Dev-SteamVR.cmd`), that stages a second mod folder, `mods\pcvr-dev`. The
  normal start, `-mod D2R-PCVR`, is unchanged. The launcher has no developer option (question 9);
- there is still no `vrmod-manifest.json` in your repository.

## What the launcher does at Play (D2R VR Unleashed)

1. Refuses while `D2R.exe` or `D2RLoader.exe` runs, when `D2RLoader.exe` is missing, or when `mods\D2R-PCVR\D2R-PCVR.mpq\modinfo.json` is not there.
2. Warns (does not block) when the game build is not 3.3.93787 / 3.3.93847, or when D2RLoader's three files are not the 1.3.1-beta ones whose
   SHA-256 your `steamvr/evidence/2026-10-13-loader-archive-repack.json` lists.
3. Starts `D2RLoader.exe -mod D2R-PCVR` in the game folder with the rules of your `launch_windows.py`: `SteamOverlayGameId` removed; Steam:
   `SteamAppId` = `SteamGameId` = 2536520; Battle.net: both removed.
4. Runtime as in your wrapper: none of its flags (system), `--frame-openxr-runtime=<SteamVR>\steamxr_win64.json`, `--frame-vdxr`
   (`Virtual Desktop Streamer\OpenXR\virtualdesktop-openxr.json`) or any manifest file. A runtime that is not found stops the start.
5. Sets `VRMOD_ACTIVE_ID`, `VRMOD_ACTIVE_VERSION`, `VRMOD_RUNTIME`, `VRMOD_STORE`, `VRMOD_GAME_BUILD` for the game (information only; ignore them if you like).
6. Takes `RESHADE_BASE_PATH_OVERRIDE` out of the game's environment, and warns when the player's own Windows environment has it. A player with both mods
   on one PC (2026-10-18) ended up with one mod's ReShade folder in the other mod's game: ReShade refused to start (error 1114) and the headset stayed empty.
   As far as we can see your plugin sets the variable itself, inside the game process, which is exactly right. What we ask of any Setup or wrapper of either
   mod: never persist it (no `setx`, no `HKCU\Environment`, no Steam launch option that exports it), and have the plugin ignore an inherited value before it
   loads its ReShade (ours is being changed to do the same). The launcher cannot clean a game started some other way (Steam's launch option, a shortcut).

## The launcher inside your installer (optional)

The player should end up with the launcher installed whichever Setup they run first: the launcher's own Setup, D2R VR Authentic's, or yours.
So a mod's Setup may carry the launcher's Setup and install it silently, then install only its own mod. Your Setup needs the launcher for
nothing, and the launcher needs your Setup for nothing. The exact rules, the same for both mods:

1. **Where the file comes from.** The unmodified `D2R_VR_Launcher_Setup_<version>.exe` that the launcher's releases publish, with its `.sha256`
   (https://github.com/natpoh/d2r-vr-launcher/releases, about 3 MB). Your release build fetches it once per release, checks the hash and
   **pins** version and SHA-256 by passing `-LauncherSetup <file> -LauncherSetupSha256 <64 digits>` to `build_native_installer.ps1`. The build
   refuses another file name or a hash that differs, puts the file into the payload as `launcher\<name>`, and the installer checks the hash
   again before it runs the file. Nothing is downloaded when the player installs.
2. **Detect.** `HKCU\Software\D2R VR Launcher`, string values `InstallDir` and `Version` (`x.y.z`), written by the launcher's Setup. The program is
   `<InstallDir>\D2RVRLauncher.exe`. The launcher counts as installed when `Version` is there **and** the program exists. Your Setup is per user,
   so this is the installing user's own hive.
3. **Version rule.** Dotted numbers, up to four parts, a missing part is 0 (`0.2` equals `0.2.0.0`). Run the carried Setup only when the launcher is
   **missing or older** than the carried one. Never downgrade (the launcher's silent Setup also stops by itself when a newer launcher is installed).
4. **Silent switches, exactly:** `D2R_VR_Launcher_Setup_<version>.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /TASKS=` (`/TASKS=` means no desktop
   shortcut, the Start menu one stays). It needs no administrator rights and shows nothing. Run it as the user who started your Setup.
5. **The exit code is ignored.** Log it and go on. A launcher that fails to install, or cannot even be started, must never block or fail the mod's
   install. It runs first, then your mod is installed as before.
6. **Never uninstall the launcher.** Your uninstaller removes your mod's files (the manifest with them) and leaves the launcher: the other mod
   or the player may use it.
7. **Manifest and icon.** Your installer writes `mods\D2R-PCVR\vrmod-manifest.json` (and the picture it names, if you add one) with the mod. Nothing
   in another mod's folder.

`install-patch-proposal.diff` (part 2) does exactly this and nothing more: your wizard, your backend and your Steam handling stay as they are, and
without the two build parameters your installer is built as before. We left your finish page alone; the contract also allows an unchecked
"Open D2R VR Launcher" there, which is not in the patch.

## How to test, without starting the game

Take a game folder with your mod installed, copy `vrmod-manifest.json` into `mods\D2R-PCVR\`, then with the draft command-line tool
(Serg can send you a build):

```
vrmodlauncher --game "<game folder>" mods
vrmodlauncher --game "<game folder>" launch d2r-pcvr --runtime vdxr --dry-run
```

The first prints the game, D2RLoader and one line per mod; yours should read
`* d2r-pcvr  D2R VR Unleashed  0.3.19  [installed, manifest]` (`built-in` instead of `manifest` without the file). The second prints the
exact command line and environment and starts nothing; add `--store battlenet` to see the Battle.net variant.

## What we would like you to decide

1. **Name.** The window shows `DISPLAY_NAME` ("D2R VR Unleashed") and `TAGLINE` plus a full stop. Right as is?
2. **Icon.** No Unleashed artwork exists and we did not invent any: the launcher shows a neutral placeholder. If you want a picture, put a
   PNG or ICO in `mods\D2R-PCVR` and add `"icon": "launcher_icon.png"`.
3. **Explorer hand-off.** Your wrapper always hands a Steam start to Explorer's desktop, so Steam's overlay injection does not reach the
   loader. The launcher starts the game from its own desktop process, never from Steam's launch options, and does not use the hand-off.
   Is that enough? If not, we can port yours (the draft launcher does not have it yet).
4. **Direct start.** May the launcher start `D2RLoader.exe -mod D2R-PCVR` itself instead of your Python wrapper? What it does not do: keep
   other arguments Steam adds after `D2R.exe`, set the Steam shortcut's OpenVR flag, write `pcvr-launch.log`.
5. **Uninstall button.** It would run `{localappdata}\Programs\D2R PCVR\unins000.exe` without `/SILENT`, so your own confirmation
   ("remove from every Steam and Battle.net copy") appears. Fine? (The ZIP setup has no such file.)
6. **Logs and update.** Is the list in `logs` right (where does D2RLoader write the plugin's `d2r-pcvr-vrcam.log` for a mod plugin, and
   `ReShade.log`)? Are your GitHub releases marked pre-release, and is `v*-windows-preview` the tag pattern to keep?
7. **Where it lives.** `install-patch-proposal.diff` puts `vrmod-manifest.json` at your repo root, beside `d2r_pcvr.ini`, and adds a test that
   its version follows `D2RPCVR_VERSION`. Another place or another way to keep the version in step is fine with us.
8. **Carrying the launcher.** Would you take part 2 of the patch, so your Setup installs the launcher when it is missing or older? The pin
   (version and SHA-256 of the launcher's Setup) would then be yours to move forward at each release. It also runs in your silent `/CONFIG`
   installs, which is the contract; say if you want a switch to skip it there.
9. **Developer mode.** `--frame-dev` stages `mods\pcvr-dev` (its own `modinfo.json`, a copy of the plugin and settings). The launcher lists
   any mod folder that has a `modinfo.json`, so once that folder exists it shows up as a plain D2RLoader mod and would be started with
   `-mod pcvr-dev` alone, without your wrapper's checks. Should the launcher hide `pcvr-dev`, show it as it is, or would you add a
   `vrmod-manifest.json` there that says what it is?

## What is in this folder

- `vrmod-manifest.json`: the file itself.
- `install-patch-proposal.diff`: the changes to your repo, in two parts. Part 1 ships the manifest (`steamvr/install.py`, the two file lists,
  two tests, one line in `docs/RELEASING.md`). Part 2 carries the launcher as above (`installer/build_native_installer.ps1`,
  `stage_native_installer.py` with its test, `D2R_PCVR_Setup.iss`, a paragraph in `installer/README.md`). The Arcane sky and the developer
  code are not touched. **Not applied and your tests not run**; written against a0a4f5f and checked only to apply without fuzz to copies of
  those files, for the patched Python and PowerShell to parse, and for the patched Inno script to compile with a dummy payload.
- This README. Nothing of yours was changed, run or contacted.

Thank you for looking at it. Whatever you decide, D2R VR Unleashed keeps working with the launcher through the built-in profile.
