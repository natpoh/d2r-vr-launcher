// gui_install_model.h - what the install view knows and decides, without any window: the three rows (D2RLoader, D2R VR Authentic, D2R VR Unleashed),
// what each says about what is installed and what is newest, which boxes are ticked and why, what may be started and what is refused, what the progress
// and the summary say in words, and the question asked before a release without a published checksum is downloaded. gui_install_view.cpp only shows it;
// the work itself is install_run.h (the same code the headless `--install-run` uses). Tests: tests/gui_install_model_test.cpp.
#pragma once

#include <string>
#include <vector>

#include "gui_model.h"
#include "install_run.h"
#include "mods.h"
#include "releases.h"

namespace vrml {
namespace gui {

// "12.3 MB" / "840 KB"
std::string FormatBytes(uint64_t bytes);

// ---- what the main window's cards show about a release that is newer than the installed one
// "Update available: 0.167.0 -> 0.168.0"; "" when nothing is newer. `installedVersion` "" = installed, version not known.
std::string UpdateBadgeText(const std::string& installedVersion, const std::string& latestVersion);
// Is `latest` newer than what is installed (an installed mod whose version is unknown counts as older). False for a release the server calls up to date, and
// never true for an older or the same release: versions are compared as numbers, a suffix ("-launcher-test", "-modlayout-candidate") is the same base version.
bool IsUpdateFor(const std::string& installedVersion, const Release& latest);

// What an installed version stands to the newest release: the install view's rule for ticking, labelling and the Newest line.
//   Unknown  installed, but its version could not be found out: nothing is proven either way
//   Older    the newest release is newer than what is installed (a real update)
//   Same     what is installed is the newest (or the source says nothing newer exists)
//   Newer    what is installed is newer than the newest release the source names (never offered as an update: that would be a downgrade)
enum class Standing { Unknown, Older, Same, Newer };
Standing StandingOf(const std::string& installedVersion, const Release& latest);

enum class CheckState { NotAsked, Checking, Done, Failed };

// One row of the view, as the window draws it.
struct InstallRow {
    Piece piece = Piece::Loader;
    std::string name;
    std::string installed;          // "reviewed 1.3.1-beta files", "missing", "files differ", "0.167.0", "not installed", "checking its files..."
    Tone installed_tone = Tone::Normal;
    std::string latest;             // "checking...", "0.168.0 (pre-release)", "could not be found out: ...", "1.3.1-beta (pinned)"
    Tone latest_tone = Tone::Normal;
    std::string badge;              // amber: "Update available: 0.167.0 -> 0.168.0"
    std::string source;             // "bodywalkvr.com - no checksum published yet"
    std::string how;                // "Its own Setup window opens; Windows asks for administrator rights."
    std::string help;               // the mod author's install help (ModEntry::install_help), while the mod is not installed; "" otherwise (and always for D2RLoader)
    std::string note;               // why it is ticked, what is wrong with ticking it
    Tone note_tone = Tone::Normal;
    std::string state;              // the mark on the right during and after a run: "Waiting", "Downloading 12.3 of 47.0 MB", "Installed 0.168.0", "Failed"
    Tone state_tone = Tone::Normal;
    bool checked = false;
    bool enabled = true;            // the check box: off for a piece that is up to date (nothing to do) and without a game folder
    // The row's own buttons, beside its Installed / Newest lines.
    std::string action;             // "Install" (not installed), "Update" (newer, or version unknown), "Repair" (D2RLoader's files differ), a greyed "Up to date"; "" = no button
    bool action_enabled = false;    // false for "Up to date", while the answers are still coming, while it runs and while the game runs
    std::string uninstall;          // "Uninstall" on an installed D2R VR Authentic / Unleashed row, else ""; never on D2RLoader (shared by every mod, nothing safe to restore)
    bool uninstall_enabled = false; // on unless an install is running; a running game is the press's refusal ("Save & Exit in the game first"), not a grey button
};

struct StartState {
    bool can = false;
    std::string reason;             // shown by the button when !can
    bool calm = false;              // !can, but nothing is wrong: everything is up to date (the window shows it as plain good news, not as a warning)
};

struct UnverifiedItem {
    Piece piece = Piece::Authentic;
    std::string version;
    std::string source;             // "bodywalkvr.com"
};
struct Prompt {
    std::string title, text;
};

class InstallModel {
public:
    InstallModel();

    // ---- the facts
    void SetGame(const fs::path& dir, bool is_game);
    void SetGameRunning(bool running);
    void SetLoader(const LoaderStatus& st, bool hashed);
    void SetLoaderPinVersion(const std::string& v) { pin_version_ = v; }     // what "newest" says for D2RLoader ("1.3.1-beta")
    // from DiscoverMods: the folder as it is now. `version` is ModEntry::found_version ("" = installed, version not known); `from` is ModEntry::found_from
    // ("manifest", "manifest (not valid)", "plugin"): the row says so when the version did not come from a valid manifest.
    void SetInstalled(Piece mod, bool installed, const std::string& version, const std::string& from = "");
    void SetInstallHelp(Piece mod, const std::string& text);                 // ModEntry::install_help: what its row shows while the mod is not installed
    void SetChecking();                                                       // the question to both servers is out
    void SetUpdate(Piece mod, const UpdateInfo& u);                           // answered; u.checked false: it failed and u.error says why
    CheckState check_state(Piece mod) const { return row(mod).check; }
    bool AllChecksFinished() const;
    bool has_game() const { return game_ok_; }

