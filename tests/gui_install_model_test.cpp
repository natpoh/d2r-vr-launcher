// What the install view decides, without the window: the rows and their words, which boxes are ticked and why, what may be started and what is refused,
// the jobs it hands to the worker, the question asked before a download without a published checksum, the progress and the summary of a run.
// (The window itself is checked by tools/install_view_smoke.py.)
#include "gui_install_model.h"
#include "sandbox.h"
#include "test.h"

using namespace vrml;
using namespace vrml::gui;

namespace {

bool Has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

UpdateInfo Answer(Piece p, const std::string& version, const std::string& sha = "", bool upToDate = false) {
    UpdateInfo u;
    u.id = PieceId(p);
    u.name = PieceName(p);
    u.checked = true;
    u.latest.id = u.id;
    u.latest.version = version;
    u.latest.source = p == Piece::Authentic ? "bodywalkvr.com" : "github.com/bearstonem/d2r-pcvr";
    u.latest.sha256 = sha;
    u.latest.sha256_source = sha.empty() ? "" : "digest";
    u.latest.up_to_date = upToDate;
    if (!upToDate) {
        u.latest.url = "https://example.invalid/" + version;
        u.latest.asset_name = "Setup-" + version + ".exe";
    }
    return u;
}

UpdateInfo Failed(const std::string& error) {
    UpdateInfo u;
    u.checked = false;
    u.error = error;
    return u;
}

const std::string kSha(64, 'a');

// A fresh game: no D2RLoader, no mod; both servers have answered.
struct Fresh {
    InstallModel m;
    Fresh() {
        m.SetGame("G:/Game", true);
        LoaderStatus none;
        m.SetLoader(none, false);
        m.SetInstalled(Piece::Authentic, false, "");
        m.SetInstalled(Piece::Unleashed, false, "");
    }
    void Answers(const std::string& authSha = "", const std::string& unleashedSha = kSha) {
        m.SetChecking();
        m.SetUpdate(Piece::Authentic, Answer(Piece::Authentic, "0.168.0", authSha));
        m.SetUpdate(Piece::Unleashed, Answer(Piece::Unleashed, "0.3.19", unleashedSha));
    }
};

LoaderStatus Reviewed() {
    LoaderStatus s;
    s.present = s.reviewed = true;
    s.version = "1.3.1-beta";
    return s;
}

const InstallRow& Row(const std::vector<InstallRow>& rows, Piece p) { return rows[static_cast<size_t>(p)]; }

}  // namespace

TEST(a_fresh_game_lists_the_three_pieces_and_says_what_is_missing) {
    Fresh f;
    f.m.SetChecking();
    std::vector<InstallRow> rows = f.m.Rows();
    REQUIRE(rows.size() == 3);
    CHECK_EQ(Row(rows, Piece::Loader).name, std::string("D2RLoader"));
    CHECK_EQ(Row(rows, Piece::Authentic).name, std::string("D2R VR Authentic"));
    CHECK_EQ(Row(rows, Piece::Unleashed).name, std::string("D2R VR Unleashed"));
    CHECK_EQ(Row(rows, Piece::Loader).installed, std::string("missing"));
    CHECK(Row(rows, Piece::Loader).installed_tone == Tone::Warn);
    CHECK_EQ(Row(rows, Piece::Loader).latest, std::string("1.3.1-beta (pinned)"));
    CHECK_EQ(Row(rows, Piece::Loader).source, std::string("d2rloader.net, hash pinned in the launcher"));
    CHECK_EQ(Row(rows, Piece::Authentic).installed, std::string("not installed"));
    // while the servers are being asked: "checking...", nothing ticked, nothing to start
    CHECK_EQ(Row(rows, Piece::Authentic).latest, std::string("checking..."));
    CHECK_EQ(Row(rows, Piece::Unleashed).latest, std::string("checking..."));
    CHECK(!Row(rows, Piece::Authentic).checked && !Row(rows, Piece::Loader).checked);
    CHECK(!f.m.AllChecksFinished());
    CHECK(!f.m.CanStart().can);
    CHECK(Has(f.m.CanStart().reason, "Still finding out what is installed"));

    f.Answers();
    rows = f.m.Rows();
    CHECK_EQ(Row(rows, Piece::Authentic).latest, std::string("0.168.0"));
    CHECK_EQ(Row(rows, Piece::Authentic).source, std::string("bodywalkvr.com - no checksum published yet"));
    CHECK_EQ(Row(rows, Piece::Unleashed).source, std::string("GitHub release of bearstonem/d2r-pcvr, SHA-256 from GitHub"));
    CHECK(Has(Row(rows, Piece::Authentic).how, "administrator"));
    CHECK(Has(Row(rows, Piece::Unleashed).how, "no administrator prompt"));
    CHECK(f.m.AllChecksFinished());
}

TEST(a_mod_that_is_not_installed_shows_its_install_help_in_its_row) {
    // bearstonem's review: his words to players about installing D2R VR Unleashed, wherever the launcher shows install help for it
    Fresh f;
    f.m.SetInstallHelp(Piece::Unleashed, "Close D2R and fully exit Steam before installing.");
    f.m.SetInstallHelp(Piece::Loader, "never shown");
    f.m.SetChecking();
    CHECK_EQ(Row(f.m.Rows(), Piece::Unleashed).help, std::string("Close D2R and fully exit Steam before installing."));     // also while the servers are asked
    f.Answers();
    std::vector<InstallRow> rows = f.m.Rows();
    CHECK_EQ(Row(rows, Piece::Unleashed).help, std::string("Close D2R and fully exit Steam before installing."));
    CHECK(Row(rows, Piece::Authentic).help.empty());                 // no text given: nothing
    CHECK(Row(rows, Piece::Loader).help.empty());                    // D2RLoader has none
    // once it is installed the row says what is installed and newest instead
    f.m.SetInstalled(Piece::Unleashed, true, "0.3.19");
    CHECK(Row(f.m.Rows(), Piece::Unleashed).help.empty());
    f.m.SetInstalled(Piece::Unleashed, false, "");
    CHECK(!Row(f.m.Rows(), Piece::Unleashed).help.empty());
}

TEST(authentic_says_when_a_checksum_is_published) {
    Fresh f;
    f.Answers(kSha);
    CHECK_EQ(Row(f.m.Rows(), Piece::Authentic).source, std::string("bodywalkvr.com - checksum published"));
    CHECK(f.m.UnverifiedTicked().empty() || f.m.UnverifiedTicked().front().piece != Piece::Authentic);
}

