# The three ways to install (the contract between the launcher, D2R VR Authentic's Setup and D2R VR Unleashed's Setup)

Serg's design: there are three ways in, and each ends with the launcher installed and the mods in the D2RLoader-mod layout (`<game>\mods\<Name>`).

| # | The player runs | What happens |
|---|---|---|
| 1 | **D2R VR Launcher Setup** (published on the launcher's GitHub releases) | Installs the launcher (per user, no UAC). On its last page the player may open the launcher's **install view**: the list of available mods (D2R VR Authentic from bodywalkvr.com, D2R VR Unleashed from bearstonem's GitHub releases) and D2RLoader. The player ticks what to install; the launcher downloads each, checks its hash and runs it. |
| 2 | **D2R VR Authentic Setup** (ours) | Installs the launcher silently if it is missing or older (it carries the launcher's Setup inside), then installs only its own mod into `mods\D2R-VR`. |
| 3 | **D2R VR Unleashed Setup** (bearstonem's) | The same, for `mods\D2R-PCVR`. |

A mod's Setup never needs the launcher to be there to install its mod, and a launcher without a mod's manifest still shows the two known mods. The launcher's part in 2 and 3 is a
convenience: the player ends up with it installed and the mod listed.

## What a mod's Setup does for the launcher (2 and 3)

1. **Detect.** `HKCU\Software\D2R VR Launcher`, string values `InstallDir` and `Version` (`x.y.z`), written by the launcher's Setup; the program is `<InstallDir>\D2RVRLauncher.exe`.
   The launcher is installed when `Version` is there and the exe exists. (Per user: read HKCU of the user who started Setup - with Inno, `runasoriginaluser` / the original user's hive;
   an administrator-only Setup that runs as another account must not look in its own hive.)
2. **Compare.** Dotted numbers, up to four parts, missing parts are 0 (`0.2.0` = `0.2.0.0`). Install the carried launcher only when it is **missing or older** than the carried one. Never downgrade.
3. **Install silently**, as the original user (a per-user program in the user's profile):
   `D2R_VR_Launcher_Setup_<version>.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /TASKS=`
   (`/TASKS=` = no desktop shortcut; the Start menu shortcut stays). The launcher's Setup itself stops without changing anything when a newer launcher is installed (exit code 1, no files touched) and
   without pages never asks anything. **The exit code never blocks the mod's install**: log it and go on. Running it needs no administrator rights and shows nothing.
4. **Write the mod's manifest** into the mod's own folder, with the mod: `<game>\mods\<Name>\vrmod-manifest.json` (+ the icon it names) - `docs/MOD_MANIFEST.md`. Nothing in another mod's folder.
5. **Uninstall** removes the mod's folder (the manifest goes with it) and nothing of the other mod; it **leaves the launcher installed** (the other mod, or the player, may use it).
6. Optional: on the finish page "Open D2R VR Launcher" (unchecked, not offered in a silent run). The launcher finds the mod by itself.

The launcher is carried **as the unmodified Setup file** the launcher's repository publishes (`D2R_VR_Launcher_Setup_<version>.exe` + its `.sha256`, on https://github.com/natpoh/d2r-vr-launcher/releases).
A mod's build script fetches it **once per release**, pins the version and its SHA-256 in the script, checks the hash, and puts the file in the installer's payload (about 3 MB). The installer
does not download it at install time.

## Install 1: what the launcher does when it installs the other pieces (the "install view")

For each ticked item the launcher: finds the latest release (our `https://bodywalkvr.com/api/version?product=d2r_vr`, GitHub's releases of `bearstonem/d2r-pcvr`, D2RLoader's pinned archive),
downloads it into `%LOCALAPPDATA%\VRModLauncher\downloads`, checks the hash, and runs it: D2RLoader is unpacked by the launcher itself (silent, hash-checked, no administrator rights unless the game folder is protected);
D2R VR Authentic's Setup is run with its wizard (administrator, `/DIR=<game folder>`); D2R VR Unleashed's Setup with its wizard (per user). Both Setups, being case 2 and 3, find the launcher already installed and do not
install it again. Details and the hash rules per piece: `docs/INSTALL_PATH.md`. The install view is built (launcher 0.3.0, `D2RVRLauncher.exe --install-view`; "The window" in that document): the launcher's own Setup offers it on its
finish page ("Choose the mods to install", ticked by default), the main window has the button "Install / update mods...", and a card with a newer release shows "Update available" and an Install / Update button.

## For bearstonem (case 3): what to change in his installer
- carry `D2R_VR_Launcher_Setup_<version>.exe` (pinned, hash-checked at build) and run it as in 3 above before his mod, only if the launcher is missing or older (an Inno `Check:` function reading the two registry values);
- write `mods\D2R-PCVR\vrmod-manifest.json` (+ optional `launcher_icon.png`) with the mod (his `steamvr/install.py` payload, like the other mod files);
- never persist `RESHADE_BASE_PATH_OVERRIDE` (no `setx`, no `HKCU\Environment`, no exported Steam launch option), and have the plugin ignore an inherited value before it loads its ReShade - `docs/MOD_MANIFEST.md`, "ReShade's folder must not leak";
- keep his Setup as it is otherwise (wizard, Steam shortcuts, setup-backups); the launcher starts his mod with `D2RLoader.exe -mod D2R-PCVR` and his Steam environment rules, so his wrapper is not needed for it.
The proposed patch is `kits/d2r-pcvr-for-review/install-patch-proposal.diff` (not applied or run by us).
