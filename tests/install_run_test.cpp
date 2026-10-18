// The part the install view's Install button and `D2RVRLauncher --install-run` share: running D2RLoader, D2R VR Authentic and D2R VR Unleashed one after
// the other, in the right order, with one outcome each in words and codes. A fake network, a fake downloader and a fake Setup runner: no network, no
// process (the real ones are exercised by tools/install_view_smoke.py against a local web server).
#include "cli.h"
#include "fakes.h"
#include "install_run.h"
#include "json.h"
#include "sandbox.h"
#include "sha256.h"
#include "test.h"
#include "zip_writer.h"

using namespace vrml;

namespace {

const char* kInstaller = "MZ-pretend-installer-bytes";
const std::string kAuthApi = "https://bodywalkvr.com/api/version?product=d2r_vr&v=";
const std::string kGhApi = "https://api.github.com/repos/bearstonem/d2r-pcvr/releases?per_page=5";
const std::string kGhSha = "https://github.com/bearstonem/d2r-pcvr/releases/download/v0.3.19-windows-preview/D2R-PCVR-Setup-0.3.19.exe.sha256";

std::string Fixture(const std::string& rel) {
    std::string s;
    ReadFile(fs::path(VRML_SOURCE_DIR) / "tests" / "fixtures" / "releases" / rel, &s);
    return s;
}

bool Has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

struct World {
    tt::Sandbox sb;
    CliEnv env;
    tt::FakeDownloader dl;
    tt::FakeHttp http;
    tt::FakeSetup setup;
    std::vector<std::string> log;
    std::vector<std::string> stages;                       // "<index>/<count> <piece> <stage>" the first time each is seen
    std::vector<Piece> done_order;
    bool cancel = false;
    std::function<bool(uint64_t, uint64_t)> cancel_on_bytes;
    fs::path pin;

    World() {
        env.probe = &sb.probe;
        env.downloader = &dl;
        env.http = &http;
        env.setup = &setup;
        env.env = {{"PATH", "x"}};
        dl.content = kInstaller;
        http.answers[kAuthApi + "0.0.0"] = Fixture("authentic_newer.json");
        http.answers[kAuthApi + "0.168.0"] = Fixture("authentic_uptodate.json");
        // Unleashed: GitHub's digest is that of the pretend installer
        Json list = Json::Parse(Fixture("github_releases.json"));
        for (Json& a : list.items()[0]["assets"].items()) {
            if (a.get_string("name") == "D2R-PCVR-Setup-0.3.19.exe") {
                a.set("digest", Json::String("sha256:" + Sha256Hex(kInstaller)));
                a.set("size", Json::Number(static_cast<double>(std::string(kInstaller).size())));
            }
        }
        http.answers[kGhApi] = list.dump();
        http.answers[kGhSha] = Sha256Hex(kInstaller) + "  D2R-PCVR-Setup-0.3.19.exe\n";
        setup.on_run = [this](const SetupCommand& c) {
            const bool authentic = c.exe.filename().string().find("D2R_VR") != std::string::npos;
            tt::LeaveMod(sb.game, authentic ? "d2r-vr" : "d2r-pcvr", authentic ? "0.168.0" : "0.3.19");
        };
        fs::remove(sb.game / "D2RLoader.exe");                  // a fresh game has no loader
        fs::remove_all(sb.game / "d2rloader" / "config");
    }
    void AuthenticSigned() {
        http.answers[kAuthApi + "0.0.0"] = R"({"version":"0.168","download_url":"https://bodywalkvr.com/static/uploads/versions/D2R_VR_Setup_v0.168.0.exe","sha256":")" +
                                           Sha256Hex(kInstaller) + R"(","size":)" + std::to_string(std::string(kInstaller).size()) + "}";
    }
    // the loader: a test pin (honoured only with the loopback switch) and a synthetic archive
    void LoaderPin() {
        const std::string zip = tt::MakeZip({{"D2RLoader.exe", "EXE"}, {"D2RCore.dll", "CORE"}, {"d2rloader.mpq", "MPQ"}, {"mods/", ""}});
        loader_zip = zip;
        Json p = Json::Object();
        p.set("version", Json::String("9.9.9"));
        p.set("url", Json::String("http://127.0.0.1:5/L.zip"));
        p.set("file", Json::String("L.zip"));
        Json hs = Json::Array();
        hs.push(Json::String(Sha256Hex(zip)));
        p.set("sha256", hs);
        Json ex = Json::Array();
        for (const auto& f : std::vector<std::pair<const char*, const char*>>{{"D2RLoader.exe", "EXE"}, {"D2RCore.dll", "CORE"}, {"d2rloader.mpq", "MPQ"}}) {
            Json e = Json::Object();
            e.set("path", Json::String(f.first));
            Json h = Json::Array();
            h.push(Json::String(Sha256Hex(f.second)));
            e.set("sha256_any_of", h);
            ex.push(e);
        }
        p.set("expected", ex);
        pin = sb.root / "pin.json";
        tt::Write(pin, p.dump());
        env.env["VRML_ALLOW_HTTP_HOST"] = "127.0.0.1:5";
    }
    std::string loader_zip;

    RunHooks Hooks() {
        RunHooks h;
        h.log = [this](const std::string& l) { log.push_back(l); };
        h.progress = [this](int i, int n, Piece p, const std::string& stage, uint64_t, uint64_t) {
            const std::string s = std::to_string(i) + "/" + std::to_string(n) + " " + PieceWord(p) + " " + stage;
            if (stages.empty() || stages.back() != s) stages.push_back(s);
        };
        h.cancelled = [this] { return cancel; };
        h.piece_done = [this](const PieceOutcome& o) { done_order.push_back(o.piece); };
        return h;
    }
    std::vector<PieceOutcome> Run(std::vector<PieceJob> jobs) {
        RunContext ctx;
        ctx.game = sb.game;
        ctx.home = sb.home;
        ctx.env = &env;
        ctx.pin_file = pin.empty() ? "" : U(pin);
        return RunInstallJobs(ctx, std::move(jobs), Hooks());
    }
    bool LogHas(const std::string& part) const {
        for (const std::string& l : log)
            if (Has(l, part)) return true;
        return false;
    }
};