TEST(missing_pieces_are_ticked_and_the_loader_follows_the_mods) {
    Fresh f;
    f.Answers();
    std::vector<InstallRow> rows = f.m.Rows();
    CHECK(Row(rows, Piece::Authentic).checked && Row(rows, Piece::Unleashed).checked);       // missing: ticked
    CHECK(Row(rows, Piece::Loader).checked);                                                 // missing, and a mod is ticked
    CHECK(Has(Row(rows, Piece::Loader).note, "D2R VR Authentic and D2R VR Unleashed run only under D2RLoader, which is missing"));
    // unticking the last mod lets the loader (which the player never touched) go
    f.m.Select(Piece::Authentic, false);
    CHECK(f.m.IsChecked(Piece::Loader));
    f.m.Select(Piece::Unleashed, false);
    CHECK(!f.m.IsChecked(Piece::Loader));
    CHECK(!f.m.CanStart().can);
    CHECK_EQ(f.m.CanStart().reason, std::string("Tick what you want to install."));
}

TEST(ticking_a_mod_while_the_loader_is_missing_ticks_the_loader_and_says_why) {
    Fresh f;
    f.Answers();
    f.m.Select(Piece::Authentic, false);
    f.m.Select(Piece::Unleashed, false);
    CHECK(!f.m.IsChecked(Piece::Loader));
    f.m.Select(Piece::Unleashed, true);
    CHECK(f.m.IsChecked(Piece::Loader));
    const InstallRow loader = Row(f.m.Rows(), Piece::Loader);
    CHECK(Has(loader.note, "Ticked because D2R VR Unleashed runs only under D2RLoader, which is missing"));
    // the player unticks the loader himself: it stays unticked until a mod is ticked again, and the row warns
    f.m.Select(Piece::Unleashed, false);
    f.m.Select(Piece::Loader, true);
    f.m.Select(Piece::Loader, false);
    CHECK(!f.m.IsChecked(Piece::Loader));
    f.m.Select(Piece::Authentic, true);
    CHECK(f.m.IsChecked(Piece::Loader));                                                     // ticking a mod ticks it again: the mods run only under it
}

TEST(unticking_the_loader_with_a_mod_ticked_is_allowed_but_warned_about_when_it_cannot_be_forced) {
    Fresh f;
    f.Answers();
    // the loader is there but not the reviewed files: pre-ticked, and the player may untick it
    LoaderStatus differs;
    differs.present = true;
    differs.reviewed = false;
    f.m.SetLoader(differs, true);
    CHECK(f.m.IsChecked(Piece::Loader));
    CHECK(Has(Row(f.m.Rows(), Piece::Loader).note, "not the reviewed ones"));
    CHECK_EQ(Row(f.m.Rows(), Piece::Loader).installed, std::string("files differ"));
    f.m.Select(Piece::Loader, false);
    CHECK(!f.m.IsChecked(Piece::Loader));
    f.m.Select(Piece::Authentic, false);
    f.m.Select(Piece::Authentic, true);
    CHECK(!f.m.IsChecked(Piece::Loader));                                                    // present (not missing): nothing forces it
}

TEST(a_reviewed_loader_is_not_ticked_and_says_so) {
    Fresh f;
    f.m.SetLoader(Reviewed(), true);
    f.Answers();
    const std::vector<InstallRow> rows = f.m.Rows();
    CHECK_EQ(Row(rows, Piece::Loader).installed, std::string("reviewed 1.3.1-beta files"));
    CHECK(Row(rows, Piece::Loader).installed_tone == Tone::Ok);
    CHECK(!Row(rows, Piece::Loader).checked);
    CHECK(Row(rows, Piece::Authentic).checked);
    CHECK(f.m.CanStart().can);
    // a loader that is there but not hashed yet says it is being looked at, and is not pre-ticked
    LoaderStatus found;
    found.present = true;
    f.m.SetLoader(found, false);
    CHECK_EQ(Row(f.m.Rows(), Piece::Loader).installed, std::string("found, checking its files..."));
    CHECK(!f.m.IsChecked(Piece::Loader));
    CHECK(!f.m.AllChecksFinished());
}

TEST(an_outdated_mod_shows_the_badge_and_is_ticked_a_current_one_is_not) {
    Fresh f;
    f.m.SetLoader(Reviewed(), true);
    f.m.SetInstalled(Piece::Authentic, true, "0.167.0");
    f.m.SetInstalled(Piece::Unleashed, true, "0.3.19");
    f.m.SetChecking();
    f.m.SetUpdate(Piece::Authentic, Answer(Piece::Authentic, "0.168.0"));
    f.m.SetUpdate(Piece::Unleashed, Answer(Piece::Unleashed, "0.3.19", kSha, true));
    const std::vector<InstallRow> rows = f.m.Rows();
    const InstallRow& a = Row(rows, Piece::Authentic);
    CHECK_EQ(a.installed, std::string("0.167.0"));
    CHECK_EQ(a.latest, std::string("0.168.0"));
    CHECK_EQ(a.badge, std::string("Update available: 0.167.0 -> 0.168.0"));
    CHECK(a.checked);
    const InstallRow& u = Row(rows, Piece::Unleashed);
    CHECK(u.badge.empty());
    CHECK(!u.checked);
    CHECK(Has(u.latest, "you have the newest version"));
    // a current one cannot be ticked any more (no "repair" tick): its box is off, a click does nothing, no job is made for it
    CHECK(!u.enabled);
    f.m.Select(Piece::Unleashed, true);
    CHECK(!f.m.IsChecked(Piece::Unleashed));
    for (const PieceJob& j : f.m.Jobs(false)) CHECK(j.piece != Piece::Unleashed && !j.force);
}

TEST(an_installed_mod_whose_version_is_unknown_counts_as_outdated) {
    CHECK(IsUpdateFor("", Answer(Piece::Authentic, "0.168.0").latest));
    CHECK(IsUpdateFor("0.167.0", Answer(Piece::Authentic, "0.168.0").latest));
    CHECK(!IsUpdateFor("0.168.0", Answer(Piece::Authentic, "0.168.0").latest));
    CHECK(!IsUpdateFor("0.169.0", Answer(Piece::Authentic, "0.168.0").latest));
    CHECK(!IsUpdateFor("0.1.0", Answer(Piece::Authentic, "0.168.0", "", true).latest));      // the server says nothing newer
    CHECK_EQ(UpdateBadgeText("0.167.0", "0.168.0"), std::string("Update available: 0.167.0 -> 0.168.0"));
    CHECK_EQ(UpdateBadgeText("", "0.168.0"), std::string("Update available: 0.168.0"));
    CHECK_EQ(UpdateBadgeText("0.1.0", ""), std::string());
}

