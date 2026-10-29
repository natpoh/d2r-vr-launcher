#include "cli_install.h"

#include <iostream>
#include <sstream>

#include "game.h"
#include "install.h"
#include "install_run.h"
#include "json.h"
#include "mods.h"
#include "releases.h"
#include "setup_run.h"
#include "sha256.h"

namespace vrml {

namespace {

constexpr uint64_t kLoaderZipMax = 300ull << 20;

std::string TestHost(const CliEnv& env) { return TestHttpHost(env.env); }

ReleaseSources SourcesFor(const CliEnv& env) { return ReleaseSourcesFor(env.env); }

// The client for the JSON questions. Tests put their own in CliEnv.
HttpClient& ClientFor(const std::string& id, CliEnv& env, std::unique_ptr<HttpClient>& holder) {
    if (env.http) return *env.http;
    holder = MakeHttpClient(HttpOptionsForQuestion(id == "d2r-vr" ? Piece::Authentic : Piece::Unleashed, env.env));
    return *holder;
}

std::string ModName(const std::string& id) { return id == "d2r-vr" ? "D2R VR Authentic" : id == "d2r-pcvr" ? "D2R VR Unleashed" : id; }

std::string MiB(uint64_t b) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(b) / (1024.0 * 1024.0));
    return buf;
}

// Progress for a terminal: a line per 10 % (or per 8 MB when the size is not known), to stderr. Also the test switch that cancels a download.
class Printer {
public:
    Printer(std::ostream& err, std::string label, uint64_t cancelAfter) : err_(err), label_(std::move(label)), cancel_after_(cancelAfter) {}
    bool operator()(uint64_t done, uint64_t total) {
        if (cancel_after_ && done >= cancel_after_) return false;
        const uint64_t step = total ? done * 10 / total : done / (8ull << 20);
        if (step != last_ || (total && done == total && !finished_)) {
            last_ = step;
            if (total && done == total) finished_ = true;
            err_ << "  " << label_ << ": " << MiB(done) << (total ? " of " + MiB(total) + " (" + std::to_string(done * 100 / total) + " %)" : std::string()) << "\n";
        }
        return true;
    }

private:
    std::ostream& err_;
    std::string label_;
    uint64_t cancel_after_;
    uint64_t last_ = ~0ull;
    bool finished_ = false;
};

uint64_t CancelAfter(const CliEnv& env) {
    if (TestHost(env).empty()) return 0;
    const std::string v = EnvGet(env.env, "VRML_TEST_CANCEL_AFTER");
    return v.empty() ? 0 : std::strtoull(v.c_str(), nullptr, 10);
}

Json StringArray(const std::vector<std::string>& v) {
    Json a = Json::Array();
    for (const std::string& s : v) a.push(Json::String(s));
    return a;
}

Json ReleaseJson(const Release& r) {
    Json o = Json::Object();
    o.set("version", Json::String(r.version));
    o.set("tag", Json::String(r.tag));
    o.set("url", Json::String(r.url));
    o.set("asset", Json::String(r.asset_name));
    o.set("sha256", Json::String(r.sha256));
    o.set("sha256_source", Json::String(r.sha256_source));
    o.set("size", Json::Number(static_cast<double>(r.size)));
    o.set("prerelease", Json::Bool(r.prerelease));
    o.set("up_to_date", Json::Bool(r.up_to_date));
    o.set("published", Json::String(r.published));
    o.set("source", Json::String(r.source));
    o.set("notes", Json::String(r.notes));
    o.set("warnings", StringArray(r.warnings));
    return o;
}

Json UpdateJson(const UpdateInfo& u) {
    Json o = Json::Object();
    o.set("id", Json::String(u.id));
    o.set("name", Json::String(u.name));
    o.set("installed", Json::Bool(u.installed));
    o.set("installed_version", Json::String(u.installed_version));
    o.set("checked", Json::Bool(u.checked));
    o.set("error", Json::String(u.error));
    o.set("update_available", Json::Bool(u.update_available));
    o.set("can_install", Json::Bool(u.can_install));
    o.set("needs_admin", Json::Bool(u.id == "d2r-vr"));
    if (u.checked) o.set("latest", ReleaseJson(u.latest));
    return o;
}