PieceJob Job(Piece p, bool accept = false, bool force = false) {
    PieceJob j;
    j.piece = p;
    j.accept_unverified = accept;
    j.force = force;
    return j;
}

}  // namespace

TEST(the_words_for_the_pieces_and_a_list_of_them) {
    Piece p = Piece::Loader;
    CHECK(ParsePiece("Authentic", &p) && p == Piece::Authentic);
    CHECK(ParsePiece("d2r-pcvr", &p) && p == Piece::Unleashed);
    CHECK(ParsePiece("D2RLoader", &p) && p == Piece::Loader);
    CHECK(!ParsePiece("bear", &p));
    CHECK_EQ(std::string(PieceId(Piece::Authentic)), std::string("d2r-vr"));
    CHECK_EQ(std::string(PieceName(Piece::Unleashed)), std::string("D2R VR Unleashed"));
    CHECK(IsMod(Piece::Authentic) && !IsMod(Piece::Loader));
    std::vector<Piece> v;
    std::string why;
    CHECK(ParsePieceList("unleashed,loader,Unleashed", &v, &why));
    REQUIRE(v.size() == 2);
    CHECK(v[0] == Piece::Loader && v[1] == Piece::Unleashed);                           // in install order, once each
    CHECK(!ParsePieceList("loader,bogus", &v, &why));
    CHECK(Has(why, "bogus"));
    CHECK(!ParsePieceList(",", &v, &why));
    CHECK(!ParsePieceList("", &v, &why));
    CHECK(PieceOfModId("d2r-vr", &p) && p == Piece::Authentic);
    CHECK(!PieceOfModId("d2rloader", &p));
}

