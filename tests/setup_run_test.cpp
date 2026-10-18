// Installing a mod through its own Setup: the installer's download and checks, the Setup run (a fake backend), and what InstallMod reports
// afterwards. No real Setup is ever started here.
#include "fakes.h"
#include "json.h"
#include "mods.h"
#include "sandbox.h"
#include "setup_run.h"
#include "sha256.h"
#include "test.h"

using namespace vrml;

namespace {

const char* kInstaller = "MZ-pretend-installer-bytes";

using tt::FakeSetup;
struct FakeDownloader : tt::FakeDownloader {          // the pretend installer is what it serves by default
    FakeDownloader() { content = kInstaller; }
};

Release MakeRelease(const std::string& id, const std::string& version = "0.168.0") {
    Release r;
    r.id = id;
    r.version = version;
    r.url = id == "d2r-vr" ? "https://bodywalkvr.com/static/uploads/versions/D2R_VR_Setup_v" + version + ".exe"
                           : "https://github.com/bearstonem/d2r-pcvr/releases/download/v" + version + "-windows-preview/D2R-PCVR-Setup-" + version + ".exe";
    r.asset_name = id == "d2r-vr" ? "D2R_VR_Setup_v" + version + ".exe" : "D2R-PCVR-Setup-" + version + ".exe";
    r.source = id == "d2r-vr" ? "bodywalkvr.com" : "github.com/bearstonem/d2r-pcvr";
    r.sha256 = Sha256Hex(kInstaller);
    r.sha256_source = "published";
    return r;
}

using tt::LeaveMod;

ModInstallOptions Options(tt::Sandbox& sb) {
    ModInstallOptions o;
    o.cache_dir = sb.home / "downloads";
    o.test_http_host = "";
    return o;
}

}  // namespace

TEST(the_dir_argument_is_quoted_for_inno) {
    CHECK_EQ(InnoDirArg("C:/Games/Diablo II Resurrected"), std::string("/DIR=\"C:\\Games\\Diablo II Resurrected\""));
    CHECK_EQ(InnoDirArg("C:\\Games\\Diablo II Resurrected\\"), std::string("/DIR=\"C:\\Games\\Diablo II Resurrected\""));    // a trailing backslash would swallow the quote
    CHECK_EQ(InnoDirArg("C:\\Games\\\\\\"), std::string("/DIR=\"C:\\Games\""));
    CHECK_EQ(InnoDirArg("D:\\"), std::string("/DIR=\"D:\\\""));                                                             // a drive root keeps its one
    CHECK_EQ(InnoDirArg(fs::u8path("D:/Spiele/\xC3\xBC")), std::string("/DIR=\"D:\\Spiele\\\xC3\xBC\""));
    CHECK_ERROR("E_PRECONDITION", InnoDirArg("C:\\Games\\\"quoted\""));
    CHECK_ERROR("E_PRECONDITION", InnoDirArg("C:\\Games\\x\ty"));
    CHECK_ERROR("E_PRECONDITION", InnoDirArg(""));
}

TEST(setup_exit_codes_are_put_in_words) {
    CHECK_EQ(DescribeSetupExit(0), std::string());
    CHECK(SetupExitIsCancel(2));
    CHECK(SetupExitIsCancel(5));
    CHECK(!SetupExitIsCancel(0));
    CHECK(!SetupExitIsCancel(1));
    CHECK(!SetupExitIsCancel(4));
    CHECK(DescribeSetupExit(5).find("cancelled") != std::string::npos);
    CHECK(DescribeSetupExit(4).find("error") != std::string::npos);
    CHECK(DescribeSetupExit(99).find("99") != std::string::npos);
}

TEST(a_setup_is_run_through_the_backend_and_its_exit_code_comes_back) {
    tt::Sandbox sb;
    tt::Write(sb.root / "setup.exe", "x");
    FakeSetup fb;
    fb.exit_code = 3;
    CHECK_EQ(RunSetup(sb.root / "setup.exe", true, {"/DIR=\"C:\\x\""}, fb), 3);
    REQUIRE(fb.runs.size() == 1);
    CHECK(fb.runs[0].elevate);
    CHECK_EQ(fb.runs[0].args.size(), size_t(1));
    CHECK(fb.runs[0].exe == sb.root / "setup.exe");
    fb.exit_code = 0;
    CHECK_EQ(RunSetup(sb.root / "setup.exe", false, {}, fb), 0);
    CHECK(!fb.runs[1].elevate);
    fb.status = SetupStatus::UacDeclined;
    CHECK_ERROR("E_UAC_DECLINED", RunSetup(sb.root / "setup.exe", true, {}, fb));
    fb.status = SetupStatus::FailedToStart;
    CHECK_ERROR("E_IO", RunSetup(sb.root / "setup.exe", true, {}, fb));
    CHECK_ERROR("E_NOT_FOUND", RunSetup(sb.root / "missing.exe", true, {}, fb));
}

