#include "install_run.h"

#include <algorithm>
#include <memory>
#include <sstream>

#include "game.h"
#include "json.h"
#include "net.h"
#include "setup_run.h"
#include "sha256.h"

namespace vrml {

namespace {

constexpr uint64_t kLoaderZipMax = 300ull << 20;

std::string TestHost(const std::map<std::string, std::string>& env) { return TestHttpHost(env); }

std::string MiB(uint64_t b) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(b) / (1024.0 * 1024.0));
    return buf;
}

struct PinFile {
    Pin pin;
    std::vector<ExpectedFile> expected;
};

// TEST switch: {"version","url","file","sha256":[...],"expected":[{"path","sha256_any_of":[...]}]}
PinFile LoadPinFile(const std::string& path) {
    std::string text;
    if (!ReadFile(P(path), &text)) Throw("E_NOT_FOUND", "cannot read the pin file " + path);
    Json j;
    try {
        j = Json::Parse(text);
    } catch (const JsonError& e) {
        Throw("E_USAGE", std::string("the pin file is not JSON: ") + e.what());
    }
    PinFile pf;
    pf.pin.id = "d2rloader";
    pf.pin.version = j.get_string("version", "test");
    pf.pin.url = j.get_string("url");
    pf.pin.file = j.get_string("file", "D2RLoader-test.zip");
    if (const Json* h = j.find("sha256"))
        for (const Json& x : h->items()) pf.pin.sha256.push_back(x.as_string());
    if (const Json* e = j.find("expected"))
        for (const Json& x : e->items()) {
            ExpectedFile f;
            f.rel = x.get_string("path");
            if (const Json* hs = x.find("sha256_any_of"))
                for (const Json& h : hs->items()) f.sha256_any_of.push_back(h.as_string());
            pf.expected.push_back(f);
        }
    return pf;
}

// An installer's download is cut by this many bytes in a test (VRML_TEST_CANCEL_AFTER; only with the loopback switch).
uint64_t CancelAfter(const std::map<std::string, std::string>& env) {
    if (TestHost(env).empty()) return 0;
    const std::string v = EnvGet(env, "VRML_TEST_CANCEL_AFTER");
    return v.empty() ? 0 : std::strtoull(v.c_str(), nullptr, 10);
}

// A Setup runner that says "now the Setup runs" to the caller before it starts the real one.
class TappedSetup : public SetupBackend {
public:
    TappedSetup(SetupBackend& inner, std::function<void(const SetupCommand&)> before) : inner_(inner), before_(std::move(before)) {}
    SetupStatus Run(const SetupCommand& cmd, int* exitCode, std::string* error) override {
        if (before_) before_(cmd);
        return inner_.Run(cmd, exitCode, error);
    }

private:
    SetupBackend& inner_;
    std::function<void(const SetupCommand&)> before_;
};

PieceOutcome FromError(Piece p, const Error& e) {
    PieceOutcome o;
    o.piece = p;
    o.status = e.code == "E_CANCELLED" ? JobStatus::Cancelled : JobStatus::Failed;
    o.code = e.code;
    o.exit_code = e.exit_code;
    o.message = e.what();
    return o;
}

PieceOutcome Plain(Piece p, JobStatus s, const std::string& code, const std::string& message) {
    PieceOutcome o;
    o.piece = p;
    o.status = s;
    o.code = code;
    o.exit_code = ExitCodeForCode(code);
    o.message = message;
    return o;
}

class Runner {
public:
    Runner(const RunContext& ctx, const RunHooks& hooks, int count) : ctx_(ctx), hooks_(hooks), count_(count), log_(ctx.home / "logs" / "launcher.log") {}

    void SetIndex(int i) { index_ = i; }

    PieceOutcome Run(const PieceJob& job) {
        try {
            if (job.piece == Piece::Loader) return RunLoader();
            return RunMod(job);
        } catch (const Error& e) {
            return FromError(job.piece, e);
        } catch (const std::exception& e) {
            return Plain(job.piece, JobStatus::Failed, "E_IO", std::string("unexpected problem: ") + e.what());
        }
    }

private:
    void Say(const std::string& line) {
        try {
            log_.Line(line);
        } catch (const std::exception&) {
        }
        if (hooks_.log) hooks_.log(line);
    }
    void Stage(Piece p, const std::string& stage, uint64_t done = 0, uint64_t total = 0) {
        if (hooks_.progress) hooks_.progress(index_, count_, p, stage, done, total);
    }
    bool Cancelled(uint64_t bytes = 0) const {
        if (hooks_.cancelled && hooks_.cancelled()) return true;
        const uint64_t after = CancelAfter(ctx_.env->env);
        return after != 0 && bytes >= after;
    }

