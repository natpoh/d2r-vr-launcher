# D2RLoader mods and the launcher (the contract)

Both VR mods are **D2RLoader mods**: D2R VR Authentic is `mods\D2R-VR`, bearstonem's D2R VR Unleashed (d2r-pcvr) is `mods\D2R-PCVR`.
D2RLoader runs one mod per start: `D2RLoader.exe -mod <name>` (or `default_mod` in `d2rloader\config\d2rloader.toml`). So **choosing the mod is the
whole switch: the launcher moves no files.** (The older swap engine - `activate`, transactions, `import` - stays for installs from before this layout.)

## Where a mod is and how the launcher finds it

```
<game>\mods\<Name>\<Name>.mpq\modinfo.json     D2RLoader's own file: { "name": "<Name>", "savepath": "../", "description": "..." }   = "a mod is installed"
<game>\mods\<Name>\vrmod-manifest.json         the launcher's descriptor (this document)        written by the mod's installer, removed with the mod folder
<game>\mods\<Name>\<icon file>                 optional picture the manifest names (`icon`)
<game>\mods\<Name>\d2rloader\plugins\...       the mod's plugin and settings (D2RLoader loads them only while the mod is active)
<game>\mods\<Name>\reshade\...                 the mod's own ReShade (RESHADE_BASE_PATH_OVERRIDE); D2R VR Authentic's 0.168/0.169 global installs still use the ReShade in the game folder
```