TEST(a_published_hash_is_enforced_and_a_good_file_is_reused) {
    tt::Sandbox sb;
    FakeDownloader dl;
    const Release r = MakeRelease("d2r-pcvr");
    VerifiedInstaller v = DownloadAndVerify(r, sb.home / "downloads", dl, {}, false, "");
    CHECK(v.verified);
    CHECK(v.downloaded);
    CHECK_EQ(v.sha256, Sha256Hex(kInstaller));
    CHECK_EQ(v.size, uint64_t(std::string(kInstaller).size()));
    CHECK(IsFile(v.path));
    CHECK_EQ(U(v.path.filename()), std::string("D2R-PCVR-Setup-0.168.0.exe"));
    CHECK_EQ(dl.last_url, r.url);
    VerifiedInstaller again = DownloadAndVerify(r, sb.home / "downloads", dl, {}, false, "");
    CHECK(again.verified);
    CHECK(!again.downloaded);
    CHECK_EQ(dl.calls, 1);
    tt::Write(v.path, "tampered on disk");                      // a cached file that no longer matches is not trusted
    again = DownloadAndVerify(r, sb.home / "downloads", dl, {}, false, "");
    CHECK(again.downloaded);
    CHECK_EQ(dl.calls, 2);
    CHECK_EQ(tt::Read(again.path), std::string(kInstaller));
}

TEST(a_wrong_file_is_deleted_and_never_run) {
    tt::Sandbox sb;
    FakeDownloader dl;
    dl.content = "something else";
    const Release r = MakeRelease("d2r-vr");
    try {
        DownloadAndVerify(r, sb.home / "downloads", dl, {}, false, "");
        CHECK(!"should have thrown");
    } catch (const Error& e) {
        CHECK_EQ(e.code, std::string("E_HASH"));
        CHECK(std::string(e.what()).find(Sha256Hex("something else")) != std::string::npos);     // the player sees both hashes
        CHECK(std::string(e.what()).find(r.sha256) != std::string::npos);
    }
    CHECK(!IsFile(sb.home / "downloads" / "installers" / "d2r-vr" / "0.168.0" / r.asset_name));
    // the size, when published, is enforced too
    FakeDownloader good;
    Release sized = MakeRelease("d2r-vr");
    sized.size = 5;
    CHECK_ERROR("E_HASH", DownloadAndVerify(sized, sb.home / "downloads", good, {}, false, ""));
    CHECK(!IsFile(sb.home / "downloads" / "installers" / "d2r-vr" / "0.168.0" / sized.asset_name));
    sized.size = std::string(kInstaller).size();
    CHECK(DownloadAndVerify(sized, sb.home / "downloads", good, {}, false, "").verified);
}

TEST(without_a_published_hash_nothing_is_downloaded_unless_the_caller_accepts) {
    tt::Sandbox sb;
    FakeDownloader dl;
    Release r = MakeRelease("d2r-vr");
    r.sha256.clear();
    r.sha256_source.clear();
    CHECK_ERROR("E_UNVERIFIED", DownloadAndVerify(r, sb.home / "downloads", dl, {}, false, ""));
    CHECK_EQ(dl.calls, 0);
    VerifiedInstaller v = DownloadAndVerify(r, sb.home / "downloads", dl, {}, true, "");
    CHECK(!v.verified);
    CHECK_EQ(v.sha256, Sha256Hex(kInstaller));                   // computed, to be shown
    CHECK_EQ(dl.calls, 1);
    // an unverified file is never reused
    DownloadAndVerify(r, sb.home / "downloads", dl, {}, true, "");
    CHECK_EQ(dl.calls, 2);
    // but a published size is still checked
    r.size = 3;
    CHECK_ERROR("E_HASH", DownloadAndVerify(r, sb.home / "downloads", dl, {}, true, ""));
}