TEST(the_exit_code_of_every_result_code) {
    CHECK_EQ(ExitCodeForCode(""), 0);
    CHECK_EQ(ExitCodeForCode("E_UNVERIFIED"), 27);
    CHECK_EQ(ExitCodeForCode("E_HASH"), 23);
    CHECK_EQ(ExitCodeForCode("E_CANCELLED"), 30);
    CHECK_EQ(ExitCodeForCode("E_UAC_DECLINED"), 31);
    CHECK_EQ(ExitCodeForCode("E_SETUP"), 28);
    CHECK_EQ(ExitCodeForCode("E_MOD_NOT_INSTALLED"), 11);
    CHECK_EQ(ExitCodeForCode("E_GAME_RUNNING"), 10);
    CHECK_EQ(ExitCodeForCode("E_GAME"), 25);
    CHECK_EQ(ExitCodeForCode("E_IO"), 20);
    CHECK_EQ(ExitCodeForCode("E_UNTRUSTED"), 29);
    CHECK_EQ(ExitCodeForCode("E_ARCHIVE"), 26);
    CHECK_EQ(ExitCodeForCode("E_NOT_THERE_AT_ALL"), 1);
    // it is the same table the command line uses
    try {
        Throw("E_UNVERIFIED", "x");
    } catch (const Error& e) {
        CHECK_EQ(e.exit_code, ExitCodeForCode(e.code));
    }
}

TEST(the_pieces_run_loader_first_then_the_mods_whatever_the_order_they_were_given) {
    World w;
    w.LoaderPin();
    w.dl.content = w.loader_zip;
    w.AuthenticSigned();
    // the loader's archive and the Setups come from the same fake downloader: tell them apart by what is asked for
    class ByUrl : public tt::FakeDownloader {
    public:
        std::string zip, setup;
        bool Fetch(const std::string& url, const fs::path& dest, std::string* e) override {
            content = url.find("L.zip") != std::string::npos ? zip : setup;
            return FakeDownloader::Fetch(url, dest, e);
        }
    } dl;
    dl.zip = w.loader_zip;
    dl.setup = kInstaller;
    w.env.downloader = &dl;
    const std::vector<PieceOutcome> out = w.Run({Job(Piece::Unleashed), Job(Piece::Authentic), Job(Piece::Loader)});
    REQUIRE(out.size() == 3);
    CHECK(out[0].piece == Piece::Loader && out[1].piece == Piece::Authentic && out[2].piece == Piece::Unleashed);
    for (const PieceOutcome& o : out) {
        CHECK(o.status == JobStatus::Installed);
        CHECK(o.code.empty());
        CHECK_EQ(o.exit_code, 0);
    }
    CHECK(w.done_order.size() == 3 && w.done_order[0] == Piece::Loader && w.done_order[2] == Piece::Unleashed);
    CHECK_EQ(FirstFailureExit(out), 0);
    // the loader is in the game folder; both Setups ran, Authentic's elevated with /DIR, Unleashed's per user with nothing
    CHECK_EQ(tt::Read(w.sb.game / "D2RCore.dll"), std::string("CORE"));
    REQUIRE(w.setup.runs.size() == 2);
    CHECK(w.setup.runs[0].exe.filename().string().find("D2R_VR") != std::string::npos);
    CHECK(w.setup.runs[0].elevate);
    REQUIRE(w.setup.runs[0].args.size() == 1);
    CHECK(Has(w.setup.runs[0].args[0], "/DIR=\""));
    CHECK(!w.setup.runs[1].elevate && w.setup.runs[1].args.empty());
    CHECK_EQ(out[1].version, std::string("0.168.0"));
    CHECK_EQ(out[2].version, std::string("0.3.19"));
    // the progress said which step of how many, in that order
    CHECK(w.stages.front() == "0/3 loader download");
    bool sawSecond = false, sawThird = false, setupSaid = false;
    for (const std::string& s : w.stages) {
        sawSecond = sawSecond || Has(s, "1/3 authentic");
        sawThird = sawThird || Has(s, "2/3 unleashed");
        setupSaid = setupSaid || s == "1/3 authentic setup";
    }
    CHECK(sawSecond && sawThird && setupSaid);
    // the Setup's start is in the log, with the way the installer was proven
    CHECK(w.LogHas("the Setup starts now (Windows asks for administrator rights)"));
    CHECK(w.LogHas("matches the published SHA-256"));
    // a second run: the loader is there; the mods are the newest
    w.http.answers[kAuthApi + "0.168.0"] = Fixture("authentic_uptodate.json");
    const std::vector<PieceOutcome> again = w.Run({Job(Piece::Loader), Job(Piece::Authentic), Job(Piece::Unleashed)});
    CHECK(again[0].status == JobStatus::AlreadyThere);
    CHECK(again[1].status == JobStatus::AlreadyThere);
    CHECK(again[2].status == JobStatus::AlreadyThere);
    CHECK_EQ(w.setup.runs.size(), size_t(2));
}