    // ---- the ticks
    // Nothing is ticked unless it is a real install or update: a missing mod, an installed mod with a PROVEN newer release, D2RLoader when it is missing or its files differ
    // (and a mod is ticked). A mod whose version is unknown is not ticked (its button says Update); one that is current, or newer than the newest release, is never ticked and
    // its box is off - to put it in again, uninstall it first.
    // The card's "Install" / "Update" and a row's own button: exactly these pieces ticked (D2RLoader by its own rule when a mod is ticked), the others not.
    void TickOnly(const std::vector<Piece>& pieces);
    void Select(Piece p, bool on);                                            // the player's click on a box
    bool IsChecked(Piece p) const { return row(p).checked; }
    bool IsUpToDate(Piece p) const;                                           // proven current (or newer than the newest release); D2RLoader: the reviewed files are there

    // ---- what to show
    std::vector<InstallRow> Rows() const;
    StartState CanStart() const;
    // The pieces to run, ticked ones only, D2RLoader first; the release each mod was announced with, `force` for one that is current.
    // `acceptUnverified` is the player's yes (it counts only for a mod whose release has no published checksum).
    std::vector<PieceJob> Jobs(bool acceptUnverified) const;
    // The ticked mods whose release has no published checksum (what the player is asked about before anything is downloaded).
    std::vector<UnverifiedItem> UnverifiedTicked() const;
    static Prompt UnverifiedPrompt(const std::vector<UnverifiedItem>& items);

    // ---- a run
    void BeginRun(const std::vector<PieceJob>& jobs);
    void SetStage(Piece p, int index, int count, const std::string& stage, uint64_t done, uint64_t total);
    void SetOutcome(const PieceOutcome& o);
    void EndRun();
    bool running() const { return running_; }
    void RequestCancel();
    bool cancel_requested() const { return cancel_; }
    bool setup_open() const { return setup_open_; }
    double Progress() const;                         // 0..1 for the bar over the whole run
    std::string StepText() const;                    // "Step 2 of 3 - D2R VR Authentic: downloading 12.3 of 47.0 MB (26 %)"
    // What is worth saying while it runs: the Setup's window is open (finish or close the wizard), or what a cancel does and does not stop. "" otherwise.
    std::string RunNotice(Tone* tone) const;
    const std::vector<PieceOutcome>& outcomes() const { return outcomes_; }
    // One line per piece that ran: "D2R VR Authentic: failed - You declined the administrator prompt (E_UAC_DECLINED)".
    std::vector<std::string> SummaryLines() const;
    std::string Headline(Tone* tone) const;          // "Done: 2 installed" / "1 installed, 1 failed - see the marks and the log"

    // ---- words for one outcome
    static std::string Meaning(const PieceOutcome& o);                  // for the log and the summary
    static std::string ShortMark(const PieceOutcome& o, Tone* tone);    // for the mark on the row

    // the pure part of Progress(), for tests
    static double SlotFraction(Piece p, const std::string& stage, uint64_t done, uint64_t total);

private:
    struct RowData {
        Piece piece = Piece::Loader;
        bool checked = false, touched = false;
        bool installed = false;
        std::string installed_version, installed_from;
        std::string help;
        CheckState check = CheckState::NotAsked;
        UpdateInfo upd;
        std::string state;
        Tone state_tone = Tone::Normal;
        bool in_run = false;
    };
    RowData& row(Piece p) { return rows_[static_cast<size_t>(p)]; }
    const RowData& row(Piece p) const { return rows_[static_cast<size_t>(p)]; }
    bool NeedsLoader() const;
    bool LoaderMissing() const { return game_ok_ && !loader_.present; }
    bool LoaderReviewed() const { return game_ok_ && loader_.present && loader_hashed_ && loader_.reviewed; }
    bool AnyModTicked() const;
    bool WantsByDefault(Piece mod) const;
    bool Outdated(Piece mod) const;                 // the newest release is PROVEN newer than the installed version
    bool UnknownVersion(Piece mod) const;           // installed, answered, and the installed version could not be read
    bool Current(Piece mod) const;                  // installed and the newest release is not newer (the same, or older than what is installed)
    bool Selectable(Piece p) const;                 // the box may be ticked: there is something to install or fix
    bool NothingToDo() const;                       // every piece is up to date (the answers are in)
    void Recompute();
    std::string ModsTicked() const;

    RowData rows_[kPieceCount];
    fs::path game_;
    bool game_ok_ = false, game_running_ = false;
    bool dropped_ = false;                          // a box the player (or a card) ticked was unticked because the answer says it is up to date
    LoaderStatus loader_;
    bool loader_hashed_ = false;
    std::string pin_version_ = "1.3.1-beta";

    bool running_ = false, cancel_ = false, setup_open_ = false;
    std::vector<Piece> run_order_;
    int run_index_ = 0, run_count_ = 0;
    Piece run_piece_ = Piece::Loader;
    std::string run_stage_;
    uint64_t run_done_ = 0, run_total_ = 0;
    std::vector<PieceOutcome> outcomes_;
};

}  // namespace gui
}  // namespace vrml
