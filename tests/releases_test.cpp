// What the newest releases are: versions, our server's answer, GitHub's list (a real trimmed sample from bearstonem/d2r-pcvr), the checksums, and
// everything that makes an answer untrustworthy. A fake HTTP client answers from a table; nothing here touches the network.
#include "fakes.h"
#include "json.h"
#include "releases.h"
#include "sandbox.h"
#include "sha256.h"
#include "test.h"

using namespace vrml;

namespace {

fs::path Fix(const std::string& name) { return fs::path(VRML_SOURCE_DIR) / "tests" / "fixtures" / "releases" / name; }
std::string Text(const std::string& name) {
    std::string s;
    ReadFile(Fix(name), &s);
    return s;
}

using tt::FakeHttp;

ReleaseSources Sources() {
    ReleaseSources s;
    s.test_http_host = "";
    return s;
}

const char* kSha256Url = "https://github.com/bearstonem/d2r-pcvr/releases/download/v0.3.19-windows-preview/D2R-PCVR-Setup-0.3.19.exe.sha256";
const char* kDigest319 = "bd37c8642e48d634c8823306776ccf5026b41f28728b492da541f7a669c3580c";

// the GitHub sample with a change made by `edit` to the parsed list
std::string Github(const std::function<void(Json&)>& edit = {}) {
    Json j = Json::Parse(Text("github_releases.json"));
    if (edit) edit(j);
    return j.dump();
}

Json& Asset(Json& list, size_t release, const std::string& name) {
    for (Json& a : list.items()[release]["assets"].items())
        if (a.get_string("name") == name) return a;
    throw std::runtime_error("no asset " + name);
}

FakeHttp WithSha256File() {
    FakeHttp h;
    h.answers[kSha256Url] = Text("D2R-PCVR-Setup-0.3.19.exe.sha256");
    return h;
}

}  // namespace

TEST(versions_are_normalised_and_compared_as_numbers) {
    CHECK_EQ(NormalizeVersion("0.168"), std::string("0.168.0"));
    CHECK_EQ(NormalizeVersion("0.168.0"), std::string("0.168.0"));
    CHECK_EQ(NormalizeVersion("v0.3.19-windows-preview"), std::string("0.3.19"));
    CHECK_EQ(NormalizeVersion("V1.2"), std::string("1.2.0"));
    CHECK_EQ(NormalizeVersion("1.2.3.4"), std::string("1.2.3.4"));
    CHECK_EQ(NormalizeVersion("0168.01"), std::string("168.1.0"));
    CHECK_EQ(NormalizeVersion("7"), std::string("7.0.0"));
    CHECK_EQ(NormalizeVersion(""), std::string());
    CHECK_EQ(NormalizeVersion("abc"), std::string());
    CHECK_EQ(NormalizeVersion("x1.2"), std::string());
    CHECK(!IsNewer("0.168", "0.168.0"));
    CHECK(!IsNewer("0.168.0", "0.168"));
    CHECK(IsNewer("0.169", "0.168.0"));
    CHECK(IsNewer("0.3.19", "0.3.18"));
    CHECK(!IsNewer("0.3.9", "0.3.19"));                            // 9 < 19, not "9" > "19" as text
    CHECK(IsNewer("0.10.0", "0.9.0"));
    CHECK(!IsNewer("v0.3.19-windows-preview", "0.3.19"));
    CHECK(IsNewer("0.3.19", ""));                                  // nothing installed
    CHECK(!IsNewer("", "0.1.0"));
    CHECK(!IsNewer("junk", "1.0"));
    CHECK(!IsNewer("", ""));
}

TEST(our_server_names_a_newer_version) {
    FakeHttp h;
    h.answers["https://bodywalkvr.com/api/version?product=d2r_vr&v=0.167.0"] = Text("authentic_newer.json");
    const Release r = LatestAuthentic(h, "0.167.0", Sources());
    CHECK_EQ(h.asked.size(), size_t(1));
    CHECK(!r.up_to_date);
    CHECK_EQ(r.id, std::string("d2r-vr"));
    CHECK_EQ(r.version, std::string("0.168.0"));
    CHECK_EQ(r.url, std::string("https://bodywalkvr.com/static/uploads/versions/D2R_VR_Setup_v0.168.0.exe"));
    CHECK_EQ(r.asset_name, std::string("D2R_VR_Setup_v0.168.0.exe"));
    CHECK(r.sha256.empty());                                       // the server publishes none today
    CHECK_EQ(r.size, uint64_t(0));
    CHECK(!r.notes.empty());
    CHECK_EQ(r.source, std::string("bodywalkvr.com"));
    CHECK_EQ(r.current, std::string("0.167.0"));
    CHECK(!r.prerelease);
}

