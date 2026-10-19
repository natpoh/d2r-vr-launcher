// mods.h - the launcher's view of the game and of the D2RLoader mods in it (docs/MOD_MANIFEST.md).
//
// Both VR mods are D2RLoader mods (<game>\mods\D2R-VR, <game>\mods\D2R-PCVR): choosing one is `D2RLoader.exe -mod <name>`, the launcher
// moves no files. This header is everything the window (gui_main.cpp) and the command line need:
//   FindGames / DescribeGame   which game folders there are (Steam, Battle.net, the one the player picked) and what store/build each is
//   DiscoverMods               the mods of one game: the VR mods the launcher knows (installed or not) and every other D2RLoader mod
//   CheckLoader                is D2RLoader there and are its files the reviewed 1.3.1-beta ones
//   ListRuntimes               the OpenXR runtimes a start can use (chosen per start, never changed system-wide)
//   GameStartedOnce            has the plain game been started on this PC (its Settings.json in Saved Games)
//   StartMod                   all the checks of a start, then the process
//   LoadSettings / SaveSettings  what the launcher remembers (game folder, last mod, a runtime per mod)
#pragma once

#include <map>
#include <string>
#include <vector>

#include "launch.h"
#include "manifest.h"
#include "process.h"
#include "util.h"

namespace vrml {

// ---- the game
struct GameInstall {
    fs::path dir;
    std::string store;     // steam | battlenet | "" (unknown)
    std::string build;     // "3.3.93787" or ""
    std::string source;    // "Steam", "Battle.net", "chosen", "remembered"
};

// Where games may be: for tests, and for the real machine (SystemGameRoots).
struct GameRoots {
    std::vector<fs::path> steam_libraries;     // Steam's folder and its libraries: <lib>\steamapps\common\Diablo II Resurrected
    std::vector<fs::path> battlenet;           // install locations Battle.net registered
    std::vector<fs::path> extra;               // remembered or chosen folders
};
// Registry (Windows) and the environment; empty roots elsewhere.
GameRoots SystemGameRoots(const std::map<std::string, std::string>& env);
RuntimeRoots SystemRuntimeRoots(const std::map<std::string, std::string>& env);

GameInstall DescribeGame(const fs::path& dir, const std::string& source);       // dir need not be a game: check LooksLikeGame
// Every game folder under the roots that has a D2R.exe (de-duplicated by normalised path, Steam first).
std::vector<GameInstall> FindGames(const GameRoots& roots);

// ---- D2RLoader
struct LoaderStatus {
    bool present = false;                      // D2RLoader.exe is in the game folder
    bool reviewed = false;                     // D2RLoader.exe, D2RCore.dll and d2rloader.mpq are the reviewed 1.3.1-beta files
    std::string version;                       // "1.3.1-beta" when reviewed (or what its log says), else ""
    std::vector<std::string> notes;            // what is wrong, in words
};
// Hashes the three files (~50 MB) only when `hash` is true; otherwise only the presence is checked.
LoaderStatus CheckLoader(const fs::path& game, bool hash);

// The files a mod's manifest lists in `x-mod-files` that are not in its folder (notes for the player; optional ones are skipped).
void MissingModFiles(const fs::path& modDir, const Manifest& m, std::vector<std::string>* notes);

// Plugins in <game>\d2rloader\plugins load into EVERY mod. An earlier D2R VR (up to 0.167) or d2r-pcvr (up to 0.3.15) install is one of those: it runs inside the
// other mod too, and d2r-pcvr's own guard then refuses to start. Words for the player, one per problem; empty when there are none.
std::vector<std::string> GlobalPluginNotes(const fs::path& game);

// Things in the player's own Windows environment that make one mod's ReShade end up in another mod's folder. RESHADE_BASE_PATH_OVERRIDE set for the user
// (setx, a wrapper, an earlier start of the other mod) sends every ReShade there; the launcher removes it from the game it starts and says so here.
// Other RESHADE_* variables are left alone and only named. Empty when the environment is clean.
std::vector<std::string> ReShadeEnvironmentNotes(const std::map<std::string, std::string>& env);

// ---- mods
struct ModEntry {
    std::string id;                            // "d2r-vr"
    std::string folder;                        // the D2RLoader mod name: "D2R-VR"
    std::string name;                          // "D2R VR Authentic"
    std::string description, version, publisher, homepage, support;     // `version`: the manifest's (the built-in profile's when the mod has none): what a launch plan carries, NOT proof of what is installed
    // What is installed, as best the launcher can tell (the version every card, badge, update comparison and install row uses), "" = unknown. Found in this order for a known mod:
    //   1. "manifest"             the `version` of a vrmod-manifest.json that validates
    //   2. "manifest (not valid)" the `version` text of a vrmod-manifest.json that does NOT validate (an unknown field from another schema), read leniently; the note "is not valid" stays
    //   3. "plugin"               the ProductVersion in the version resource of the DLL under <mod>\d2rloader\plugins whose Comments say mod-id=<id> (Windows)
    //   4. nothing                (the built-in profile's version is a guess about the release the launcher shipped with, never used here)
    // Another D2RLoader mod with a valid manifest has it from the manifest; one without has none.
    std::string found_version, found_from;
    fs::path dir;                              // <game>\mods\<folder>
    fs::path icon;                             // a picture the mod ships, or empty (the window has its own for the known mods)
    bool installed = false;                    // <folder>.mpq\modinfo.json is there
    long long installed_at = -1;               // when it was first installed on this computer: the older of the creation times of mods\<folder> and its modinfo.json (survives an update); -1 not installed / unreadable
    bool known = false;                        // one of the VR mods the launcher has a built-in profile for
    std::string source;                        // "manifest" (vrmod-manifest.json), "built-in" or "plain" (a D2RLoader mod without launcher data)
    bool launchable = true;                    // has a launch option
    Manifest manifest;                         // always filled
    std::vector<std::string> notes;            // problems found reading it (an invalid manifest falls back to the built-in profile)
};
// The known VR mods first, then every other mod folder with a modinfo.json. The known ones are listed in the order they were installed on this computer
// (the earlier `installed_at` first); the ones that are not installed follow the installed ones; when the times are equal or one cannot be read, or
// when neither is installed, D2R VR Unleashed goes before D2R VR Authentic. The window and `mods` both show this order.
std::vector<ModEntry> DiscoverMods(const fs::path& game);
// The built-in profiles, as manifests (what DiscoverMods falls back to).
const std::vector<Manifest>& BuiltinMods();

// ---- what a mod's buttons run (the window's Settings, Logs and Uninstall)
// "{game}", "{localappdata}", "{appdata}" and "{programfiles}" in a manifest string -> the real folder (UTF-8, '/'); other text stays.
std::string ExpandModPath(const std::string& text, const fs::path& game, const std::map<std::string, std::string>& env);
struct ModTools {
    fs::path settings_exe;                     // empty: the mod has no settings app
    std::string settings_label;                // the settings button's name: the manifest's `settings_app.label`, else "<mod name> Settings"; empty with no settings app
    std::vector<std::string> settings_args;
    fs::path uninstall_exe;                    // empty: the manifest names no uninstaller
    std::vector<std::string> uninstall_args;
    bool uninstall_needs_admin = false;
    std::vector<fs::path> logs;                // every file `logs` names, expanded (they need not exist)
};
ModTools ResolveModTools(const ModEntry& mod, const fs::path& game, const std::map<std::string, std::string>& env);

// ---- runtimes
struct RuntimeChoice {
    std::string id;                            // system | steamvr | vdxr | file
    std::string label;                         // "SteamVR", "Virtual Desktop (VDXR)", "System default", "A manifest file..."
    bool available = false;
    std::string detail;                        // the manifest path, or why not
};
std::vector<RuntimeChoice> ListRuntimes(const RuntimeRoots& roots, const std::map<std::string, std::string>& env);

// ---- has the game been started once without mods
// A fresh install has no user settings yet: the game writes <Saved Games>\Diablo II Resurrected\Settings.json on its first start (Steam and Battle.net
// alike), and a mod started before that dies at the logo screen (D2RLoader.exe, an unhandled C++ exception).
fs::path SavedGamesFolder();                                       // Windows' Saved Games folder (FOLDERID_SavedGames); empty when it cannot be found, and elsewhere
fs::path GameSettingsFile(const fs::path& savedGames);            // <savedGames>/Diablo II Resurrected/Settings.json
bool GameStartedOnce(const fs::path& savedGames);                  // that file is there (a missing folder counts as not started)
// The check of a start: true when it may go on (the file is there, `skip`, or no Saved Games folder to look in); *line gets one line for launcher.log
// naming the path checked.
bool FirstStartCheck(const fs::path& savedGames, bool skip, std::string* line);
// What StartMod's E_FIRST_START says (the window shows it with OK and "Start anyway").
constexpr const char* kFirstStartMessage =
    "Diablo II: Resurrected has not been started on this PC yet. Start it once without mods (from Steam or Battle.net), create a character, quit, "
    "then press Play again.";

// ---- a start
struct StartRequest {
    fs::path game;
    ModEntry mod;
    std::string option;                        // "" = the first that fits the store
    std::string runtime;                       // "" = the option's first | system | steamvr | vdxr | file
    fs::path runtime_file;
    std::string store;                         // "" = detected
    bool dry_run = false;                      // build the plan and run the checks, start nothing
    fs::path saved_games;                      // where the game keeps its Settings.json (SavedGamesFolder()); empty: the first-start check is not made
    bool skip_first_start_check = false;       // "Start anyway" / --skip-first-start-check: the check is reported, not obeyed
};
struct StartOutcome {
    bool started = false;
    std::string code;                          // "" or E_GAME, E_GAME_RUNNING, E_MOD_NOT_INSTALLED, E_LOADER_MISSING, E_RUNTIME, E_PRECONDITION, E_FIRST_START, E_IO ...
    std::string message;                       // for the player, one or two sentences
    std::vector<std::string> warnings;         // the start went on (or would)
    std::string first_start;                   // the first-start check, one line for launcher.log (the path checked); "" when it was not reached
    LaunchPlan plan;
};
// Never throws for a refusal: the outcome says why. The checks: D2R.exe in the folder; the game and D2RLoader not running; the mod installed; D2RLoader.exe
// there; the launch plan (option, store, runtime) builds; the game has been started once without mods (E_FIRST_START). Warnings: the game build differs from the mod's, D2RLoader's files are not the reviewed ones.
StartOutcome StartMod(const StartRequest& req, ProcessProbe& probe, LaunchBackend& backend, const std::map<std::string, std::string>& env,
                      const RuntimeRoots& roots);
// The process exit code of a refusal (the CLI contract, also what the window's `--play` returns): 0 for "".
int StartExitCode(const std::string& code);

// ---- what the launcher remembers: <home>\settings.json
struct ModSettings {
    std::string runtime;                       // "" = the option's first
    std::string runtime_file;
};
struct LauncherSettings {
    std::string game;                          // the folder the player picked last
    std::string last_mod;                      // mod id
    bool close_on_start = false;               // the window closes itself once the game has been started
    bool check_updates_on_start = true;        // a quiet look at both mods' newest releases once per launch (one request per source); off = never asks by itself
    bool install_view_offered = false;         // the install view was offered once on a first run (no mod installed): not offered again
    std::map<std::string, ModSettings> mods;   // by mod id
};
LauncherSettings LoadSettings(const fs::path& home);      // missing or unreadable file: defaults
void SaveSettings(const fs::path& home, const LauncherSettings& s);

}  // namespace vrml