There is **no registration step**: the launcher scans `<game>\mods\*\` of the game folders it knows (Steam, Battle.net, the one the player picked).
`<game>\mods\<Name>\vrmod-manifest.json` wins; when a mod folder named `D2R-VR` or `D2R-PCVR` has `modinfo.json` but no manifest, the launcher uses its built-in copy
of the example manifest (`examples/d2rloader-mods/`), so an older install still shows up. Any other D2RLoader mod folder is listed as a plain D2RLoader mod
(started with `-mod`, no VR options). A mod's installer must **never write into another mod's folder** (nor remove it).

## `vrmod-manifest.json`: schema `vrmod-manifest/1` with `d2rloader_mod`

The same schema file as the swap manifests (`schema/vrmod-manifest.1.schema.json`); a manifest with `d2rloader_mod` needs no `exclusive_group` and no `files`
(giving them is an error). Unknown keys are errors except `x-...`. Examples that are checked by the tests: `examples/d2rloader-mods/d2r-vr/vrmod-manifest.json`
and `.../d2r-pcvr/vrmod-manifest.json`.

| key | meaning |
|---|---|
| `schema` | `"vrmod-manifest/1"` |
| `id` | `[a-z0-9-]{2,40}`: `d2r-vr`, `d2r-pcvr` |
| `d2rloader_mod.name` | the mod folder name, exactly: `D2R-VR`, `D2R-PCVR` |
| `name` | the display name: **D2R VR Authentic**, **D2R VR Unleashed** |
| `description` | one line under the name (Authentic: "Diablo II as it is, in VR: the game's own rules, your body and hands."; Unleashed: "Diablo II reimagined for physical, immersive VR.") |
| `version`, `publisher`, `homepage`, `support`, `license` | shown in the window and the support report. `version` is also how the launcher knows **what is installed** (see "What is installed" below): major.minor.patch with an optional `-suffix`; versions are compared as numbers and a suffix is the same base version |
| `icon` | a PNG or ICO next to the manifest (`launcher_icon.png`), shown in the mod's card |
| `game` | `builds` the mod was made for (a warning when the game differs), `stores` (`steam`, `battlenet`), `processes` that must not run when it is started |
| `requires[]` | `d2rloader` with `files[].sha256_any_of` (the three 1.3.1-beta files) and `x-archive` (URL and the two reviewed ZIP hashes) |
| `launch.options[0]` | how Play starts the mod: `exe`, `args` (the `-mod` is in them), `cwd`, `via` (`auto`/`direct`/`explorer`), `stores`, `runtimes` (`system`, `steamvr`, `vdxr`, `file`), `env[]` rules (`set`/`unset`, per store) |
| `settings_app` | `{ "exe": "{game}/D2R_VR_Settings.exe", "label": "D2R VR Authentic Settings", "hint": "..." }`: the big settings button right after Play. `label` is optional (default: the mod's `name` + " Settings"); a mod with no `settings_app` gets a greyed "Settings" |
| `logs[]` | files the support zip and the Logs button open |
| `update`, `uninstall` | where a newer version is announced; the mod's own uninstaller (`{ "exe": ..., "args": [...], "needs_admin": ... }`): the **Uninstall** button on the mod's card runs it after a question that names the mod, refuses while the game runs, and passes no `/SILENT` unless `args` says so; with no uninstaller on the computer the button explains how to remove the mod by hand (the launcher never deletes a mod folder) |

`{game}` is the game folder. The launcher builds one start from the option: the runtime picked for this start becomes `XR_RUNTIME_JSON` in the child's environment (the
system's active runtime is never changed), `VRMOD_ACTIVE_ID`, `VRMOD_ACTIVE_VERSION`, `VRMOD_RUNTIME`, `VRMOD_STORE` and `VRMOD_GAME_BUILD` tell the mod who started it.

## What is installed (which version)

The window's cards, update badges and install rows, `vrmodlauncher mods` and the question to the update servers all use one value, `ModEntry::found_version`, found in this order:

1. the `version` of `<mod>\vrmod-manifest.json` when it validates;
2. the `version` string of a manifest that does **not** validate (a field of a newer or older schema - the launcher reads it leniently from the raw JSON, only when the file's `id` is this mod's and its `d2rloader_mod`, if named, is this folder); the "is not valid" note stays;
3. the ProductVersion in the version resource (VERSIONINFO) of the DLL in `<mod>\d2rloader\plugins` whose **Comments** carry `mod-id=<id>` (`d2r-vr`, `d2r-pcvr`; tokens separated by spaces, `;` or `,`; the file name does not matter). A mod whose Setup writes no manifest
   (today D2R VR Unleashed) should keep `mod-id=` and its version in its plugin's resource: `ProductName "D2R PCVR"`, `ProductVersion "0.3.19"`, `Comments "mod-id=d2r-pcvr; plugin-id=d2r-pcvr-vrcam; ..."`; ours says `mod-id=d2r-vr edition=full`;
4. otherwise the version is unknown (the install view says "version unknown"; the card offers no update): nothing is ticked for it and nothing is claimed about updates. The built-in profile's `version` is never used for this.

The manifest's own `version` stays what a launch plan carries (`VRMOD_ACTIVE_VERSION`). Versions are compared as numbers part by part (`0.3.9` < `0.3.19`, `0.169` = `0.169.0`); a `-suffix` is the same base version, so
`0.170.0-launcher-test` is `0.170.0`, and the launcher never offers an update to a release that is older than or equal to what is installed.

## Launch contracts

- **D2R VR Authentic**: `{game}\D2R_VR_Settings.exe --start` (headless START: reads `d2r_vr.ini`, starts `D2RLoader.exe -mod D2R-VR`, and BodyWalk / FlatVR when the ini asks for them;
  exit 0 started, 1 no ini, 2 no D2RLoader.exe, 3 start failed, 4 the game runs). Its Settings button is the same exe without arguments.
- **D2R VR Unleashed**: `{game}\D2RLoader.exe -mod D2R-PCVR` with the Steam / Battle.net environment rules of his `launch_windows.py` (SteamOverlayGameId removed; Steam: SteamAppId = SteamGameId = 2536520; Battle.net: both removed).
- The launcher refuses to start while `D2R.exe` or `D2RLoader.exe` runs, when `D2RLoader.exe` is missing, or when the mod is not installed in the chosen game folder; it warns when
  D2RLoader's files are not the reviewed 1.3.1-beta ones.

## ReShade's folder must not leak from one mod to the other

In the mod layout each mod keeps its ReShade in `mods\<Name>\reshade` (D2R VR Unleashed: `mods\D2R-PCVR\reshade`; D2R VR Authentic: `mods\D2R-VR\reshade`) and points ReShade there
with `RESHADE_BASE_PATH_OVERRIDE`, set by its plugin inside the game process. The 0.168/0.169 global installs of Authentic use the ReShade beside the game
(`ReShade64.dll`, `ReShade.ini`, `ReShade.log` in the game folder) and set no variable. A copy of that variable that reaches the OTHER mod's game process from
**outside** sends the other mod's ReShade into the wrong folder; ReShade then refuses to start (`ReShade64.dll did not load (error 1114)`) and the headset stays empty. A player case
(2026-10-18, PS VR2): Unleashed tried first, Authentic installed after, no ReShade log written in the game folder at all.

- **The launcher** never sets any `RESHADE_*` variable for the user (not with `setx`, not in the registry). It removes `RESHADE_BASE_PATH_OVERRIDE` from the environment of the
  game it starts (every D2RLoader mod, `env - RESHADE_BASE_PATH_OVERRIDE` in the plan), unless the mod's own `launch.options[].env` rule sets it on purpose. When the launcher's own
  environment has it, the start's warnings say so with the value and how to remove it from Windows. Other `RESHADE_*` variables are passed on unchanged and only named.
  It cannot clean a start that does not go through it (Steam's launch option, a shortcut), which is why the mods should guard themselves as well.
- **A mod** sets `RESHADE_BASE_PATH_OVERRIDE` (if it needs it at all) only inside its own game process, right before it loads its ReShade, and **ignores/clears an inherited value first**: whatever is in the
  environment when the plugin starts did not come from this mod. A mod's Setup or wrapper must never persist it (no `setx`, no `HKCU\Environment`, no Steam launch option that exports it).
- An earlier install of the other mod in `d2rloader\plugins` (a global plugin) is reported by the launcher separately (`GlobalPluginNotes`).

## What a mod's installer does for the launcher

1. Writes `<game>\mods\<Name>\vrmod-manifest.json` (and the icon) with the rest of the mod - nothing outside its own mod folder, nothing in another mod's.
2. Its uninstaller removes the mod folder (the manifest with it).
3. It does not need the launcher to be installed, and the launcher does not need it to run.