TEST(an_address_that_is_not_ours_is_refused_before_any_download) {
    tt::Sandbox sb;
    FakeDownloader dl;
    Release r = MakeRelease("d2r-vr");
    r.url = "https://evil.example/D2R_VR_Setup.exe";
    CHECK_ERROR("E_UNTRUSTED", DownloadAndVerify(r, sb.home / "downloads", dl, {}, true, ""));
    r = MakeRelease("d2r-vr");
    r.sha256 = "not a hash";
    CHECK_ERROR("E_UNTRUSTED", DownloadAndVerify(r, sb.home / "downloads", dl, {}, true, ""));
    r = MakeRelease("d2r-vr");
    r.asset_name = "..\\..\\evil.exe";
    CHECK_ERROR("E_UNTRUSTED", DownloadAndVerify(r, sb.home / "downloads", dl, {}, true, ""));
    CHECK_EQ(dl.calls, 0);
    CHECK(!Exists(sb.home / "downloads"));
    // the loopback test host is a valid address only when it is named
    r = MakeRelease("d2r-vr");
    r.url = "http://127.0.0.1:9/D2R_VR_Setup_v0.168.0.exe";
    CHECK_ERROR("E_UNTRUSTED", DownloadAndVerify(r, sb.home / "downloads", dl, {}, true, ""));
    CHECK(DownloadAndVerify(r, sb.home / "downloads", dl, {}, true, "127.0.0.1:9").verified);
}

TEST(a_failed_or_cancelled_download_leaves_no_file) {
    tt::Sandbox sb;
    FakeDownloader dl;
    const Release r = MakeRelease("d2r-pcvr");
    dl.fail = true;
    CHECK_ERROR("E_IO", DownloadAndVerify(r, sb.home / "downloads", dl, {}, false, ""));
    dl.fail = false;
    int polls = 0;
    CHECK_ERROR("E_CANCELLED", DownloadAndVerify(r, sb.home / "downloads", dl, [&](uint64_t, uint64_t) { ++polls; return false; }, false, ""));
    CHECK_EQ(polls, 1);
    CHECK(!IsFile(sb.home / "downloads" / "installers" / "d2r-pcvr" / "0.168.0" / r.asset_name));
    CHECK(!dl.progress);                                          // the progress callback does not outlive the call
}

TEST(older_installers_are_cleared_from_the_cache_after_a_new_download) {
    tt::Sandbox sb;
    FakeDownloader dl;
    DownloadAndVerify(MakeRelease("d2r-pcvr", "0.3.18"), sb.home / "downloads", dl, {}, false, "");
    DownloadAndVerify(MakeRelease("d2r-vr", "0.167.0"), sb.home / "downloads", dl, {}, false, "");
    DownloadAndVerify(MakeRelease("d2r-pcvr", "0.3.19"), sb.home / "downloads", dl, {}, false, "");
    CHECK(!Exists(sb.home / "downloads" / "installers" / "d2r-pcvr" / "0.3.18"));
    CHECK(IsFile(sb.home / "downloads" / "installers" / "d2r-pcvr" / "0.3.19" / "D2R-PCVR-Setup-0.3.19.exe"));
    CHECK(IsFile(sb.home / "downloads" / "installers" / "d2r-vr" / "0.167.0" / "D2R_VR_Setup_v0.167.0.exe"));    // the other mod's is not touched
}

TEST(authentic_is_installed_elevated_with_the_game_folder) {
    tt::Sandbox sb;
    FakeDownloader dl;
    FakeSetup fb;
    fb.on_run = [&](const SetupCommand&) { LeaveMod(sb.game, "d2r-vr", "0.168.0"); };
    const ModInstallResult r = InstallMod(sb.game, "d2r-vr", MakeRelease("d2r-vr"), dl, sb.probe, fb, Options(sb));
    REQUIRE(fb.runs.size() == 1);
    CHECK(fb.runs[0].elevate);
    REQUIRE(fb.runs[0].args.size() == 1);
    CHECK_EQ(fb.runs[0].args[0], InnoDirArg(sb.game));
    CHECK(fb.runs[0].args[0].find("/DIR=\"") == 0);
    CHECK_EQ(U(fb.runs[0].exe.filename()), std::string("D2R_VR_Setup_v0.168.0.exe"));
    CHECK(r.setup_ran && r.elevated);
    CHECK_EQ(r.exit_code, 0);
    CHECK(r.code.empty());
    CHECK(r.installed && !r.was_installed);
    CHECK_EQ(r.installed_version, std::string("0.168.0"));
    CHECK(r.installer.verified);
    CHECK(r.message.find("D2R VR Authentic") != std::string::npos);
}