TEST(a_release_without_a_published_checksum_is_not_downloaded_unless_the_player_said_yes) {
    World w;                                                                // Authentic: no sha256 in the answer; Unleashed: GitHub's digest
    std::vector<PieceOutcome> out = w.Run({Job(Piece::Authentic), Job(Piece::Unleashed)});
    REQUIRE(out.size() == 2);
    CHECK(out[0].status == JobStatus::Failed);
    CHECK_EQ(out[0].code, std::string("E_UNVERIFIED"));
    CHECK_EQ(out[0].exit_code, 27);
    CHECK(Has(out[0].message, "no SHA-256 is published"));
    CHECK(out[1].status == JobStatus::Installed);                           // the other piece goes on
    CHECK_EQ(FirstFailureExit(out), 27);
    CHECK_EQ(w.dl.calls, 1);                                                // only Unleashed's installer was fetched
    CHECK_EQ(w.setup.runs.size(), size_t(1));

    World w2;
    out = w2.Run({Job(Piece::Authentic, true)});
    REQUIRE(out.size() == 1);
    CHECK(out[0].status == JobStatus::Installed);
    CHECK(!out[0].sha_verified);
    CHECK_EQ(out[0].sha256, Sha256Hex(kInstaller));                         // shown to the player
    CHECK(w2.LogHas("without a published checksum; its SHA-256 is " + Sha256Hex(kInstaller)));
}

TEST(a_wrong_hash_deletes_the_installer_and_the_setup_never_runs) {
    World w;
    w.AuthenticSigned();
    w.dl.content = "tampered bytes";
    const std::vector<PieceOutcome> out = w.Run({Job(Piece::Authentic)});
    REQUIRE(out.size() == 1);
    CHECK(out[0].status == JobStatus::Failed);
    CHECK_EQ(out[0].code, std::string("E_HASH"));
    CHECK_EQ(out[0].exit_code, 23);
    CHECK(w.setup.runs.empty());
    CHECK(!IsFile(w.sb.home / "downloads" / "installers" / "d2r-vr" / "0.168.0" / "D2R_VR_Setup_v0.168.0.exe"));
}

TEST(a_mod_that_is_current_is_left_alone_unless_it_is_forced) {
    World w;
    w.AuthenticSigned();
    tt::LeaveMod(w.sb.game, "d2r-vr", "0.168.0");
    std::vector<PieceOutcome> out = w.Run({Job(Piece::Authentic)});
    REQUIRE(out.size() == 1);
    CHECK(out[0].status == JobStatus::AlreadyThere);
    CHECK(Has(out[0].message, "already the newest version"));
    CHECK_EQ(out[0].version, std::string("0.168.0"));
    CHECK_EQ(w.dl.calls, 0);
    out = w.Run({Job(Piece::Authentic, false, true)});                       // force: ask for the newest regardless (a repair)
    CHECK(out[0].status == JobStatus::Installed);
    CHECK_EQ(w.setup.runs.size(), size_t(1));
}

TEST(the_release_the_player_saw_is_used_without_asking_the_server_again) {
    World w;
    w.AuthenticSigned();
    const Release seen = ParseAuthenticAnswer(w.http.answers[kAuthApi + "0.0.0"], "");
    PieceJob j = Job(Piece::Authentic);
    j.has_release = true;
    j.release = seen;
    const std::vector<PieceOutcome> out = w.Run({j});
    CHECK(out[0].status == JobStatus::Installed);
    CHECK(w.http.asked.empty());
    // a release the server called up to date is not usable: the job asks again
    World w2;
    w2.AuthenticSigned();
    PieceJob k = Job(Piece::Authentic);
    k.has_release = true;
    k.release = ParseAuthenticAnswer(Fixture("authentic_uptodate.json"), "0.168.0");
    CHECK(k.release.up_to_date);
    CHECK(w2.Run({k})[0].status == JobStatus::Installed);
    CHECK(!w2.http.asked.empty());
}