    PieceOutcome RunLoader() {
        const Piece p = Piece::Loader;
        const LoaderPlan plan = LoaderPlanFor(ctx_.env->env, ctx_.pin_file);
        HttpOptions ho;
        ho.allow_hosts = {"d2rloader.net"};
        ho.max_bytes = kLoaderZipMax;
        ho.allow_http_host = TestHost(ctx_.env->env);
        HttpDownloader own(ho);
        Downloader* dl = ctx_.env->downloader ? ctx_.env->downloader : &own;
        DepsCache cache(ctx_.home, dl, &log_);
        Stage(p, "download");
        InstallProgress prog = [&](const std::string& stage, uint64_t done, uint64_t total) {
            Stage(p, stage, done, total);
            return !Cancelled(stage == "download" ? done : 0);
        };
        const InstallResult r = InstallD2RLoader(ctx_.game, plan.pin, cache, *ctx_.env->probe, prog, ctx_.home / "backups", plan.own_expected ? &plan.expected : nullptr, &log_);
        PieceOutcome o;
        o.piece = p;
        o.version = plan.pin.version;
        o.downloaded = r.downloaded;
        o.notes = r.notes;
        if (r.already_installed) {
            o.status = JobStatus::AlreadyThere;
            o.message = "D2RLoader " + plan.pin.version + " is already installed: the reviewed files are in the game folder. Nothing was downloaded or changed.";
            o.notes.clear();
            return o;
        }
        o.status = JobStatus::Installed;
        o.message = "D2RLoader " + plan.pin.version + " is installed (" + std::string(r.downloaded ? "downloaded" : "from the cache") + ", " + std::to_string(r.written.size()) + " file(s) written).";
        return o;
    }

    Release Lookup(Piece mod, bool force) {
        const std::string id = PieceId(mod);
        const ReleaseSources src = ReleaseSourcesFor(ctx_.env->env);
        std::unique_ptr<HttpClient> holder;
        HttpClient* http = ctx_.env->http;
        if (!http) {
            holder = MakeHttpClient(HttpOptionsForQuestion(mod, ctx_.env->env));
            http = holder.get();
        }
        const std::string installed = force ? std::string() : InstalledVersion(ctx_.game, id);
        return mod == Piece::Authentic ? LatestAuthentic(*http, installed, src) : LatestUnleashed(*http, installed, src);
    }

