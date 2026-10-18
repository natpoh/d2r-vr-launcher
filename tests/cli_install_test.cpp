// `releases` and `install ...` on the command line, in this process, with a fake network and a fake Setup runner (the real ones are exercised by
// tools/install_smoke.py against a local server).
#include <sstream>

#include "cli.h"
#include "fakes.h"
#include "json.h"
#include "sandbox.h"
#include "sha256.h"
#include "test.h"
#include "zip_writer.h"

using namespace vrml;

namespace {

struct Run {
    int code;
    std::string out, err;
};

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
    World() {
        env.probe = &sb.probe;
        env.downloader = &dl;
        env.http = &http;
        env.setup = &setup;
        env.env = {{"PATH", "x"}};
        dl.content = kInstaller;
        http.answers[kAuthApi + "0.0.0"] = Fixture("authentic_newer.json");
        http.answers[kAuthApi + "0.168.0"] = Fixture("authentic_uptodate.json");
        http.answers[kGhApi] = Fixture("github_releases.json");
        http.answers[kGhSha] = Fixture("D2R-PCVR-Setup-0.3.19.exe.sha256");
    }
    Run Cli(std::vector<std::string> args, bool withGame = true) {
        args.insert(args.begin(), {"--home", U(sb.home)});
        if (withGame) args.insert(args.begin(), {"--game", U(sb.game)});
        std::ostringstream out, err;
        const int code = RunCli(args, out, err, env);
        return {code, out.str(), err.str()};
    }
    // our server's answer with a SHA-256 for the pretend installer (so that it is "verified")
    void AuthenticSigned(const std::string& current = "0.0.0") {
        http.answers[kAuthApi + current] = R"({"version":"0.168","download_url":"https://bodywalkvr.com/static/uploads/versions/D2R_VR_Setup_v0.168.0.exe","sha256":")" +
                                           Sha256Hex(kInstaller) + R"(","size":)" + std::to_string(std::string(kInstaller).size()) + "}";
    }
};

}  // namespace

TEST(releases_lists_both_mods_with_what_is_installed_and_what_is_newer) {
    World w;
    tt::LeaveMod(w.sb.game, "d2r-vr", "0.167.0");
    w.http.answers[kAuthApi + "0.167.0"] = Fixture("authentic_newer.json");
    Run r = w.Cli({"releases", "--json"});
    CHECK_EQ(r.code, 0);
    const Json j = Json::Parse(r.out);
    const Json* mods = j.find("mods");
    REQUIRE(mods && mods->items().size() == 2);
    const Json& a = mods->items()[0];
    CHECK_EQ(a.get_string("id"), std::string("d2r-vr"));
    CHECK(a.get_bool("installed") && a.get_bool("update_available") && !a.get_bool("can_install") && a.get_bool("needs_admin"));
    CHECK_EQ(a.get_string("installed_version"), std::string("0.167.0"));
    CHECK_EQ(a.find("latest")->get_string("version"), std::string("0.168.0"));
    CHECK_EQ(a.find("latest")->get_string("sha256"), std::string());
    const Json& u = mods->items()[1];
    CHECK_EQ(u.get_string("id"), std::string("d2r-pcvr"));
    CHECK(!u.get_bool("installed") && u.get_bool("can_install") && !u.get_bool("needs_admin"));
    CHECK_EQ(u.find("latest")->get_string("version"), std::string("0.3.19"));
    CHECK_EQ(u.find("latest")->get_string("sha256"), std::string("bd37c8642e48d634c8823306776ccf5026b41f28728b492da541f7a669c3580c"));
    CHECK(u.find("latest")->get_bool("prerelease"));

    r = w.Cli({"releases"});
    CHECK_EQ(r.code, 0);
    CHECK(Has(r.out, "D2R VR Authentic"));
    CHECK(Has(r.out, "installed   0.167.0"));
    CHECK(Has(r.out, "UPDATE AVAILABLE"));
    CHECK(Has(r.out, "none published"));
    CHECK(Has(r.out, "D2R-PCVR-Setup-0.3.19.exe"));
    CHECK(Has(r.out, "can be installed"));
}