TEST(the_real_answer_of_our_server_is_understood) {
    // authentic_real.json is what https://bodywalkvr.com/api/version answered on 2026-10-18 (version 0.169; no sha256 or size in it)
    const Release r = ParseAuthenticAnswer(Text("authentic_real.json"), "0.168.0", Sources());
    CHECK(!r.up_to_date);
    CHECK_EQ(r.version, std::string("0.169.0"));
    CHECK_EQ(r.url, std::string("https://bodywalkvr.com/static/uploads/versions/D2R_VR_Setup_v0.169.0.exe"));
    CHECK_EQ(r.asset_name, std::string("D2R_VR_Setup_v0.169.0.exe"));
    CHECK(r.sha256.empty() && r.size == 0);
    CHECK(r.notes.find("weight and grip") != std::string::npos);
    std::string why;
    CHECK(ReleaseUrlOk(r, "", &why));
    CHECK(ParseAuthenticAnswer(Text("authentic_real.json"), "0.169", Sources()).up_to_date);        // it is the newest
    CHECK(ParseAuthenticAnswer(Text("authentic_real.json"), "0.170.0", Sources()).up_to_date);       // newer than the server knows: nothing to offer
}

TEST(our_server_says_nothing_is_newer) {
    FakeHttp h;
    h.answers["https://bodywalkvr.com/api/version?product=d2r_vr&v=0.168.0"] = Text("authentic_uptodate.json");
    const Release r = LatestAuthentic(h, "0.168", Sources());       // "0.168" is sent as 0.168.0
    CHECK(r.up_to_date);
    CHECK(r.url.empty());
    CHECK_EQ(r.version, std::string("0.168.0"));
    // an answer that names a version that is not newer counts as up to date as well
    h.answers["https://bodywalkvr.com/api/version?product=d2r_vr&v=0.170.0"] = Text("authentic_newer.json");
    CHECK(LatestAuthentic(h, "0.170.0", Sources()).up_to_date);
}

TEST(nothing_installed_asks_for_version_zero) {
    FakeHttp h;
    h.answers["https://bodywalkvr.com/api/version?product=d2r_vr&v=0.0.0"] = Text("authentic_newer.json");
    const Release r = LatestAuthentic(h, "", Sources());
    CHECK(!r.up_to_date);
    CHECK_EQ(r.version, std::string("0.168.0"));
}

