// Installing D2RLoader: a fake downloader serving a synthetic archive whose three files have known hashes, a fake game folder. The
// questions: nothing written when anything is wrong, the player's files kept, backups, a rollback when a move fails, idempotence.
#include <fstream>

#include "deps.h"
#include "fakes.h"
#include "install.h"
#include "mods.h"
#include "sandbox.h"
#include "sha256.h"
#include "test.h"
#include "zip_writer.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

using namespace vrml;

namespace {

using tt::FakeDownloader;

const char* kExe = "LOADER-EXE-BYTES";
const char* kCore = "CORE-DLL-BYTES";
const char* kMpq = "MPQ-DATA-BYTES";

struct Kit {
    tt::Sandbox sb;
    FakeDownloader dl;
    Pin pin;
    std::vector<ExpectedFile> expected;
    std::string zipBytes;
    fs::path backups;

    explicit Kit(std::vector<tt::ZEntry> entries = {}) {
        if (entries.empty())
            entries = {{"D2RLoader.exe", kExe}, {"D2RCore.dll", kCore}, {"d2rloader.mpq", kMpq}, {"d2rloader/config/d2rloader.toml", "[from]\narchive = true\n"}, {"mods/", ""}};
        SetZip(entries);
        expected = {{"D2RLoader.exe", {Sha256Hex(kExe)}}, {"D2RCore.dll", {Sha256Hex(kCore)}}, {"d2rloader.mpq", {Sha256Hex(kMpq)}}};
        backups = sb.home / "backups";
        fs::remove(sb.game / "D2RLoader.exe");                      // the sandbox's fake loader: a fresh game has none
        fs::remove_all(sb.game / "d2rloader" / "config");
    }
    void SetZip(const std::vector<tt::ZEntry>& entries) {
        zipBytes = tt::MakeZip(entries);
        dl.content = zipBytes;
        pin.id = "d2rloader";
        pin.version = "9.9.9-test";
        pin.url = "https://example.invalid/L.zip";
        pin.file = "L.zip";
        pin.sha256 = {Sha256Hex(zipBytes)};
    }
    InstallResult Run(const InstallProgress& p = {}) {
        DepsCache cache(sb.home, &dl, &sb.log);
        return InstallD2RLoader(sb.game, pin, cache, sb.probe, p, backups, &expected, &sb.log);
    }
    int StagingLeft() const {
        int n = 0;
        for (const auto& e : fs::directory_iterator(sb.game))
            if (StartsWith(U(e.path().filename()), ".vrml-install-")) ++n;
        return n;
    }
};

}  // namespace

TEST(the_expected_files_are_the_three_reviewed_ones) {
    const std::vector<ExpectedFile> e = LoaderExpectedFiles();
    REQUIRE(e.size() == 3);
    CHECK_EQ(e[0].rel, std::string("D2RLoader.exe"));
    CHECK_EQ(e[1].rel, std::string("D2RCore.dll"));
    CHECK_EQ(e[2].rel, std::string("d2rloader.mpq"));
    CHECK_EQ(e[0].sha256_any_of[0], std::string("93021dad48533bcfca8a95c69a0cc00cdb9008a3ce6ceba1d04c7aca71985a10"));
    CHECK_EQ(e[1].sha256_any_of[0], std::string("2a868d013d2e0830bd2d9e04b918b19e46a73cf726c833e70d089b948fdeb5a2"));
    CHECK_EQ(e[2].sha256_any_of[0], std::string("1f02aa4e3179cb8a8417125160aa3ef18003ea81ee1a4f54e0a76fd3f22d933b"));
}

TEST(a_fresh_install_writes_the_files_and_makes_the_mods_folder) {
    Kit k;
    const auto before = tt::Snapshot(k.sb.game);
    std::vector<std::string> stages;
    InstallResult r = k.Run([&](const std::string& s, uint64_t, uint64_t) { if (stages.empty() || stages.back() != s) stages.push_back(s); return true; });
    CHECK(!r.already_installed);
    CHECK(r.downloaded);
    CHECK_EQ(k.dl.calls, 1);
    CHECK_EQ(tt::Read(k.sb.game / "D2RLoader.exe"), std::string(kExe));
    CHECK_EQ(tt::Read(k.sb.game / "D2RCore.dll"), std::string(kCore));
    CHECK_EQ(tt::Read(k.sb.game / "d2rloader.mpq"), std::string(kMpq));
    CHECK(IsDir(k.sb.game / "mods"));
    CHECK_EQ(tt::Read(k.sb.game / "d2rloader" / "config" / "d2rloader.toml"), std::string("[from]\narchive = true\n"));   // absent before: written
    CHECK_EQ(r.written.size(), size_t(4));
    CHECK(r.backed_up.empty());
    CHECK(r.backup_dir.empty());
    CHECK_EQ(k.StagingLeft(), 0);
    CHECK(FilesMatch(k.sb.game, k.expected));
    CHECK(stages.size() == 4 && stages[0] == "download" && stages[1] == "unpack" && stages[2] == "verify" && stages[3] == "install");
    // the neutral files of the game were not touched
    const auto after = tt::Snapshot(k.sb.game);
    for (const auto& kv : before) CHECK_EQ(after.at(kv.first), kv.second);
}