TEST(releases_without_a_game_folder_only_asks_the_servers) {
    World w;
    Run r = w.Cli({"releases"}, false);
    CHECK_EQ(r.code, 0);
    CHECK(!Has(r.out, "installed   "));
    CHECK(Has(r.out, "0.168.0"));
}

TEST(releases_reports_a_server_that_cannot_be_asked_and_fails_only_when_none_can) {
    World w;
    w.http.answers.erase(kGhApi);
    Run r = w.Cli({"releases"});
    CHECK_EQ(r.code, 0);
    CHECK(Has(r.out, "could not be found out"));
    CHECK(Has(r.out, "0.168.0"));
    w.http.answers.clear();
    r = w.Cli({"releases"});
    CHECK_EQ(r.code, 20);
    r = w.Cli({"releases", "--json"});
    CHECK_EQ(r.code, 20);
    CHECK(!Json::Parse(r.out).find("mods")->items()[0].get_bool("checked"));
}

TEST(a_dry_run_of_the_loader_says_what_it_would_do_and_does_nothing) {
    World w;
    const auto before = tt::Snapshot(w.sb.game);
    Run r = w.Cli({"install", "loader", "--dry-run"});
    CHECK_EQ(r.code, 0);
    CHECK(Has(r.out, "https://d2rloader.net/downloads/D2RLoader-1.3.1-beta.zip"));
    CHECK(Has(r.out, "f7bf0287398890b09bcc68b576327e727304bd88f7be80160f9676e8bd6871e2"));
    CHECK(Has(r.out, "9286c6b5bff7f1043658411dbea305faa9698b6a455e6441668fc6d02c22aad7"));
    CHECK(Has(r.out, "D2RLoader.exe  93021dad"));
    CHECK(Has(r.out, "needs admin"));
    CHECK(Has(r.out, "d2rloader.toml"));
    CHECK_EQ(w.dl.calls, 0);
    CHECK(tt::DiffText(before, tt::Snapshot(w.sb.game)).empty());
    r = w.Cli({"install", "loader", "--dry-run", "--json"});
    CHECK_EQ(r.code, 0);
    const Json j = Json::Parse(r.out);
    CHECK(j.get_bool("dry_run"));
    CHECK_EQ(j.find("archive_sha256_any_of")->items().size(), size_t(2));
    CHECK(!j.get_bool("needs_admin"));                  // the sandbox can be written
    // a dry run refuses what the real run would
    w.sb.probe.Start("D2R.exe");
    CHECK_EQ(w.Cli({"install", "loader", "--dry-run"}).code, 10);
    w.sb.probe.Stop("D2R.exe");
    fs::remove(w.sb.game / "D2R.exe");
    CHECK_EQ(w.Cli({"install", "loader", "--dry-run"}).code, 25);
}

TEST(the_loader_is_installed_from_a_test_pin_and_only_with_the_loopback_switch) {
    World w;
    const std::string zip = tt::MakeZip({{"D2RLoader.exe", "EXE"}, {"D2RCore.dll", "CORE"}, {"d2rloader.mpq", "MPQ"}, {"mods/", ""}});
    w.dl.content = zip;
    const fs::path pin = w.sb.root / "pin.json";
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
    tt::Write(pin, p.dump());
    fs::remove(w.sb.game / "D2RLoader.exe");

    Run r = w.Cli({"install", "loader", "--pin-file", U(pin)});
    CHECK_EQ(r.code, 2);                                // the switch is off: a test pin is refused
    CHECK(Has(r.err, "test switch"));
    CHECK_EQ(w.dl.calls, 0);

    w.env.env["VRML_ALLOW_HTTP_HOST"] = "127.0.0.1:5";
    r = w.Cli({"install", "loader", "--pin-file", U(pin)});
    CHECK_EQ(r.code, 0);
    CHECK(Has(r.out, "installed"));
    CHECK_EQ(tt::Read(w.sb.game / "D2RCore.dll"), std::string("CORE"));
    CHECK_EQ(w.dl.calls, 1);
    r = w.Cli({"install", "loader", "--pin-file", U(pin)});
    CHECK_EQ(r.code, 0);
    CHECK(Has(r.out, "already installed"));
    CHECK_EQ(w.dl.calls, 1);
    r = w.Cli({"install", "loader", "--pin-file", U(pin), "--json"});
    CHECK(Json::Parse(r.out).get_bool("already_installed"));

    // a repair keeps a backup and says so
    tt::Write(w.sb.game / "D2RCore.dll", "DAMAGED");
    r = w.Cli({"install", "loader", "--pin-file", U(pin)});
    CHECK_EQ(r.code, 0);
    CHECK(Has(r.out, "backup"));
    CHECK(IsDir(w.sb.home / "backups"));

    // the game running: refused before anything else, also when there would be nothing to do
    w.sb.probe.Start("D2RLoader.exe");
    CHECK_EQ(w.Cli({"install", "loader", "--pin-file", U(pin)}).code, 10);
    tt::Write(w.sb.game / "D2RCore.dll", "DAMAGED AGAIN");
    CHECK_EQ(w.Cli({"install", "loader", "--pin-file", U(pin)}).code, 10);
    CHECK_EQ(tt::Read(w.sb.game / "D2RCore.dll"), std::string("DAMAGED AGAIN"));
    w.sb.probe.Stop("D2RLoader.exe");
}