fs::path GameArg(const InstallCliArgs& a) { return a.game.empty() ? fs::path() : P(a.game); }

void NeedProbe(CliEnv& env) {
    if (!env.probe) Throw("E_USAGE", "no process probe: cannot check that the game is closed");
}

// ---------------------------------------------------------------- install loader

int InstallLoader(const InstallCliArgs& a, std::ostream& out, std::ostream& err, CliEnv& env, const fs::path& home, Logger& log) {
    const fs::path game = GameArg(a);
    const LoaderPlan plan = LoaderPlanFor(env.env, a.pin_file);
    const Pin& pin = plan.pin;
    const std::vector<ExpectedFile>& expected = plan.expected;
    const bool ownExpected = plan.own_expected;
    const std::string testHost = TestHost(env);

    HttpOptions ho;
    ho.allow_hosts = {"d2rloader.net"};
    ho.max_bytes = kLoaderZipMax;
    ho.allow_http_host = testHost;
    HttpDownloader own(ho);
    Downloader* dl = env.downloader ? env.downloader : &own;
    DepsCache cache(home, dl, &log);
    const fs::path backups = home / "backups";

    if (a.dry_run || a.no_run) {
        if (!LooksLikeGame(game)) Throw("E_GAME", U(game) + " has no D2R.exe. Pick the folder Diablo II: Resurrected is installed in.");
    }
    if (a.dry_run) {
        NeedProbe(env);
        const std::vector<std::string> running = env.probe->Running({"D2R.exe", "D2RLoader.exe"});
        if (!running.empty()) Throw("E_GAME_RUNNING", "Close " + running.front() + " first: the game is running.");
        const bool already = FilesMatch(game, expected);
        const bool cached = IsFile(cache.PathOf(pin));
        const bool writable = FolderWritable(game);
        if (a.json) {
            Json o = Json::Object();
            o.set("command", Json::String("install loader"));
            o.set("dry_run", Json::Bool(true));
            o.set("game", Json::String(U(game)));
            o.set("version", Json::String(pin.version));
            o.set("url", Json::String(pin.url));
            o.set("archive_sha256_any_of", StringArray(pin.sha256));
            o.set("in_cache", Json::Bool(cached));
            o.set("already_installed", Json::Bool(already));
            o.set("needs_admin", Json::Bool(!writable));
            o.set("backup_dir", Json::String(U(backups)));
            out << o.dump();
            return 0;
        }
        out << "install loader (dry run): nothing is downloaded or changed\n";
        out << "  game              " << U(game) << "\n";
        out << "  D2RLoader         " << pin.version << "\n";
        out << "  would download    " << pin.url << (cached ? "   (a verified copy is in the cache: no download)" : "") << "\n";
        out << "  archive SHA-256   one of:";
        for (const std::string& h : pin.sha256) out << " " << h;
        out << "\n  unpacked files    must be the reviewed ones:\n";
        for (const ExpectedFile& f : expected) out << "      " << f.rel << "  " << (f.sha256_any_of.empty() ? std::string("(any)") : f.sha256_any_of.front()) << "\n";
        out << "  already installed " << (already ? "yes: nothing would be done" : "no") << "\n";
        out << "  needs admin       " << (writable ? "no (the game folder can be written)" : "YES: the game folder is protected; run the launcher as administrator") << "\n";
        out << "  kept as they are  d2rloader/config/* and mods/* that already exist (d2rloader.toml is written only when absent)\n";
        out << "  backups           files it replaces are copied to " << U(backups / "d2rloader-<time>") << " first\n";
        return 0;
    }

    if (a.no_run) {                                      // download and verify the archive, install nothing
        Printer pr(err, pin.file, CancelAfter(env));
        struct Reset { Downloader* d; ~Reset() { if (d) d->SetProgress({}); } } reset{dl};
        if (dl) dl->SetProgress(std::ref(pr));
        const bool had = IsFile(cache.PathOf(pin));
        const fs::path zip = cache.Ensure(pin);
        std::string h;
        Sha256File(zip, &h);
        if (a.json) {
            Json o = Json::Object();
            o.set("command", Json::String("install loader"));
            o.set("no_run", Json::Bool(true));
            o.set("archive", Json::String(U(zip)));
            o.set("sha256", Json::String(h));
            o.set("downloaded", Json::Bool(!had));
            out << o.dump();
        } else {
            out << "the D2RLoader " << pin.version << " archive is " << (had ? "in the cache" : "downloaded") << " and verified: " << U(zip) << "\n  SHA-256 " << h << "\n  (nothing was installed)\n";
        }
        return 0;
    }

    NeedProbe(env);
    Printer pr(err, pin.file, CancelAfter(env));
    InstallProgress prog = [&](const std::string& stage, uint64_t done, uint64_t total) {
        if (stage == "download") return pr(done, total);
        return true;
    };
    const InstallResult r = InstallD2RLoader(game, pin, cache, *env.probe, prog, backups, ownExpected ? &expected : nullptr, &log);
    if (a.json) {
        Json o = Json::Object();
        o.set("command", Json::String("install loader"));
        o.set("ok", Json::Bool(true));
        o.set("already_installed", Json::Bool(r.already_installed));
        o.set("downloaded", Json::Bool(r.downloaded));
        o.set("written", StringArray(r.written));
        o.set("kept", StringArray(r.kept));
        o.set("unchanged", StringArray(r.unchanged));
        o.set("backed_up", StringArray(r.backed_up));
        o.set("backup_dir", Json::String(U(r.backup_dir)));
        o.set("notes", StringArray(r.notes));
        out << o.dump();
        return 0;
    }
    if (r.already_installed) {
        out << "D2RLoader " << pin.version << " is already installed in " << U(game) << " (the reviewed files are there). Nothing was downloaded or changed.\n";
        return 0;
    }
    out << "D2RLoader " << pin.version << " installed in " << U(game) << " (" << (r.downloaded ? "downloaded" : "from the cache") << ")\n";
    for (const std::string& f : r.written) out << "  wrote   " << f << "\n";
    for (const std::string& f : r.unchanged) out << "  same    " << f << "\n";
    for (const std::string& f : r.kept) out << "  kept    " << f << "  (yours)\n";
    if (!r.backed_up.empty()) out << "  backup  " << r.backed_up.size() << " replaced file(s) copied to " << U(r.backup_dir) << "\n";
    for (const std::string& n : r.notes) out << "  note: " << n << "\n";
    return 0;
}