TEST(the_players_d2rloader_toml_is_never_overwritten) {
    Kit k;
    tt::Write(k.sb.game / "d2rloader" / "config" / "d2rloader.toml", "[mine]\ndefault_mod = \"D2R-VR\"\n");
    tt::Write(k.sb.game / "D2RLoader.exe", "an older loader");                          // so that a replace happens too
    InstallResult r = k.Run();
    CHECK_EQ(tt::Read(k.sb.game / "d2rloader" / "config" / "d2rloader.toml"), std::string("[mine]\ndefault_mod = \"D2R-VR\"\n"));
    CHECK_EQ(r.kept.size(), size_t(1));
    CHECK_EQ(r.kept[0], std::string("d2rloader/config/d2rloader.toml"));
    CHECK_EQ(tt::Read(k.sb.game / "D2RLoader.exe"), std::string(kExe));
}

TEST(a_repair_backs_up_what_it_replaces) {
    Kit k;
    tt::Write(k.sb.game / "D2RLoader.exe", "OLD-LOADER");
    tt::Write(k.sb.game / "D2RCore.dll", kCore);                                          // already right: not replaced, not backed up
    InstallResult r = k.Run();
    CHECK_EQ(r.backed_up.size(), size_t(1));
    CHECK_EQ(r.backed_up[0], std::string("D2RLoader.exe"));
    REQUIRE(!r.backup_dir.empty());
    CHECK(StartsWith(U(r.backup_dir.filename()), "d2rloader-"));
    CHECK(r.backup_dir.parent_path() == k.backups);
    CHECK_EQ(tt::Read(r.backup_dir / "D2RLoader.exe"), std::string("OLD-LOADER"));
    CHECK_EQ(tt::Read(k.sb.game / "D2RLoader.exe"), std::string(kExe));
    CHECK_EQ(r.unchanged.size(), size_t(1));
    CHECK_EQ(r.unchanged[0], std::string("D2RCore.dll"));
    CHECK(!Exists(r.backup_dir / "D2RCore.dll"));
}

TEST(a_second_run_is_a_no_op_that_downloads_nothing) {
    Kit k;
    k.Run();
    CHECK_EQ(k.dl.calls, 1);
    const auto snap = tt::Snapshot(k.sb.game);
    InstallResult r = k.Run();
    CHECK(r.already_installed);
    CHECK(r.written.empty());
    CHECK_EQ(k.dl.calls, 1);
    CHECK(tt::DiffText(snap, tt::Snapshot(k.sb.game)).empty());
    CHECK(!Exists(k.backups));
}

TEST(the_cached_archive_is_reused_for_a_repair) {
    Kit k;
    k.Run();
    tt::Write(k.sb.game / "D2RCore.dll", "damaged");
    InstallResult r = k.Run();
    CHECK(!r.already_installed);
    CHECK(!r.downloaded);                       // the verified archive was in the cache
    CHECK_EQ(k.dl.calls, 1);
    CHECK_EQ(tt::Read(k.sb.game / "D2RCore.dll"), std::string(kCore));
    CHECK_EQ(r.backed_up.size(), size_t(1));
}

TEST(an_unpacked_file_that_is_not_the_reviewed_one_writes_nothing) {
    // the archive hash is accepted (it is the pin we made) but D2RCore.dll inside is not what was reviewed
    Kit k({{"D2RLoader.exe", kExe}, {"D2RCore.dll", "TAMPERED"}, {"d2rloader.mpq", kMpq}});
    tt::Write(k.sb.game / "d2rloader.mpq", "mine, old");
    const auto before = tt::Snapshot(k.sb.game);
    CHECK_ERROR("E_HASH", k.Run());
    CHECK(tt::DiffText(before, tt::Snapshot(k.sb.game)).empty());
    CHECK_EQ(k.StagingLeft(), 0);
    CHECK(!Exists(k.backups));
    // a file that is simply missing from the archive
    Kit k2({{"D2RLoader.exe", kExe}, {"d2rloader.mpq", kMpq}});
    CHECK_ERROR("E_HASH", k2.Run());
}