TEST(a_failed_question_is_said_in_words_and_blocks_only_what_needs_it) {
    Fresh f;
    f.m.SetChecking();
    f.m.SetUpdate(Piece::Authentic, Failed("could not ask bodywalkvr.com for the newest D2R VR Authentic: cannot reach bodywalkvr.com: A connection with the server could not be established (error 12029)"));
    f.m.SetUpdate(Piece::Unleashed, Answer(Piece::Unleashed, "0.3.19", kSha));
    std::vector<InstallRow> rows = f.m.Rows();
    CHECK_EQ(Row(rows, Piece::Authentic).latest, std::string("could not be checked: no connection to the server"));
    CHECK(Row(rows, Piece::Authentic).latest_tone == Tone::Warn);
    CHECK(!Row(rows, Piece::Authentic).checked);                                             // not ticked by itself
    CHECK(f.m.AllChecksFinished());
    // Unleashed alone can be installed
    f.m.Select(Piece::Authentic, false);
    CHECK(f.m.CanStart().can);
    // the player ticks the one that could not be checked: refused, with the reason
    f.m.Select(Piece::Authentic, true);
    const StartState ss = f.m.CanStart();
    CHECK(!ss.can);
    CHECK(Has(ss.reason, "D2R VR Authentic could not be found out"));
    CHECK(Has(ss.reason, "Check again"));
    CHECK(Has(Row(f.m.Rows(), Piece::Authentic).note, "press Check again"));
    f.m.Select(Piece::Authentic, false);
    // other wordings of a problem stay short
    f.m.SetUpdate(Piece::Unleashed, Failed("the answer about the newest D2R VR Unleashed is not readable (x)"));
    CHECK_EQ(Row(f.m.Rows(), Piece::Unleashed).latest, std::string("could not be checked: an answer that makes no sense"));
}

TEST(what_may_be_started_and_why_not) {
    InstallModel m;
    CHECK(!m.CanStart().can);                                                                // no game at all
    CHECK(Has(m.CanStart().reason, "Pick the folder"));
    CHECK(!Row(m.Rows(), Piece::Loader).enabled);                                            // nothing can be ticked without a game
    m.SetGame("G:/Not a game", false);
    CHECK(Has(m.CanStart().reason, "Pick the folder"));
    Fresh f;
    f.Answers();
    CHECK(f.m.CanStart().can);
    f.m.SetGameRunning(true);
    CHECK(!f.m.CanStart().can);
    CHECK(Has(f.m.CanStart().reason, "game is running"));
    f.m.SetGameRunning(false);
    CHECK(f.m.CanStart().can);
    f.m.Select(Piece::Authentic, true);
    f.m.SetChecking();                                                                       // asked again: a ticked mod is still waiting for its answer
    CHECK(!f.m.CanStart().can);
    CHECK(Has(f.m.CanStart().reason, "Still finding out"));
}

TEST(the_jobs_come_in_the_right_order_with_what_the_player_saw) {
    Fresh f;
    f.Answers("", kSha);
    std::vector<PieceJob> jobs = f.m.Jobs(true);
    REQUIRE(jobs.size() == 3);
    CHECK(jobs[0].piece == Piece::Loader && jobs[1].piece == Piece::Authentic && jobs[2].piece == Piece::Unleashed);
    CHECK(!jobs[0].has_release);
    CHECK(jobs[1].has_release && jobs[1].release.version == "0.168.0");
    CHECK(jobs[1].accept_unverified);                                                        // no published checksum, and the player said yes
    CHECK(jobs[2].has_release && !jobs[2].accept_unverified);                                // GitHub's digest is there: nothing to accept
    CHECK(!f.m.Jobs(false)[1].accept_unverified);
    CHECK(!jobs[1].force && !jobs[2].force);
}

TEST(a_download_without_a_published_checksum_is_asked_about_first) {
    Fresh f;
    f.Answers();                                                                             // Authentic: no checksum; Unleashed: GitHub's digest
    std::vector<UnverifiedItem> items = f.m.UnverifiedTicked();
    REQUIRE(items.size() == 1);
    CHECK(items[0].piece == Piece::Authentic);
    CHECK_EQ(items[0].version, std::string("0.168.0"));
    const Prompt p = InstallModel::UnverifiedPrompt(items);
    CHECK_EQ(p.title, std::string("No checksum published"));
    CHECK(Has(p.text, "D2R VR Authentic 0.168.0 comes from bodywalkvr.com, which does not publish a checksum for it"));
    CHECK(Has(p.text, "cannot prove"));
    CHECK(Has(p.text, "https and on bodywalkvr.com only"));
    CHECK(Has(p.text, "SHA-256 is written to the log"));
    CHECK(Has(p.text, "probably fine"));
    CHECK(Has(p.text, "will publish") || Has(p.text, "published later"));
    CHECK(Has(p.text, "trust bodywalkvr.com"));
    CHECK(Has(p.text, "anyway?"));
    // both without: a list
    f.m.SetUpdate(Piece::Unleashed, Answer(Piece::Unleashed, "0.3.19", ""));
    items = f.m.UnverifiedTicked();
    REQUIRE(items.size() == 2);
    const Prompt both = InstallModel::UnverifiedPrompt(items);
    CHECK(Has(both.text, "These downloads have no published checksum"));
    CHECK(Has(both.text, "D2R VR Unleashed 0.3.19 (github.com/bearstonem/d2r-pcvr)"));
    // an unticked one is not asked about
    f.m.Select(Piece::Authentic, false);
    CHECK_EQ(f.m.UnverifiedTicked().size(), size_t(1));
}