TEST(unleashed_is_installed_per_user_without_arguments) {
    tt::Sandbox sb;
    FakeDownloader dl;
    FakeSetup fb;
    fb.on_run = [&](const SetupCommand&) { LeaveMod(sb.game, "d2r-pcvr", "0.3.19"); };
    LeaveMod(sb.game, "d2r-pcvr", "0.3.18");                       // an update: the older one is there
    const ModInstallResult r = InstallMod(sb.game, "d2r-pcvr", MakeRelease("d2r-pcvr", "0.3.19"), dl, sb.probe, fb, Options(sb));
    REQUIRE(fb.runs.size() == 1);
    CHECK(!fb.runs[0].elevate);
    CHECK(fb.runs[0].args.empty());
    CHECK(r.was_installed);
    CHECK_EQ(r.previous_version, std::string("0.3.18"));
    CHECK_EQ(r.installed_version, std::string("0.3.19"));
    CHECK(r.code.empty());
}

TEST(a_cancelled_setup_is_reported_with_the_exit_code_and_the_installed_state) {
    tt::Sandbox sb;
    FakeDownloader dl;
    FakeSetup fb;
    fb.exit_code = 5;                                              // the player pressed Cancel
    ModInstallResult r = InstallMod(sb.game, "d2r-vr", MakeRelease("d2r-vr"), dl, sb.probe, fb, Options(sb));
    CHECK_EQ(r.code, std::string("E_CANCELLED"));
    CHECK_EQ(r.exit_code, 5);
    CHECK(!r.installed);
    CHECK(r.message.find("cancelled") != std::string::npos);
    CHECK(r.message.find("nothing was installed") != std::string::npos);
    // cancelled while an older version is there: it stays
    LeaveMod(sb.game, "d2r-vr", "0.167.0");
    r = InstallMod(sb.game, "d2r-vr", MakeRelease("d2r-vr"), dl, sb.probe, fb, Options(sb));
    CHECK_EQ(r.code, std::string("E_CANCELLED"));
    CHECK(r.installed && r.was_installed);
    CHECK_EQ(r.installed_version, std::string("0.167.0"));
}

TEST(a_failed_setup_and_a_setup_that_installed_elsewhere_are_told_apart) {
    tt::Sandbox sb;
    FakeDownloader dl;
    FakeSetup fb;
    fb.exit_code = 4;
    ModInstallResult r = InstallMod(sb.game, "d2r-pcvr", MakeRelease("d2r-pcvr"), dl, sb.probe, fb, Options(sb));
    CHECK_EQ(r.code, std::string("E_SETUP"));
    CHECK_EQ(r.exit_code, 4);
    CHECK(r.message.find("4") != std::string::npos);
    fb.exit_code = 0;                                              // finished, but nothing arrived in THIS folder
    r = InstallMod(sb.game, "d2r-pcvr", MakeRelease("d2r-pcvr"), dl, sb.probe, fb, Options(sb));
    CHECK_EQ(r.code, std::string("E_MOD_NOT_INSTALLED"));
    CHECK(!r.installed);
    CHECK(r.message.find("another folder") != std::string::npos);
}

TEST(a_declined_uac_prompt_is_its_own_error_and_installs_nothing) {
    tt::Sandbox sb;
    FakeDownloader dl;
    FakeSetup fb;
    fb.status = SetupStatus::UacDeclined;
    CHECK_ERROR("E_UAC_DECLINED", InstallMod(sb.game, "d2r-vr", MakeRelease("d2r-vr"), dl, sb.probe, fb, Options(sb)));
    CHECK(!DiscoverMods(sb.game).front().installed);
}