TEST(a_download_with_another_hash_writes_nothing) {
    Kit k;
    k.dl.content = "not the archive";
    const auto before = tt::Snapshot(k.sb.game);
    CHECK_ERROR("E_HASH", k.Run());
    CHECK(tt::DiffText(before, tt::Snapshot(k.sb.game)).empty());
    CHECK_EQ(k.StagingLeft(), 0);
}

TEST(no_network_is_an_io_error_and_a_cancel_is_its_own) {
    Kit k;
    k.dl.fail = true;
    CHECK_ERROR("E_IO", k.Run());
    k.dl.fail = false;
    int polled = 0;
    CHECK_ERROR("E_CANCELLED", k.Run([&](const std::string& stage, uint64_t, uint64_t) { ++polled; return stage != "download"; }));    // the download says stop
    CHECK(polled > 0);
    CHECK(!Exists(k.sb.game / "D2RLoader.exe"));
    CHECK(!IsFile(DepsCache(k.sb.home, nullptr, nullptr).PathOf(k.pin)));          // no half archive in the cache
    // a cancel between steps, before the first file moves
    CHECK_ERROR("E_CANCELLED", k.Run([&](const std::string& stage, uint64_t, uint64_t) { return stage != "verify"; }));
    CHECK(!Exists(k.sb.game / "D2RLoader.exe"));
    CHECK_EQ(k.StagingLeft(), 0);
}

TEST(a_running_game_or_a_missing_game_is_refused_first) {
    Kit k;
    k.sb.probe.Start("D2R.exe");
    CHECK_ERROR("E_GAME_RUNNING", k.Run());
    k.sb.probe.Stop("D2R.exe");
    k.sb.probe.Start("D2RLoader.exe");
    CHECK_ERROR("E_GAME_RUNNING", k.Run());
    CHECK_EQ(k.dl.calls, 0);
    k.sb.probe.Stop("D2RLoader.exe");
    fs::remove(k.sb.game / "D2R.exe");
    CHECK_ERROR("E_GAME", k.Run());
    CHECK_EQ(k.dl.calls, 0);
}

TEST(an_archive_with_names_it_may_not_hold_is_refused_whole) {
    for (const char* bad : {"evil.dll", "plugins/x.dll", "../escape.txt", "d2rloader.exe/inner", "C:/Windows/x.dll", "mods", "D2RCORE.DLL/"}) {
        Kit k({{"D2RLoader.exe", kExe}, {"D2RCore.dll", kCore}, {"d2rloader.mpq", kMpq}, {bad, "x"}});
        const auto before = tt::Snapshot(k.sb.game);
        CHECK_ERROR("E_ARCHIVE", k.Run());
        if (!tt::DiffText(before, tt::Snapshot(k.sb.game)).empty()) std::cerr << "    the game folder changed for " << bad << "\n";
        CHECK(tt::DiffText(before, tt::Snapshot(k.sb.game)).empty());
        CHECK_EQ(k.StagingLeft(), 0);
    }
    // the top-level names that are allowed, in any case
    Kit ok({{"d2rloader.EXE", kExe}, {"d2rcore.DLL", kCore}, {"D2RLOADER.MPQ", kMpq}, {"D2RLoader/Config/x.toml", "t"}, {"Mods/", ""}});
    ok.expected = {{"d2rloader.exe", {Sha256Hex(kExe)}}, {"d2rcore.dll", {Sha256Hex(kCore)}}, {"d2rloader.mpq", {Sha256Hex(kMpq)}}};
    ok.Run();
    CHECK(FilesMatch(ok.sb.game, ok.expected));
}