TEST(a_published_sha256_and_size_are_taken_and_a_bad_one_is_refused) {
    FakeHttp h;
    const std::string base = "https://bodywalkvr.com/api/version?product=d2r_vr&v=0.0.0";
    const std::string good(64, 'A');
    h.answers[base] = R"({"version":"0.169","download_url":"https://bodywalkvr.com/static/x/D2R_VR_Setup_v0.169.0.exe","sha256":")" + good + R"(","size":12345678})";
    Release r = LatestAuthentic(h, "", Sources());
    CHECK_EQ(r.sha256, std::string(64, 'a'));                      // lower-cased
    CHECK_EQ(r.sha256_source, std::string("published"));
    CHECK_EQ(r.size, uint64_t(12345678));
    for (const std::string bad : {std::string(R"("sha256":"abc")"), std::string(R"("sha256":12)"), "\"sha256\":\"" + std::string(64, 'z') + "\"", std::string(R"("size":"big")"),
                                  std::string(R"("size":-5)"), std::string(R"("size":0)"), std::string(R"("size":1.5)")}) {
        h.answers[base] = std::string(R"({"version":"0.169","download_url":"https://bodywalkvr.com/static/x/D2R_VR_Setup_v0.169.0.exe",)") + bad + "}";
        CHECK_ERROR("E_UNTRUSTED", LatestAuthentic(h, "", Sources()));
    }
}

TEST(an_address_off_our_host_or_not_https_is_refused) {
    FakeHttp h;
    const std::string base = "https://bodywalkvr.com/api/version?product=d2r_vr&v=0.0.0";
    for (const char* url : {"http://bodywalkvr.com/static/a.exe", "https://evil.example/static/a.exe", "https://bodywalkvr.com@evil.example/a.exe", "https://bodywalkvr.com.evil.example/a.exe",
                            "https://www.bodywalkvr.com/a.exe", "ftp://bodywalkvr.com/a.exe", "https://bodywalkvr.com/static/a.msi", "https://bodywalkvr.com/static/",
                            "https://bodywalkvr.com/static/a b.exe", "//bodywalkvr.com/a.exe", "bodywalkvr.com/a.exe", "https://bodywalkvr.com/static/..exe"}) {
        h.answers[base] = std::string(R"({"version":"0.169","download_url":")") + url + "\"}";
        CHECK_ERROR("E_UNTRUSTED", LatestAuthentic(h, "", Sources()));
    }
    h.answers[base] = R"({"version":"0.169"})";
    CHECK_ERROR("E_IO", LatestAuthentic(h, "", Sources()));          // no address at all
}

TEST(a_loopback_test_host_is_accepted_only_when_the_switch_names_it) {
    FakeHttp h;
    const std::string base = "http://127.0.0.1:8123/api/version?product=d2r_vr&v=0.0.0";
    h.answers[base] = R"({"version":"0.169","download_url":"http://127.0.0.1:8123/files/D2R_VR_Setup_v0.169.0.exe"})";
    ReleaseSources s = Sources();
    s.authentic_api = "http://127.0.0.1:8123/api/version";
    CHECK_ERROR("E_UNTRUSTED", LatestAuthentic(h, "", s));           // the switch is off
    s.test_http_host = "127.0.0.1:8123";
    CHECK_EQ(LatestAuthentic(h, "", s).url, std::string("http://127.0.0.1:8123/files/D2R_VR_Setup_v0.169.0.exe"));
    s.test_http_host = "127.0.0.1:9999";
    CHECK_ERROR("E_UNTRUSTED", LatestAuthentic(h, "", s));           // another port
}

TEST(unreadable_or_missing_answers_are_io_errors) {
    FakeHttp h;
    CHECK_ERROR("E_IO", LatestAuthentic(h, "0.1.0", Sources()));     // not reachable
    CHECK_ERROR("E_IO", LatestUnleashed(h, "", Sources()));
    const std::string base = "https://bodywalkvr.com/api/version?product=d2r_vr&v=0.0.0";
    for (const char* body : {"", "not json", "[]", "\"text\"", "{}", R"({"version":""})", R"({"version":"abc"})", R"({"version":12})"}) {
        h.answers[base] = body;
        CHECK_ERROR("E_IO", LatestAuthentic(h, "", Sources()));
    }
    h.answers[Sources().unleashed_api] = "{\"not\":\"a list\"}";
    CHECK_ERROR("E_IO", LatestUnleashed(h, "", Sources()));
}

TEST(the_newest_release_of_the_real_github_sample_is_picked) {
    FakeHttp h = WithSha256File();
    h.answers[Sources().unleashed_api] = Text("github_releases.json");
    const Release r = LatestUnleashed(h, "0.3.18", Sources());
    CHECK_EQ(h.asked.size(), size_t(2));                             // the list, then the tiny .sha256 file
    CHECK_EQ(h.asked[0], std::string("https://api.github.com/repos/bearstonem/d2r-pcvr/releases?per_page=5"));
    CHECK_EQ(h.asked[1], std::string(kSha256Url));
    CHECK_EQ(r.id, std::string("d2r-pcvr"));
    CHECK_EQ(r.version, std::string("0.3.19"));
    CHECK_EQ(r.tag, std::string("v0.3.19-windows-preview"));
    CHECK_EQ(r.asset_name, std::string("D2R-PCVR-Setup-0.3.19.exe"));
    CHECK_EQ(r.url, std::string("https://github.com/bearstonem/d2r-pcvr/releases/download/v0.3.19-windows-preview/D2R-PCVR-Setup-0.3.19.exe"));
    CHECK_EQ(r.size, uint64_t(46966441));
    CHECK_EQ(r.sha256, std::string(kDigest319));
    CHECK_EQ(r.sha256_source, std::string("digest + .sha256"));
    CHECK(r.prerelease);
    CHECK(!r.up_to_date);
    CHECK_EQ(r.published, std::string("2026-10-18T08:17:11Z"));
    CHECK(r.notes.find("Unleashed") != std::string::npos);
    CHECK(r.warnings.empty());
    CHECK(LatestUnleashed(h, "0.3.19", Sources()).up_to_date);
    CHECK(LatestUnleashed(h, "0.3.20", Sources()).up_to_date);
    CHECK(!LatestUnleashed(h, "", Sources()).up_to_date);
}

TEST(the_digest_and_the_sha256_file_must_agree) {
    FakeHttp h;
    h.answers[Sources().unleashed_api] = Text("github_releases.json");
    h.answers[kSha256Url] = std::string(64, 'a') + "  D2R-PCVR-Setup-0.3.19.exe\n";
    try {
        LatestUnleashed(h, "", Sources());
        CHECK(!"should have thrown");
    } catch (const Error& e) {
        CHECK_EQ(e.code, std::string("E_HASH"));
        CHECK(std::string(e.what()).find(kDigest319) != std::string::npos);
    }
    // the same hash in upper case and in sha256sum's binary form is the same hash
    std::string upper = kDigest319;
    for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    h.answers[kSha256Url] = upper + " *D2R-PCVR-Setup-0.3.19.exe\r\n";
    CHECK_EQ(LatestUnleashed(h, "", Sources()).sha256_source, std::string("digest + .sha256"));
}

TEST(a_sha256_file_that_cannot_be_used_leaves_the_digest_alone_with_a_warning) {
    FakeHttp h;                                                       // the .sha256 address is not answered
    h.answers[Sources().unleashed_api] = Text("github_releases.json");
    Release r = LatestUnleashed(h, "", Sources());
    CHECK_EQ(r.sha256, std::string(kDigest319));
    CHECK_EQ(r.sha256_source, std::string("digest"));
    CHECK_EQ(r.warnings.size(), size_t(1));
    h.answers[kSha256Url] = "garbage that is not a hash\n";
    r = LatestUnleashed(h, "", Sources());
    CHECK_EQ(r.sha256_source, std::string("digest"));
    CHECK_EQ(r.warnings.size(), size_t(1));
    h.answers[kSha256Url] = std::string(kDigest319) + "  some-other-file.exe\n";                  // a hash for another file
    r = LatestUnleashed(h, "", Sources());
    CHECK_EQ(r.sha256_source, std::string("digest"));
    CHECK(r.warnings.size() == 1 && r.warnings[0].find("some-other-file.exe") != std::string::npos);
}

TEST(without_a_digest_the_sha256_file_is_the_checksum_and_without_both_there_is_none) {
    FakeHttp h = WithSha256File();
    h.answers[Sources().unleashed_api] = Github([](Json& j) { Asset(j, 0, "D2R-PCVR-Setup-0.3.19.exe").set("digest", Json::String("")); });
    Release r = LatestUnleashed(h, "", Sources());
    CHECK_EQ(r.sha256, std::string(kDigest319));
    CHECK_EQ(r.sha256_source, std::string(".sha256 file"));
    FakeHttp none;                                                    // no digest and no .sha256 reachable
    none.answers[Sources().unleashed_api] = h.answers[Sources().unleashed_api];
    r = LatestUnleashed(none, "", Sources());
    CHECK(r.sha256.empty());
    CHECK(r.sha256_source.empty());
}

TEST(only_a_sha256_digest_counts) {
    FakeHttp h;
    h.answers[Sources().unleashed_api] = Github([](Json& j) { Asset(j, 0, "D2R-PCVR-Setup-0.3.19.exe").set("digest", Json::String("sha512:abcdef")); });
    CHECK(LatestUnleashed(h, "", Sources()).sha256.empty());         // another algorithm: not used, not an error
    h.answers[Sources().unleashed_api] = Github([](Json& j) { Asset(j, 0, "D2R-PCVR-Setup-0.3.19.exe").set("digest", Json::String("sha256:12345")); });
    CHECK_ERROR("E_UNTRUSTED", LatestUnleashed(h, "", Sources()));  // claims to be a SHA-256 and is not
}

TEST(drafts_releases_without_the_installer_and_older_ones_are_not_chosen) {
    FakeHttp h;
    // the newest is a draft: the next is taken
    h.answers[Sources().unleashed_api] = Github([](Json& j) { j.items()[0].set("draft", Json::Bool(true)); });
    CHECK_EQ(LatestUnleashed(h, "", Sources()).version, std::string("0.3.18"));
    // the newest has no installer asset
    h.answers[Sources().unleashed_api] = Github([](Json& j) {
        std::vector<Json>& assets = j.items()[0]["assets"].items();
        assets.erase(assets.begin());
    });
    CHECK_EQ(LatestUnleashed(h, "", Sources()).version, std::string("0.3.18"));
    // the list is in no particular order: the highest version wins
    h.answers[Sources().unleashed_api] = Github([](Json& j) { std::swap(j.items()[0], j.items()[2]); });
    CHECK_EQ(LatestUnleashed(h, "", Sources()).version, std::string("0.3.19"));
    // every release a draft, or an empty list
    h.answers[Sources().unleashed_api] = Github([](Json& j) { for (Json& r : j.items()) r.set("draft", Json::Bool(true)); });
    CHECK_ERROR("E_NOT_FOUND", LatestUnleashed(h, "", Sources()));
    h.answers[Sources().unleashed_api] = "[]";
    CHECK_ERROR("E_NOT_FOUND", LatestUnleashed(h, "", Sources()));
    // an asset that is not "uploaded" yet
    h.answers[Sources().unleashed_api] = Github([](Json& j) { Asset(j, 0, "D2R-PCVR-Setup-0.3.19.exe").set("state", Json::String("starter")); });
    CHECK_EQ(LatestUnleashed(h, "", Sources()).version, std::string("0.3.18"));
}

TEST(an_installer_address_off_the_repository_is_refused) {
    FakeHttp h;
    auto with = [&](const std::string& url) {
        h.answers[Sources().unleashed_api] = Github([&](Json& j) { Asset(j, 0, "D2R-PCVR-Setup-0.3.19.exe").set("browser_download_url", Json::String(url)); });
    };
    with("https://github.com/someoneelse/d2r-pcvr/releases/download/v0.3.19-windows-preview/D2R-PCVR-Setup-0.3.19.exe");
    CHECK_ERROR("E_UNTRUSTED", LatestUnleashed(h, "", Sources()));
    with("https://evil.example/bearstonem/d2r-pcvr/releases/download/v0.3.19-windows-preview/D2R-PCVR-Setup-0.3.19.exe");
    CHECK_ERROR("E_UNTRUSTED", LatestUnleashed(h, "", Sources()));
    with("http://github.com/bearstonem/d2r-pcvr/releases/download/v0.3.19-windows-preview/D2R-PCVR-Setup-0.3.19.exe");
    CHECK_ERROR("E_UNTRUSTED", LatestUnleashed(h, "", Sources()));
    with("https://github.com/bearstonem/d2r-pcvr/archive/refs/heads/main.exe");
    CHECK_ERROR("E_UNTRUSTED", LatestUnleashed(h, "", Sources()));
    with("https://raw.githubusercontent.com/bearstonem/d2r-pcvr/releases/download/x/D2R-PCVR-Setup-0.3.19.exe");
    CHECK_ERROR("E_UNTRUSTED", LatestUnleashed(h, "", Sources()));
}

TEST(the_download_check_knows_each_source) {
    Release r;
    r.id = "d2r-vr";
    r.url = "https://bodywalkvr.com/static/D2R_VR_Setup_v0.168.0.exe";
    r.asset_name = "D2R_VR_Setup_v0.168.0.exe";
    std::string why;
    CHECK(ReleaseUrlOk(r, "", &why));
    r.url = "https://github.com/bearstonem/d2r-pcvr/releases/download/v1/D2R_VR_Setup_v0.168.0.exe";
    CHECK(!ReleaseUrlOk(r, "", &why));                               // the other mod's host
    r.id = "d2r-pcvr";
    CHECK(ReleaseUrlOk(r, "", &why));
    r.asset_name = "..\\evil.exe";
    CHECK(!ReleaseUrlOk(r, "", &why));
    r.asset_name = "setup.exe.txt";
    CHECK(!ReleaseUrlOk(r, "", &why));
    r.asset_name = "ok.exe";
    r.url = "";
    CHECK(!ReleaseUrlOk(r, "", &why));
    CHECK(DownloadHostsFor("nope").empty());
    CHECK(HostAllowed("release-assets.githubusercontent.com", DownloadHostsFor("d2r-pcvr")));   // where GitHub redirects a release download to
    CHECK(!HostAllowed("release-assets.githubusercontent.com", DownloadHostsFor("d2r-vr")));
}

TEST(what_is_installed_comes_from_the_mods_own_manifest_only) {
    tt::Sandbox sb;
    CHECK_EQ(InstalledVersion(sb.game, "d2r-vr"), std::string());
    // modinfo.json only: installed, but the version of the built-in profile is not "what is installed"
    tt::Write(sb.game / "mods" / "D2R-VR" / "D2R-VR.mpq" / "modinfo.json", "{}");
    CHECK_EQ(InstalledVersion(sb.game, "d2r-vr"), std::string());
    // with the mod's own manifest: its version
    std::string manifest;
    ReadFile(fs::path(VRML_SOURCE_DIR) / "examples" / "d2rloader-mods" / "d2r-vr" / "vrmod-manifest.json", &manifest);
    Json j = Json::Parse(manifest);
    j.set("version", Json::String("0.166.2"));
    tt::Write(sb.game / "mods" / "D2R-VR" / "vrmod-manifest.json", j.dump());
    CHECK_EQ(InstalledVersion(sb.game, "d2r-vr"), std::string("0.166.2"));
    CHECK_EQ(InstalledVersion(sb.game, "d2r-pcvr"), std::string());
    CHECK_EQ(InstalledVersion(sb.game, "no-such-mod"), std::string());
}

TEST(an_update_check_puts_installed_and_newest_together) {
    tt::Sandbox sb;
    FakeHttp h = WithSha256File();
    h.answers[Sources().unleashed_api] = Text("github_releases.json");
    h.answers["https://bodywalkvr.com/api/version?product=d2r_vr&v=0.0.0"] = Text("authentic_newer.json");

    UpdateInfo a = CheckUpdate(h, sb.game, "d2r-vr", Sources());       // not installed
    CHECK(a.checked);
    CHECK(!a.installed);
    CHECK(a.can_install);
    CHECK(!a.update_available);
    CHECK_EQ(a.name, std::string("D2R VR Authentic"));

    tt::Write(sb.game / "mods" / "D2R-PCVR" / "D2R-PCVR.mpq" / "modinfo.json", "{}");      // installed, version unknown (no manifest)
    UpdateInfo u = CheckUpdate(h, sb.game, "d2r-pcvr", Sources());
    CHECK(u.checked && u.installed);
    CHECK(u.update_available);                                          // an unknown version is offered the newest
    CHECK(!u.can_install);
    CHECK_EQ(u.latest.version, std::string("0.3.19"));

    std::string manifest;
    ReadFile(fs::path(VRML_SOURCE_DIR) / "examples" / "d2rloader-mods" / "d2r-pcvr" / "vrmod-manifest.json", &manifest);
    Json j = Json::Parse(manifest);
    j.set("version", Json::String("0.3.19"));
    tt::Write(sb.game / "mods" / "D2R-PCVR" / "vrmod-manifest.json", j.dump());
    u = CheckUpdate(h, sb.game, "d2r-pcvr", Sources());
    CHECK(u.installed && !u.update_available);
    CHECK_EQ(u.installed_version, std::string("0.3.19"));
    j.set("version", Json::String("0.3.17"));
    tt::Write(sb.game / "mods" / "D2R-PCVR" / "vrmod-manifest.json", j.dump());
    CHECK(CheckUpdate(h, sb.game, "d2r-pcvr", Sources()).update_available);

    FakeHttp offline;                                                   // no network: the question is reported, nothing throws
    u = CheckUpdate(offline, sb.game, "d2r-pcvr", Sources());
    CHECK(!u.checked);
    CHECK(!u.error.empty());
    CHECK(!u.update_available);
    CHECK(!CheckUpdate(offline, fs::path(), "d2r-vr", Sources()).checked);     // no game folder at all is fine
    CHECK(!CheckUpdate(h, sb.game, "no-such-mod", Sources()).checked);
}

TEST_MAIN()