    PieceOutcome RunMod(const PieceJob& job) {
        const Piece p = job.piece;
        const std::string id = PieceId(p);
        const std::string name = PieceName(p);
        const std::string testHost = TestHost(ctx_.env->env);
        if (!LooksLikeGame(ctx_.game)) Throw("E_GAME", U(ctx_.game) + " has no D2R.exe. Pick the folder Diablo II: Resurrected is installed in.");
        const std::vector<std::string> running = ctx_.env->probe->Running({"D2R.exe", "D2RLoader.exe"});
        if (!running.empty()) Throw("E_GAME_RUNNING", "Close " + running.front() + " first: the game is running.");

        Release rel;
        if (job.has_release && !job.release.up_to_date && !job.release.url.empty()) {
            rel = job.release;
        } else {
            Stage(p, "lookup");
            rel = Lookup(p, job.force);
        }
        if (rel.up_to_date) {
            PieceOutcome o;
            o.piece = p;
            o.status = JobStatus::AlreadyThere;
            o.version = InstalledVersion(ctx_.game, id);
            o.message = name + (o.version.empty() ? std::string() : " " + o.version) + " is already the newest version. Nothing was downloaded or run.";
            return o;
        }
        if (Cancelled()) Throw("E_CANCELLED", "cancelled before the download");

        HttpOptions ho = HttpOptionsForRelease(rel);
        ho.allow_http_host = testHost;
        HttpDownloader own(ho);
        Downloader* dl = ctx_.env->downloader ? ctx_.env->downloader : &own;
        std::unique_ptr<SetupBackend> realSetup;
        SetupBackend* setup = ctx_.env->setup;
        if (!setup) {
            realSetup = MakeSystemSetupBackend();
            setup = realSetup.get();
        }
        TappedSetup tapped(*setup, [&](const SetupCommand& cmd) {
            std::string h;
            if (rel.sha256.empty() && Sha256File(cmd.exe, &h)) Say(name + ": downloaded " + rel.asset_name + " without a published checksum; its SHA-256 is " + h);
            else if (!rel.sha256.empty()) Say(name + ": " + rel.asset_name + " matches the published SHA-256 " + rel.sha256 + " (" + rel.sha256_source + ")");
            Stage(p, "setup");
            Say(name + ": the Setup starts now" + (cmd.elevate ? " (Windows asks for administrator rights)" : "") + "; finish or close its wizard to go on");
        });
        ModInstallOptions mo;
        mo.cache_dir = ctx_.home / "downloads";
        mo.accept_unverified = job.accept_unverified;
        mo.run_setup = true;
        mo.test_http_host = testHost;
        mo.test_never_elevate = !testHost.empty() && EnvGet(ctx_.env->env, "VRML_TEST_NO_ELEVATE") == "1";
        mo.progress = [&](uint64_t done, uint64_t total) {
            Stage(p, "download", done, total);
            return !Cancelled(done);
        };
        Stage(p, "download");
        Say("download " + name + " " + rel.version + " from " + rel.url + (rel.size ? " (" + MiB(rel.size) + ")" : std::string()));
        const ModInstallResult r = InstallMod(ctx_.game, id, rel, *dl, *ctx_.env->probe, tapped, mo);
        Say("install " + id + " " + rel.version + ": " + (r.setup_ran ? "Setup ran, exit " + std::to_string(r.exit_code) : std::string("Setup not run")) +
            ", installed after: " + (r.installed ? "yes" : "no") + (r.code.empty() ? "" : ", " + r.code));
        PieceOutcome o;
        o.piece = p;
        o.code = r.code;
        o.setup_exit = r.setup_ran ? r.exit_code : -1;
        o.message = r.message;
        o.notes = r.notes;
        o.version = r.installed_version;
        o.sha256 = r.installer.sha256;
        o.sha_verified = r.installer.verified;
        o.downloaded = r.installer.downloaded;
        if (r.code.empty()) {
            o.status = JobStatus::Installed;
        } else {
            o.status = r.code == "E_CANCELLED" ? JobStatus::Cancelled : JobStatus::Failed;
            o.exit_code = ExitCodeForCode(r.code);
        }
        return o;
    }

    RunContext ctx_;
    RunHooks hooks_;
    int count_;
    int index_ = 0;
    Logger log_;
};

}  // namespace

// ------------------------------------------------------------------ the pieces

const char* PieceId(Piece p) {
    switch (p) {
        case Piece::Loader: return "d2rloader";
        case Piece::Authentic: return "d2r-vr";
        case Piece::Unleashed: return "d2r-pcvr";
    }
    return "";
}
const char* PieceName(Piece p) {
    switch (p) {
        case Piece::Loader: return "D2RLoader";
        case Piece::Authentic: return "D2R VR Authentic";
        case Piece::Unleashed: return "D2R VR Unleashed";
    }
    return "";
}
const char* PieceWord(Piece p) {
    switch (p) {
        case Piece::Loader: return "loader";
        case Piece::Authentic: return "authentic";
        case Piece::Unleashed: return "unleashed";
    }
    return "";
}
bool IsMod(Piece p) { return p != Piece::Loader; }

bool ParsePiece(const std::string& text, Piece* out) {
    const std::string t = Lower(text);
    for (int i = 0; i < kPieceCount; ++i) {
        const Piece p = static_cast<Piece>(i);
        if (t == PieceWord(p) || t == PieceId(p)) {
            *out = p;
            return true;
        }
    }
    return false;
}

bool ParsePieceList(const std::string& csv, std::vector<Piece>* out, std::string* problem) {
    std::vector<bool> want(kPieceCount, false);
    int n = 0;
    for (const std::string& word : Split(csv, ',')) {
        if (word.empty()) continue;
        Piece p;
        if (!ParsePiece(word, &p)) {
            if (problem) *problem = "\"" + word + "\" is none of loader, authentic, unleashed";
            return false;
        }
        want[static_cast<size_t>(p)] = true;
        ++n;
    }
    if (n == 0) {
        if (problem) *problem = "the list is empty: use loader, authentic and/or unleashed, separated by commas";
        return false;
    }
    out->clear();
    for (int i = 0; i < kPieceCount; ++i)
        if (want[static_cast<size_t>(i)]) out->push_back(static_cast<Piece>(i));
    return true;
}

