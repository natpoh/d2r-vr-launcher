// gui_model.h - what the launcher window knows and decides, without any window: the games, the mods of the chosen game, the runtime per
// mod, what Play may do and why not, what the status line says. gui_window.cpp only shows it; gui_main.cpp's `--play` uses the same
// Model, so what the headless run does is what the Play button does. (Tests: tests/gui_model_test.cpp.)
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "mods.h"
#include "releases.h"

namespace vrml {
namespace gui {

// ---- the command line (for tests and support; the player never needs it)
struct Options {
    std::string game;          // --game DIR: use this folder only, never touch the remembered one
    std::string home;          // --home DIR: the launcher's folder (settings, logs); default DefaultHome
    std::string select;        // --select MOD_ID: the mod to preselect
    std::string screenshot;    // --screenshot FILE.png: render the window, save, exit
    bool dry_run = false;      // --dry-run: Play shows the plan and starts nothing
    bool play = false;         // --play: no window: do Play once, print the outcome, exit with its code
    bool skip_first_start_check = false;       // --skip-first-start-check: Play starts even with no Settings.json in Saved Games (support, tests)
    bool help = false;         // --help
    bool no_search = false;    // --no-search: do not look for installed games (Steam, Battle.net); only the remembered folder counts (a test aid)
    bool fake_processes = false;               // --processes LIST: the processes that "run" are exactly these (a test aid; "-" = none)
    std::vector<std::string> processes;
    int dpi = 0;               // --dpi N: lay the window out as at N dpi (a test aid for 125/150/200 %), 0 = the monitor's
    int width_dp = 0, height_dp = 0;   // --size WxH: the first client size in dp (a test aid), 0 = the default
    // ---- installing from the launcher (docs/INSTALL_PATH.md)
    bool install_view = false;                 // --install-view: open the install view right after start (the launcher's Setup finish page, first runs)
    std::vector<std::string> install_tick;     // --install-tick LIST: the pieces ticked when the view opens (what a card's Install / Update button does)
    bool no_update_check = false;              // --no-update-check: no quiet look at the newest releases at start (a test aid: tests never ask the internet)
    // test aids that touch the network: honoured only together with VRML_ALLOW_HTTP_HOST (TestSwitchProblems says so)
    bool has_install_run = false;              // --install-run LIST: no window: install these pieces like the view's Install button, print one line each, exit 0 or the first failure's code
    std::vector<std::string> install_run;      // the pieces of that list: loader, authentic, unleashed
    bool accept_unverified = false;            // --accept-unverified: a release without a published checksum is accepted (the view does not ask)
    std::string pin_file;                      // --pin-file FILE: the D2RLoader pin and reviewed files of a test (install.cpp's test switch)
    bool install_now = false;                  // --install-now: the view presses Install by itself when its checks are done, with the boxes as they are
    bool shot_now = false;                     // --shot-now: with --install-view --screenshot: shoot at the first paint (the "checking..." state), not when the checks are done
    std::vector<std::string> problems;   // arguments that were not understood
};
Options ParseOptions(const std::vector<std::string>& args);
const char* UsageText();
// The switches in `opt` that are test aids for the network and are not allowed without the loopback test switch (VRML_ALLOW_HTTP_HOST in `env`): one sentence each.
std::vector<std::string> TestSwitchProblems(const Options& opt, const std::map<std::string, std::string>& env);

// ---- how a line of the status area looks
enum class Tone { Normal, Ok, Warn, Error };

// What the launcher needs from the machine; tests give it a stand-in of each.
struct Machine {
    std::map<std::string, std::string> env;
    RuntimeRoots runtime_roots;
    GameRoots game_roots;
    fs::path saved_games;                // the player's Saved Games folder (the first-start check); empty = not checked
    ProcessProbe* probe = nullptr;       // not owned
    LaunchBackend* backend = nullptr;    // not owned; null = nothing can be started (Play is a dry run)
};

struct GameChoice {
    GameInstall install;
    bool is_game = false;                // D2R.exe is in the folder
    std::string label;                   // "D:\SteamLibrary\...\Diablo II Resurrected  (Steam)"
};

struct PlayState {
    bool can = false;
    std::string reason;                  // shown beside the button when !can
};

// A path as the player knows it: backslashes.
std::string DisplayPath(const fs::path& p);

class Model {
public:
    Model(const Options& opt, const Machine& svc);

    const Options& options() const { return opt_; }
    const fs::path& home() const { return home_; }
    bool first_run() const { return first_run_; }
    const std::vector<std::string>& startup_notes() const { return notes_; }   // things the log should say once (a remembered runtime that is gone...)

    // ---- the game
    const std::vector<GameChoice>& games() const { return games_; }
    int game_index() const { return game_; }                 // -1: none
    const GameChoice* game() const { return game_ >= 0 ? &games_[static_cast<size_t>(game_)] : nullptr; }
    void SelectGame(int index);                              // reloads the mods; remembers the folder (not with --game)
    // Browse...: adds `dir` (when it has D2R.exe), selects it and returns its index; -1 and *error otherwise.
    int AddGame(const fs::path& dir, std::string* error);
    const LoaderStatus& loader() const { return loader_; }
    bool loader_hashed() const { return loader_hashed_; }
    void SetLoaderStatus(const LoaderStatus& st);            // the result of the hashing the window does off the UI thread
    std::string GameLine(Tone* tone) const;                  // "Steam edition - build 3.3.93787 - D2RLoader 1.3.1-beta (reviewed files)"

