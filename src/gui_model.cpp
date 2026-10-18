#include "gui_model.h"

#include <algorithm>
#include <sstream>

#include "cli.h"
#include "game.h"
#include "gui_install_model.h"

namespace vrml {
namespace gui {

namespace {

struct NoBackend : LaunchBackend {
    bool Start(const LaunchPlan&, const std::map<std::string, std::string>&, std::string* error) override {
        if (error) *error = "no launch backend";
        return false;
    }
};

// The key two spellings of one folder share ("D:\Games\D2R\" and "d:/games/d2r").
std::string FolderKey(const fs::path& p) {
    if (p.empty()) return "";
    std::error_code ec;
    const fs::path canon = fs::weakly_canonical(p, ec);
    return Lower(U(ec ? p : canon));
}

GameChoice MakeChoice(const fs::path& dir, const std::string& source) {
    GameChoice c;
    c.is_game = LooksLikeGame(dir);
    if (c.is_game) {
        c.install = DescribeGame(dir, source);
    } else {
        c.install.dir = dir;
        c.install.source = source;
    }
    c.label = DisplayPath(dir);
    if (!source.empty()) c.label += "  (" + source + ")";
    return c;
}

// What changes the cards: when this stays the same the window keeps its controls.
std::string ModSignature(const std::vector<ModEntry>& mods) {
    std::string s;
    for (const ModEntry& m : mods)
        s += m.id + "|" + m.name + "|" + m.version + "|" + m.found_version + "|" + m.found_from + "|" + (m.installed ? "1" : "0") + "|" + U(m.icon) + "|" + std::to_string(m.notes.size()) + "|" + m.source + "\n";
    return s;
}

}  // namespace

std::string DisplayPath(const fs::path& p) {
    std::string s = U(p);
    std::replace(s.begin(), s.end(), '/', '\\');
    return s;
}

// ------------------------------------------------------------------ the command line

Options ParseOptions(const std::vector<std::string>& args) {
    Options o;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto value = [&](std::string* out) {
            if (i + 1 >= args.size()) {
                o.problems.push_back(a + " needs a value");
                return;
            }
            *out = args[++i];
        };
        if (a == "--game") value(&o.game);
        else if (a == "--home") value(&o.home);
        else if (a == "--select") value(&o.select);
        else if (a == "--screenshot") value(&o.screenshot);
        else if (a == "--dpi") {
            std::string v;
            value(&v);
            o.dpi = std::atoi(v.c_str());
            if (o.dpi < 48 || o.dpi > 480) {
                o.problems.push_back("--dpi wants a number between 48 and 480");
                o.dpi = 0;
            }
        } else if (a == "--size") {
            std::string v;
            value(&v);
            const size_t x = v.find('x');
            o.width_dp = x == std::string::npos ? 0 : std::atoi(v.substr(0, x).c_str());
            o.height_dp = x == std::string::npos ? 0 : std::atoi(v.substr(x + 1).c_str());
            if (o.width_dp < 200 || o.width_dp > 4000 || o.height_dp < 200 || o.height_dp > 4000) {
                o.problems.push_back("--size wants WIDTHxHEIGHT in dp, e.g. 640x770");
                o.width_dp = o.height_dp = 0;
            }
        } else if (a == "--install-view") {
            o.install_view = true;
        } else if (a == "--install-tick" || a == "--install-run") {
            std::string v;
            value(&v);
            std::vector<Piece> pieces;
            std::string why;
            if (v.empty() || !ParsePieceList(v, &pieces, &why)) {
                if (!v.empty()) o.problems.push_back(a + ": " + why);
                continue;
            }
            std::vector<std::string>& dest = a == "--install-run" ? o.install_run : o.install_tick;
            dest.clear();
            for (Piece p : pieces) dest.push_back(PieceWord(p));
            if (a == "--install-run") o.has_install_run = true;
        } else if (a == "--accept-unverified") {
            o.accept_unverified = true;
        } else if (a == "--pin-file") {
            value(&o.pin_file);
        } else if (a == "--no-update-check") {
            o.no_update_check = true;
        } else if (a == "--install-now") {
            o.install_now = true;
        } else if (a == "--shot-now") {
            o.shot_now = true;
        } else if (a == "--dry-run") o.dry_run = true;
        else if (a == "--play") o.play = true;
        else if (a == "--no-search") o.no_search = true;
        else if (a == "--processes") {
            std::string v;
            value(&v);
            o.fake_processes = true;
            if (v != "-")
                for (const std::string& n : Split(v, ','))
                    if (!n.empty()) o.processes.push_back(n);
        } else if (a == "--help" || a == "-h" || a == "/?") {
            o.help = true;
        } else {
            o.problems.push_back("unknown argument: " + a);
        }
    }
    return o;
}

std::vector<std::string> TestSwitchProblems(const Options& opt, const std::map<std::string, std::string>& env) {
    std::vector<std::string> v;
    const bool allowed = !TestHttpHost(env).empty();
    auto need = [&](bool used, const char* name) {
        if (used && !allowed) v.push_back(std::string(name) + " is a test aid that reaches a web server: it works only together with VRML_ALLOW_HTTP_HOST (a loopback test server)");
    };
    need(opt.has_install_run, "--install-run");
    need(opt.accept_unverified, "--accept-unverified");
    need(!opt.pin_file.empty(), "--pin-file");
    need(opt.install_now, "--install-now");
    if (opt.has_install_run && opt.game.empty()) v.push_back("--install-run needs --game DIR: the folder to install into");
    return v;
}

const char* UsageText() {
    return "D2RVRLauncher [--game DIR] [--home DIR] [--select MOD_ID] [--install-view] [--dry-run] [--play] [--screenshot FILE.png] [--dpi N]\n"
           "  --install-view     open the install view (D2RLoader and the two mods: what is installed, what is newer, install / update) right after start\n"
           "  --install-tick L   with it (and what a card's Install / Update button does): tick these pieces: loader, authentic, unleashed (comma separated)\n"
           "  --no-update-check  do not look at the newest releases at start (the \"Check for updates when the launcher starts\" setting, for this run)\n"
           "  --game DIR         use this game folder (the remembered one is left alone)\n"
           "  --home DIR         the launcher's own folder (settings.json, logs); default %LOCALAPPDATA%\\VRModLauncher\n"
           "  --select MOD_ID    preselect this mod (d2r-vr, d2r-pcvr, or the folder name of another D2RLoader mod)\n"
           "  --dry-run          Play shows what it would start and starts nothing\n"
           "  --play             no window: do Play once, print the outcome, exit with 0 or the refusal's code\n"
           "  --screenshot FILE  render the window to a PNG and exit\n"
           "  --dpi N            lay the window out as at N dpi (a test aid)\n"
           "  --size WxH         the first size of the window's client area, in dp (a test aid)\n"
           "  --no-search        do not look for installed games; only the remembered folder counts (a test aid)\n"
           "  --processes LIST   the processes that \"run\" are exactly these, e.g. D2R.exe; - for none (a test aid: the real process list is not read)\n"
           "  test aids that reach a web server (only together with VRML_ALLOW_HTTP_HOST, a loopback test server):\n"
           "  --install-run L    no window: install these pieces like the view's Install button (loader,authentic,unleashed), one line each, exit 0 or the first failure's code\n"
           "  --accept-unverified  a release without a published checksum is accepted (the view does not ask)\n"
           "  --pin-file FILE    the D2RLoader pin and reviewed files of a test\n"
           "  --install-now      the view presses Install by itself once its checks are done\n"
           "  --shot-now         with --install-view --screenshot: shoot at the first paint (\"checking...\"), not when the checks are done\n";
}

// ------------------------------------------------------------------ the model

Model::Model(const Options& opt, const Machine& svc) : opt_(opt), svc_(svc) {
    home_ = !opt.home.empty() ? P(opt.home) : DefaultHome(svc.env);
    log_.SetFile(home_ / "logs" / "launcher.log");
    first_run_ = !IsFile(home_ / "settings.json");
    settings_ = LoadSettings(home_);
    runtimes_ = ListRuntimes(svc.runtime_roots, svc.env);

    if (!opt.game.empty()) {
        // a folder given on the command line is the only game: tests and support see exactly what they asked for
        std::error_code ec;
        const fs::path given = fs::absolute(P(opt.game), ec);
        games_.push_back(MakeChoice(ec ? P(opt.game) : given, "given"));
        game_ = 0;
    } else {
        GameRoots roots = opt.no_search ? GameRoots() : svc.game_roots;
        if (!settings_.game.empty()) roots.extra.push_back(P(settings_.game));
        for (const GameInstall& g : FindGames(roots)) {
            GameChoice c;
            c.install = g;
            c.is_game = true;
            c.label = DisplayPath(g.dir) + "  (" + g.source + ")";
            games_.push_back(c);
        }
        const std::string want = FolderKey(settings_.game.empty() ? fs::path() : P(settings_.game));
        for (size_t i = 0; i < games_.size() && game_ < 0; ++i)
            if (!want.empty() && FolderKey(games_[i].install.dir) == want) game_ = static_cast<int>(i);
        if (game_ < 0 && !games_.empty()) game_ = 0;
    }
    LoadGame();
    if (!opt.select.empty()) {
        if (!SelectModById(opt.select)) notes_.push_back("There is no mod \"" + opt.select + "\" in this game folder.");
    }
}

void Model::LoadGame() {
    mods_.clear();
    mod_ = -1;
    loader_ = LoaderStatus();
    loader_hashed_ = false;
    const GameChoice* g = game();
    if (!g || !g->is_game) return;
    mods_ = DiscoverMods(g->install.dir);
    loader_ = CheckLoader(g->install.dir, false);
    ChooseInitialMod();
}

void Model::ChooseInitialMod() {
    mod_ = -1;
    const int last = FindMod(settings_.last_mod);
    if (last >= 0 && mods_[static_cast<size_t>(last)].installed) {
        mod_ = last;
        return;
    }
    for (size_t i = 0; i < mods_.size(); ++i)
        if (mods_[i].installed) {
            mod_ = static_cast<int>(i);
            return;
        }
    if (!mods_.empty()) mod_ = 0;
}

int Model::FindMod(const std::string& idOrFolder) const {
    if (idOrFolder.empty()) return -1;
    for (size_t i = 0; i < mods_.size(); ++i)
        if (IEquals(mods_[i].id, idOrFolder) || IEquals(mods_[i].folder, idOrFolder)) return static_cast<int>(i);
    return -1;
}

bool Model::SelectModById(const std::string& idOrFolder) {
    const int i = FindMod(idOrFolder);
    if (i < 0) return false;
    mod_ = i;
    return true;
}

void Model::SelectGame(int index) {
    if (index < 0 || index >= static_cast<int>(games_.size())) return;
    game_ = index;
    LoadGame();
    // the folder the player picked last is the one to come back to (a folder given on the command line is not his choice)
    if (opt_.game.empty() && games_[static_cast<size_t>(game_)].is_game) settings_.game = U(games_[static_cast<size_t>(game_)].install.dir);
}

int Model::AddGame(const fs::path& dir, std::string* error) {
    if (!LooksLikeGame(dir)) {
        if (error) *error = "There is no D2R.exe in " + DisplayPath(dir) + ". Pick the folder Diablo II: Resurrected is installed in.";
        return -1;
    }
    const std::string key = FolderKey(dir);
    for (size_t i = 0; i < games_.size(); ++i)
        if (FolderKey(games_[i].install.dir) == key) {
            SelectGame(static_cast<int>(i));
            return static_cast<int>(i);
        }
    // a --game folder that is not a game was never usable: the picked one replaces it
    games_.erase(std::remove_if(games_.begin(), games_.end(), [](const GameChoice& c) { return !c.is_game; }), games_.end());
    games_.push_back(MakeChoice(dir, "picked"));
    const int index = static_cast<int>(games_.size()) - 1;
    SelectGame(index);
    return index;
}

void Model::SetLoaderStatus(const LoaderStatus& st) {
    loader_ = st;
    loader_hashed_ = true;
}

std::string Model::GameLine(Tone* tone) const {
    if (tone) *tone = Tone::Normal;
    const GameChoice* g = game();
    if (!g) {
        if (tone) *tone = Tone::Warn;
        return "No Diablo II: Resurrected folder found. Use Browse... to pick the folder it is installed in.";
    }
    if (!g->is_game) {
        if (tone) *tone = Tone::Error;
        return "There is no D2R.exe in this folder, so it is not a Diablo II: Resurrected folder.";
    }
    std::string s = g->install.store == "steam" ? "Steam edition" : g->install.store == "battlenet" ? "Battle.net edition" : "Edition not recognised";
    s += " - " + (g->install.build.empty() ? std::string("build unknown") : "build " + g->install.build);
    s += " - ";
    if (!loader_.present) {
        if (tone) *tone = Tone::Warn;
        return s + "D2RLoader is missing";
    }
    if (!loader_hashed_) return s + "D2RLoader found, checking its files...";
    if (loader_.reviewed) return s + "D2RLoader " + loader_.version + " (reviewed files)";
    if (tone) *tone = Tone::Warn;
    return s + "D2RLoader " + (loader_.version.empty() ? std::string("found") : loader_.version) + " (not the reviewed files)";
}

bool Model::Refresh() {
    const GameChoice* g = game();
    if (!g || !g->is_game) return false;
    std::vector<ModEntry> now = DiscoverMods(g->install.dir);
    const bool had = loader_.present;
    const bool has = IsFile(g->install.dir / "D2RLoader.exe");
    bool changed = ModSignature(now) != ModSignature(mods_);
    if (has != had) {
        loader_ = CheckLoader(g->install.dir, false);
        loader_hashed_ = false;
        changed = true;
    }
    if (!changed) return false;
    const std::string keep = mod() ? mod()->id : "";
    mods_ = std::move(now);
    mod_ = FindMod(keep);
    if (mod_ < 0) ChooseInitialMod();
    return true;
}

ModTools Model::Tools() const {
    const GameChoice* g = game();
    if (!g || !mod()) return ModTools();
    return ResolveModTools(*mod(), g->install.dir, svc_.env);
}

// ---- what the servers said

void Model::SetLatest(const UpdateInfo& u) {
    if (u.checked) latest_[u.id] = u.latest;               // a question that failed (offline for a moment) leaves what was known: still the best answer there is
}

const Release* Model::LatestOf(const std::string& id) const {
    auto it = latest_.find(id);
    return it == latest_.end() ? nullptr : &it->second;
}

Model::CardOffer Model::OfferFor(const ModEntry& m) const {
    CardOffer o;
    if (!m.known) return o;
    if (!m.installed) {
        o.kind = CardOffer::Kind::Install;
        o.label = "Install";
        return o;
    }
    const Release* r = LatestOf(m.id);
    const std::string have = NormalizeVersion(m.found_version);       // "" = installed, version not known (never the built-in profile's)
    if (r && IsUpdateFor(have, *r)) {
        o.kind = CardOffer::Kind::Update;
        o.label = "Update";
        o.badge = UpdateBadgeText(have, r->version);
    }
    return o;
}

Model::UninstallOffer Model::UninstallFor(const ModEntry& m) const {
    UninstallOffer o;
    const GameChoice* g = game();
    if (!g || !m.known || !m.installed) return o;
    o.label = "Uninstall";
    const ModTools t = ResolveModTools(m, g->install.dir, svc_.env);
    if (!t.uninstall_exe.empty() && IsFile(t.uninstall_exe)) {
        o.kind = UninstallOffer::Kind::Run;
        o.exe = t.uninstall_exe;
        o.args = t.uninstall_args;
        o.needs_admin = t.uninstall_needs_admin;
        return o;
    }
    o.kind = UninstallOffer::Kind::Explain;
    o.explanation = m.name + " has no uninstaller on this computer" +
                    (t.uninstall_exe.empty() ? std::string(".") : " (" + DisplayPath(t.uninstall_exe) + " is not there).") +
                    " That happens when it was put in by hand (from a ZIP) and not by its Setup.\n\nTo remove it: close the game, then delete the folder\n" +
                    DisplayPath(m.dir) +
                    "\nyourself. The launcher never deletes a mod's folder, because it did not make it. Other mods and the game itself are not in that folder.";
    return o;
}

std::string Model::UninstallRefusal() const {
    const GameChoice* g = game();
    if (!g || !g->is_game) return "Pick the folder Diablo II: Resurrected is installed in first.";
    if (GameRunning()) return "The game is running. Save & Exit in the game first (the launcher does not close it): a mod cannot be removed while the game uses its files.";
    return "";
}

bool Model::UpdateCheckWanted() const {
    const GameChoice* g = game();
    return settings_.check_updates_on_start && g && g->is_game && !opt_.no_update_check;
}

bool Model::ShouldOfferInstallView() const {
    const GameChoice* g = game();
    if (!g || !g->is_game || !opt_.game.empty() || settings_.install_view_offered) return false;
    for (const ModEntry& m : mods_)
        if (m.known && m.installed) return false;
    return true;
}

// ---- runtimes

int Model::runtime_index() const {
    const ModEntry* m = mod();
    std::string want;
    fs::path file;
    if (m) {
        auto it = settings_.mods.find(m->id);
        if (it != settings_.mods.end()) {
            want = it->second.runtime;
            file = P(it->second.runtime_file);
        }
    }
    if (want.empty()) want = "system";
    for (size_t i = 0; i < runtimes_.size(); ++i) {
        if (runtimes_[i].id != want) continue;
        if (!runtimes_[i].available) break;
        if (want == "file" && !IsFile(file)) break;
        return static_cast<int>(i);
    }
    return 0;   // "System default", always the first
}

fs::path Model::runtime_file() const {
    const ModEntry* m = mod();
    if (!m) return {};
    auto it = settings_.mods.find(m->id);
    return it == settings_.mods.end() ? fs::path() : P(it->second.runtime_file);
}

bool Model::SetRuntime(int index, const fs::path& file, std::string* why) {
    const ModEntry* m = mod();
    if (!m || index < 0 || index >= static_cast<int>(runtimes_.size())) return false;
    const RuntimeChoice& c = runtimes_[static_cast<size_t>(index)];
    if (!c.available) {
        if (why) *why = c.label + ": " + c.detail + ".";
        return false;
    }
    if (c.id == "file" && !IsFile(file)) {
        if (why) *why = "That runtime file is not there.";
        return false;
    }
    ModSettings& ms = settings_.mods[m->id];
    ms.runtime = c.id;
    if (c.id == "file") ms.runtime_file = U(file);
    return true;
}

// ---- Play

std::vector<std::string> Model::WatchedProcesses() const {
    if (mod() && !mod()->manifest.processes.empty()) return mod()->manifest.processes;
    return {"D2R.exe", "D2RLoader.exe"};
}

bool Model::GameRunning() const {
    return svc_.probe && !svc_.probe->Running(WatchedProcesses()).empty();
}

PlayState Model::CanPlay(bool game_running) const {
    PlayState p;
    const GameChoice* g = game();
    if (!g) {
        p.reason = "No game folder yet. Use Browse... to pick it.";
        return p;
    }
    if (!g->is_game) {
        p.reason = "There is no D2R.exe in this folder.";
        return p;
    }
    const ModEntry* m = mod();
    if (!m) {
        p.reason = "No mod to play.";
        return p;
    }
    // the order StartMod refuses in, so the reason here is the reason it would give
    if (game_running) {
        p.reason = "The game is running";
        return p;
    }
    if (!m->installed) {
        p.reason = m->name + " is not installed in this game folder.";
        return p;
    }
    if (!m->launchable) {
        p.reason = m->name + " has no way to be started.";
        return p;
    }
    if (!loader_.present) {
        p.reason = "D2RLoader.exe is missing: the mods run only under D2RLoader.";
        return p;
    }
    p.can = true;
    return p;
}

StartOutcome Model::Play(bool dry_run) {
    StartOutcome out;
    const GameChoice* g = game();
    const ModEntry* m = mod();
    if (!g || !m) {
        out.code = "E_GAME";
        out.message = !g ? "There is no game folder. Pick the folder Diablo II: Resurrected is installed in."
                         : "There is no mod to start in this game folder.";
        return out;
    }
    StartRequest req;
    req.game = g->install.dir;
    req.mod = *m;
    const RuntimeChoice& rt = runtimes_[static_cast<size_t>(runtime_index())];
    req.runtime = rt.id;
    if (rt.id == "file") req.runtime_file = runtime_file();
    NoBackend nb;
    LaunchBackend& backend = svc_.backend ? *svc_.backend : static_cast<LaunchBackend&>(nb);
    req.dry_run = dry_run || !svc_.backend;
    ProcessProbe* probe = svc_.probe;
    FakeProbe none;
    out = StartMod(req, probe ? *probe : static_cast<ProcessProbe&>(none), backend, svc_.env, svc_.runtime_roots);
    if (!out.code.empty()) {
        Log("refused " + m->id + " [" + out.code + "]: " + out.message);
        return out;
    }
    if (req.dry_run) return out;
    // started: remember what was used
    settings_.last_mod = m->id;
    ModSettings& ms = settings_.mods[m->id];
    ms.runtime = rt.id;
    if (rt.id == "file") ms.runtime_file = U(req.runtime_file);
    if (opt_.game.empty()) settings_.game = U(g->install.dir);
    SaveSettings();
    Log("started " + m->id + " " + m->version + " (D2RLoader mod " + m->folder + ", option " + out.plan.option + ", runtime " + out.plan.runtime +
        ", store " + out.plan.store + ")");
    return out;
}

void Model::SaveSettings() {
    try {
        vrml::SaveSettings(home_, settings_);
    } catch (const Error& e) {
        Log(std::string("could not save settings.json: ") + e.what());
    } catch (const std::exception& e) {
        Log(std::string("could not save settings.json: ") + e.what());
    }
}

void Model::Log(const std::string& line) {
    try {
        log_.Line(line);
    } catch (const std::exception&) {
        // a log that cannot be written is not worth stopping for
    }
}

// ------------------------------------------------------------------ text for the console and the log

std::string UninstallQuestion(const ModEntry& m, const Model::UninstallOffer& o) {
    return "Uninstall " + m.name + "?\n\nThis runs the mod's own uninstaller:\n" + DisplayPath(o.exe) +
           "\n\nIt may ask its own questions. The game itself and the other mods are not touched.";
}

std::string FormatOutcome(const StartOutcome& so, bool dry_run, int* exit_code) {
    std::ostringstream out;
    for (const std::string& w : so.warnings) out << "warning: " << w << "\n";
    if (!so.code.empty()) {
        out << "error [" << so.code << "]: " << so.message << "\n";
    } else if (dry_run) {
        out << so.plan.Describe();
    } else {
        out << "started " << so.plan.mod_id << " " << so.plan.mod_version << "\n";
    }
    if (exit_code) *exit_code = StartExitCode(so.code);
    return out.str();
}

Machine SystemMachine() {
    static std::unique_ptr<ProcessProbe> probe = MakeSystemProbe();
    static std::unique_ptr<LaunchBackend> backend = MakeSystemBackend();
    Machine s;
    s.env = CurrentEnvironment();
    s.runtime_roots = SystemRuntimeRoots(s.env);
    s.game_roots = SystemGameRoots(s.env);
    s.probe = probe.get();
    s.backend = backend.get();
    return s;
}

}  // namespace gui
}  // namespace vrml