TEST(tick_only_is_what_a_cards_install_button_asks_for) {
    Fresh f;
    f.Answers();
    f.m.TickOnly({Piece::Unleashed});
    CHECK(f.m.IsChecked(Piece::Unleashed));
    CHECK(!f.m.IsChecked(Piece::Authentic));
    CHECK(f.m.IsChecked(Piece::Loader));                                                     // missing: it goes with the mod
    // answers that come later do not undo what the card asked for
    f.m.SetUpdate(Piece::Authentic, Answer(Piece::Authentic, "0.168.0"));
    CHECK(!f.m.IsChecked(Piece::Authentic));
    f.m.TickOnly({});
    CHECK(!f.m.IsChecked(Piece::Unleashed) && !f.m.IsChecked(Piece::Loader));
}

TEST(a_run_marks_the_rows_and_counts_the_progress_and_says_what_it_did) {
    Fresh f;
    f.Answers();
    const std::vector<PieceJob> jobs = f.m.Jobs(true);
    f.m.BeginRun(jobs);
    CHECK(f.m.running());
    std::vector<InstallRow> rows = f.m.Rows();
    CHECK_EQ(Row(rows, Piece::Loader).state, std::string("Waiting"));
    f.m.Select(Piece::Loader, false);
    CHECK(f.m.IsChecked(Piece::Loader));                                                     // no ticking while it runs: the click is ignored
    CHECK(!f.m.CanStart().can);
    CHECK_EQ(f.m.Progress(), 0.0);

    f.m.SetStage(Piece::Loader, 0, 3, "download", 2 * 1024 * 1024, 8 * 1024 * 1024);
    CHECK(Has(f.m.StepText(), "Step 1 of 3 - D2RLoader: downloading 2.0 MB of 8.0 MB (25 %)"));
    CHECK(Has(Row(f.m.Rows(), Piece::Loader).state, "Downloading 2.0 MB of 8.0 MB"));
    const double p1 = f.m.Progress();
    f.m.SetStage(Piece::Loader, 0, 3, "download", 8 * 1024 * 1024, 8 * 1024 * 1024);
    const double p2 = f.m.Progress();
    f.m.SetStage(Piece::Loader, 0, 3, "install", 2, 4);
    const double p3 = f.m.Progress();
    CHECK(p1 > 0.0 && p2 > p1 && p3 > p2 && p3 < 1.0 / 3.0);

    PieceOutcome ol;
    ol.piece = Piece::Loader;
    ol.status = JobStatus::Installed;
    ol.version = "1.3.1-beta";
    f.m.SetOutcome(ol);
    CHECK(Row(f.m.Rows(), Piece::Loader).state_tone == Tone::Ok);
    CHECK(f.m.Progress() >= 1.0 / 3.0 - 1e-9);

    f.m.SetStage(Piece::Authentic, 1, 3, "setup", 0, 0);
    CHECK(f.m.setup_open());
    CHECK(Has(f.m.StepText(), "Step 2 of 3 - D2R VR Authentic: its Setup is running - finish or close its wizard"));
    Tone t = Tone::Normal;
    CHECK(Has(f.m.RunNotice(&t), "finish or close its wizard"));
    f.m.RequestCancel();
    CHECK(f.m.cancel_requested());
    CHECK(Has(f.m.RunNotice(&t), "Setup's window is open and is not closed by this"));
    CHECK(t == Tone::Warn);

    PieceOutcome oa;
    oa.piece = Piece::Authentic;
    oa.status = JobStatus::Failed;
    oa.code = "E_UAC_DECLINED";
    oa.message = "Windows asked ... No.";
    f.m.SetOutcome(oa);
    PieceOutcome ou;
    ou.piece = Piece::Unleashed;
    ou.status = JobStatus::NotRun;
    ou.code = "E_CANCELLED";
    ou.message = "Not started: the install was cancelled.";
    f.m.SetOutcome(ou);
    f.m.EndRun();
    CHECK(!f.m.running());
    CHECK_EQ(f.m.Progress(), 1.0);
    rows = f.m.Rows();
    CHECK_EQ(Row(rows, Piece::Authentic).state, std::string("Failed"));
    CHECK(Row(rows, Piece::Authentic).state_tone == Tone::Error);
    CHECK(Has(Row(rows, Piece::Authentic).note, "you declined the administrator prompt"));
    CHECK_EQ(Row(rows, Piece::Unleashed).state, std::string("Not run"));
    const std::vector<std::string> lines = f.m.SummaryLines();
    REQUIRE(lines.size() == 3);
    CHECK_EQ(lines[0], std::string("D2RLoader: installed"));
    CHECK(Has(lines[1], "D2R VR Authentic: you declined the administrator prompt, so nothing was installed (E_UAC_DECLINED)"));
    CHECK(Has(lines[2], "Not started"));
    Tone ht = Tone::Normal;
    const std::string head = f.m.Headline(&ht);
    CHECK(Has(head, "1 installed") && Has(head, "1 failed") && Has(head, "1 not run"));
    CHECK(ht == Tone::Error);
    CHECK(f.m.CanStart().can);                                                               // it may be tried again
}

TEST(every_failure_has_words_and_a_short_mark) {
    auto mean = [](const std::string& code, int setupExit = -1, const std::string& msg = "") {
        PieceOutcome o;
        o.piece = Piece::Authentic;
        o.status = JobStatus::Failed;
        o.code = code;
        o.setup_exit = setupExit;
        o.message = msg;
        return InstallModel::Meaning(o);
    };
    CHECK(Has(mean("E_UAC_DECLINED"), "you declined the administrator prompt"));
    CHECK(Has(mean("E_UNVERIFIED"), "no checksum is published"));
    CHECK(Has(mean("E_HASH", -1, "the downloaded x has SHA-256 aa"), "a checksum did not match"));
    CHECK(Has(mean("E_SETUP", 4), "the Setup stopped with an error while installing (exit code 4)"));
    CHECK(Has(mean("E_SETUP", 42), "exit code 42"));
    CHECK(Has(mean("E_MOD_NOT_INSTALLED", 0), "not in this game folder"));
    CHECK(Has(mean("E_GAME_RUNNING"), "the game is running"));
    CHECK(Has(mean("E_ARCHIVE", -1, "bad"), "damaged"));
    CHECK(Has(mean("E_UNTRUSTED", -1, "bad"), "does not trust"));
    CHECK(Has(mean("E_IO", -1, "administrator rights are needed for this folder (G:/x)"), "Run as administrator"));
    CHECK(Has(mean("E_IO", -1, "cannot reach the server"), "a network or disk problem"));
    CHECK(Has(mean("E_ROLLBACK", -1, "x"), "putting the old files back failed"));
    PieceOutcome c;
    c.piece = Piece::Authentic;
    c.status = JobStatus::Cancelled;
    c.code = "E_CANCELLED";
    CHECK(Has(InstallModel::Meaning(c), "the download was stopped and deleted"));
    c.setup_exit = 5;
    CHECK(Has(InstallModel::Meaning(c), "cancelled in its wizard"));
    Tone t = Tone::Normal;
    CHECK_EQ(InstallModel::ShortMark(c, &t), std::string("Cancelled"));
    CHECK(t == Tone::Warn);
    PieceOutcome ok;
    ok.piece = Piece::Authentic;
    ok.status = JobStatus::Installed;
    ok.version = "0.168.0";
    ok.sha256 = std::string(64, 'b');
    CHECK_EQ(InstallModel::ShortMark(ok, &t), std::string("Installed 0.168.0"));
    CHECK(t == Tone::Ok);
    CHECK(Has(InstallModel::Meaning(ok), "no published checksum to compare with"));
    ok.sha_verified = true;
    CHECK_EQ(InstallModel::Meaning(ok), std::string("installed 0.168.0"));
}