    // ---- the mods of the chosen game
    const std::vector<ModEntry>& mods() const { return mods_; }
    int mod_index() const { return mod_; }                   // -1: none
    const ModEntry* mod() const { return mod_ >= 0 ? &mods_[static_cast<size_t>(mod_)] : nullptr; }
    void SelectMod(int index) { if (index >= -1 && index < static_cast<int>(mods_.size())) mod_ = index; }
    bool SelectModById(const std::string& idOrFolder);       // id or D2RLoader folder name, any case
    // Looks at the disk again (a mod installed while the window is open); true when something changed. The selection stays on the same mod.
    bool Refresh();
    ModTools Tools() const;                                  // what Settings, Logs and Uninstall run for the chosen mod

    // ---- what the servers said about the newest releases (the quiet check at start, the install view's own check)
    void SetLatest(const UpdateInfo& u);                     // an answer; a question that failed (u.checked false) leaves what was known
    const Release* LatestOf(const std::string& id) const;    // nullptr: not asked, or it failed
    // What a mod's card offers: "Install" for a known mod that is not installed, "Update" when the newest release is newer than the installed one,
    // and then the amber badge "Update available: 0.167.0 -> 0.168.0". Nothing for a mod that is current, unknown or not one of the two VR mods.
    struct CardOffer {
        enum class Kind { None, Install, Update } kind = Kind::None;
        std::string label;                                   // "Install", "Update"
        std::string badge;                                   // "Update available: ..." or ""
    };
    CardOffer OfferFor(const ModEntry& m) const;
    // The "Uninstall" button of an installed card. Run: the mod's own uninstaller exists (D2R VR Authentic's unins000.exe in the game folder, D2R VR Unleashed's
    // {localappdata}\Programs\D2R PCVR\unins000.exe, started without /SILENT so its own question shows). Explain: a known mod is installed but has no uninstaller on this
    // computer (a ZIP install): the button says how to remove it. None: not installed, or a D2RLoader mod the launcher does not know (it never removes what it did not install).
    struct UninstallOffer {
        enum class Kind { None, Run, Explain } kind = Kind::None;
        std::string label;                                   // "Uninstall"
        fs::path exe;
        std::vector<std::string> args;
        bool needs_admin = false;
        std::string explanation;                             // for Explain: what the player does by hand
    };
    UninstallOffer UninstallFor(const ModEntry& m) const;
    // Why "Uninstall" is refused right now ("" = it may go on): the game runs, or there is no game folder.
    std::string UninstallRefusal() const;
    // The quiet look at start: the setting is on, there is a game folder, and --no-update-check was not given.
    bool UpdateCheckWanted() const;
    // First run: a game folder was found (not given with --game), none of the two VR mods is installed, and the install view was not offered before.
    bool ShouldOfferInstallView() const;

    // ---- the OpenXR runtime of this start
    const std::vector<RuntimeChoice>& runtimes() const { return runtimes_; }
    int runtime_index() const;                               // of the chosen mod: where its remembered choice is in runtimes()
    fs::path runtime_file() const;                           // for the "file" choice
    // false (and *why) when the choice cannot be used; `file` is for "file". The choice is remembered for the chosen mod.
    bool SetRuntime(int index, const fs::path& file, std::string* why);

    // ---- Play
    PlayState CanPlay(bool game_running) const;
    // Every process name the chosen mod must not run beside (D2R.exe, D2RLoader.exe): what the window polls.
    std::vector<std::string> WatchedProcesses() const;
    bool GameRunning() const;
    // StartMod, then remembers what was used (not after a dry run, not after a refusal). skip_first_start_check: the player chose "Start anyway".
    StartOutcome Play(bool dry_run, bool skip_first_start_check = false);

    // ---- what is remembered
    LauncherSettings& settings() { return settings_; }
    void SaveSettings();                                     // never with --dry-run
    void Log(const std::string& line);                       // <home>\logs\launcher.log, the same file the console launcher writes

    const Machine& services() const { return svc_; }

private:
    void LoadGame();
    void ChooseInitialMod();
    int FindMod(const std::string& idOrFolder) const;

    Options opt_;
    Machine svc_;
    fs::path home_;
    bool first_run_ = false;
    LauncherSettings settings_;
    std::vector<std::string> notes_;
    std::vector<GameChoice> games_;
    int game_ = -1;
    LoaderStatus loader_;
    bool loader_hashed_ = false;
    std::vector<ModEntry> mods_;
    int mod_ = -1;
    std::vector<RuntimeChoice> runtimes_;
    std::map<std::string, Release> latest_;      // by mod id
    Logger log_;
};

// The small links on a known mod's card, in this order: "Website" (`homepage`), then the mod's support place (`support`, the manifest's or else the
// built-in profile's): "Discord" for a discord.gg / discord.com link, else "Support". Only http(s) links; none for another D2RLoader mod.
struct CardLink {
    std::string label, url;
};
std::vector<CardLink> CardLinks(const ModEntry& m);

// What a card's tooltip adds for a known mod that is not installed: the mod's install help (its manifest's `install_help`, else the built-in profile's). "" otherwise.
std::string CardInstallHelp(const ModEntry& m);

// The question asked before a mod's own uninstaller is run (the cards' Uninstall button and the install view's row ask the same): it names the mod and the uninstaller.
std::string UninstallQuestion(const ModEntry& m, const Model::UninstallOffer& o);

// The text a headless `--play` prints (and the window logs): warnings, then the refusal / the plan / "started ...". `exit_code` gets the
// process exit code.
std::string FormatOutcome(const StartOutcome& so, bool dry_run, int* exit_code);

// The services of this machine (registry, process list, CreateProcess). The probe and backend live as long as the process.
Machine SystemMachine();

}  // namespace gui
}  // namespace vrml