bool PieceOfModId(const std::string& id, Piece* out) {
    if (id == "d2r-vr") *out = Piece::Authentic;
    else if (id == "d2r-pcvr") *out = Piece::Unleashed;
    else return false;
    return true;
}

// ------------------------------------------------------------------ sources and the loader's pin

ReleaseSources ReleaseSourcesFor(const std::map<std::string, std::string>& env) {
    ReleaseSources s;
    s.test_http_host = TestHost(env);
    if (!s.test_http_host.empty()) {                    // the test addresses count only together with the loopback switch
        std::string v = EnvGet(env, "VRML_URL_AUTHENTIC_API");
        if (!v.empty()) s.authentic_api = v;
        v = EnvGet(env, "VRML_URL_UNLEASHED_API");
        if (!v.empty()) s.unleashed_api = v;
    }
    return s;
}

HttpOptions HttpOptionsForQuestion(Piece mod, const std::map<std::string, std::string>& env) {
    HttpOptions o;
    o.allow_hosts = mod == Piece::Authentic ? std::vector<std::string>{"bodywalkvr.com"} : std::vector<std::string>{"api.github.com", "github.com", "*.githubusercontent.com"};
    o.allow_http_host = TestHost(env);
    o.max_bytes = 4ull << 20;
    return o;
}

LoaderPlan LoaderPlanFor(const std::map<std::string, std::string>& env, const std::string& pinFile) {
    LoaderPlan plan;
    plan.pin = D2RLoaderPin();
    plan.expected = LoaderExpectedFiles();
    const std::string testHost = TestHost(env);
    if (!pinFile.empty()) {
        if (testHost.empty()) Throw("E_USAGE", "--pin-file is a test switch and works only together with VRML_ALLOW_HTTP_HOST");
        PinFile pf = LoadPinFile(pinFile);
        plan.pin = pf.pin;
        plan.expected = pf.expected;
        plan.own_expected = true;
    }
    if (!testHost.empty()) {
        const std::string u = EnvGet(env, "VRML_URL_LOADER");
        if (!u.empty()) plan.pin.url = u;
    }
    return plan;
}

UpdateInfo CheckModRelease(Piece mod, const fs::path& game, const std::map<std::string, std::string>& env, HttpClient* http) {
    std::unique_ptr<HttpClient> holder;
    if (!http) {
        holder = MakeHttpClient(HttpOptionsForQuestion(mod, env));
        http = holder.get();
    }
    return CheckUpdate(*http, game, PieceId(mod), ReleaseSourcesFor(env));
}

LoaderStatus CheckLoaderAgainst(const fs::path& game, const LoaderPlan& plan) {
    if (!plan.own_expected) return CheckLoader(game, true);
    LoaderStatus st;
    st.present = IsFile(game / "D2RLoader.exe");
    if (!st.present) {
        st.notes.push_back("D2RLoader.exe is not in the game folder: the mods run only under D2RLoader");
        return st;
    }
    st.reviewed = FilesMatch(game, plan.expected, &st.notes);
    st.version = st.reviewed ? plan.pin.version : DetectLoaderVersion(game);
    return st;
}

// ------------------------------------------------------------------ running

const char* JobStatusWord(JobStatus s) {
    switch (s) {
        case JobStatus::Installed: return "installed";
        case JobStatus::AlreadyThere: return "already-there";
        case JobStatus::Failed: return "failed";
        case JobStatus::Cancelled: return "cancelled";
        case JobStatus::NotRun: return "not-run";
    }
    return "";
}

