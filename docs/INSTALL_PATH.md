# Installing from the launcher: the path of each piece

"The player installs the launcher, and from it everything else gets installed." Three pieces can be installed or updated from the launcher; each has its own
source and its own way of being proven. This page is the table, the trust model, the API the window calls, and what is still missing.
Everything here is in the library (`src/net`, `zip`, `install`, `releases`, `setup_run`, and `install_run`, which runs the three one after the other), on the command line
(`vrmodlauncher releases`, `install ...`) and in the window (the install view and the update badges, "The window" below); the window calls the same functions.

## One table

| | **D2RLoader** | **D2R VR Authentic** (ours) | **D2R VR Unleashed** (bearstonem's d2r-pcvr) |
|---|---|---|---|
| What is fetched | the ZIP `D2RLoader-1.3.1-beta.zip` (45.4 MB on 2026-10-18); the version is **pinned** (the mods were made for 1.3.1-beta), the launcher never asks for "newest" | the mod's own Setup, `D2R_VR_Setup_v<version>.exe` | the mod's own Setup, `D2R-PCVR-Setup-<version>.exe` (47 MB) |
| Where the address comes from | fixed in the launcher: `https://d2rloader.net/downloads/D2RLoader-1.3.1-beta.zip` | `GET https://bodywalkvr.com/api/version?product=d2r_vr&v=<installed>` -> `{"version","download_url","changelog"}`; `"version":"0.0.0"` = nothing newer | `GET https://api.github.com/repos/bearstonem/d2r-pcvr/releases?per_page=5` (all are pre-releases: no `/releases/latest`); newest non-draft release with an asset `D2R-PCVR-Setup-<version>.exe` |
| Hosts it may use | `d2rloader.net` | `bodywalkvr.com` | the address GitHub lists must be `github.com/bearstonem/d2r-pcvr/releases/download/...`; GitHub may redirect to `*.githubusercontent.com` |
| How it is proven | the **archive SHA-256** must be one of the two reviewed ones (the author repacked the ZIP on 2026-10-13: both are accepted; the live file on 2026-10-18 is the older, `9286c6b5...`); then the **three unpacked files** (D2RLoader.exe, D2RCore.dll, d2rloader.mpq) must hash to the reviewed ones in the manifests' `requires[d2rloader].files`; every entry's CRC-32 | **today: nothing is published**, so https + the host list + (if the answer has one) `size` as a cap; the file's SHA-256 is computed and shown. If the answer carries `sha256` (and `size`) it is **enforced**. Without a published hash the caller must say `accept_unverified` (`--accept-unverified`; the window asks the player) | GitHub's `digest` of the asset (`sha256:<hex>`, computed by GitHub on upload) **and** the tiny `D2R-PCVR-Setup-<version>.exe.sha256` asset; when both exist they must agree (else `E_HASH`, nothing is downloaded); the downloaded file must have exactly that SHA-256 and the listed size |
| A wrong file | deleted, `E_HASH` (23), nothing is written to the game folder | deleted, `E_HASH` (23), the Setup is never run | deleted, `E_HASH` (23), the Setup is never run |
| What runs | nothing: the launcher unpacks the ZIP itself | the Setup, started **elevated** (UAC) with `/DIR="<game folder>"`; it shows its own wizard | the Setup, **per user** (no UAC), no arguments; it shows its own wizard (Steam / headset choices: a silent run would need a choices file, so it runs visibly) |
| Silent / admin | silent; needs administrator rights **only** when the game folder is protected (`C:\Program Files...`): then `E_IO` "administrator rights are needed for this folder" and the folder is untouched | not silent; needs admin (the prompt is Windows', a declined prompt is `E_UAC_DECLINED`, 31) | not silent; no admin |
| Where it ends up | `D2RLoader.exe`, `D2RCore.dll`, `d2rloader.mpq` in the game folder; empty `mods\`, `d2rloader\{config,data,plugins,logs,crashes}`; `d2rloader\config\d2rloader.toml` only when absent | `<game>\mods\D2R-VR` (by the Setup) | `<game>\mods\D2R-PCVR` (by the Setup) |
| After | `FilesMatch` on the three files | `DiscoverMods` again: is the mod in THIS game folder, which version (its manifest, else the version resource of its plugin) | the same |
| Cancelled / failed | `E_CANCELLED` (30) for a cancelled download; any failed step leaves the folder as it was | Setup exit codes reported as they are (Inno: 2 and 5 = cancelled by the player -> `E_CANCELLED`, others `E_SETUP` 28); "finished but not installed here" is `E_MOD_NOT_INSTALLED` (11) | the same |
| What the launcher does itself | download, verify, unpack, back up, move in, roll back | download, verify, run the Setup, check the result | the same |

## The D2RLoader install, step by step (`InstallD2RLoader`)

1. `D2R.exe` must be in the folder (`E_GAME`, 25) and neither `D2R.exe` nor `D2RLoader.exe` may run (`E_GAME_RUNNING`, 10).
2. The three reviewed files already there with the reviewed hashes: "already installed", nothing downloaded or written.
3. The archive: a cached file with an accepted hash is reused; else it is downloaded (https, host `d2rloader.net`, up to 5 redirects each checked, 300 MB cap,
   written to `<file>.vrml-part` and renamed when whole). A wrong hash deletes it (`E_HASH`).
4. It is unpacked into `<game>\.vrml-install-<time>\` (same disk, so the last step is renames). Only the names `d2r-pcvr`'s `install.py` accepts are allowed
   (top level `mods`, `d2rloader`, `d2rloader.exe`, `d2rcore.dll`, `d2rloader.mpq`, any case); **one other name refuses the whole archive before a byte is
   written** (`E_ARCHIVE`, 26), as do `..`, absolute paths, drive letters, `:` streams, backslashes, device names, duplicates (also differing in case), symbolic links,
   an entry over 256 MB, an archive over 1 GB, a CRC-32 that does not match, a size that lies.
5. The three unpacked files must hash to the reviewed ones. Otherwise `E_HASH` and **nothing has been written to the game folder**.
6. Files that would be replaced are first **copied** to `<home>\backups\d2rloader-<time>\...`. A file under `d2rloader\config\` or `mods\` that already exists is the
   player's and is never replaced.
7. The new files are moved in. If a move fails (a file in use, the disk) the files already moved are put back from the backup and the new ones removed: the folder is
   as it was (`E_ROLLBACK` only if even that fails, and then the message says where the copies are).

## Trust: what a hash does and does not prove

- A SHA-256 proves the file is **the one that was announced** - not corrupted, not swapped on the way or on a CDN. It does not prove that the publisher's account or
  server was not taken over: the hash and the file come from the same place. For D2RLoader the hashes are **in the launcher** (reviewed by us), which is stronger.
  For the two mods the hash comes from the publisher (GitHub's own `digest` is computed by GitHub, which is a little better than a file the publisher uploads).
- Nothing is run before it is verified, a file that fails is deleted, and a Setup is started only from `<home>\downloads\installers\<mod>\<version>\` after the
  check. The files carry no "downloaded from the internet" mark, so Windows' SmartScreen does not add a second prompt; the launcher has not checked an Authenticode
  signature (see "missing").
- Plain http is never used. The one exception is a **test switch**: `VRML_ALLOW_HTTP_HOST=127.0.0.1:<port>` (`localhost:` and `[::1]:` too) lets exactly that loopback
  host and port be reached over http; any other value is ignored. The test addresses (`VRML_URL_LOADER`, `VRML_URL_AUTHENTIC_API`, `VRML_URL_UNLEASHED_API`), the
  `--pin-file` of `install loader`, `VRML_TEST_NO_ELEVATE` and `VRML_TEST_CANCEL_AFTER` are honoured only together with it. Production sets none of them.

## The command line

```
vrmodlauncher releases [--game DIR] [--json]
vrmodlauncher install loader|authentic|unleashed --game DIR [--dry-run] [--no-run] [--accept-unverified] [--force] [--json] [--home DIR]
```

- `releases`: the newest of both mods, what `--game` has installed ("UPDATE AVAILABLE" when the newest is newer or the installed version is unknown), where the
  installer is, its checksum and where the checksum comes from. Exit 0 when at least one server answered, 20 when none did. Needs the network, changes nothing.
- `install ... --dry-run`: says what would be downloaded from where, the expected hash (or that none is published), whether administrator rights are needed (for the loader:
  can the game folder be written; Authentic: always, the Setup is started elevated; Unleashed: no), what would be run - and stops. Refusals show here too (exit 27 for a
  release without a hash and no `--accept-unverified`, 10 game running, 25 not a game).
- `install ... --no-run`: download and verify only (loader: the archive in the cache; mods: the installer in `<home>\downloads`), install nothing.
- `install authentic|unleashed --force`: ask for the newest even when the installed version is current (a repair).
- Progress goes to stderr, results to stdout (`--json` for a program). Exit codes: 0 done; 2 usage; 10 game running; 11 the Setup ended but the mod is not in this folder;
  20 I/O or network (also "administrator rights are needed"); 21 rollback incomplete; 23 hash mismatch; 25 not a game folder; 26 damaged or unsafe ZIP; 27 no published
  checksum and no `--accept-unverified`; 28 the Setup failed; 29 an address or answer from a server the launcher does not trust; 30 cancelled; 31 UAC declined.

## The window: the install view and the update badges (launcher 0.3.0)

`D2RVRLauncher.exe` has a button **Install / update mods...**; it opens the **install view**, a window over the main one (`--install-view` opens it right after start: the finish page of the
launcher's own Setup does that, "Choose the mods to install"). Pictures: `docs/screens/install_view_100pct.png` (a fresh game: Install buttons, everything ticked), `install_view_installed_current_100pct.png` (both mods installed and current: nothing ticked, Install selected off, Uninstall
buttons, greyed Up to date), `install_view_update_available_100pct.png` (Update buttons, ticked cards, the amber badge), `install_view_plugin_version_100pct.png` (a version read from a plugin, a version that could not be read),
`install_view_result_100pct.png`, `launcher_update_badges_100pct.png`.

- **Three rows**: D2RLoader, D2R VR Authentic, D2R VR Unleashed. Each has a check box, *Installed:* (D2RLoader: "reviewed 1.3.1-beta files" / "missing" / "files differ"; a mod: its version - with "(from its plugin)" or
  "(manifest not valid)" when it was not read from a valid manifest -, "installed (version unknown)", "not installed"), *Newest:* ("checking..." while the two servers are asked on a worker thread, a version, "you have the
  newest version", "0.169.0 - you have a newer version" when the site's newest is older than what is installed, or the problem in words), where it comes from and how it is
  proven ("d2rloader.net, hash pinned in the launcher" / "bodywalkvr.com - no checksum published yet" or "- checksum published" / "GitHub release of bearstonem/d2r-pcvr, SHA-256 from GitHub"), how it is run
  (the Setups open their own windows; Authentic's asks Windows for administrator rights), and a mark on the right: the amber "Update available: 0.167.0 -> 0.168.0" (only for a proven newer release), then during and after a
  run "Waiting", "Downloading 12.3 MB of 47.0 MB", "Setup window open", "Installed 0.168.0", "Already installed", "Failed", "Cancelled", "Not run".
- **Which version is installed** (`ModEntry::found_version`, in this order; the card, the badge, the row, `vrmodlauncher mods` and the update question all use it): (1) the `version` of a vrmod-manifest.json that validates;
  (2) the `version` text of a manifest that does NOT validate (a field of another schema), read from the raw JSON - only when it names this mod - and the "is not valid" note stays; (3) the ProductVersion in the version
  resource of the DLL in `<mod>\d2rloader\plugins` whose Comments say `mod-id=<id>` (`d2r-vr` / `d2r-pcvr`; any file name: Bear's is `d2rl-pcvr.dll` with ProductVersion "0.3.19" and Comments "mod-id=d2r-pcvr; plugin-id=d2r-pcvr-vrcam; ...",
  earlier builds `d2rl-vrcam.dll`); (4) otherwise unknown. The built-in profile's version is never used (it says which release the launcher shipped with). **The order of versions**: numbers compared part by part
  (0.3.9 < 0.3.19), missing parts are 0 (0.169 = 0.169.0), a suffix after the numbers (`-launcher-test`, `-modlayout-candidate`) is the same base version.
- **Never a downgrade**: when the newest release is older than or equal to what is installed the row says it is up to date ("you have the newest version" / "<newest> - you have a newer version"), has no badge, is not ticked,
  its box is off and its button is a greyed *Up to date*. (Authentic's server is asked "anything newer than <installed>?"; a site that answers with an older number of its own is handled the same.)
- **What is ticked**: nothing unless it is a real install or update. A missing mod is ticked (install); an installed mod with a **proven** newer release is ticked (update); an installed mod whose version could not be read is
  **not** ticked - its button says *Update* and its note says the version could not be read and that Update runs the newest Setup over it; a current (or newer-than-the-newest) mod is not ticked, its box is off and it cannot be
  ticked - there is no "repair" tick any more (to put a mod in again, uninstall it, then install it). D2RLoader: ticked when it is missing or its files differ **and** a mod is ticked; reviewed files = up to date, box off.
  Ticking a mod while D2RLoader is missing ticks D2RLoader too and the row says why; the player may untick it, the row then warns. What the boxes do is `src/gui_install_model.*` (unit-tested without a window), not the window.
- **Each row's own buttons**, on the right of its card beside its Installed / Newest lines. The action button: *Install* (not installed), *Update* (newer proven, or the version unknown), a greyed *Up to date* (current);
  D2RLoader: *Install* (missing), *Repair* (its files differ: the reviewed ones are put back, what it replaces is backed up as always), a greyed *Up to date* (reviewed files). Pressing it ticks **only that piece** (and D2RLoader
  when it is missing and the mod needs it, as the card's button does) and starts the install at once, through the same path as *Install selected* - the same refusals (no game folder, the game runs, ...) and the same question
  about a download without a published checksum. The buttons are off while the answers are still coming, while an install runs and while the game runs (the notice says why). The card's *Install* / *Update* button opens the view
  with exactly that mod ticked.
- **Uninstall**, a second button on the row of an **installed** D2R VR Authentic or Unleashed (not on a missing mod, never on D2RLoader), does what the card's button in the main window does (`Model::UninstallFor` /
  `UninstallRefusal`): refused while D2R.exe or D2RLoader.exe runs ("Save & Exit in the game first" - the launcher never closes the game); a question that names the mod and its uninstaller (default No); then the mod's **own**
  uninstaller (Authentic's `unins000.exe` in the game folder, Unleashed's `%LOCALAPPDATA%\Programs\D2R PCVR\unins000.exe` without `/SILENT`, so its own question shows); with no uninstaller on the computer (a ZIP install) a message that
  says how to remove the mod by hand. The launcher never deletes a mod folder. While the uninstaller runs the row buttons and *Install selected* are off ("Waiting for the uninstaller of ... to finish"); when it ends the view
  reads the game folder again and asks the servers again, so the row changes (Install instead of Uninstall, or "still installed" when the player said No in its own question).
  **D2RLoader has no Uninstall**: it is shared by both mods and by any other D2RLoader mod, `InstallD2RLoader` keeps copies only of the files it *replaced* (for its rollback and for the player; a first install has none) and has no restore step.
- **Install selected** runs on a worker thread (`install_run.h`), D2RLoader first, then Authentic, then Unleashed, each: look up the release (the one the player saw is used), download, check the hash, install (D2RLoader)
  or run the Setup with its wizard (Authentic elevated with `/DIR="<game folder>"`, Unleashed per user). One piece failing does not stop the next; a cancel, a running game or a folder that is not a game does.
  The bar covers the whole run, the line under it says "Step 2 of 3 - D2R VR Authentic: downloading 12.3 MB of 47.0 MB (26 %)", the log keeps everything.
- **Cancel** (the Close button while it runs): the download in progress is stopped and deleted, the rest is not started. A Setup wizard that is open is **not** closed: the view says "finish or close its wizard".
  The window itself does not close while an install runs.
- **Refused, with the reason beside the button**: no valid game folder ("Pick the folder Diablo II: Resurrected is installed in first"), the game is running, nothing ticked ("Tick what you want to install."; "Nothing to install: everything is up to date." when everything is current;
  "Nothing to install: everything you ticked is up to date." when a card's Update turned out to be for a current mod), a ticked mod whose newest release is not known yet
  or could not be found out ("press Check again"). *Install selected* is on only when a ticked row is a real install, update or loader fix.
- **A release without a published checksum** (today D2R VR Authentic): before anything is downloaded a question says what cannot be verified (the file cannot be proven to be the announced one), what is still checked
  (https, only bodywalkvr.com, a size cap, the SHA-256 written to the log before the Setup starts) and why it is probably fine (the server will publish a checksum later and the launcher then enforces it with no update).
  The default button is **No**; Yes passes `accept_unverified`. Nothing is accepted silently.
- **After the run** the view looks at the game folder again (`DiscoverMods`, D2RLoader's files), the main window refreshes, and the summary is in words per piece: "you declined the administrator prompt" (`E_UAC_DECLINED`),
  "the Setup failed: ... (exit code 4)" (`E_SETUP`), "a checksum did not match" (`E_HASH`), "administrator rights are needed for this game folder: ... Run as administrator" (`E_IO`), "cancelled" ...
- **Update badges**: once per launch, in the background, the window asks the two servers (one request each; the setting *Check for updates when the launcher starts*, on by default, is remembered in `settings.json` as
  `check_updates_on_start`; `--no-update-check` switches it off for one run). A card with a **proven** newer release shows the amber "Update available: 0.167.0 -> 0.168.0" and its button says **Update** (never for an older or the same release; a mod whose version is unknown is still offered the newest, without a "from"); a known mod that is not installed says
  **Install**; both open the view. The old "Get..." link is now a small "Website" link. A server that cannot be reached shows nothing (the reason goes to `launcher.log`).
- **First run**: when the game folder was found (not given with `--game`) and neither VR mod is installed, the window offers the view once (`install_view_offered` in `settings.json`; the answer does not matter).
- **Command line**: `D2RVRLauncher.exe --install-view [--install-tick loader,authentic,unleashed]`. Test aids, honoured **only together with `VRML_ALLOW_HTTP_HOST`** (a loopback test server; exit 2 otherwise): `--install-run LIST --game DIR`
  (no window: exactly what Install does for those pieces, one `<id>  <status>[  <code>]` line each and the message under it, exit 0 or the first failure's code), `--accept-unverified`, `--pin-file FILE` (the D2RLoader pin of a test),
  `--install-now` (the view presses Install itself once its checks are done), `--shot-now` (with `--screenshot`: shoot at the first paint, the "checking..." state); `--screenshot` with `--install-view` shoots the view after its checks.
  A screenshot run asks no server unless the test switch is on, so no test reaches the internet.

## After the install: the first start (launcher 0.3.4)

Installing a mod is not enough on a fresh game install: the game writes its user settings (`<Saved Games>\Diablo II Resurrected\Settings.json`, the folder from `SHGetKnownFolderPath(FOLDERID_SavedGames)`,
Steam and Battle.net alike) on its first start, and a mod started before that crashes at the logo screen (D2RLoader.exe, an unhandled C++ exception). So Play and `vrmodlauncher launch` refuse while that file
(or its folder) is missing: the window says "Diablo II: Resurrected has not been started on this PC yet. Start it once without mods (from Steam or Battle.net), create a character, quit, then press Play again." with
**OK** and **Start anyway**; the command line exits with `E_FIRST_START` (32) unless `--skip-first-start-check` is given (the window's `--play` takes the same switch). Nothing is written; `launcher.log` gets one line with the path checked.

## The library, for the window

```cpp
#include "releases.h"    // Release, LatestAuthentic, LatestUnleashed, CheckUpdate, InstalledVersion, IsNewer
#include "setup_run.h"   // DownloadAndVerify, RunSetup, InstallMod, SetupBackend
#include "install.h"     // InstallD2RLoader
#include "net.h"         // HttpDownloader, HttpClient, HttpOptions
#include "install_run.h" // Piece, PieceJob, RunInstallJobs, CheckModRelease: what the window's Install button and --install-run call (below)

std::vector<PieceOutcome> out = RunInstallJobs(RunContext{game, home, &cliEnv, ""}, {PieceJob{Piece::Loader}, PieceJob{Piece::Authentic /*accept_unverified*/}}, RunHooks{});   // order, outcomes, never throws
UpdateInfo u = CheckModRelease(Piece::Authentic, game, env);                                 // what is newest, never throws

HttpDownloader dl(HttpOptions{...});  DepsCache cache(home, &dl, &log);
InstallD2RLoader(game, D2RLoaderPin(), cache, *probe, progress, home / "backups");        // throws vrml::Error{code, exit_code, what()}

auto http = MakeHttpClient(opts);  UpdateInfo u = CheckUpdate(*http, game, "d2r-vr");     // never throws: u.checked / u.error
InstallMod(game, "d2r-vr", u.latest, dl, *probe, *MakeSystemSetupBackend(), ModInstallOptions{...});   // blocks until the Setup is closed: worker thread
```

`ProgressFn` is `bool(done, total)`; returning false cancels (the partial file is deleted; `E_CANCELLED`). The window should: show `u.latest.sha256.empty()` as "no checksum
published" and ask before setting `accept_unverified`; call `InstallMod` from a worker thread; show `ModInstallResult::message` and `notes`; refresh `DiscoverMods` afterwards.

## What is not done

- **A published hash for Authentic.** `D2R_VR_Setup_*.exe` has none today. Proposed server change: add `"sha256":"<64 hex>"` and `"size":<bytes>` to `/api/version`; the launcher
  enforces both as soon as they are there, no launcher update needed.
- **Protected game folders.** The loader install reports "administrator rights are needed" and stops; it does not relaunch itself elevated. (Authentic's Setup is elevated by itself.)
- **Authenticode.** The installers' signatures are not checked (WinVerifyTrust) - useful once both installers are signed.
- **Cancel while a server is silent.** A cancel is noticed at the next piece of data; a server that stops sending is given up after 30 s (the receive timeout).
- **No resume** of a partial download; no proxy credentials (the system proxy is used as it is); GitHub allows 60 anonymous questions an hour (a `releases` is 2 requests).
- **The elevated start of a Setup (`runas`) is not run by any automated test** (it would raise a real UAC prompt); the tests run the same code path non-elevated with a stand-in Setup.
- **Uninstall** is the mod's own (manifest `uninstall`), a button on each installed card and on each installed row of the install view (launcher 0.3.1; the bottom-row button of 0.3.0 is gone); the launcher installs nothing it
  cannot take out again except D2RLoader, whose files stay (no Uninstall for it: shared by every mod; the backups of what an install replaced are for a rollback, not a restore).
- **A mod whose installed version cannot be found out** (no valid manifest, a manifest without a usable version, and no plugin that says `mod-id=<id>` with a version: an old build) is "installed (version unknown)": its card says
  "Update available: 0.3.19" (no "from" version) and its row says Update, but the view does **not** tick it - nothing is proven. Bear's Setup writes no manifest (docs/INSTALLER_CONTRACT.md); his plugin's version resource gives the version (0.3.1+).
  An earlier launcher build with the same number 0.3.0 did not know that and offered a downgrade; Setup never replaces a launcher with the same version number (docs/INSTALLER_CONTRACT.md), which is why this is 0.3.1.
- **In the window**: a Setup wizard that is open cannot be closed by the view's Cancel (it is another program: the player finishes or closes it); the elevated start of a Setup (the real UAC prompt) and the Setups' own wizards are
  not run by any automated test (the stand-in Setup stands for them); the answer to the "no published checksum" question is not remembered (it is asked at every install); the update check is once per start, not periodic;
  Check again asks both servers again but there is no "ignore this update".
- **Updating D2RLoader itself** is deliberately not offered: the pinned 1.3.1-beta is what both mods were reviewed against.

## Tests

`ctest` runs: `zip` (archives made by Python's `zipfile` - stored, deflate 0/1/6/9, empty files, nested folders, UTF-8 names - and the same with one fault patched in: bad CRC, damaged
data, a lying or huge size, truncation, encryption, zip-slip and other unsafe names, limits), `net` (URL parsing, the host list, the loopback switch), `install` (a fake downloader and a fake game),
`releases` (a real trimmed GitHub sample, our real answer, every kind of bad answer), `setup_run` (a fake Setup runner), `cli_install` (the commands in-process), and
`install_smoke` (`tools/install_smoke.py`: the real binary against a local web server on 127.0.0.1 with a stand-in Setup, including redirects, size cap, a cancelled and a truncated download,
a protected folder made with `icacls`, and a path with spaces through `/DIR="..."`). For the window: `install_run` (the shared runner with fakes: order, E_UNVERIFIED and the player's yes, a wrong hash, cancel, a
running game, UAC declined, Setup exit codes), `gui_install_model` (rows, tick rules, refusals, the question, progress and summary words), `gui_model` (options, settings, card offers, the first-run offer) and
`install_view_smoke` (`tools/install_view_smoke.py`: the real exe against a local web server - `--install-run` of every piece, the view driven with window messages including the question and its default button,
a cancel in a slow download, the badges and the quiet check, the first-run offer, screenshots at 100 / 150 / 200 %). None touches the internet, a real game folder or `%LOCALAPPDATA%\VRModLauncher`.
