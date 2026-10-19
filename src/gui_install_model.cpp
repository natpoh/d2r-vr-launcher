#include "gui_install_model.h"

#include <algorithm>
#include <cstdio>

#include "setup_run.h"

namespace vrml {
namespace gui {

namespace {

std::string Join(const std::vector<std::string>& v, const std::string& sep) {
    std::string s;
    for (const std::string& x : v) s += (s.empty() ? "" : sep) + x;
    return s;
}

bool Has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

// A server problem in a few words for the row (the log has the whole message).
std::string ShortError(const std::string& e) {
    if (Has(e, "cannot reach") || Has(e, "connection")) return "no connection to the server";
    if (Has(e, "timed out") || Has(e, "timeout")) return "the server did not answer in time";
    if (Has(e, "untrust") || Has(e, "refused")) return "the answer was refused";
    if (Has(e, "not readable") || Has(e, "not what was expected") || Has(e, "no version") || Has(e, "not a list")) return "an answer that makes no sense";
    if (Has(e, "no release")) return "no release with an installer";
    std::string s = e.substr(0, e.find(':') == std::string::npos || e.find(':') > 60 ? 60 : e.find(':'));
    return s.size() < e.size() ? s + "..." : s;
}

std::string Percent(uint64_t done, uint64_t total) { return std::to_string(total ? std::min<uint64_t>(100, done * 100 / total) : 0) + " %"; }

}  // namespace

std::string FormatBytes(uint64_t b) {
    char buf[32];
    if (b >= (1ull << 20)) std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(b) / (1024.0 * 1024.0));
    else if (b >= 1024) std::snprintf(buf, sizeof buf, "%llu KB", static_cast<unsigned long long>(b / 1024));
    else std::snprintf(buf, sizeof buf, "%llu bytes", static_cast<unsigned long long>(b));
    return buf;
}

bool IsUpdateFor(const std::string& installedVersion, const Release& latest) {
    if (latest.up_to_date || latest.version.empty()) return false;
    return installedVersion.empty() || IsNewer(latest.version, installedVersion);
}

Standing StandingOf(const std::string& installedVersion, const Release& latest) {
    const std::string have = NormalizeVersion(installedVersion);
    if (have.empty()) return latest.up_to_date ? Standing::Same : Standing::Unknown;
    // the newest version the source named: Authentic's answer to "anything newer than v?" is "0.0.0" (named none) or, when the site is behind, its own older number
    const std::string named = !latest.announced.empty() ? latest.announced : latest.version;
    if (latest.up_to_date || NormalizeVersion(latest.version).empty()) return !named.empty() && IsNewer(have, named) ? Standing::Newer : Standing::Same;
    if (IsNewer(latest.version, have)) return Standing::Older;
    return IsNewer(have, latest.version) ? Standing::Newer : Standing::Same;
}

std::string UpdateBadgeText(const std::string& installedVersion, const std::string& latestVersion) {
    if (latestVersion.empty()) return "";
    if (installedVersion.empty()) return "Update available: " + latestVersion;
    return "Update available: " + installedVersion + " -> " + latestVersion;
}

InstallModel::InstallModel() {
    for (int i = 0; i < kPieceCount; ++i) rows_[i].piece = static_cast<Piece>(i);
}

// ------------------------------------------------------------------ the facts

void InstallModel::SetGame(const fs::path& dir, bool is_game) {
    game_ = dir;
    game_ok_ = is_game;
    Recompute();
}

void InstallModel::SetGameRunning(bool running) { game_running_ = running; }

void InstallModel::SetLoader(const LoaderStatus& st, bool hashed) {
    loader_ = st;
    loader_hashed_ = hashed;
    Recompute();
}

void InstallModel::SetInstalled(Piece mod, bool installed, const std::string& version, const std::string& from) {
    if (!IsMod(mod)) return;
    row(mod).installed = installed;
    row(mod).installed_version = installed ? NormalizeVersion(version) : "";
    row(mod).installed_from = installed && !row(mod).installed_version.empty() ? from : "";
    Recompute();
}

void InstallModel::SetInstallHelp(Piece mod, const std::string& text) {
    if (IsMod(mod)) row(mod).help = text;
}

void InstallModel::SetChecking() {
    for (Piece m : {Piece::Authentic, Piece::Unleashed}) {
        row(m).check = CheckState::Checking;
        row(m).upd = UpdateInfo();
    }
    Recompute();
}

void InstallModel::SetUpdate(Piece mod, const UpdateInfo& u) {
    if (!IsMod(mod)) return;
    row(mod).upd = u;
    row(mod).check = u.checked ? CheckState::Done : CheckState::Failed;
    Recompute();
}

bool InstallModel::AllChecksFinished() const {
    for (Piece m : {Piece::Authentic, Piece::Unleashed})
        if (row(m).check == CheckState::Checking) return false;
    if (game_ok_ && loader_.present && !loader_hashed_) return false;
    return true;
}

// ------------------------------------------------------------------ the ticks

bool InstallModel::NeedsLoader() const {
    if (!game_ok_) return false;
    if (!loader_.present) return true;
    return loader_hashed_ && !loader_.reviewed;
}

bool InstallModel::AnyModTicked() const { return row(Piece::Authentic).checked || row(Piece::Unleashed).checked; }

bool InstallModel::Outdated(Piece mod) const {
    const RowData& d = row(mod);
    return d.check == CheckState::Done && d.installed && StandingOf(d.installed_version, d.upd.latest) == Standing::Older;
}

bool InstallModel::UnknownVersion(Piece mod) const {
    const RowData& d = row(mod);
    return d.check == CheckState::Done && d.installed && StandingOf(d.installed_version, d.upd.latest) == Standing::Unknown;
}

bool InstallModel::Current(Piece mod) const {
    const RowData& d = row(mod);
    if (d.check != CheckState::Done || !d.installed) return false;
    const Standing s = StandingOf(d.installed_version, d.upd.latest);
    return s == Standing::Same || s == Standing::Newer;
}

bool InstallModel::IsUpToDate(Piece p) const {
    if (p == Piece::Loader) return LoaderReviewed();
    return Current(p);
}

bool InstallModel::Selectable(Piece p) const {
    if (!game_ok_) return false;
    if (p == Piece::Loader) return !loader_.present || (loader_hashed_ && !loader_.reviewed);       // present and not hashed yet: wait for what the files say
    return !Current(p);
}

bool InstallModel::NothingToDo() const {
    if (!game_ok_ || !AllChecksFinished()) return false;
    for (Piece p : {Piece::Loader, Piece::Authentic, Piece::Unleashed})
        if (!IsUpToDate(p)) return false;
    return true;
}

bool InstallModel::WantsByDefault(Piece mod) const {
    const RowData& d = row(mod);
    if (!game_ok_ || d.check != CheckState::Done) return false;
    if (!d.installed) return !d.upd.latest.up_to_date && !d.upd.latest.url.empty();
    return Outdated(mod);                           // an unknown version is not proof of anything: not ticked, the row's Update button is the way
}

void InstallModel::Recompute() {
    // a box the player has not touched follows the facts; one he has touched stays as he left it - except that a piece which is up to date is never ticked
    for (Piece m : {Piece::Authentic, Piece::Unleashed}) {
        if (!row(m).touched) row(m).checked = WantsByDefault(m);
        if (row(m).checked && Current(m)) {
            row(m).checked = false;
            dropped_ = true;
        }
    }
    RowData& l = row(Piece::Loader);
    if (!l.touched) l.checked = NeedsLoader() && AnyModTicked();
    if (l.checked && LoaderReviewed()) {
        l.checked = false;
        dropped_ = true;
    }
}

void InstallModel::TickOnly(const std::vector<Piece>& pieces) {
    dropped_ = false;
    for (Piece m : {Piece::Authentic, Piece::Unleashed}) {
        row(m).touched = true;
        row(m).checked = std::find(pieces.begin(), pieces.end(), m) != pieces.end();
    }
    RowData& l = row(Piece::Loader);
    l.touched = std::find(pieces.begin(), pieces.end(), Piece::Loader) != pieces.end();
    l.checked = l.touched;
    Recompute();
}

void InstallModel::Select(Piece p, bool on) {
    if (running_) return;
    dropped_ = false;
    if (on && !Selectable(p)) {                     // nothing to install: the box is off, a click (or a program) cannot tick it
        row(p).checked = false;
        return;
    }
    row(p).touched = true;
    row(p).checked = on;
    Recompute();
    // ticking a mod while D2RLoader is missing ticks it too, even when the player had unticked it: the mods run only under D2RLoader. (The player may
    // untick it again - the row then warns - but not by merely changing his mind about something else.)
    if (IsMod(p) && on && LoaderMissing()) row(Piece::Loader).checked = true;
}

std::string InstallModel::ModsTicked() const {
    std::vector<std::string> names;
    for (Piece m : {Piece::Authentic, Piece::Unleashed})
        if (row(m).checked) names.push_back(PieceName(m));
    return Join(names, " and ");
}

// ------------------------------------------------------------------ the rows

std::vector<InstallRow> InstallModel::Rows() const {
    std::vector<InstallRow> out;
    // a row's own button starts an install: not while one runs, not while the game runs (Install selected says why in the notice)
    const bool can_act = game_ok_ && !running_ && !game_running_;
    for (int i = 0; i < kPieceCount; ++i) {
        const Piece p = static_cast<Piece>(i);
        const RowData& d = row(p);
        InstallRow r;
        r.piece = p;
        r.name = PieceName(p);
        r.checked = d.checked;
        r.enabled = Selectable(p);                             // off without a game folder and for what is up to date; while it runs the boxes keep their look, a click is ignored (Select)
        r.state = d.state;
        r.state_tone = d.state_tone;
        const PieceOutcome* oc = nullptr;
        for (const PieceOutcome& o : outcomes_)
            if (o.piece == p) oc = &o;

        if (p == Piece::Loader) {
            if (!game_ok_) {
                r.installed = "-";
            } else if (!loader_.present) {
                r.installed = "missing";
                r.installed_tone = Tone::Warn;
                r.action = "Install";
                r.action_enabled = can_act;
            } else if (!loader_hashed_) {
                r.installed = "found, checking its files...";
                r.action = "Checking...";
            } else if (loader_.reviewed) {
                r.installed = "reviewed " + (loader_.version.empty() ? pin_version_ : loader_.version) + " files";
                r.installed_tone = Tone::Ok;
                r.action = "Up to date";
            } else {
                r.installed = "files differ";
                r.installed_tone = Tone::Warn;
                r.action = "Repair";                           // puts the reviewed files in; the ones it replaces are backed up first
                r.action_enabled = can_act;
            }
            r.latest = pin_version_ + " (pinned)";
            r.source = "d2rloader.net, hash pinned in the launcher";
            r.how = "Unpacked by the launcher itself, no wizard; files it replaces are backed up first.";
            const bool forced = d.checked && NeedsLoader() && AnyModTicked();
            if (forced && LoaderMissing()) {
                r.note = "Ticked because " + ModsTicked() + (AnyModTicked() && ModsTicked().find(" and ") != std::string::npos ? " run" : " runs") + " only under D2RLoader, which is missing.";
            } else if (forced) {
                r.note = "Ticked because the files here are not the reviewed ones; they are replaced (your own files are backed up first).";
            } else if (!d.checked && LoaderMissing() && AnyModTicked()) {
                r.note = "Without D2RLoader the mods cannot be started.";
                r.note_tone = Tone::Warn;
            } else if (!d.checked && game_ok_ && loader_.present && loader_hashed_ && !loader_.reviewed) {
                r.note = "The files here are not the reviewed ones. Repair puts the reviewed ones in (your own files are backed up first).";
            }
        } else {
            const Release& L = d.upd.latest;
            const Standing st = d.installed ? StandingOf(d.installed_version, L) : Standing::Older;
            if (!game_ok_) {
                r.installed = "-";
            } else if (!d.installed) {
                r.installed = "not installed";
            } else if (d.installed_version.empty()) {
                r.installed = "installed (version unknown)";
            } else {
                r.installed = d.installed_version;
                if (d.installed_from == "plugin") r.installed += " (from its plugin)";
                else if (d.installed_from == "manifest (not valid)") r.installed += " (manifest not valid)";
                r.installed_tone = Tone::Ok;
            }
            switch (d.check) {
                case CheckState::NotAsked: r.latest = "-"; break;
                case CheckState::Checking: r.latest = "checking..."; break;
                case CheckState::Failed:
                    r.latest = "could not be checked: " + ShortError(d.upd.error);
                    r.latest_tone = Tone::Warn;
                    break;
                case CheckState::Done:
                    if (st == Standing::Newer) {
                        // the site's newest is older than what is installed: never offered, said in plain words
                        const std::string named = !L.announced.empty() ? L.announced : L.version;
                        r.latest = (named.empty() ? std::string() : named + " - ") + "you have a newer version";
                        r.latest_tone = Tone::Ok;
                    } else if (st == Standing::Same) {
                        r.latest = L.version.empty() || L.up_to_date ? std::string("you have the newest version") : L.version + " - you have the newest version";
                        r.latest_tone = Tone::Ok;
                    } else {
                        r.latest = L.version + (L.prerelease ? " (pre-release)" : "");
                    }
                    break;
            }
            if (Outdated(p)) r.badge = UpdateBadgeText(d.installed_version, L.version);
            const bool known = d.check == CheckState::Done && !L.up_to_date;
            if (p == Piece::Authentic) {
                r.source = "bodywalkvr.com";
                if (known) r.source += L.sha256.empty() ? " - no checksum published yet" : " - checksum published";
                r.how = "Its own Setup opens in a window; Windows asks for administrator rights first.";
            } else {
                r.source = "GitHub release of bearstonem/d2r-pcvr";
                if (known && L.sha256.empty()) r.source += " - no checksum published";
                else r.source += ", SHA-256 from GitHub";
                r.how = "Its own Setup opens in a window and installs for you only: no administrator prompt.";
            }
            if (!d.installed) r.help = d.help;                // the author's words for a player who is about to install it
            // the row's own buttons
            if (game_ok_) {
                if (d.check != CheckState::Done) {
                    r.action = d.installed ? (d.check == CheckState::Failed ? "Update" : "Checking...") : "Install";     // not usable until the answer is in
                } else if (!d.installed) {
                    r.action = "Install";
                    r.action_enabled = can_act && !L.url.empty();
                } else if (Current(p)) {
                    r.action = "Up to date";
                } else {
                    r.action = "Update";                       // a proven newer release, or a version that could not be read
                    r.action_enabled = can_act && !L.url.empty();
                }
                if (d.installed) {
                    r.uninstall = "Uninstall";
                    r.uninstall_enabled = !running_;
                }
            }
            if (d.checked) {
                if (d.check == CheckState::Failed) {
                    r.note = "The newest version could not be found out: press Check again first.";
                    r.note_tone = Tone::Warn;
                } else if (d.check == CheckState::Checking) {
                    r.note = "Waiting for the answer about the newest version...";
                } else if (known && L.sha256.empty()) {
                    r.note = "No published checksum: you will be asked before anything is downloaded.";
                    r.note_tone = Tone::Warn;
                }
            }
            if (r.note.empty() && UnknownVersion(p)) {
                r.note = "Its version could not be read, so it is not ticked. Update runs the Setup of " + L.version + " over it.";
                r.note_tone = Tone::Warn;
            }
        }
        if (oc && (oc->status == JobStatus::Failed || oc->status == JobStatus::Cancelled || oc->status == JobStatus::NotRun)) {
            r.note = Meaning(*oc);
            r.note_tone = oc->status == JobStatus::Failed ? Tone::Error : Tone::Warn;
        }
        out.push_back(r);
    }
    return out;
}

StartState InstallModel::CanStart() const {
    StartState s;
    if (!game_ok_) {
        s.reason = "Pick the folder Diablo II: Resurrected is installed in first (Browse... in the main window).";
        return s;
    }
    if (running_) {
        s.reason = "An install is running.";
        return s;
    }
    if (game_running_) {
        s.reason = "The game is running. Close it first: its files cannot be replaced while it runs.";
        return s;
    }
    if (!row(Piece::Loader).checked && !AnyModTicked()) {
        // (a ticked box is always a real install, update or fix: what is up to date cannot be ticked)
        if (!AllChecksFinished()) {
            s.reason = "Still finding out what is installed and what is newest...";
        } else if (dropped_ || NothingToDo()) {
            s.reason = dropped_ ? "Nothing to install: everything you ticked is up to date." : "Nothing to install: everything is up to date.";
            s.calm = true;
        } else {
            s.reason = "Tick what you want to install.";
        }
        return s;
    }
    if (row(Piece::Loader).checked && loader_.present && !loader_hashed_) {
        s.reason = "Still looking at D2RLoader's files...";
        return s;
    }
    for (Piece m : {Piece::Authentic, Piece::Unleashed}) {
        const RowData& d = row(m);
        if (!d.checked) continue;
        if (d.check == CheckState::Checking || d.check == CheckState::NotAsked) {
            s.reason = std::string("Still finding out the newest ") + PieceName(m) + "...";
            return s;
        }
        if (d.check == CheckState::Failed) {
            s.reason = std::string("The newest ") + PieceName(m) + " could not be found out (" + d.upd.error + "). Press Check again, or untick it.";
            return s;
        }
    }
    s.can = true;
    return s;
}

std::vector<PieceJob> InstallModel::Jobs(bool acceptUnverified) const {
    std::vector<PieceJob> jobs;
    for (int i = 0; i < kPieceCount; ++i) {
        const Piece p = static_cast<Piece>(i);
        const RowData& d = row(p);
        if (!d.checked) continue;
        PieceJob j;
        j.piece = p;
        if (IsMod(p)) {
            const Release& L = d.upd.latest;
            if (d.check == CheckState::Done && !L.up_to_date && !L.url.empty()) {
                j.has_release = true;
                j.release = L;
            }
            j.force = false;                           // what is up to date is never ticked: to put it in again, uninstall it first
            j.accept_unverified = acceptUnverified && L.sha256.empty();
        }
        jobs.push_back(j);
    }
    return jobs;
}

std::vector<UnverifiedItem> InstallModel::UnverifiedTicked() const {
    std::vector<UnverifiedItem> items;
    for (Piece m : {Piece::Authentic, Piece::Unleashed}) {
        const RowData& d = row(m);
        if (!d.checked || d.check != CheckState::Done || !d.upd.latest.sha256.empty()) continue;
        UnverifiedItem it;
        it.piece = m;
        it.version = d.upd.latest.up_to_date ? d.installed_version : d.upd.latest.version;
        it.source = d.upd.latest.source.empty() ? (m == Piece::Authentic ? "bodywalkvr.com" : "github.com/bearstonem/d2r-pcvr") : d.upd.latest.source;
        items.push_back(it);
    }
    return items;
}

Prompt InstallModel::UnverifiedPrompt(const std::vector<UnverifiedItem>& items) {
    Prompt p;
    p.title = "No checksum published";
    std::vector<std::string> sources;
    for (const UnverifiedItem& it : items)
        if (std::find(sources.begin(), sources.end(), it.source) == sources.end()) sources.push_back(it.source);
    if (items.size() == 1) {
        const UnverifiedItem& it = items[0];
        p.text = std::string(PieceName(it.piece)) + (it.version.empty() ? "" : " " + it.version) + " comes from " + it.source + ", which does not publish a checksum for it.";
    } else {
        p.text = "These downloads have no published checksum:";
        for (const UnverifiedItem& it : items) p.text += std::string("\n  - ") + PieceName(it.piece) + (it.version.empty() ? "" : " " + it.version) + " (" + it.source + ")";
    }
    p.text +=
        "\n\nThat means the launcher cannot prove that the file it downloads is exactly the one that was announced. What it still checks: the address is "
        "https and on " + Join(sources, " / ") + " only (no other server is followed), the size is capped, and the file's SHA-256 is written to the log "
        "before its Setup starts.\n\n"
        "This is probably fine: the download goes over HTTPS from the publisher's own address, and a checksum can be published later - the launcher then "
        "enforces it with no update. Install anyway only if you trust " + Join(sources, " and ") + ".\n\nDownload it and run its Setup anyway?";
    return p;
}

// ------------------------------------------------------------------ a run

void InstallModel::BeginRun(const std::vector<PieceJob>& jobs) {
    running_ = true;
    cancel_ = false;
    setup_open_ = false;
    outcomes_.clear();
    run_order_.clear();
    for (RowData& d : rows_) {
        d.in_run = false;
        d.state.clear();
        d.state_tone = Tone::Normal;
    }
    for (const PieceJob& j : OrderedJobs(jobs)) {
        run_order_.push_back(j.piece);
        row(j.piece).in_run = true;
        row(j.piece).state = "Waiting";
    }
    run_count_ = static_cast<int>(run_order_.size());
    run_index_ = 0;
    run_stage_.clear();
    run_done_ = run_total_ = 0;
    if (!run_order_.empty()) run_piece_ = run_order_.front();
}

void InstallModel::SetStage(Piece p, int index, int count, const std::string& stage, uint64_t done, uint64_t total) {
    run_piece_ = p;
    run_index_ = index;
    run_count_ = count;
    run_stage_ = stage;
    run_done_ = done;
    run_total_ = total;
    setup_open_ = stage == "setup";
    RowData& d = row(p);
    d.state_tone = Tone::Normal;
    if (stage == "lookup") d.state = "Looking up...";
    else if (stage == "download") d.state = done ? "Downloading " + FormatBytes(done) + (total ? " of " + FormatBytes(total) : std::string()) : std::string("Downloading...");
    else if (stage == "unpack") d.state = "Unpacking...";
    else if (stage == "verify") d.state = "Checking the files...";
    else if (stage == "install") d.state = "Installing...";
    else if (stage == "setup") d.state = "Setup window open";
    else d.state = "Working...";
}

void InstallModel::SetOutcome(const PieceOutcome& o) {
    outcomes_.push_back(o);
    RowData& d = row(o.piece);
    d.state = ShortMark(o, &d.state_tone);
    setup_open_ = false;
    run_stage_.clear();
}

void InstallModel::EndRun() {
    running_ = false;
    setup_open_ = false;
    for (RowData& d : rows_) d.in_run = false;
}

void InstallModel::RequestCancel() {
    if (running_) cancel_ = true;
}

double InstallModel::SlotFraction(Piece p, const std::string& stage, uint64_t done, uint64_t total) {
    const double ratio = total ? std::min(1.0, static_cast<double>(done) / static_cast<double>(total)) : 0.3;
    if (p == Piece::Loader) {
        if (stage == "download") return 0.05 + 0.65 * ratio;
        if (stage == "unpack") return 0.72;
        if (stage == "verify") return 0.8;
        if (stage == "install") return 0.85 + 0.15 * (total ? ratio : 0.0);
        return 0.0;
    }
    if (stage == "lookup") return 0.02;
    if (stage == "download") return 0.05 + 0.6 * ratio;
    if (stage == "setup") return 0.7;
    return 0.0;
}

double InstallModel::Progress() const {
    if (run_count_ <= 0) return 0.0;
    const double finished = static_cast<double>(outcomes_.size());
    double slot = 0.0;
    if (running_ && outcomes_.size() < static_cast<size_t>(run_count_)) slot = run_stage_.empty() ? 0.0 : SlotFraction(run_piece_, run_stage_, run_done_, run_total_);
    return std::min(1.0, (finished + slot) / run_count_);
}

std::string InstallModel::StepText() const {
    if (!running_) return "";
    const std::string head = "Step " + std::to_string(std::min(run_index_ + 1, run_count_)) + " of " + std::to_string(run_count_) + " - " + PieceName(run_piece_) + ": ";
    if (run_stage_.empty()) return head + "starting...";
    if (run_stage_ == "lookup") return head + "looking for the newest version...";
    if (run_stage_ == "download") {
        if (run_done_ == 0) return head + "downloading...";
        return head + "downloading " + FormatBytes(run_done_) + (run_total_ ? " of " + FormatBytes(run_total_) + " (" + Percent(run_done_, run_total_) + ")" : std::string());
    }
    if (run_stage_ == "unpack") return head + "unpacking the archive...";
    if (run_stage_ == "verify") return head + "checking the files against the reviewed hashes...";
    if (run_stage_ == "install") return head + "putting the files into the game folder...";
    if (run_stage_ == "setup") return head + "its Setup is running - finish or close its wizard";
    return head + "working...";
}

std::string InstallModel::RunNotice(Tone* tone) const {
    if (tone) *tone = Tone::Normal;
    if (!running_) return "";
    if (cancel_) {
        if (tone) *tone = Tone::Warn;
        return setup_open_ ? "Cancelling: the Setup's window is open and is not closed by this - finish or close its wizard, then the rest is skipped."
                           : "Cancelling: the step that is running stops at the next safe point, the rest is skipped.";
    }
    if (setup_open_) return "The Setup's window is open (look for it on the taskbar if it is hidden): finish or close its wizard to go on. Cancel does not close it.";
    return "";
}

std::vector<std::string> InstallModel::SummaryLines() const {
    std::vector<std::string> lines;
    for (const PieceOutcome& o : outcomes_) lines.push_back(std::string(PieceName(o.piece)) + ": " + Meaning(o));
    return lines;
}

std::string InstallModel::Headline(Tone* tone) const {
    if (tone) *tone = Tone::Normal;
    if (outcomes_.empty()) return "";
    int installed = 0, already = 0, failed = 0, cancelled = 0, notrun = 0;
    for (const PieceOutcome& o : outcomes_) {
        switch (o.status) {
            case JobStatus::Installed: ++installed; break;
            case JobStatus::AlreadyThere: ++already; break;
            case JobStatus::Failed: ++failed; break;
            case JobStatus::Cancelled: ++cancelled; break;
            case JobStatus::NotRun: ++notrun; break;
        }
    }
    std::vector<std::string> parts;
    if (installed) parts.push_back(std::to_string(installed) + " installed");
    if (already) parts.push_back(std::to_string(already) + " already there");
    if (failed) parts.push_back(std::to_string(failed) + " failed");
    if (cancelled) parts.push_back(std::to_string(cancelled) + " cancelled");
    if (notrun) parts.push_back(std::to_string(notrun) + " not run");
    if (tone) *tone = failed ? Tone::Error : (cancelled || notrun) ? Tone::Warn : Tone::Ok;
    return (failed || cancelled || notrun ? "Finished: " : "Done: ") + Join(parts, ", ") + (failed || cancelled || notrun ? " - see the marks and the log." : ".");
}

// ------------------------------------------------------------------ words

std::string InstallModel::ShortMark(const PieceOutcome& o, Tone* tone) {
    if (tone) *tone = Tone::Normal;
    switch (o.status) {
        case JobStatus::Installed:
            if (tone) *tone = Tone::Ok;
            return "Installed" + (o.version.empty() ? std::string() : " " + o.version);
        case JobStatus::AlreadyThere:
            if (tone) *tone = Tone::Ok;
            return "Already installed";
        case JobStatus::Failed:
            if (tone) *tone = Tone::Error;
            return "Failed";
        case JobStatus::Cancelled:
            if (tone) *tone = Tone::Warn;
            return "Cancelled";
        case JobStatus::NotRun:
            if (tone) *tone = Tone::Warn;
            return "Not run";
    }
    return "";
}

std::string InstallModel::Meaning(const PieceOutcome& o) {
    const std::string code = o.code.empty() ? "" : " (" + o.code + ")";
    switch (o.status) {
        case JobStatus::Installed: {
            std::string s = "installed" + (o.version.empty() || o.piece == Piece::Loader ? std::string() : " " + o.version);
            if (!o.sha256.empty() && !o.sha_verified) s += " (no published checksum to compare with; its SHA-256 is " + o.sha256.substr(0, 16) + "...)";
            return s;
        }
        case JobStatus::AlreadyThere:
            return o.piece == Piece::Loader ? "already installed: the reviewed files are there, nothing was downloaded" : "already the newest version, nothing was downloaded or run";
        case JobStatus::NotRun:
            return o.message.empty() ? "not run" : o.message;
        case JobStatus::Cancelled:
            return (o.setup_exit >= 0 ? "the Setup was cancelled in its wizard" : "cancelled") + std::string(o.setup_exit >= 0 || o.piece == Piece::Loader ? "" : ": the download was stopped and deleted") + code;
        case JobStatus::Failed:
            break;
    }
    const std::string& c = o.code;
    if (c == "E_UAC_DECLINED") return "you declined the administrator prompt, so nothing was installed" + code;
    if (c == "E_UNVERIFIED") return "no checksum is published and the download was not accepted without one, so nothing was downloaded" + code;
    if (c == "E_HASH") return "a checksum did not match, so nothing was installed (the download was deleted and no Setup was run)" + code + ": " + o.message;
    if (c == "E_SETUP") return "the Setup failed" + (o.setup_exit >= 0 ? ": " + DescribeSetupExit(o.setup_exit) + " (exit code " + std::to_string(o.setup_exit) + ")" : std::string()) + code;
    if (c == "E_MOD_NOT_INSTALLED") return "the Setup finished, but the mod is not in this game folder - did you choose another folder in its wizard?" + code;
    if (c == "E_GAME_RUNNING") return "the game is running: close it first" + code;
    if (c == "E_GAME") return "this is not a game folder" + code;
    if (c == "E_ARCHIVE") return "the archive is damaged or holds names it must not hold, so nothing was installed" + code + ": " + o.message;
    if (c == "E_UNTRUSTED") return "a server gave an address the launcher does not trust, so nothing was downloaded" + code + ": " + o.message;
    if (c == "E_ROLLBACK") return "the install failed half way and putting the old files back failed too" + code + ": " + o.message;
    if (c == "E_IO" && Has(o.message, "administrator rights are needed"))
        return "administrator rights are needed for this game folder: close the launcher, start it again with Run as administrator, and install again" + code;
    if (c == "E_IO") return "a network or disk problem" + code + ": " + o.message;
    return (o.message.empty() ? "failed" : o.message) + code;
}

}  // namespace gui
}  // namespace vrml