// ---------------------------------------------------------------- install authentic | unleashed

int InstallModCommand(const InstallCliArgs& a, const std::string& id, std::ostream& out, std::ostream& err, CliEnv& env, const fs::path& home, Logger& log) {
    const fs::path game = GameArg(a);
    const std::string name = ModName(id);
    if (!LooksLikeGame(game)) Throw("E_GAME", U(game) + " has no D2R.exe. Pick the folder Diablo II: Resurrected is installed in.");
    NeedProbe(env);
    const std::vector<std::string> running = env.probe->Running({"D2R.exe", "D2RLoader.exe"});
    if (!running.empty()) Throw("E_GAME_RUNNING", "Close " + running.front() + " first: the game is running.");

    ReleaseSources src = SourcesFor(env);
    if (id == "d2r-vr") src.develop = AuthenticDevelopWanted(game);
    const std::string testHost = src.test_http_host;
    const std::string installed = InstalledVersion(game, id);
    std::unique_ptr<HttpClient> holder;
    HttpClient& http = ClientFor(id, env, holder);
    const Release rel = id == "d2r-vr" ? LatestAuthentic(http, a.force ? std::string() : installed, src) : LatestUnleashed(http, a.force ? std::string() : installed, src);
    const bool needsAdmin = id == "d2r-vr";

    if (rel.up_to_date) {
        if (a.json) {
            Json o = Json::Object();
            o.set("command", Json::String("install " + id));
            o.set("up_to_date", Json::Bool(true));
            o.set("installed_version", Json::String(installed));
            out << o.dump();
        } else {
            out << name << (installed.empty() ? std::string() : " " + installed) << " is already the newest version. (--force installs it again.)\n";
        }
        return 0;
    }

    if (a.dry_run) {
        std::string why;
        const bool urlOk = ReleaseUrlOk(rel, testHost, &why);
        const bool refused = rel.sha256.empty() && !a.accept_unverified;
        if (a.json) {
            Json o = Json::Object();
            o.set("command", Json::String("install " + id));
            o.set("dry_run", Json::Bool(true));
            o.set("installed_version", Json::String(installed));
            o.set("release", ReleaseJson(rel));
            o.set("needs_admin", Json::Bool(needsAdmin));
            o.set("address_ok", Json::Bool(urlOk));
            o.set("would_be_refused", Json::Bool(refused || !urlOk));
            out << o.dump();
        } else {
            out << "install " << id << " (dry run): nothing is downloaded or run\n";
            out << "  mod               " << name << "   installed: " << (installed.empty() ? std::string("none / unknown") : installed) << "\n";
            out << "  newest            " << rel.version << (rel.tag.empty() ? "" : "  (" + rel.tag + ")") << (rel.prerelease ? "  [pre-release]" : "") << "\n";
            out << "  would download    " << rel.url << "\n";
            out << "  file              " << rel.asset_name << (rel.size ? "  (" + MiB(rel.size) + ")" : std::string("  (size not published)")) << "\n";
            if (!rel.sha256.empty()) out << "  expected SHA-256  " << rel.sha256 << "  (" << rel.sha256_source << ")\n";
            else out << "  expected SHA-256  NONE PUBLISHED: " << (a.accept_unverified ? "you accepted that (--accept-unverified); the SHA-256 of the download will be shown" : "the download is refused unless you add --accept-unverified") << "\n";
            out << "  needs admin       " << (needsAdmin ? "YES: Windows asks for permission when the Setup starts" : "no (this Setup installs for the current user)") << "\n";
            out << "  would run         " << rel.asset_name << (id == "d2r-vr" ? " " + InnoDirArg(game) : std::string()) << "   (the Setup shows its own window)\n";
            for (const std::string& w : rel.warnings) out << "  warning: " << w << "\n";
            if (!urlOk) out << "  REFUSED: " << why << "\n";
        }
        if (!urlOk) return 29;
        if (refused) return 27;
        return 0;
    }

    HttpOptions ho = HttpOptionsForRelease(rel);
    ho.allow_http_host = testHost;
    HttpDownloader own(ho);
    Downloader* dl = env.downloader ? env.downloader : &own;
    SetupBackend* setup = env.setup;
    std::unique_ptr<SetupBackend> setupHolder;
    if (!setup) {
        setupHolder = MakeSystemSetupBackend();
        setup = setupHolder.get();
    }
    ModInstallOptions mo;
    mo.cache_dir = home / "downloads";
    mo.accept_unverified = a.accept_unverified;
    mo.run_setup = !a.no_run;
    mo.test_http_host = testHost;
    mo.test_never_elevate = !testHost.empty() && EnvGet(env.env, "VRML_TEST_NO_ELEVATE") == "1";
    Printer pr(err, rel.asset_name, CancelAfter(env));
    mo.progress = std::ref(pr);
    const ModInstallResult r = InstallMod(game, id, rel, *dl, *env.probe, *setup, mo);
    log.Line("install " + id + " " + rel.version + ": " + (r.setup_ran ? "Setup ran, exit " + std::to_string(r.exit_code) : std::string("Setup not run")) + ", installed after: " + (r.installed ? "yes" : "no") +
             (r.code.empty() ? "" : ", " + r.code));
    if (a.json) {
        Json o = Json::Object();
        o.set("command", Json::String("install " + id));
        o.set("ok", Json::Bool(r.code.empty()));
        o.set("code", Json::String(r.code));
        o.set("message", Json::String(r.message));
        o.set("installer", Json::String(U(r.installer.path)));
        o.set("sha256", Json::String(r.installer.sha256));
        o.set("verified", Json::Bool(r.installer.verified));
        o.set("setup_ran", Json::Bool(r.setup_ran));
        o.set("elevated", Json::Bool(r.elevated));
        o.set("exit_code", Json::Number(r.exit_code));
        o.set("was_installed", Json::Bool(r.was_installed));
        o.set("installed", Json::Bool(r.installed));
        o.set("installed_version", Json::String(r.installed_version));
        o.set("notes", StringArray(r.notes));
        out << o.dump();
    } else {
        out << r.message << "\n";
        out << "  installer   " << U(r.installer.path) << "\n";
        out << "  SHA-256     " << r.installer.sha256 << (r.installer.verified ? "  (matches the published one)" : "  (no published checksum to compare with)") << "\n";
        if (r.setup_ran) out << "  Setup       exit code " << r.exit_code << (r.elevated ? ", started with administrator rights" : "") << "\n";
        if (r.setup_ran) out << "  now         " << name << (r.installed ? " is installed" : " is NOT installed") << (r.installed_version.empty() ? "" : ", version " + r.installed_version) << "\n";
        for (const std::string& n : r.notes) out << "  note: " << n << "\n";
    }
    if (r.code.empty()) return 0;
    if (r.code == "E_CANCELLED") return 30;
    if (r.code == "E_SETUP") return 28;
    return 11;
}

}  // namespace