TEST(the_slot_of_a_piece_in_the_bar_grows_with_the_work) {
    CHECK(InstallModel::SlotFraction(Piece::Loader, "download", 0, 100) < InstallModel::SlotFraction(Piece::Loader, "download", 100, 100));
    CHECK(InstallModel::SlotFraction(Piece::Loader, "download", 100, 100) < InstallModel::SlotFraction(Piece::Loader, "unpack", 0, 0));
    CHECK(InstallModel::SlotFraction(Piece::Loader, "unpack", 0, 0) < InstallModel::SlotFraction(Piece::Loader, "verify", 0, 0));
    CHECK(InstallModel::SlotFraction(Piece::Loader, "verify", 0, 0) < InstallModel::SlotFraction(Piece::Loader, "install", 0, 4));
    CHECK(InstallModel::SlotFraction(Piece::Loader, "install", 4, 4) <= 1.0);
    CHECK(InstallModel::SlotFraction(Piece::Authentic, "download", 50, 100) < InstallModel::SlotFraction(Piece::Authentic, "setup", 0, 0));
    CHECK(InstallModel::SlotFraction(Piece::Authentic, "setup", 0, 0) < 1.0);
    CHECK(InstallModel::SlotFraction(Piece::Authentic, "download", 50, 0) > 0.0);            // a size that is not known still moves the bar
    CHECK_EQ(FormatBytes(5), std::string("5 bytes"));
    CHECK_EQ(FormatBytes(2048), std::string("2 KB"));
    CHECK_EQ(FormatBytes(3 * 1024 * 1024 / 2), std::string("1.5 MB"));
}

TEST(a_finished_run_that_went_well_says_done) {
    Fresh f;
    f.Answers();
    f.m.BeginRun(f.m.Jobs(true));
    for (Piece p : {Piece::Loader, Piece::Authentic, Piece::Unleashed}) {
        PieceOutcome o;
        o.piece = p;
        o.status = p == Piece::Loader ? JobStatus::AlreadyThere : JobStatus::Installed;
        o.version = "1.0.0";
        f.m.SetOutcome(o);
    }
    f.m.EndRun();
    Tone t = Tone::Normal;
    const std::string head = f.m.Headline(&t);
    CHECK_EQ(head, std::string("Done: 2 installed, 1 already there."));
    CHECK(t == Tone::Ok);
    CHECK(Has(f.m.SummaryLines()[0], "already installed"));
}

// ================================================================== the rows: what is installed against what is newest, the ticks, the row's own buttons

namespace {

// a failed expectation of a table, with the row of the table in the message
void Expect(bool ok, const std::string& what) {
    if (!ok) t::Fail(__FILE__, __LINE__, what);
}

// The site's answer when it knows what is installed and has nothing newer ("0.0.0" for Authentic, "up_to_date" for the model), optionally naming its own older number.
UpdateInfo Quiet(Piece p, const std::string& installed, const std::string& announced = "") {
    UpdateInfo u = Answer(p, installed, "", true);
    u.latest.announced = announced;
    return u;
}

// A game with D2RLoader's reviewed files; both servers asked (the answers are set by the test).
struct Quiet2 {
    InstallModel m;
    Quiet2() {
        m.SetGame("G:/Game", true);
        m.SetLoader(Reviewed(), true);
    }
    void Mod(Piece p, bool installed, const std::string& version, const UpdateInfo& answer, const std::string& from = "manifest") {
        m.SetInstalled(p, installed, version, from);
        m.SetUpdate(p, answer);
    }
};

}  // namespace