int ExitCodeForCode(const std::string& code) {
    if (code.empty()) return 0;
    if (code == "E_USAGE") return 2;
    if (code == "E_NOT_FOUND") return 3;
    if (code == "E_MANIFEST") return 4;
    if (code == "E_GAME_RUNNING") return 10;
    if (code == "E_PRECONDITION" || code == "E_HOOK" || code == "E_MOD_NOT_INSTALLED") return 11;
    if (code == "E_COLLISION" || code == "E_IN_USE" || code == "E_IDENTITY") return 12;
    if (code == "E_SCHEMA") return 13;
    if (code == "E_IO") return 20;
    if (code == "E_ROLLBACK") return 21;
    if (code == "E_JOURNAL") return 22;
    if (code == "E_HASH") return 23;
    if (code == "E_RUNTIME") return 24;
    if (code == "E_GAME") return 25;
    if (code == "E_ARCHIVE") return 26;
    if (code == "E_UNVERIFIED") return 27;
    if (code == "E_SETUP") return 28;
    if (code == "E_UNTRUSTED") return 29;
    if (code == "E_CANCELLED") return 30;
    if (code == "E_UAC_DECLINED") return 31;
    return 1;
}

std::vector<PieceJob> OrderedJobs(std::vector<PieceJob> jobs) {
    std::stable_sort(jobs.begin(), jobs.end(), [](const PieceJob& a, const PieceJob& b) { return static_cast<int>(a.piece) < static_cast<int>(b.piece); });
    jobs.erase(std::unique(jobs.begin(), jobs.end(), [](const PieceJob& a, const PieceJob& b) { return a.piece == b.piece; }), jobs.end());
    return jobs;
}

std::vector<PieceOutcome> RunInstallJobs(const RunContext& ctx, std::vector<PieceJob> jobsIn, const RunHooks& hooks) {
    const std::vector<PieceJob> jobs = OrderedJobs(std::move(jobsIn));
    std::vector<PieceOutcome> out;
    if (!ctx.env || !ctx.env->probe) {
        for (const PieceJob& j : jobs) {
            out.push_back(Plain(j.piece, JobStatus::Failed, "E_USAGE", "no process probe: the launcher cannot tell whether the game is closed"));
            if (hooks.piece_done) hooks.piece_done(out.back());
        }
        return out;
    }
    Runner runner(ctx, hooks, static_cast<int>(jobs.size()));
    bool stop = false;
    std::string stopWhy, stopCode;
    for (size_t i = 0; i < jobs.size(); ++i) {
        const PieceJob& job = jobs[i];
        PieceOutcome o;
        if (!stop && hooks.cancelled && hooks.cancelled()) {
            stop = true;
            stopCode = "E_CANCELLED";
            stopWhy = "Not started: the install was cancelled.";
        }
        if (stop) {
            o = Plain(job.piece, JobStatus::NotRun, stopCode, stopWhy);
        } else {
            runner.SetIndex(static_cast<int>(i));
            o = runner.Run(job);
            if (o.code == "E_CANCELLED" && o.status == JobStatus::Cancelled && o.setup_exit < 0) {      // a download that was cancelled: the rest is not started
                stop = true;
                stopCode = "E_CANCELLED";
                stopWhy = "Not started: the install was cancelled.";
            } else if (o.code == "E_GAME_RUNNING" || o.code == "E_GAME") {
                stop = true;
                stopWhy = "Not started: " + std::string(o.code == "E_GAME" ? "this is not a game folder." : "the game is running.");
            }
        }
        out.push_back(o);
        if (hooks.piece_done) hooks.piece_done(o);
    }
    return out;
}

int FirstFailureExit(const std::vector<PieceOutcome>& outcomes) {
    for (const PieceOutcome& o : outcomes)
        if (o.status == JobStatus::Failed || o.status == JobStatus::Cancelled || (o.status == JobStatus::NotRun && !o.code.empty())) return o.exit_code ? o.exit_code : 1;
    return 0;
}

std::string FormatOutcomes(const std::vector<PieceOutcome>& outcomes) {
    std::ostringstream s;
    for (const PieceOutcome& o : outcomes) {
        s << PieceId(o.piece) << "  " << JobStatusWord(o.status);
        if (!o.code.empty()) s << "  " << o.code;
        s << "\n";
        if (!o.message.empty()) s << "  " << o.message << "\n";
        for (const std::string& n : o.notes) s << "  note: " << n << "\n";
        if (!o.sha256.empty()) s << "  SHA-256 " << o.sha256 << (o.sha_verified ? "  (matches the published one)" : "  (no published checksum to compare with)") << "\n";
        if (!o.version.empty() && o.status == JobStatus::Installed) s << "  version " << o.version << "\n";
    }
    return s.str();
}

}  // namespace vrml