int RunReleasesCommand(const InstallCliArgs& a, std::ostream& out, std::ostream& err, CliEnv& env) {
    const ReleaseSources src = SourcesFor(env);
    const fs::path game = GameArg(a);
    std::vector<UpdateInfo> all;
    for (const char* id : {"d2r-vr", "d2r-pcvr"}) {
        std::unique_ptr<HttpClient> holder;
        HttpClient& http = ClientFor(id, env, holder);
        all.push_back(CheckUpdate(http, game, id, src));
    }
    bool any = false;
    for (const UpdateInfo& u : all) any = any || u.checked;
    if (a.json) {
        Json o = Json::Object();
        o.set("game", Json::String(U(game)));
        Json arr = Json::Array();
        for (const UpdateInfo& u : all) arr.push(UpdateJson(u));
        o.set("mods", arr);
        out << o.dump();
        return any ? 0 : 20;
    }
    for (const UpdateInfo& u : all) {
        out << u.name << "\n";
        if (!game.empty()) out << "  installed   " << (u.installed ? (u.installed_version.empty() ? "yes (version unknown)" : u.installed_version) : "no") << "\n";
        if (!u.checked) {
            out << "  newest      could not be found out: " << u.error << "\n";
            continue;
        }
        const Release& r = u.latest;
        if (r.up_to_date) {
            out << "  newest      " << (u.installed_version.empty() ? std::string("(nothing newer than what you have)") : u.installed_version + " (you have the newest)") << "\n";
        } else {
            out << "  newest      " << r.version << (r.prerelease ? " [pre-release]" : "") << (u.update_available ? "   UPDATE AVAILABLE" : u.can_install ? "   can be installed" : "") << "\n";
            out << "  installer   " << r.asset_name << (r.size ? "  " + MiB(r.size) : std::string()) << "\n";
            out << "  from        " << r.url << "\n";
            out << "  SHA-256     " << (r.sha256.empty() ? std::string("none published: installing needs --accept-unverified") : r.sha256 + "  (" + r.sha256_source + ")") << "\n";
            out << "  admin       " << (u.id == "d2r-vr" ? "yes: Windows asks when the Setup starts" : "no (per user)") << "\n";
            for (const std::string& w : r.warnings) out << "  warning: " << w << "\n";
        }
    }
    if (!any) err << "error [E_IO]: neither server could be asked\n";
    return any ? 0 : 20;
}

int RunInstallCommand(const InstallCliArgs& a, std::ostream& out, std::ostream& err, CliEnv& env, const fs::path& home, Logger& log) {
    if (a.pos.size() < 2) Throw("E_USAGE", "install loader|authentic|unleashed --game DIR [--dry-run] [--no-run] [--accept-unverified] [--force]");
    const std::string what = Lower(a.pos[1]);
    if (what == "loader" || what == "d2rloader") return InstallLoader(a, out, err, env, home, log);
    if (what == "authentic" || what == "d2r-vr") return InstallModCommand(a, "d2r-vr", out, err, env, home, log);
    if (what == "unleashed" || what == "d2r-pcvr") return InstallModCommand(a, "d2r-pcvr", out, err, env, home, log);
    Throw("E_USAGE", "install what? loader, authentic or unleashed (not \"" + a.pos[1] + "\")");
}

}  // namespace vrml