TEST(cancelling_stops_at_the_next_safe_point_and_the_rest_is_not_started) {
    {
        World w;                                                              // the player cancelled before anything started
        w.cancel = true;
        const std::vector<PieceOutcome> out = w.Run({Job(Piece::Authentic, true), Job(Piece::Unleashed)});
        REQUIRE(out.size() == 2);
        CHECK(out[0].status == JobStatus::NotRun && out[1].status == JobStatus::NotRun);
        CHECK_EQ(out[0].code, std::string("E_CANCELLED"));
        CHECK_EQ(FirstFailureExit(out), 30);
        CHECK_EQ(w.dl.calls, 0);
        CHECK(w.setup.runs.empty());
    }
    {
        World w;                                                              // a download that is cancelled half way
        class Cancelling : public tt::FakeDownloader {
        public:
            bool* flag = nullptr;
            bool Fetch(const std::string& url, const fs::path& dest, std::string* e) override {
                *flag = true;                                                 // the player presses Cancel while the bytes come
                return FakeDownloader::Fetch(url, dest, e);
            }
        } dl;
        dl.content = kInstaller;
        dl.flag = &w.cancel;
        w.env.downloader = &dl;
        const std::vector<PieceOutcome> out = w.Run({Job(Piece::Authentic, true), Job(Piece::Unleashed)});
        REQUIRE(out.size() == 2);
        CHECK(out[0].status == JobStatus::Cancelled);
        CHECK_EQ(out[0].code, std::string("E_CANCELLED"));
        CHECK_EQ(out[0].exit_code, 30);
        CHECK(out[1].status == JobStatus::NotRun);
        CHECK(w.setup.runs.empty());
        CHECK_EQ(FirstFailureExit(out), 30);
    }
}

TEST(a_cancel_after_so_many_bytes_is_a_test_switch_that_needs_the_loopback_switch) {
    World w;
    w.env.env["VRML_TEST_CANCEL_AFTER"] = "1";
    std::vector<PieceOutcome> out = w.Run({Job(Piece::Authentic, true)});
    CHECK(out[0].status == JobStatus::Installed);                            // ignored: the switch is off
    World w2;
    w2.env.env["VRML_ALLOW_HTTP_HOST"] = "127.0.0.1:5";
    w2.env.env["VRML_TEST_CANCEL_AFTER"] = "1";
    out = w2.Run({Job(Piece::Authentic, true)});
    CHECK(out[0].status == JobStatus::Cancelled);
    CHECK(w2.setup.runs.empty());
}

TEST(a_running_game_stops_everything_before_anything_is_downloaded) {
    World w;
    w.LoaderPin();
    w.sb.probe.Start("D2R.exe");
    const std::vector<PieceOutcome> out = w.Run({Job(Piece::Loader), Job(Piece::Authentic, true), Job(Piece::Unleashed)});
    REQUIRE(out.size() == 3);
    CHECK(out[0].status == JobStatus::Failed);
    CHECK_EQ(out[0].code, std::string("E_GAME_RUNNING"));
    CHECK_EQ(out[0].exit_code, 10);
    CHECK(out[1].status == JobStatus::NotRun && out[2].status == JobStatus::NotRun);
    CHECK(out[1].code.empty());                                              // the first one has the code
    CHECK(Has(out[1].message, "the game is running"));
    CHECK_EQ(FirstFailureExit(out), 10);
    CHECK_EQ(w.dl.calls, 0);
    CHECK(w.setup.runs.empty());
    CHECK(!IsFile(w.sb.game / "D2RLoader.exe"));
}

TEST(a_folder_that_is_not_a_game_is_refused) {
    World w;
    fs::remove(w.sb.game / "D2R.exe");
    const std::vector<PieceOutcome> out = w.Run({Job(Piece::Authentic, true), Job(Piece::Unleashed)});
    CHECK_EQ(out[0].code, std::string("E_GAME"));
    CHECK_EQ(out[0].exit_code, 25);
    CHECK(out[1].status == JobStatus::NotRun);
    CHECK_EQ(w.dl.calls, 0);
}