TEST(the_game_running_or_a_bad_request_stops_before_the_download) {
    tt::Sandbox sb;
    FakeDownloader dl;
    FakeSetup fb;
    sb.probe.Start("D2RLoader.exe");
    CHECK_ERROR("E_GAME_RUNNING", InstallMod(sb.game, "d2r-vr", MakeRelease("d2r-vr"), dl, sb.probe, fb, Options(sb)));
    sb.probe.Stop("D2RLoader.exe");
    CHECK_ERROR("E_USAGE", InstallMod(sb.game, "nonsense", MakeRelease("d2r-vr"), dl, sb.probe, fb, Options(sb)));
    CHECK_ERROR("E_USAGE", InstallMod(sb.game, "d2r-vr", MakeRelease("d2r-pcvr"), dl, sb.probe, fb, Options(sb)));       // the other mod's release
    Release same = MakeRelease("d2r-vr");
    same.up_to_date = true;
    CHECK_ERROR("E_PRECONDITION", InstallMod(sb.game, "d2r-vr", same, dl, sb.probe, fb, Options(sb)));
    fs::remove(sb.game / "D2R.exe");
    CHECK_ERROR("E_GAME", InstallMod(sb.game, "d2r-vr", MakeRelease("d2r-vr"), dl, sb.probe, fb, Options(sb)));
    CHECK_EQ(dl.calls, 0);
    CHECK(fb.runs.empty());
}

TEST(a_bad_download_never_reaches_the_setup) {
    tt::Sandbox sb;
    FakeDownloader dl;
    dl.content = "not the installer";
    FakeSetup fb;
    CHECK_ERROR("E_HASH", InstallMod(sb.game, "d2r-vr", MakeRelease("d2r-vr"), dl, sb.probe, fb, Options(sb)));
    CHECK(fb.runs.empty());
    Release unverified = MakeRelease("d2r-vr");
    unverified.sha256.clear();
    CHECK_ERROR("E_UNVERIFIED", InstallMod(sb.game, "d2r-vr", unverified, dl, sb.probe, fb, Options(sb)));
    CHECK(fb.runs.empty());
}

TEST(an_unverified_installer_runs_only_when_accepted_and_the_hash_is_reported) {
    tt::Sandbox sb;
    FakeDownloader dl;
    FakeSetup fb;
    fb.on_run = [&](const SetupCommand&) { LeaveMod(sb.game, "d2r-vr", "0.168.0"); };
    Release r = MakeRelease("d2r-vr");
    r.sha256.clear();
    ModInstallOptions o = Options(sb);
    o.accept_unverified = true;
    const ModInstallResult res = InstallMod(sb.game, "d2r-vr", r, dl, sb.probe, fb, o);
    CHECK(!res.installer.verified);
    CHECK_EQ(res.installer.sha256, Sha256Hex(kInstaller));
    CHECK(res.installed);
    bool noted = false;
    for (const std::string& n : res.notes) noted = noted || n.find(Sha256Hex(kInstaller)) != std::string::npos;
    CHECK(noted);
}

TEST(no_run_downloads_and_checks_only) {
    tt::Sandbox sb;
    FakeDownloader dl;
    FakeSetup fb;
    ModInstallOptions o = Options(sb);
    o.run_setup = false;
    const ModInstallResult r = InstallMod(sb.game, "d2r-pcvr", MakeRelease("d2r-pcvr"), dl, sb.probe, fb, o);
    CHECK(fb.runs.empty());
    CHECK(!r.setup_ran);
    CHECK(r.installer.verified);
    CHECK(IsFile(r.installer.path));
    CHECK(r.message.find("not run") != std::string::npos);
    CHECK(r.code.empty());
}

TEST(the_test_switch_keeps_a_stand_in_setup_from_asking_for_administrator_rights) {
    tt::Sandbox sb;
    FakeDownloader dl;
    FakeSetup fb;
    ModInstallOptions o = Options(sb);
    o.test_never_elevate = true;
    InstallMod(sb.game, "d2r-vr", MakeRelease("d2r-vr"), dl, sb.probe, fb, o);
    REQUIRE(fb.runs.size() == 1);
    CHECK(!fb.runs[0].elevate);
}

TEST(a_missing_d2rloader_is_pointed_out_after_the_setup) {
    tt::Sandbox sb;
    fs::remove(sb.game / "D2RLoader.exe");
    FakeDownloader dl;
    FakeSetup fb;
    fb.on_run = [&](const SetupCommand&) { LeaveMod(sb.game, "d2r-vr", "0.168.0"); };
    const ModInstallResult r = InstallMod(sb.game, "d2r-vr", MakeRelease("d2r-vr"), dl, sb.probe, fb, Options(sb));
    bool pointed = false;
    for (const std::string& n : r.notes) pointed = pointed || n.find("D2RLoader.exe is not in the game folder") != std::string::npos;
    CHECK(pointed);
}

TEST_MAIN()