TEST(a_downgrade_is_never_offered_and_a_suffix_is_the_same_base_version) {
    struct Case {
        const char* installed;
        const char* newest;
        Standing want;
    };
    const Case cases[] = {
        {"0.170.0", "0.169", Standing::Newer},
        {"0.170.0", "0.169.0", Standing::Newer},
        {"0.170.0", "0.170.0", Standing::Same},
        {"0.170.0", "0.171", Standing::Older},
        {"0.170.0-launcher-test", "0.169", Standing::Newer},
        {"0.170.0-launcher-test", "0.169.0", Standing::Newer},
        {"0.170.0-launcher-test", "0.170.0", Standing::Same},
        {"0.170.0-launcher-test", "0.171", Standing::Older},
        {"0.169.0-modlayout-candidate", "0.169", Standing::Same},
        {"0.169.0-modlayout-candidate", "0.169.0", Standing::Same},
        {"0.169.0-modlayout-candidate", "0.170.0", Standing::Older},
        {"0.169.0-modlayout-candidate", "0.171", Standing::Older},
        {"0.3.19", "0.3.19", Standing::Same},
        {"0.3.19", "0.3.17", Standing::Newer},
        {"0.3.19", "0.3.20", Standing::Older},
    };
    for (const Case& c : cases) {
        const std::string label = std::string("installed ") + c.installed + ", newest " + c.newest;
        const Release r = Answer(Piece::Authentic, c.newest).latest;               // a source that names its number whatever is installed
        Expect(StandingOf(c.installed, r) == c.want, label + ": standing");
        Expect(IsUpdateFor(c.installed, r) == (c.want == Standing::Older), label + ": IsUpdateFor");
        Expect(IsNewer(c.newest, c.installed) == (c.want == Standing::Older), label + ": IsNewer");
        // ... and the row of the install view
        Quiet2 q;
        q.Mod(Piece::Authentic, true, c.installed, Answer(Piece::Authentic, c.newest));
        q.Mod(Piece::Unleashed, true, "0.3.19", Quiet(Piece::Unleashed, "0.3.19"));
        const std::vector<InstallRow> all = q.m.Rows();
        const InstallRow& a = Row(all, Piece::Authentic);
        const bool update = c.want == Standing::Older;
        Expect(a.checked == update, label + ": ticked only for a proven update");
        Expect(a.badge.empty() == !update, label + ": the amber badge only for a proven update");
        Expect(a.action == (update ? "Update" : "Up to date") && a.action_enabled == update, label + ": the row button");
        Expect(a.enabled == update, label + ": the box is on only for a proven update");
        if (c.want == Standing::Newer) Expect(a.latest == std::string(c.newest) + " - you have a newer version", label + ": the Newest line says so (" + a.latest + ")");
        if (c.want == Standing::Same) Expect(Has(a.latest, "you have the newest version"), label + ": the Newest line (" + a.latest + ")");
        Expect(q.m.CanStart().can == update, label + ": Install selected is on only for a proven update");
    }
    // a site that answers with its own older number in the shape Authentic's server does: "nothing newer than yours" and the number it has
    CHECK(StandingOf("0.170.0", Quiet(Piece::Authentic, "0.170.0", "0.169.0").latest) == Standing::Newer);
    CHECK(StandingOf("0.170.0", Quiet(Piece::Authentic, "0.170.0").latest) == Standing::Same);              // "0.0.0": it named none
    // not known what is installed: nothing is proven
    CHECK(StandingOf("", Answer(Piece::Authentic, "0.169.0").latest) == Standing::Unknown);
    CHECK(StandingOf("junk", Answer(Piece::Authentic, "0.169.0").latest) == Standing::Unknown);
    CHECK(IsUpdateFor("", Answer(Piece::Authentic, "0.169.0").latest));                                      // (the main window's cards still offer it)
}

TEST(installed_0_170_0_against_a_site_at_0_169_is_quiet_nothing_ticked_install_selected_off) {
    // the report from the field: Authentic 0.170.0 is installed, the site's newest is 0.169; Unleashed's version is read from its plugin
    Quiet2 q;
    q.Mod(Piece::Authentic, true, "0.170.0", Quiet(Piece::Authentic, "0.170.0", "0.169.0"), "manifest (not valid)");
    q.Mod(Piece::Unleashed, true, "0.3.19", Quiet(Piece::Unleashed, "0.3.19", "0.3.19"), "plugin");
    const std::vector<InstallRow> rows = q.m.Rows();
    const InstallRow& a = Row(rows, Piece::Authentic);
    CHECK_EQ(a.installed, std::string("0.170.0 (manifest not valid)"));
    CHECK_EQ(a.latest, std::string("0.169.0 - you have a newer version"));
    CHECK(a.latest_tone == Tone::Ok);
    CHECK(a.badge.empty() && a.note.empty());
    CHECK(!a.checked && !a.enabled);
    CHECK_EQ(a.action, std::string("Up to date"));
    CHECK(!a.action_enabled);
    CHECK_EQ(a.uninstall, std::string("Uninstall"));
    CHECK(a.uninstall_enabled);
    const InstallRow& u = Row(rows, Piece::Unleashed);
    CHECK_EQ(u.installed, std::string("0.3.19 (from its plugin)"));
    CHECK(Has(u.latest, "you have the newest version"));
    CHECK(!u.checked && !u.enabled);
    CHECK_EQ(u.action, std::string("Up to date"));
    CHECK_EQ(u.uninstall, std::string("Uninstall"));
    const InstallRow& l = Row(rows, Piece::Loader);
    CHECK(!l.checked && !l.enabled);
    CHECK_EQ(l.action, std::string("Up to date"));
    CHECK(!l.action_enabled);
    CHECK(l.uninstall.empty());                                                     // D2RLoader is shared by every mod: no Uninstall
    const StartState ss = q.m.CanStart();
    CHECK(!ss.can);
    CHECK_EQ(ss.reason, std::string("Nothing to install: everything is up to date."));
    CHECK(ss.calm);                                                                  // good news, not a warning
    CHECK(q.m.Jobs(false).empty());
    // a click on a box, or a program ticking it, does nothing
    q.m.Select(Piece::Authentic, true);
    q.m.Select(Piece::Loader, true);
    CHECK(!q.m.IsChecked(Piece::Authentic) && !q.m.IsChecked(Piece::Loader));
    // a card's Update button that is pressed for a mod that turns out to be current: the answer unticks it and says why
    q.m.TickOnly({Piece::Authentic});
    CHECK(!q.m.IsChecked(Piece::Authentic));
    CHECK_EQ(q.m.CanStart().reason, std::string("Nothing to install: everything you ticked is up to date."));
    CHECK(q.m.CanStart().calm);
    q.m.Select(Piece::Authentic, false);
    CHECK_EQ(q.m.CanStart().reason, std::string("Nothing to install: everything is up to date."));
}

TEST(a_mod_whose_version_cannot_be_read_is_not_ticked_and_its_button_says_update) {
    Quiet2 q;
    q.Mod(Piece::Authentic, true, "", Answer(Piece::Authentic, "0.169.0"));
    q.Mod(Piece::Unleashed, true, "", Answer(Piece::Unleashed, "0.3.19", kSha));
    std::vector<InstallRow> rows = q.m.Rows();
    const InstallRow& a = Row(rows, Piece::Authentic);
    CHECK_EQ(a.installed, std::string("installed (version unknown)"));
    CHECK_EQ(a.latest, std::string("0.169.0"));
    CHECK(!a.checked);
    CHECK(a.enabled);                                                               // the player may tick it himself
    CHECK_EQ(a.action, std::string("Update"));
    CHECK(a.action_enabled);
    CHECK(a.badge.empty());                                                         // nothing is proven, so no amber "Update available"
    CHECK(Has(a.note, "version could not be read") && Has(a.note, "not ticked") && Has(a.note, "Update runs the Setup of 0.169.0 over it"));
    CHECK_EQ(a.uninstall, std::string("Uninstall"));
    CHECK(!Row(rows, Piece::Unleashed).checked);
    CHECK(!q.m.CanStart().can);
    CHECK_EQ(q.m.CanStart().reason, std::string("Tick what you want to install."));
    CHECK(!q.m.CanStart().calm);                                                    // that one is a nudge, not good news
    CHECK(q.m.Jobs(false).empty());
    // the row's Update: only that piece is ticked, and it is a real install of the announced release (not "again": nothing is forced)
    q.m.TickOnly({Piece::Authentic});
    CHECK(q.m.IsChecked(Piece::Authentic) && !q.m.IsChecked(Piece::Unleashed) && !q.m.IsChecked(Piece::Loader));
    CHECK(q.m.CanStart().can);
    const std::vector<PieceJob> jobs = q.m.Jobs(true);
    REQUIRE(jobs.size() == 1);
    CHECK(jobs[0].piece == Piece::Authentic && jobs[0].has_release && jobs[0].release.version == "0.169.0" && !jobs[0].force);
    // the player ticks the other one's box himself: allowed
    q.m.Select(Piece::Unleashed, true);
    CHECK(q.m.IsChecked(Piece::Unleashed));
}