TEST(what_the_setup_does_decides_the_outcome) {
    {
        World w;                                                              // UAC declined
        w.AuthenticSigned();
        w.setup.status = SetupStatus::UacDeclined;
        const std::vector<PieceOutcome> out = w.Run({Job(Piece::Authentic)});
        CHECK(out[0].status == JobStatus::Failed);
        CHECK_EQ(out[0].code, std::string("E_UAC_DECLINED"));
        CHECK_EQ(out[0].exit_code, 31);
    }
    {
        World w;                                                              // the Setup fails (Inno exit 4)
        w.AuthenticSigned();
        w.setup.exit_code = 4;
        w.setup.on_run = nullptr;
        const std::vector<PieceOutcome> out = w.Run({Job(Piece::Authentic)});
        CHECK(out[0].status == JobStatus::Failed);
        CHECK_EQ(out[0].code, std::string("E_SETUP"));
        CHECK_EQ(out[0].exit_code, 28);
        CHECK_EQ(out[0].setup_exit, 4);
    }
    {
        World w;                                                              // the player cancels the first Setup's wizard (5): the next piece still runs
        w.AuthenticSigned();
        int n = 0;
        w.setup.on_run = [&](const SetupCommand& c) {
            if (n++ == 0) {
                w.setup.exit_code = 5;
                return;
            }
            w.setup.exit_code = 0;
            tt::LeaveMod(w.sb.game, "d2r-pcvr", "0.3.19");
            (void)c;
        };
        const std::vector<PieceOutcome> out = w.Run({Job(Piece::Authentic), Job(Piece::Unleashed)});
        CHECK(out[0].status == JobStatus::Cancelled);
        CHECK_EQ(out[0].code, std::string("E_CANCELLED"));
        CHECK_EQ(out[0].setup_exit, 5);
        CHECK(out[1].status == JobStatus::Installed);
        CHECK_EQ(FirstFailureExit(out), 30);
    }
    {
        World w;                                                              // the Setup ends well but the mod is not in this folder
        w.AuthenticSigned();
        w.setup.on_run = nullptr;
        const std::vector<PieceOutcome> out = w.Run({Job(Piece::Authentic)});
        CHECK(out[0].status == JobStatus::Failed);
        CHECK_EQ(out[0].code, std::string("E_MOD_NOT_INSTALLED"));
        CHECK_EQ(out[0].exit_code, 11);
    }
    {
        World w;                                                              // the server cannot be asked
        w.http.answers.clear();
        const std::vector<PieceOutcome> out = w.Run({Job(Piece::Authentic, true)});
        CHECK(out[0].status == JobStatus::Failed);
        CHECK_EQ(out[0].code, std::string("E_IO"));
        CHECK_EQ(out[0].exit_code, 20);
        CHECK_EQ(w.dl.calls, 0);
    }
}

TEST(the_test_switches_for_elevation_work_only_with_the_loopback_switch) {
    World w;
    w.AuthenticSigned();
    w.env.env["VRML_TEST_NO_ELEVATE"] = "1";
    w.Run({Job(Piece::Authentic)});
    REQUIRE(w.setup.runs.size() == 1);
    CHECK(w.setup.runs[0].elevate);                                           // the switch is off: Authentic's Setup is still elevated
    World w2;
    w2.AuthenticSigned();
    w2.env.env["VRML_TEST_NO_ELEVATE"] = "1";
    w2.env.env["VRML_ALLOW_HTTP_HOST"] = "127.0.0.1:5";
    w2.Run({Job(Piece::Authentic)});
    REQUIRE(w2.setup.runs.size() == 1);
    CHECK(!w2.setup.runs[0].elevate);
}

TEST(the_loader_job_installs_checks_and_reports_what_it_did) {
    World w;
    w.LoaderPin();
    w.dl.content = w.loader_zip;
    std::vector<PieceOutcome> out = w.Run({Job(Piece::Loader)});
    REQUIRE(out.size() == 1);
    CHECK(out[0].status == JobStatus::Installed);
    CHECK(out[0].downloaded);
    CHECK_EQ(out[0].version, std::string("9.9.9"));
    CHECK(Has(out[0].message, "D2RLoader 9.9.9 is installed"));
    CHECK_EQ(tt::Read(w.sb.game / "D2RLoader.exe"), std::string("EXE"));
    // the window's own check, against the same plan
    const LoaderPlan plan = LoaderPlanFor(w.env.env, U(w.pin));
    const LoaderStatus st = CheckLoaderAgainst(w.sb.game, plan);
    CHECK(st.present && st.reviewed);
    CHECK_EQ(st.version, std::string("9.9.9"));
    out = w.Run({Job(Piece::Loader)});
    CHECK(out[0].status == JobStatus::AlreadyThere);
    CHECK_EQ(w.dl.calls, 1);
    tt::Write(w.sb.game / "D2RCore.dll", "DAMAGED");
    CHECK(!CheckLoaderAgainst(w.sb.game, plan).reviewed);
    out = w.Run({Job(Piece::Loader)});
    CHECK(out[0].status == JobStatus::Installed);
    CHECK(!out[0].notes.empty());                                             // it says where the backup of the replaced file is
    CHECK(IsDir(w.sb.home / "backups"));
}

