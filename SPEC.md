# VR Mod Launcher — draft specification (v0.1, 2026-10-15)

A shared launcher that installs and switches **D2R VR** (BodyWalkVR) and **D2R PCVR** (bearstonem/d2r-pcvr) — and any third mod
that brings a manifest — in one Diablo II: Resurrected folder, without conflicts. This is a **draft**: the core (manifest,
registry, the switch transaction, recovery, launch plans, hash checks) is written and tested in this folder; the window, the real
D2RLoader download and the Explorer start are described here and not built.

Status words: **settled** = agreed with the other author in his reply of 2026-10-15; **proposed** = ours, to be agreed;
**not verified** = could not be checked from the files at hand.

Contents: 1 Settled · 2 Layout and state · 3 Manifest · 4 The switch (transaction) · 5 Update, uninstall, import · 6 Installer contract (CLI) ·
7 Launching · 8 Dependencies · 9 What the launcher shows and logs · 10 The prototype and its tests · 11 Not in the prototype · 12 Risks

---

## 1. What is settled

| # | Agreed (the other author's reply) | Where it lands |
|---|---|---|
| 1 | The **launcher** downloads and verifies D2RLoader (a verified download is reused by every mod, but the files in the game are still checked against what the *selected* mod needs), checks the game build, activates the chosen mod, launches it | §8, `deps.*`, `requires[]`, §7 |
| 2 | Explorer-desktop launch handling and the **per-launch OpenXR runtime** (`XR_RUNTIME_JSON`: SteamVR or VDXR, the system-wide runtime untouched) move into the launcher. Steam and Battle.net start differently; **each mod declares the launch options it supports** | §7, `launch.options[]`, `launch.*` |
| 3 | A mod's **installer only puts its package in its own folder and registers it**. Settings, calibration and preparation stay with the mod (d2r-pcvr extracts item icons and potion models from the player's own game; those caches stay part of that mod) | §6, `files[].role = generated` |
| 4 | The manifest describes **all** files a mod needs — plugin, ini, shaders, textures, ReShade configuration, an OpenXR loader beside the exe. Switching activates the **complete** setup and **keeps each mod's settings** | §3, §4 |
| 5 | The shared launcher folder is **outside the game folder**, with **separate state per game installation** | §2 |
| 6 | Switching only while the game is closed; **recoverable** when interrupted; leaves unrelated files alone; updating or uninstalling either mod keeps the other; **import** of existing installations and their backups | §4, §5 |
| 7 | The launcher and the logs show the active mod, its version, the game build and the runtime, for support | §9 |
| 8 | He offers to share his launch and installer code | §7 (his `launch_windows.py` is the model), §13 |

Roles after the answer:

| | Launcher | Mod's installer | The mod itself |
|---|---|---|---|
| Put files in the game folder | **yes** (activate / deactivate) | no | no |
| Keep the package | no | **yes** (`<home>/mods/<id>/`) | – |
| Register / unregister | does it | **asks for it** (`register`, `unregister`) | – |
| D2RLoader (download, hash, unpack) | **yes** | no (tells the launcher it needs it: `requires`) | – |
| ReShade, BodyWalk, Steam shortcuts, video settings | no | **yes** (the mod's own business; listed in `external[]`) | – |
| Settings, calibration, caches | keeps each mod's settings file per game; never reads it | seeds a first one | **yes** |
| Game build / D2RLoader / precondition checks | **yes** (from the manifest) | – | may add its own |
| Starting the game | **yes** (runtime, store, Explorer) | – | `--start` helper if it needs one (D2R VR) |
| Identity for support | shows it, logs it | writes it into the manifest | prints it in its own log (env `VRMOD_*`) |

---

## 2. Layout and state

One launcher folder per Windows user, **outside every game folder**; one sub-folder of state per game installation, named by the
first 16 hex digits of SHA-256 of the lower-cased, normalised absolute game path (`GameKey`). `VRMODLAUNCHER_HOME` or `--home` move the
whole thing (tests, portable installs). Default: `%LOCALAPPDATA%\VRModLauncher\`.

```
%LOCALAPPDATA%\VRModLauncher\
  VRModLauncher.exe, launcher.json        the program and its version (`--install-self`, later)
  registry.json                           which mods are registered, their versions, previous versions
  mods\<id>\manifest.json                 the package a mod's installer put here
  mods\<id>\payload\...                   its files (the layout is the manifest's `src` paths)
  deps\<dep>\<file>                       verified downloads shared by every mod (D2RLoader zip)
  logs\launcher.log                       one line per event (support)
  games\<key>\                            one per game installation
      state.json                          which mod is active in each exclusive group, and exactly what was written for it
      journal.json                        only while a switch runs (or died)
      store\<id>\<game-relative path>     the mod's own settings for THIS game (its `config` files), captured when it is switched away
      foreign\<tx>\<n>                    a file that was in the way when a mod was activated: given back when that mod goes
      backups\<tx>\<n>                    what a switch took out (the last 3 switches are kept)
      imports\<id>\...                    backups collected by `import`
```

`state.json` (shape):

```json
{ "schema": 1, "path": "D:/SteamLibrary/steamapps/common/Diablo II Resurrected", "store": "steam", "build": "3.3.93787",
  "last_tx": "20261015-101530-0001",
  "active": { "d2r-vr-plugin": { "id": "d2r-pcvr", "version": "0.3.13", "group": "d2r-vr-plugin", "activated_at": "...",
      "applied": [ { "dst": "d2rloader/plugins/d2rl-vrcam.dll", "role": "program", "sha256": "…" },
                   { "dst": "d2rloader/plugins/d2r_vr.ini", "role": "config", "sha256": "…", "foreign": "foreign/…/3", "foreign_sha": "…" },
                   { "dst": "reshade-shaders/Textures/D2R_Sky_ours/D2R_Sky_act1.png", "role": "shared", "sha256": "…", "pre": true } ] } },
  "last_launch": { "mod": "d2r-pcvr 0.3.13", "option": "openxr", "runtime": "vdxr", "at": "..." } }
```

The state records **what was written** (path, role, hash), not what the manifest says now: deactivation works from it, so it works even
after the mod's package changed or vanished.

---

## 3. The manifest, `vrmod-manifest/1`

`<home>/mods/<id>/manifest.json`, UTF-8 JSON. The contract is **`schema/vrmod-manifest.1.schema.json`** (JSON Schema draft-07). The C++
validator (`src/manifest.cpp`) enforces it plus what a schema cannot say; `tools/check_schema.py` (a ctest) runs 36 manifests —
the two examples and 34 fixtures — through both and fails if they disagree. Unknown keys are errors (typos), except `x-…` extension keys.

Placeholders: `{game}` (the game folder). External entries may also use `{localappdata}`, `{appdata}`, `{programfiles}`, `{steam}`, `{saved_games}`.
All swap paths must be under `{game}/`; no `..`, no drive, no `:` stream, no names ending in a dot or space; no link or junction anywhere on the way.

| Field | Meaning |
|---|---|
| `schema`, `id`, `name`, `version` | `"vrmod-manifest/1"`; `[a-z0-9-]{2,40}`; text; `major.minor.patch[-pre]` |
| `edition`, `publisher`, `homepage`, `support`, `license`, `description`, `build{commit,date,channel}` | shown in the window and the support report; `support` = where THIS mod's bugs go (the card's Discord / Support link next to Website) |
| `install_help` | optional (0.3.5): the author's words to the player about installing the mod, shown in its install-view row and card tooltip while it is not installed |
| `game{id, builds[], stores[], processes[]}` | `builds` = the game builds the mod was made for (a warning when the game differs; a `game_build_in` precondition makes it a block); `stores` = `steam`/`battlenet`; `processes` = what must not run while files change (default `D2R.exe`, `D2RLoader.exe`) |
| `exclusive_group` | only one mod per group is active per game. Both VR mods are in `d2r-vr-plugin` (they hook the same game functions). Mods of different groups may be active together and must not share a file |
| `identity{plugin_id, plugin_file, product_name, dll_sha256[], markers[], about_file, log}` | how the launcher recognises this mod's plugin in a game folder (`import`, `verify`): the published hashes, else a marker string inside the DLL (ASCII or UTF-16) |
| `requires[]{id, version, provider, optional, note, files[]{path, sha256_any_of[]}}` | what the game folder must hold. `provider`: `launcher` (it can fetch it: D2RLoader), `mod` (the mod's installer: ReShade, BodyWalk), `user` (Steam, an OpenXR runtime) |
| `files[]` | **everything the mod puts in the game folder** (below) |
| `external[]{dst, note}` | files outside the game folder the mod owns (BodyWalk's Game Link plugin, Steam launch options...). Listed for uninstall and the support report; **never swapped** |
| `settings_app`, `settings_hint` | the mod's settings program, or `null` + a hint ("in VR: left grip + …") |
| `launch.options[]` | how the mod is started (§7) |
| `hooks{pre_activate, post_activate, pre_deactivate, post_deactivate}` | `{exe, args, timeout_s}`; a failing `pre_*` stops the switch before anything changes, a failing `post_*` is a warning |
| `preconditions[]` | `process_absent`, `file_exists`, `file_sha256_any_of`, `toml_value`, `game_build_in`; `severity` block/warn; `applies_to` of install, repair, activate, deactivate, start |
| `import{backups{globs[], record{file, paths_at}}}` | where the old installation's backups are (§5) |
| `logs[]`, `update{kind,url,repo,tag,opens}`, `uninstall{exe,args,needs_admin}` | the support zip; where a newer version is announced (the launcher only opens the page); the mod's own uninstaller |

### `files[]`: roles

| `role` | Activate | Deactivate | Used for |
|---|---|---|---|
| `program` | copied in; hash checked | taken out (into the backup) | plugin DLL, settings exe, OpenXR loader, the FlatVR add-on |
| `asset` | copied in | taken out | shaders, textures that only this mod has, licence |
| `config` | the settings **saved for this game** if there are any, else the `seed`, else nothing | the live file is **captured** into `store/<id>/…`, then taken out | `d2r_vr.ini`, `ReShade.ini`, `D2R_Frame_ReShade.ini`: the player's settings, kept per mod and per game |
| `shared` | copied in, or left if the same bytes are there | **left in place** | pictures that two mods ship byte for byte (the 25 sky/crosshair pictures are identical in both mods) |
| `generated` | – | – | made while the mod runs (logs, caches, `d2r_vr_icons\`, `pcvr-install.json`): never touched; listed for the support zip and a purge |

`src` is relative to the manifest's folder; a trailing `/` makes a folder entry (every file below it, recursively; `dst` must end with `/` too).
`optional: true` = a missing `src` is fine (a flat-only package without the VR files). `sha256` = the package file must have it.

### Examples

* **`examples/d2r-vr/manifest.json`** — D2R VR 0.165.0: plugin `d2rl-vrcam.dll`, `D2R_VR_Settings.exe`, `D2R_DepthFog.fx`, the sky pictures (24 common
  + 12 ceilings only this mod has), `FlatVR_DepthProvider.addon64` and `FlatVR_Keepalive.addonfx` (VR, optional), `d2r_vr.ini` / `ReShade.ini` /
  `ReShadePreset.ini` as `config`; BodyWalk's Game Link plugin, profile and manifest and the portable BodyWalk as `external`;
  requires D2RLoader 1.3.1-beta (file hashes measured on the development machine's installed copy), game build 3.3.93787; launch `--start`.
* **`examples/d2r-pcvr/manifest.json`** — D2R PCVR 0.3.13 **from his installer** (`install.py`, `launch_windows.py`, `setup_native.py`): plugin,
  `openxr_loader.dll` **beside the exe**, shader, sky pictures (both places), `d2r_vr.ini` / `ReShade.ini` / `D2R_Frame_ReShade.ini` as `config`,
  caches as `generated`, Steam launch options and shortcuts, video settings and his helper folder as `external`; launch through `D2RLoader.exe`
  with SteamVR or VDXR chosen per start.

**Fields of his manifest that could not be verified** (the file lists them in `x-unverified` too):

| Field | Why not verified |
|---|---|
| `identity.dll_sha256` | no build of his was measured |
| `identity.markers` | the string is in his source (`vrcam.cpp:8146`), not checked in a binary |
| `identity.about_file` | a proposal: his plugin does not write one yet |
| `identity.plugin_id` | today `d2r-vr-vrcam`, the same as D2R VR's [verified in his source]; a unique id is proposed |
| `files[].src` (the `payload/…` layout) and the `seed` files | a proposal: today `install.py` writes the files itself and builds the inis from `d2r_vr.ini` plus a table of forced values |
| `requires[reshade].files` | the DLL's own hash was not measured (the pin in `install.py` is the hash of the *setup EXE*) |
| `launch.options[0]` | derived from `launch_windows.py`; not run |
| `uninstall.exe` | the default Inno name inside his `{app}` |
| `game.builds[1]` | 3.3.93847 is his experimental Battle.net build |

---

## 4. The switch as a transaction

`activate <id>` makes `<id>` the active mod of its exclusive group in one game. Written as *plan → journal → apply → verify → commit*;
`src/engine.cpp`.

**Refusals first** (nothing is read or written beyond looking): the game folder has no `D2R.exe` (`E_GAME`); the game, D2RLoader or a process
the mod names runs (`E_GAME_RUNNING`; the check is a `ProcessProbe`, the tests use a stand-in); a journal from an earlier crash exists
(`E_JOURNAL`: `recover` first); the mod's store is not supported by the game folder; the package no longer matches its manifest; a link or junction
lies on a path to be written; a folder is where a file goes; another group's active mod owns the same file (`E_COLLISION`); a blocking precondition
fails (`E_PRECONDITION`); a `pre_*` hook fails (`E_HOOK`).

**1. Plan** (pure: reads the disk, hashes files, changes nothing; `--dry-run` prints it). The list of operations:

| op | does | undo |
|---|---|---|
| `capture` | copy a live `config` file into `store/<old>/…` (the previous saved copy is kept aside first) | restore the previous saved copy, or delete the new one |
| `backup` | copy a file into `backups/<tx>/n` (or `foreign/<tx>/n` for a file that is not ours), verify the copy, delete the original | copy it back |
| `install` | copy a payload / saved-settings / seed file to its place through `<dst>.vrml-tmp` and a rename; check the hash | delete it if it has the expected hash |
| `restore` | copy a foreign file back to where it was | delete it if it has the expected hash |

The plan for a switch from A to B: for every file A wrote, in reverse order — `config`: capture + back up; `program`/`asset`: back up (a changed one is
warned about, kept); `shared`: left; then the foreign file A had pushed aside is restored — **unless B's file sits on the same path, in which case the
duty to give it back passes to B** (no pointless restore-and-hide). Then for every file B names: nothing there → install; the same bytes there → recorded
as `pre` (not ours, never removed); other bytes there → moved to `foreign/` and recorded; `config` → the saved settings, else the seed. A **refresh**
(the active mod's package changed) rewrites only the files whose hash changed, removes files the new manifest dropped, adds new ones, and never
touches `config` files that are already in place.

**2. Journal.** `journal.json` (atomic write) holds the whole plan before the first operation. **3. Apply** runs the operations in order; the test hook
can "die" before or after any operation or between a copy's temporary file and its rename. **4. Verify**: every installed file has the hash the plan said.
**5. Commit**: `state.json` is replaced atomically (this is the commit point), the journal is deleted, files that were given back are deleted from `foreign/`,
old `backups/` beyond the last 3 are dropped.

**Failure inside Apply** (an exception, a hash mismatch — a virus scanner ate a file, the package changed): the same operations are undone in reverse, the
journal is deleted, the game folder is as it was (`tests/engine_test.cpp: an_error_in_the_middle_undoes_itself`, `a_damaged_payload_is_caught_and_undone`).
A rollback that itself fails keeps the journal and says so (`E_ROLLBACK`).

**Recovery** (`recover`, and automatically before any command that changes or trusts a game folder): journal present and `state.json.last_tx == journal.tx` →
the commit happened, finish the clean-up (*rolled forward*); otherwise undo every operation in reverse (every undo is idempotent and safe for an operation that
never ran), remove the temporary files, delete the journal and the transaction's backup folders (*rolled back*). The game folder is then **exactly** as before — the
test compares every file's SHA-256 — and the switch can be tried again. Recovery refuses while the game runs.

**Unrelated files**: the engine only ever names paths the manifests list. `tests/engine_test.cpp: unrelated_files_are_never_touched` hashes `D2R.exe`,
`D2RLoader.exe`, `d2rloader.toml`, another plugin's DLL, `ReShade64.dll` and the saves' `Settings.json` across four switches and a deactivation.

**Settings are kept per mod and per game**: the live ini is captured into `store/` when the mod goes and comes back when it returns; each mod starts from its own seed.
**Same names, different mods** (`d2rl-vrcam.dll`, `d2r_vr.ini`, `D2R_DepthFog.fx`, `ReShade.ini` in both) are the normal case, not an error; a collision is only an error between *groups*.

---

## 5. Update, uninstall, import

* **Update of a mod.** Its installer replaces `mods/<id>/` and runs `register` again (same id, new version; the registry remembers the old version).
  *Not active*: the game folder is not touched at all (tested); its saved settings stay and are used at the next activation, with the new package.
  *Active*: `status`/`verify` say "newer package registered"; `activate <id>` is the refresh described in §4. (An installer that wants it at once runs `activate <id>`.)
* **Uninstall of a mod.** `unregister <id>`: not active anywhere → just the record (the package folder is the installer's, or `--purge`). Active somewhere →
  `E_IN_USE`; with `--force` the mod is deactivated in every game first (the games must be closed), foreign files are given back, then unregistered.
  Its saved settings stay unless `--purge-settings`. The other mod is untouched (tested both orders).
* **A mod's own uninstaller** (Inno `unins000.exe`, his Python backend) must call `unregister --force` first and must stop deleting files by wildcard
  in the game folder — today D2R VR's `[UninstallDelete]` removes `d2r_vr*` and d2r-pcvr's restore plan deletes D2RLoader/ReShade files it brought.
* **Import** (`import <id>`): an installation made before the launcher. Identity check first: the plugin in the game folder must be a build of `<id>` (its published hash,
  else a marker string in the DLL; `--force` skips it) — a plugin of the other mod gives `E_IDENTITY`. Then, **without moving a file**: every file the manifest names that
  is there is recorded as owned (hash = what is there now, so an older build is fine), `config` files are copied into `store/` (an earlier saved copy is kept in
  `imports/<id>/previous-settings/`), and the state says `<id>` is active. `activate <id>` afterwards brings the package's build in. The mod's **backups** are
  *copied* (originals stay) into `imports/<id>/`: files by glob (D2R VR's `d2r_vr.ini.before_*`, `d2rl-vrcam.dll.before_*`, `…/*.before_*` copies) and the folders
  a record file lists (d2r-pcvr's `d2rloader/pcvr-install.json`, key `snapshots`: his `setup-backups\game\<timestamp>` folders). A listed backup that is gone is reported, not an error.
  `verify` also warns when a registered mod's plugin sits in the game folder but the mod is not active (`UNMANAGED_MOD_FILES`): that is the old-installer or mixed-install case.
* **Import of settings only** (`import-settings <id> --from DIR`): the `config` files of `<id>` found under `DIR` (a folder laid out like the game folder - a backup the player made, or a snapshot an
  installer left) become the settings saved for this game and come back at the next `activate <id>`. Nothing in the game folder is touched; an earlier saved copy is kept in `imports/<id>/previous-settings/`;
  refused (`E_IN_USE`, exit 12) while `<id>` is active (its live files are its settings). This is how the player's own D2R VR settings survive a d2r-pcvr install made over them.


---

## 6. Installer contract (the CLI an installer calls)

```
vrmodlauncher [--home DIR] [--game DIR] [--json] <command>
  register <manifest>        validate the manifest and the package in <home>/mods/<id>/, record it            (installer: last step)
  unregister <id> [--force] [--purge] [--purge-settings]                                                     (uninstaller: first step)
  status                     launcher, game (store, build, D2RLoader), active mods, pending work             (--json for scripts)
  list | activate <id> [--dry-run] | deactivate <id|group> | verify | recover | import <id> | import-settings <id> --from DIR | requirements <id>
  launch <id> [--option ID] [--runtime steamvr|vdxr|system|file] [--runtime-file F] [--store steam|battlenet] [--dry-run]
  deps verify <zip>          is this file the reviewed D2RLoader archive
```

An installer's steps: (1) put the package in `<home>/mods/<id>/` (`manifest.json` + the `src` files); (2) `register <home>/mods/<id>/manifest.json`; (3) optionally
`activate <id> --game <dir>` (a first mod is activated at once; a second asks the player). An uninstaller: `unregister <id> --force [--purge]`, then its own cleanup.
Exit codes: 0 ok · 1 verify found errors · 2 usage · 3 not found · 4 invalid manifest or package · 10 the game runs · 11 a precondition blocks (or a hook failed) · 12 a decision is needed
(collision, in use, wrong identity) · 13 newer schema · 20 I/O error, undone · 21 rollback incomplete · 22 an unfinished switch needs `recover` · 23 hash mismatch · 24 OpenXR runtime ·
25 not a game folder. Errors print `error [CODE]: message` on stderr.

The launcher's own installation (`--install-self`: install if absent, upgrade if older, **skip if the same or newer**, never downgrade; the bundled exe decides, so the rule lives in one place) is
specified in the concept document §7.4 and not built.

---

## 7. Launching

`launch <id>` = make `<id>` active (if the game is closed and it is not), then start it. The plan is built by `BuildLaunchPlan` (pure, tested):

* **Option**: `--option`, else the first that supports the game's store. `launch.options[].stores` says which of Steam / Battle.net a mod supports; the store comes
  from the game folder (`Data\.build.config` = Steam, `.build.info` = Battle.net — the test d2r-pcvr's `game_install.py` makes) or `--store`.
* **Runtime of this start** (`runtimes[]`: `system`, `steamvr`, `vdxr`, `file`): the player's choice or the option's first. `steamvr` →
  `<Steam library>\steamapps\common\SteamVR\steamxr_win64.json` (libraries from `libraryfolders.vdf`); `vdxr` →
  `<Program Files>\Virtual Desktop Streamer\OpenXR\virtualdesktop-openxr.json`; `file` → the given manifest. The result is **only** `XR_RUNTIME_JSON` in the child's
  environment: the system-wide active runtime is never changed. `system` ("System default") **removes** an inherited `XR_RUNTIME_JSON` from the child's
  environment, so the game uses the runtime Windows has set, as the window shows. A missing runtime or one the option does not offer is `E_RUNTIME` (nothing starts).
* **Environment rules** (`env[]`, per store; unset then set, in order). d2r-pcvr's rules as data: `SteamOverlayGameId` removed always; Steam edition: `SteamAppId`,
  `SteamGameId` replaced by `2536520`; Battle.net: both removed.
* **`via`**: `direct`, `explorer`, or `auto` = Explorer when Steam started the launcher (`SteamGameId` / `SteamOverlayGameId` in the environment). The reason is d2r-pcvr's finding: a loader
  started inside Steam's injected process chain crashed with `0xC0000005` before D2RCore loaded. **Explorer hand-off protocol** (proposed, a port of his `launch_from_desktop`):
  the launcher writes the `LaunchPlan` as `request.json` in a temporary folder, asks Explorer's desktop (`Shell.Application` → `ShellExecute`) to start
  `vrmodlauncher launch-child <request.json>`, and waits for `status.json` `{state: running|finished, pid, result, error}`; the child (a clean environment, not Steam's) starts the game with the
  plan's environment. `LaunchPlan::ToJson/FromJson` and the plan are tested; the Explorer call is not written.
* **Support variables** in every child: `VRMOD_ACTIVE_ID`, `VRMOD_ACTIVE_VERSION`, `VRMOD_GAME_BUILD`, `VRMOD_STORE`, `VRMOD_RUNTIME`. The mods print them in their own logs.
* **Steam shortcuts in a headset** (Steam Frame, Quest): a library entry can run `vrmodlauncher launch d2r-pcvr --runtime steamvr`, so the right mod is active whatever was last used.
  It replaces d2r-pcvr's Python wrapper in the D2R launch options for starts that come from the launcher; a plain Steam *Play* stays the game without a mod (the launcher touches no Steam file).

D2R VR's option is `{game}\D2R_VR_Settings.exe --start` (a new switch: the START button without a window — D2RLoader.exe, BodyWalk and FlatVR as the ini asks): the launcher does
not learn BodyWalk. The runtime the player picked reaches it as `XR_RUNTIME_JSON` in the environment.

---

## 8. Dependencies: D2RLoader

`deps.h`: `Pin{id, version, url, file, sha256[]}`; `D2RLoaderPin()` = `https://d2rloader.net/downloads/D2RLoader-1.3.1-beta.zip` with **two** accepted archive hashes —
`f7bf0287…71e2` (current) and `9286c6b5…aad7` (before the author's repack of 2026-10-13: same payload bytes, new archive hash). Both mods' installers carry exactly these two; a test reads
`installer/D2R_VR_Setup.iss` and `tools/settings/settings.cpp` and fails if they differ from `deps.cpp`. `DepsCache::Ensure(pin)`:

1. a cached file with an accepted hash → reused (no network, any mod);
2. a cached file with another hash → deleted, fetched again;
3. a fetched file with another hash → **deleted, `E_HASH`, nothing is installed** (tested with a stand-in `Downloader`);
4. no cache and no downloader → `E_NOT_FOUND`.

The network (`Downloader`), the unpack (an allow-list of names like d2r-pcvr's: `D2RLoader.exe`, `D2RCore.dll`, `d2rloader.mpq`, `mods\`, `d2rloader\`) and the toml edit
(`allow_global_extensions`) are **not written**. After a download the files in the game are still checked against each mod: `requires[].files[].sha256_any_of`
(`requirements <id>`). The three file hashes in D2R VR's example are of the 1.3.1-beta files installed on the development machine; **confirm them against a fresh unpack of the reviewed ZIP before pinning**.

---

## 9. What the launcher shows and logs

**Window** (not built; sketch):

```
┌ VR Mod Launcher ─────────────────────────────────────────────────────────────────────┐
│ Game  [ D:\SteamLibrary\steamapps\common\Diablo II Resurrected   ▾ ]  Steam edition   │
│       build 3.3.93787 ✔   D2RLoader 1.3.1-beta ✔   ReShade ✔                          │
│                                                                                       │
│  (●) D2R VR      0.165.0  full     BodyWalkVR       [Settings] [Logs]                 │
│  ( ) D2R PCVR    0.3.13   preview  bearstonem       [Logs]                            │
│       ⚠ files of D2R PCVR are in the game folder (an old install): [Take over]        │
│                                                                                       │
│  Runtime  ( ) SteamVR   (●) Virtual Desktop   ( ) system's          [ START D2R VR ]  │
│  [ Switch to D2R PCVR ]   [ About / copy report ]   [ Verify ]                         │
│  newer version of D2R PCVR announced: 0.3.14 — open its page                          │
└───────────────────────────────────────────────────────────────────────────────────────┘
```

The runtime row shows what the active mod's launch option offers. **Report** (`status` + `verify` text, one paste for a bug report): launcher version, game path / store / build /
D2RLoader, active mod + version + edition, the live plugin's SHA-256, `verify` findings, the last start (mod, option, runtime, time), the mod's `support` URL.

**Log** (`<home>/logs/launcher.log`, one line each): `registered d2r-pcvr 0.3.13 (replaces 0.3.12)`, `activate d2r-pcvr (from d2r-vr) in <game>: 15 operation(s), tx …`, `committed <tx>`,
`recover: …`, `switch failed and was undone: …`, `deps: D2RLoader-1.3.1-beta.zip reused from the cache (hash ok)`, `started d2r-pcvr 0.3.13 (option openxr, runtime vdxr, store steam, build 3.3.93787)`.
The mod's own log is expected to print its identity from `VRMOD_ACTIVE_*` (and, proposed, its plugin id/name/version unique per mod: D2RLoader's log line should say which mod it loaded).

---

## 10. The prototype and its tests

`vr_launcher/` — C++17, standard library only, no network. Build: `cmake -S vr_launcher -B build -G "Visual Studio 18 2026" -A x64`, `cmake --build build --config Release`,
`ctest --test-dir build -C Release` (tests use a fake game in a temp folder, `VRML_TEST_DIR` moves it). `tools/demo.py` walks the example manifests through a fake game and prints
what a player's support log would show.

| Source | What |
|---|---|
| `json`, `sha256`, `util` | the JSON value/parser/writer, SHA-256 (NIST vectors), paths and atomic file helpers |
| `manifest` | parse + validate + package check, `CompareVersions` |
| `registry` | `mods/<id>/manifest.json` + `registry.json`, `register`/`unregister` |
| `engine` | plan, journal, apply, verify, recover, import, remove, status |
| `deps`, `launch`, `game`, `process`, `cli` | the D2RLoader pin and cache, launch plans, store/build detection, the process probe, the command line |

Tests (`tests/`, 9 ctest targets, 93 cases; Release and Debug both green): `json_test` 9 · `sha256_test` 6 · `manifest_test` 11 · `deps_test` 8 · `launch_test` 10 · `cli_test` 8 · `examples_test` 3 ·
**`engine_test` 37**, of which: a crash at **41 points** (before and after each of the 13 operations, between each of the 13 copies' temp file and rename, before the commit, after it) —
each followed by "the next start refuses and `verify` says so", "`recover` refuses while the game runs", "`recover` puts the game folder and the launcher folder back **byte for byte**", "the switch then works and
the tuned settings were never lost"; an error (not a crash) in the middle undoes itself; a damaged payload is caught by the hash and undone; refusal while `D2R.exe`, `D2RLoader.exe`, a differently-cased name or a
process only the mod names runs (the game folder is unchanged); two mods with colliding names and a switch both ways; settings kept per mod; foreign and identical files; a link in the way; wrong store;
update of the active and of the inactive mod; uninstall of the inactive and of the active mod (both orders, the other keeps its saved settings); two games with separate state; `verify` after tampering; import of a legacy
installation with `.before_*` copies and of a record-listed backup chain; hooks in order and their failure; preconditions; retention of backups.
`schema_parity` (Python): schema ⇄ C++ validator on 36 manifests.

**End to end on the real installs** (`tools/e2e_switch.py`, 59 checks, 2026-10-15): on a copy of the mod files of a real game folder (D2R VR's installer, then d2r-pcvr 0.3.15 installed over it -
the real state of the author's machine) it makes d2r-pcvr's package from the copy and his own record (`tools/make_pcvr_package.py`: `pcvr-install.json` → the snapshot's `manifest.json` lists the 741 files his
installer put in), registers it and D2R VR's package (made by D2R VR's `build_installer.ps1`), refuses `import d2r-vr`, imports d2r-pcvr, saves D2R VR's own settings from a backup (`import-settings`),
switches to D2R VR and back, launches each with a stand-in `D2RLoader.exe` (D2R VR through `D2R_VR_Settings.exe --start`, d2r-pcvr with his Steam environment) and checks that the copy is byte for byte the starting
point after the way back, that every byte of it was always in the game copy or the launcher's folder, that each mod's settings stayed its own, and that a running `D2R.exe` stops the switch. The real folder is only read.


---

## 11. Not in the prototype

The window; the real D2RLoader download, unpack and toml edit; the Explorer hand-off start; Steam discovery beyond reading `libraryfolders.vdf`; `--install-self` and the launcher's own update;
the update check (`update{}` is parsed, never fetched); reading a plugin DLL's VERSIONINFO or `about_file` (`identity` uses hashes and markers); hooks execution (an interface, tested with a recorder;
the real runner is not written); `generated` purge; long paths over 260 characters; non-Windows process probe (nothing is ever "running").

## 12. Risks

| Risk | Why | What the draft does about it |
|---|---|---|
| A crash between the copy and the commit | any copy-based switch | the journal and the idempotent undo; tested at 41 points; recovery before any command |
| Antivirus quarantines a file we write (Defender flagged the official D2RLoader on one PC) | new exe that writes DLLs into a game | writes through a temporary name and verifies the hash; a mismatch is undone and reported; no injection, no network in the core |
| A mod's own old-style installer or uninstaller writes or deletes the hot paths behind our back | both of today's do | `verify` (MODIFIED, MISSING), `activate` repairs from the package; `import`; both installers must stop writing there (§5) |
| Settings of two mods share names (`[gameplay]`, `[calibration]` mean different things) | same `d2r_vr.ini` | one saved file per mod and game, never merged |
| `config` captured while a mod's settings program is writing | live file | the game and the settings program are in `processes`; the capture is a copy |
| Admin vs user: D2R VR's installer is admin, d2r-pcvr's is per-user; `{localappdata}` under over-the-shoulder elevation is another account's | Inno | the launcher runs as the user; open question §13 |
| The manifest is a new contract two authors keep | schema drift | JSON Schema file + C++ validator + parity test + `x-` extensions + `schema` version in the file |
| An unverified field of his manifest is wrong | §3 table | `x-unverified`; the importer's identity check refuses a wrong plugin |
| The `exclusive_group` idea is too coarse for a third kind of mod | – | groups are free-form; different groups may be active together and may not share a file |

## 13. Open questions for the other author (ready to paste)

> 1. **Plugin identity.** D2RLoader's log prints `vrcam 0.152.0 by BodyWalkVR` for your 0.3.13 (the id `d2r-vr-vrcam`, name `vrcam`, author, and version in `PluginInfo` are our 0.152 values). May we agree a unique id/name/author and the real version for your plugin (e.g. `d2r-pcvr-vrcam`), plus a VERSIONINFO? That fixes "which mod is this" even without the launcher.
> 2. **Package layout.** Is it OK that your installer puts a `manifest.json` and a `payload\…` tree in `%LOCALAPPDATA%\VRModLauncher\mods\d2r-pcvr\` and runs `vrmodlauncher register`, and that the *launcher* writes the files into the game folder? Or do you want to keep your own `put()` + snapshots for the game-folder files for now?
> 3. **Seeds.** The manifest needs the recommended `d2r_vr.ini`, `ReShade.ini` and `D2R_Frame_ReShade.ini` as files (today `install.py` builds them from `d2r_vr.ini` plus a table of forced values). Can your build emit them? Calibration lives in `d2r_vr.ini` `[calibration]`; the launcher would keep your file per game and never merge it with ours.
> 4. **What stays yours.** ReShade (pinned 6.8.0 — what is the *DLL's* SHA-256?), the icon and potion caches (`d2r_vr_icons\`, `d2r_vr_potions\`), `pcvr-install.json`, Steam launch options and shortcuts, `Settings.json`: we list them as `generated`/`external` and never touch them. Right? Is `d2rloader\runtime\d3dcompiler_47.dll` Proton-only?
> 5. **Steam.** If the launcher starts the game, your Python wrapper in D2R's Steam launch options is not needed for starts from the launcher. Shortcuts could run `vrmodlauncher launch d2r-pcvr --runtime steamvr|vdxr`. Which Steam edits remain the installer's job? Should the plain *Play* stay exactly as now?
> 6. **Explorer hand-off.** We port `launch_from_desktop` (request file + `status.json`, 20 s). Is it needed only for the Steam edition, only when Steam started the launcher, or always for Steam VR? Anything beyond removing `SteamOverlayGameId`?
> 7. **Environment per edition.** Confirm: always remove `SteamOverlayGameId`; Steam edition: remove then set `SteamAppId` and `SteamGameId` to `2536520`; Battle.net: remove both.
> 8. **Runtimes.** SteamVR (from app 250820's `steamxr_win64.json`) and VDXR (`…\Virtual Desktop Streamer\OpenXR\virtualdesktop-openxr.json`) — any other runtime you want offered (Oculus/Meta, WMR)?
> 9. **Game builds.** Steam 3.3.93787 and Battle.net 3.3.93847 only? On another build: warn (our default) or refuse?
> 10. **D2RLoader.** We pin your two reviewed archive hashes and would unpack with your allow-list (`mods`, `d2rloader`, `d2rloader.exe`, `d2rcore.dll`, `d2rloader.mpq`). Your install never edits `d2rloader.toml`; ours flips `allow_global_extensions = false` to `true`. Is `true` the shipped default?
> 11. **Uninstall.** `uninstall_windows.py` restores the first snapshot, which also deletes D2RLoader and ReShade files your install brought, and refuses when the first snapshot already held another mod's DLL. Under the launcher the hot-path files are the launcher's: OK to make your uninstall call `unregister --force` and leave D2RLoader/ReShade alone?
> 12. **Backups import.** Is the `snapshots` array of `d2rloader\pcvr-install.json` the complete list of your backup folders, and is its format stable? Anything else we should collect (`managed-installations.json`)?
> 13. **Where it lives.** `%LOCALAPPDATA%\VRModLauncher` (per user). Your installer is per-user; ours is admin today. Who builds and signs the launcher, and is a C++17, dependency-free exe acceptable to you?
> 14. **A leftover.** Your repo still has our `installer/D2R_VR_Setup.iss` and `build_installer.ps1` with our `AppId {8C1F3A52-6D0B-4E47-9B7E-2D5A0C9E4F31}` ("not used by this build"). A build of it would replace our Uninstall entry. Could they go, or get their own AppId?
> 15. **Names.** Ids `d2r-vr` and `d2r-pcvr`, exclusive group `d2r-vr-plugin`, the `mods\` folder inside the launcher folder (it is not the game's `<game>\mods\`). OK? Does a desktop settings program for your mod exist or come (our `D2R_VR_Settings.exe` name is taken)?