TEST(a_proven_update_is_ticked_and_offered_by_the_row) {
    Quiet2 q;
    q.Mod(Piece::Authentic, true, "0.167.0", Answer(Piece::Authentic, "0.168.0"));
    q.Mod(Piece::Unleashed, true, "0.3.17", Answer(Piece::Unleashed, "0.3.19", kSha), "plugin");
    const std::vector<InstallRow> rows = q.m.Rows();
    for (Piece p : {Piece::Authentic, Piece::Unleashed}) {
        const InstallRow& r = Row(rows, p);
        CHECK(r.checked && r.enabled);
        CHECK_EQ(r.action, std::string("Update"));
        CHECK(r.action_enabled);
        CHECK(Has(r.badge, "Update available: "));
        CHECK_EQ(r.uninstall, std::string("Uninstall"));
    }
    CHECK(q.m.CanStart().can);
    CHECK_EQ(q.m.Jobs(false).size(), size_t(2));
    // the row of one of them: only that one
    q.m.TickOnly({Piece::Unleashed});
    CHECK(!q.m.IsChecked(Piece::Authentic) && q.m.IsChecked(Piece::Unleashed));
    CHECK_EQ(q.m.Jobs(false).size(), size_t(1));
}

TEST(the_rows_own_buttons_follow_what_is_installed_and_what_is_going_on) {
    Fresh f;
    f.m.SetChecking();
    // while the servers are asked: Install is there for what is missing but cannot be pressed; no Uninstall for what is not installed
    std::vector<InstallRow> rows = f.m.Rows();
    CHECK_EQ(Row(rows, Piece::Loader).action, std::string("Install"));
    CHECK(Row(rows, Piece::Loader).action_enabled);                                 // D2RLoader needs no answer
    CHECK_EQ(Row(rows, Piece::Authentic).action, std::string("Install"));
    CHECK(!Row(rows, Piece::Authentic).action_enabled);
    CHECK(Row(rows, Piece::Authentic).uninstall.empty() && Row(rows, Piece::Unleashed).uninstall.empty() && Row(rows, Piece::Loader).uninstall.empty());
    f.Answers();
    rows = f.m.Rows();
    CHECK_EQ(Row(rows, Piece::Authentic).action, std::string("Install"));
    CHECK(Row(rows, Piece::Authentic).action_enabled && Row(rows, Piece::Unleashed).action_enabled);
    // the game runs: nothing can be started from a row (the notice says so); a running install blocks them too
    f.m.SetGameRunning(true);
    CHECK(!Row(f.m.Rows(), Piece::Authentic).action_enabled && !Row(f.m.Rows(), Piece::Loader).action_enabled);
    f.m.SetGameRunning(false);
    // installed: Uninstall appears (enabled even while the game runs: pressing it is refused with the words, it is not a grey button)
    f.m.SetLoader(Reviewed(), true);
    f.m.SetInstalled(Piece::Authentic, true, "0.168.0");
    f.m.SetUpdate(Piece::Authentic, Quiet(Piece::Authentic, "0.168.0"));
    f.m.SetGameRunning(true);
    rows = f.m.Rows();
    CHECK_EQ(Row(rows, Piece::Authentic).uninstall, std::string("Uninstall"));
    CHECK(Row(rows, Piece::Authentic).uninstall_enabled);
    CHECK_EQ(Row(rows, Piece::Authentic).action, std::string("Up to date"));
    CHECK(Row(rows, Piece::Unleashed).uninstall.empty());
    f.m.SetGameRunning(false);
    // a run: every button is off (and nothing is ticked by a click)
    f.m.BeginRun(f.m.Jobs(true));
    rows = f.m.Rows();
    for (Piece p : {Piece::Loader, Piece::Authentic, Piece::Unleashed}) CHECK(!Row(rows, p).action_enabled);
    CHECK(!Row(rows, Piece::Authentic).uninstall_enabled);
    f.m.EndRun();
    // a question that failed: no button can be pressed for that mod
    f.m.SetUpdate(Piece::Unleashed, Failed("cannot reach github.com"));
    CHECK_EQ(Row(f.m.Rows(), Piece::Unleashed).action, std::string("Install"));
    CHECK(!Row(f.m.Rows(), Piece::Unleashed).action_enabled);
    // no game folder: no buttons at all
    InstallModel none;
    for (const InstallRow& r : none.Rows()) CHECK(r.action.empty() && r.uninstall.empty());
}

TEST(only_the_authentic_and_unleashed_rows_ever_have_an_uninstall_button) {
    Quiet2 q;
    q.Mod(Piece::Authentic, true, "0.168.0", Quiet(Piece::Authentic, "0.168.0"));
    q.Mod(Piece::Unleashed, false, "", Answer(Piece::Unleashed, "0.3.19", kSha));
    std::vector<InstallRow> rows = q.m.Rows();
    CHECK_EQ(Row(rows, Piece::Authentic).uninstall, std::string("Uninstall"));
    CHECK(Row(rows, Piece::Unleashed).uninstall.empty());                           // not installed
    CHECK(Row(rows, Piece::Loader).uninstall.empty());
    // D2RLoader whose files differ, or missing: still none (its files are shared; the launcher keeps a backup of what it replaced, there is nothing to put back after a first install)
    LoaderStatus differs;
    differs.present = true;
    q.m.SetLoader(differs, true);
    CHECK(Row(q.m.Rows(), Piece::Loader).uninstall.empty());
    q.m.SetLoader(LoaderStatus(), false);
    CHECK(Row(q.m.Rows(), Piece::Loader).uninstall.empty());
    q.m.SetInstalled(Piece::Authentic, false, "");
    CHECK(Row(q.m.Rows(), Piece::Authentic).uninstall.empty());
}