TEST(a_loader_archive_with_the_wrong_hash_changes_nothing) {
    World w;
    w.LoaderPin();
    w.dl.content = tt::MakeZip({{"D2RLoader.exe", "EVIL"}, {"D2RCore.dll", "CORE"}, {"d2rloader.mpq", "MPQ"}});   // not the archive that was pinned
    const auto before = tt::Snapshot(w.sb.game);
    const std::vector<PieceOutcome> out = w.Run({Job(Piece::Loader)});
    CHECK(out[0].status == JobStatus::Failed);
    CHECK_EQ(out[0].code, std::string("E_HASH"));
    CHECK_EQ(out[0].exit_code, 23);
    CHECK(tt::DiffText(before, tt::Snapshot(w.sb.game)).empty());
}

TEST(a_test_pin_is_refused_without_the_loopback_switch) {
    World w;
    w.LoaderPin();
    w.env.env.erase("VRML_ALLOW_HTTP_HOST");
    CHECK_ERROR("E_USAGE", (LoaderPlanFor(w.env.env, U(w.pin))));
    const std::vector<PieceOutcome> out = w.Run({Job(Piece::Loader)});
    CHECK(out[0].status == JobStatus::Failed);
    CHECK_EQ(out[0].code, std::string("E_USAGE"));
    // the built-in plan: the pinned archive of d2rloader.net and the three reviewed files
    const LoaderPlan plan = LoaderPlanFor({{"PATH", "x"}}, "");
    CHECK(Has(plan.pin.url, "https://d2rloader.net/downloads/"));
    CHECK_EQ(plan.expected.size(), size_t(3));
    CHECK(!plan.own_expected);
}

TEST(the_question_about_the_newest_release_never_throws) {
    World w;
    UpdateInfo u = CheckModRelease(Piece::Authentic, w.sb.game, w.env.env, &w.http);
    CHECK(u.checked);
    CHECK_EQ(u.latest.version, std::string("0.168.0"));
    CHECK(u.can_install);
    u = CheckModRelease(Piece::Unleashed, w.sb.game, w.env.env, &w.http);
    CHECK(u.checked);
    CHECK_EQ(u.latest.version, std::string("0.3.19"));
    CHECK(!u.latest.sha256.empty());
    w.http.answers.clear();
    u = CheckModRelease(Piece::Authentic, w.sb.game, w.env.env, &w.http);
    CHECK(!u.checked);
    CHECK(Has(u.error, "bodywalkvr.com"));
    tt::LeaveMod(w.sb.game, "d2r-vr", "0.167.0");
    w.http.answers[kAuthApi + "0.167.0"] = Fixture("authentic_newer.json");
    u = CheckModRelease(Piece::Authentic, w.sb.game, w.env.env, &w.http);
    CHECK(u.installed && u.update_available);
    CHECK_EQ(u.installed_version, std::string("0.167.0"));
}

TEST(the_headless_text_has_a_line_per_piece_and_the_words) {
    World w;
    const std::vector<PieceOutcome> out = w.Run({Job(Piece::Authentic), Job(Piece::Unleashed)});
    const std::string text = FormatOutcomes(out);
    CHECK(Has(text, "d2r-vr  failed  E_UNVERIFIED\n"));
    CHECK(Has(text, "d2r-pcvr  installed\n"));
    CHECK(Has(text, "version 0.3.19"));
    CHECK(Has(text, "matches the published one"));
}

TEST_MAIN()