TEST(files_of_the_player_under_mods_and_config_are_kept) {
    Kit k({{"D2RLoader.exe", kExe}, {"D2RCore.dll", kCore}, {"d2rloader.mpq", kMpq}, {"mods/Sample/readme.txt", "from the archive"}, {"d2rloader/config/extra.toml", "from the archive"}});
    tt::Write(k.sb.game / "mods" / "Sample" / "readme.txt", "my mod");
    tt::Write(k.sb.game / "d2rloader" / "config" / "extra.toml", "my config");
    InstallResult r = k.Run();
    CHECK_EQ(tt::Read(k.sb.game / "mods" / "Sample" / "readme.txt"), std::string("my mod"));
    CHECK_EQ(tt::Read(k.sb.game / "d2rloader" / "config" / "extra.toml"), std::string("my config"));
    CHECK_EQ(r.kept.size(), size_t(2));
}

TEST(other_mods_and_plugins_in_the_game_folder_are_left_alone) {
    Kit k;
    tt::Write(k.sb.game / "mods" / "D2R-VR" / "D2R-VR.mpq" / "modinfo.json", "{}");
    tt::Write(k.sb.game / "d2rloader" / "plugins" / "d2rl-other.dll", "plugin");
    const auto before = tt::Snapshot(k.sb.game);
    k.Run();
    const auto after = tt::Snapshot(k.sb.game);
    for (const auto& kv : before) CHECK_EQ(after.at(kv.first), kv.second);
    CHECK(DiscoverMods(k.sb.game).front().installed);
}

#ifdef _WIN32
TEST(when_a_move_fails_half_way_the_game_folder_is_put_back) {
    // order in the archive: D2RCore.dll (replaces an old one), d2rloader.mpq (new), D2RLoader.exe (old file held open by someone: cannot be replaced)
    Kit k({{"D2RCore.dll", kCore}, {"d2rloader.mpq", kMpq}, {"D2RLoader.exe", kExe}});
    tt::Write(k.sb.game / "D2RCore.dll", "OLD-CORE");
    tt::Write(k.sb.game / "D2RLoader.exe", "OLD-EXE");
    const auto before = tt::Snapshot(k.sb.game);
    HANDLE h = CreateFileW((k.sb.game / "D2RLoader.exe").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);   // no FILE_SHARE_DELETE
    REQUIRE(h != INVALID_HANDLE_VALUE);
    bool threw = false;
    std::string what;
    try {
        k.Run();
    } catch (const Error& e) {
        threw = true;
        what = e.what();
        CHECK_EQ(e.code, std::string("E_IO"));
    }
    CloseHandle(h);
    REQUIRE(threw);
    CHECK(what.find("put back as it was") != std::string::npos);
    CHECK(tt::DiffText(before, tt::Snapshot(k.sb.game)).empty());
    CHECK_EQ(k.StagingLeft(), 0);
    // and once the file is free the same install goes through
    k.Run();
    CHECK(FilesMatch(k.sb.game, k.expected));
}
#endif

TEST(io_errors_are_put_in_words) {
#ifdef _WIN32
    const std::string denied = DescribeIoError(std::error_code(5, std::system_category()), "C:/Program Files/Game");
    CHECK(denied.find("administrator rights are needed") != std::string::npos);
    CHECK(denied.find("C:/Program Files/Game") != std::string::npos);
    CHECK(DescribeIoError(std::error_code(32, std::system_category()), "x").find("in use by another program") != std::string::npos);
    CHECK(DescribeIoError(std::error_code(112, std::system_category()), "x").find("disk is full") != std::string::npos);
    CHECK(DescribeIoError(std::error_code(2, std::system_category()), "x").find("x") != std::string::npos);
#endif
}

TEST(the_writable_check_creates_nothing_that_stays) {
    tt::Sandbox sb;
    CHECK(FolderWritable(sb.game));
    CHECK(!FolderWritable(sb.game / "no" / "such" / "folder"));
    int leftovers = 0;
    for (const auto& e : fs::directory_iterator(sb.game))
        if (StartsWith(U(e.path().filename()), ".vrml-write-test")) ++leftovers;
    CHECK_EQ(leftovers, 0);
}

TEST(a_cancelled_download_is_reported_by_the_cache_as_e_cancelled) {
    tt::Sandbox sb;
    FakeDownloader dl;
    dl.content = "x";
    dl.progress = [](uint64_t, uint64_t) { return false; };
    DepsCache cache(sb.home, &dl, &sb.log);
    Pin p;
    p.id = "t";
    p.version = "1";
    p.url = "https://example.invalid/t.zip";
    p.file = "t.zip";
    p.sha256 = {Sha256Hex("x")};
    CHECK_ERROR("E_CANCELLED", cache.Ensure(p));
    CHECK(!Exists(cache.PathOf(p)));
}

TEST_MAIN()