TEST(a_loader_archive_with_another_hash_is_refused_with_exit_23) {
    World w;
    const fs::path pin = w.sb.root / "pin.json";
    tt::Write(pin, R"({"version":"9","url":"http://127.0.0.1:5/L.zip","file":"L.zip","sha256":["0000000000000000000000000000000000000000000000000000000000000000"],"expected":[{"path":"D2RLoader.exe"}]})");
    w.env.env["VRML_ALLOW_HTTP_HOST"] = "127.0.0.1:5";
    fs::remove(w.sb.game / "D2RLoader.exe");
    const auto before = tt::Snapshot(w.sb.game);
    Run r = w.Cli({"install", "loader", "--pin-file", U(pin)});
    CHECK_EQ(r.code, 23);
    CHECK(Has(r.err, "E_HASH"));
    CHECK(tt::DiffText(before, tt::Snapshot(w.sb.game)).empty());
    // --no-run downloads and checks but installs nothing
    r = w.Cli({"install", "loader", "--no-run", "--pin-file", U(pin)});
    CHECK_EQ(r.code, 23);
}

TEST(a_dry_run_of_a_mod_shows_where_the_installer_comes_from_and_what_it_needs) {
    World w;
    const auto before = tt::Snapshot(w.sb.game);
    // Authentic: no published SHA-256 today
    Run r = w.Cli({"install", "authentic", "--dry-run"});
    CHECK_EQ(r.code, 27);
    CHECK(Has(r.out, "https://bodywalkvr.com/static/uploads/versions/D2R_VR_Setup_v0.168.0.exe"));
    CHECK(Has(r.out, "NONE PUBLISHED"));
    CHECK(Has(r.out, "--accept-unverified"));
    CHECK(Has(r.out, "YES"));
    CHECK(Has(r.out, "/DIR=\"" ));
    r = w.Cli({"install", "authentic", "--dry-run", "--accept-unverified"});
    CHECK_EQ(r.code, 0);
    CHECK(Has(r.out, "you accepted that"));
    // Unleashed: GitHub's digest
    r = w.Cli({"install", "unleashed", "--dry-run"});
    CHECK_EQ(r.code, 0);
    CHECK(Has(r.out, "bd37c8642e48d634c8823306776ccf5026b41f28728b492da541f7a669c3580c"));
    CHECK(Has(r.out, "digest + .sha256"));
    CHECK(Has(r.out, "no (this Setup installs for the current user)"));
    CHECK(Has(r.out, "pre-release"));
    r = w.Cli({"install", "unleashed", "--dry-run", "--json"});
    const Json j = Json::Parse(r.out);
    CHECK(j.get_bool("dry_run"));
    CHECK(!j.get_bool("needs_admin"));
    CHECK(!j.get_bool("would_be_refused"));
    CHECK_EQ(j.find("release")->get_string("version"), std::string("0.3.19"));
    CHECK_EQ(w.dl.calls, 0);
    CHECK(w.setup.runs.empty());
    CHECK(tt::DiffText(before, tt::Snapshot(w.sb.game)).empty());
}

TEST(a_mod_is_installed_through_its_setup_and_the_exit_code_follows_the_result) {
    World w;
    w.AuthenticSigned();
    w.setup.on_run = [&](const SetupCommand&) { tt::LeaveMod(w.sb.game, "d2r-vr", "0.168.0"); };
    Run r = w.Cli({"install", "authentic"});
    CHECK_EQ(r.code, 0);
    CHECK(Has(r.out, "is installed"));
    CHECK(Has(r.out, "matches the published one"));
    CHECK(Has(r.out, "started with administrator rights"));
    REQUIRE(w.setup.runs.size() == 1);
    CHECK(w.setup.runs[0].elevate);

    // the same again: installed and the server says nothing is newer
    w.http.answers[kAuthApi + "0.168.0"] = Fixture("authentic_uptodate.json");
    r = w.Cli({"install", "authentic"});
    CHECK_EQ(r.code, 0);
    CHECK(Has(r.out, "already the newest version"));
    CHECK_EQ(w.setup.runs.size(), size_t(1));
    // --force asks for the newest regardless
    w.AuthenticSigned("0.0.0");
    r = w.Cli({"install", "authentic", "--force"});
    CHECK_EQ(r.code, 0);
    CHECK_EQ(w.setup.runs.size(), size_t(2));
}

TEST(the_exit_codes_of_the_install_failures) {
    {
        World w;                                                   // no published SHA-256 and no --accept-unverified
        Run r = w.Cli({"install", "authentic"});
        CHECK_EQ(r.code, 27);
        CHECK(Has(r.err, "E_UNVERIFIED"));
        CHECK_EQ(w.dl.calls, 0);
        CHECK(w.setup.runs.empty());
    }
    {
        World w;                                                   // accepted: runs, shows the SHA-256 it computed
        w.setup.on_run = [&](const SetupCommand&) { tt::LeaveMod(w.sb.game, "d2r-vr", "0.168.0"); };
        Run r = w.Cli({"install", "authentic", "--accept-unverified"});
        CHECK_EQ(r.code, 0);
        CHECK(Has(r.out, Sha256Hex(kInstaller)));
        CHECK(Has(r.out, "no published checksum"));
    }
    {
        World w;                                                   // the download is not what was announced
        w.AuthenticSigned();
        w.dl.content = "tampered";
        Run r = w.Cli({"install", "authentic"});
        CHECK_EQ(r.code, 23);
        CHECK(w.setup.runs.empty());
    }
    {
        World w;                                                   // the player cancels the Setup
        w.AuthenticSigned();
        w.setup.exit_code = 5;
        Run r = w.Cli({"install", "authentic"});
        CHECK_EQ(r.code, 30);
        CHECK(Has(r.out, "cancelled"));
    }
    {
        World w;                                                   // the Setup fails
        w.AuthenticSigned();
        w.setup.exit_code = 4;
        CHECK_EQ(w.Cli({"install", "authentic"}).code, 28);
    }
    {
        World w;                                                   // the Setup ends well but the mod is not in this folder
        w.AuthenticSigned();
        Run r = w.Cli({"install", "authentic"});
        CHECK_EQ(r.code, 11);
        CHECK(Has(r.out, "NOT installed"));
    }
    {
        World w;                                                   // UAC declined
        w.AuthenticSigned();
        w.setup.status = SetupStatus::UacDeclined;
        Run r = w.Cli({"install", "authentic"});
        CHECK_EQ(r.code, 31);
        CHECK(Has(r.err, "E_UAC_DECLINED"));
    }
    {
        World w;                                                   // the download is cancelled
        w.AuthenticSigned();
        w.dl.progress = nullptr;
        class Cancelling : public tt::FakeDownloader {
        public:
            bool Fetch(const std::string&, const fs::path&, std::string* e) override { cancelled = true; *e = "cancelled"; return false; }
        } cancelling;
        w.env.downloader = &cancelling;
        CHECK_EQ(w.Cli({"install", "authentic"}).code, 30);
    }
    {
        World w;                                                   // the game runs
        w.AuthenticSigned();
        w.sb.probe.Start("D2R.exe");
        CHECK_EQ(w.Cli({"install", "authentic"}).code, 10);
        CHECK_EQ(w.Cli({"install", "unleashed"}).code, 10);
        CHECK_EQ(w.dl.calls, 0);
    }
    {
        World w;                                                   // a server that cannot be reached
        w.http.answers.clear();
        CHECK_EQ(w.Cli({"install", "authentic"}).code, 20);
    }
}

TEST(unleashed_is_installed_without_a_flag_because_github_publishes_the_digest) {
    World w;
    w.dl.content = kInstaller;
    // the digest in the sample is of the real 47 MB installer: serve a release whose digest is the pretend installer's
    Json list = Json::Parse(Fixture("github_releases.json"));
    for (Json& a : list.items()[0]["assets"].items()) {
        if (a.get_string("name") == "D2R-PCVR-Setup-0.3.19.exe") {
            a.set("digest", Json::String("sha256:" + Sha256Hex(kInstaller)));
            a.set("size", Json::Number(static_cast<double>(std::string(kInstaller).size())));
        }
    }
    w.http.answers[kGhApi] = list.dump();
    w.http.answers[kGhSha] = Sha256Hex(kInstaller) + "  D2R-PCVR-Setup-0.3.19.exe\n";
    w.setup.on_run = [&](const SetupCommand&) { tt::LeaveMod(w.sb.game, "d2r-pcvr", "0.3.19"); };
    Run r = w.Cli({"install", "unleashed", "--json"});
    CHECK_EQ(r.code, 0);
    const Json j = Json::Parse(r.out);
    CHECK(j.get_bool("ok") && j.get_bool("verified") && j.get_bool("installed") && !j.get_bool("elevated"));
    CHECK_EQ(j.get_string("installed_version"), std::string("0.3.19"));
    REQUIRE(w.setup.runs.size() == 1);
    CHECK(!w.setup.runs[0].elevate);
    CHECK(w.setup.runs[0].args.empty());
    // the digest and the .sha256 file disagree: refused with 23, nothing downloaded
    World w2;
    w2.http.answers[kGhSha] = std::string(64, 'a') + "  D2R-PCVR-Setup-0.3.19.exe\n";
    CHECK_EQ(w2.Cli({"install", "unleashed"}).code, 23);
    CHECK_EQ(w2.dl.calls, 0);
}

TEST(no_run_downloads_and_checks_a_mod_installer_and_stops) {
    World w;
    w.AuthenticSigned();
    Run r = w.Cli({"install", "authentic", "--no-run"});
    CHECK_EQ(r.code, 0);
    CHECK(Has(r.out, "the Setup was not run"));
    CHECK(w.setup.runs.empty());
    CHECK_EQ(w.dl.calls, 1);
}

TEST(usage_errors) {
    World w;
    CHECK_EQ(w.Cli({"install"}).code, 2);
    CHECK_EQ(w.Cli({"install", "bogus"}).code, 2);
    CHECK_EQ(w.Cli({"install", "loader", "--dry-run"}, false).code, 2);        // no game folder
    Run r = w.Cli({"install", "loader", "--dry-run"}, false);
    CHECK(Has(r.err, "--game"));
}

TEST(the_test_addresses_count_only_together_with_the_loopback_switch) {
    World w;
    w.env.env["VRML_URL_AUTHENTIC_API"] = "http://127.0.0.1:7/api/version";
    w.env.env["VRML_URL_UNLEASHED_API"] = "http://127.0.0.1:7/releases";
    w.Cli({"releases"}, false);
    for (const std::string& u : w.http.asked) CHECK(Has(u, "bodywalkvr.com") || Has(u, "github.com"));      // ignored without the switch
    w.http.asked.clear();
    w.env.env["VRML_ALLOW_HTTP_HOST"] = "127.0.0.1:7";
    w.Cli({"releases"}, false);
    bool authentic = false, unleashed = false;
    for (const std::string& u : w.http.asked) {
        authentic = authentic || StartsWith(u, "http://127.0.0.1:7/api/version?product=d2r_vr&v=0.0.0");
        unleashed = unleashed || u == "http://127.0.0.1:7/releases";
    }
    CHECK(authentic);
    CHECK(unleashed);
}

TEST_MAIN()
