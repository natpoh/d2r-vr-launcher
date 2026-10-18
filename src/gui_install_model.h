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
// Is `latest` newer than what is installed (an installed mod whose version is unknown counts as older). False for a release the server calls up to date.
bool IsUpdateFor(const std::string& installedVersion, const Release& latest);

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
    std::string note;               // why it is ticked, what is wrong with ticking it
    Tone note_tone = Tone::Normal;
    std::string state;              // the mark on the right during and after a run: "Waiting", "Downloading 12.3 of 47.0 MB", "Installed 0.168.0", "Failed"
    Tone state_tone = Tone::Normal;
    bool checked = false;
    bool enabled = true;
};

struct StartState {
    bool can = false;
    std::string reason;             // shown by the button when !can
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
    void SetInstalled(Piece mod, bool installed, const std::string& version);     // from DiscoverMods: the folder as it is now
    void SetChecking();                                                       // the question to both servers is out
    void SetUpdate(Piece mod, const UpdateInfo& u);                           // answered; u.checked false: it failed and u.error says why
    CheckState check_state(Piece mod) const { return row(mod).check; }
    bool AllChecksFinished() const;
    bool has_game() const { return game_ok_; }

    // ---- the ticks
    // The card's "Install" / "Update": exactly these mods ticked (D2RLoader by its own rule), the others not.
    void TickOnly(const std::vector<Piece>& pieces);
    void Select(Piece p, bool on);                                            // the player's click on a box
    bool IsChecked(Piece p) const { return row(p).checked; }

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
        std::string installed_version;
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
    bool AnyModTicked() const;
    bool WantsByDefault(Piece mod) const;
    bool Outdated(Piece mod) const;
    bool Current(Piece mod) const;
    void Recompute();
    std::string ModsTicked() const;

    RowData rows_[kPieceCount];
    fs::path game_;
    bool game_ok_ = false, game_running_ = false;
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