TEST(d2rloader_has_install_repair_and_up_to_date_on_its_row) {
    Fresh f;
    f.Answers();
    // missing
    InstallRow l = Row(f.m.Rows(), Piece::Loader);
    CHECK_EQ(l.action, std::string("Install"));
    CHECK(l.action_enabled && l.enabled);
    // files differ: Repair, the box is on, and with no mod ticked it is not ticked by itself (as before: it follows the mods)
    LoaderStatus differs;
    differs.present = true;
    differs.reviewed = false;
    f.m.SetInstalled(Piece::Authentic, true, "0.168.0");
    f.m.SetInstalled(Piece::Unleashed, true, "0.3.19");
    f.m.SetUpdate(Piece::Authentic, Quiet(Piece::Authentic, "0.168.0"));
    f.m.SetUpdate(Piece::Unleashed, Quiet(Piece::Unleashed, "0.3.19"));
    f.m.SetLoader(differs, true);
    l = Row(f.m.Rows(), Piece::Loader);
    CHECK_EQ(l.installed, std::string("files differ"));
    CHECK_EQ(l.action, std::string("Repair"));
    CHECK(l.action_enabled && l.enabled && !l.checked);
    CHECK(Has(l.note, "Repair puts the reviewed ones in") && Has(l.note, "backed up"));
    CHECK(!f.m.CanStart().can);
    // the row's Repair: only D2RLoader
    f.m.TickOnly({Piece::Loader});
    CHECK(f.m.IsChecked(Piece::Loader) && !f.m.IsChecked(Piece::Authentic) && !f.m.IsChecked(Piece::Unleashed));
    CHECK(f.m.CanStart().can);
    const std::vector<PieceJob> jobs = f.m.Jobs(false);
    REQUIRE(jobs.size() == 1);
    CHECK(jobs[0].piece == Piece::Loader);
    // reviewed: greyed Up to date, the box is off and cannot be ticked
    f.m.SetLoader(Reviewed(), true);
    l = Row(f.m.Rows(), Piece::Loader);
    CHECK_EQ(l.action, std::string("Up to date"));
    CHECK(!l.action_enabled && !l.enabled && !l.checked);
    f.m.Select(Piece::Loader, true);
    CHECK(!f.m.IsChecked(Piece::Loader));
    // found but not hashed yet: wait, nothing to press
    LoaderStatus found;
    found.present = true;
    f.m.SetLoader(found, false);
    l = Row(f.m.Rows(), Piece::Loader);
    CHECK_EQ(l.action, std::string("Checking..."));
    CHECK(!l.action_enabled && !l.enabled);
}

TEST(a_row_button_ticks_only_its_piece_and_takes_d2rloader_along_when_it_is_missing) {
    Fresh f;
    f.Answers();
    f.m.TickOnly({Piece::Authentic});
    CHECK(f.m.IsChecked(Piece::Authentic) && !f.m.IsChecked(Piece::Unleashed));
    CHECK(f.m.IsChecked(Piece::Loader));                                            // the mod runs only under it, and it is missing
    std::vector<PieceJob> jobs = f.m.Jobs(false);
    REQUIRE(jobs.size() == 2);
    CHECK(jobs[0].piece == Piece::Loader && jobs[1].piece == Piece::Authentic);
    // D2RLoader's own row: only D2RLoader, the missing mods stay unticked
    f.m.TickOnly({Piece::Loader});
    CHECK(f.m.IsChecked(Piece::Loader) && !f.m.IsChecked(Piece::Authentic) && !f.m.IsChecked(Piece::Unleashed));
    jobs = f.m.Jobs(false);
    REQUIRE(jobs.size() == 1);
    CHECK(jobs[0].piece == Piece::Loader);
    // with D2RLoader reviewed a mod's button ticks just the mod
    f.m.SetLoader(Reviewed(), true);
    f.m.TickOnly({Piece::Unleashed});
    jobs = f.m.Jobs(false);
    REQUIRE(jobs.size() == 1);
    CHECK(jobs[0].piece == Piece::Unleashed);
}

TEST(nothing_is_ticked_by_the_facts_unless_it_is_a_real_install_or_update) {
    // every combination of the mod's state: missing, unknown, older, current, newer
    struct Case {
        bool installed;
        const char* have;
        UpdateInfo answer;
        bool ticked;
        const char* action;
    };
    const Case cases[] = {
        {false, "", Answer(Piece::Unleashed, "0.3.19", kSha), true, "Install"},
        {true, "", Answer(Piece::Unleashed, "0.3.19", kSha), false, "Update"},
        {true, "0.3.17", Answer(Piece::Unleashed, "0.3.19", kSha), true, "Update"},
        {true, "0.3.19", Quiet(Piece::Unleashed, "0.3.19"), false, "Up to date"},
        {true, "0.3.20", Answer(Piece::Unleashed, "0.3.19", kSha), false, "Up to date"},
        {true, "0.3.20", Quiet(Piece::Unleashed, "0.3.20", "0.3.19"), false, "Up to date"},
    };
    int n = 0;
    for (const Case& c : cases) {
        Quiet2 q;
        q.Mod(Piece::Authentic, true, "0.168.0", Quiet(Piece::Authentic, "0.168.0"));
        q.Mod(Piece::Unleashed, c.installed, c.have, c.answer);
        const InstallRow r = Row(q.m.Rows(), Piece::Unleashed);
        const std::string label = "case " + std::to_string(n++) + " (" + (c.installed ? std::string("installed '") + c.have + "'" : std::string("missing")) + ")";
        Expect(r.checked == c.ticked, label + ": ticked");
        Expect(r.action == c.action, label + ": the row button is '" + r.action + "'");
        Expect(q.m.CanStart().can == c.ticked, label + ": Install selected");
        Expect(r.enabled == (std::string(c.action) != "Up to date"), label + ": the box");
    }
}

TEST_MAIN()
