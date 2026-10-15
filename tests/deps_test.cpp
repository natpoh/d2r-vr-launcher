// D2RLoader's pin and the checks around it: hash acceptance, the cache, requirement checks. No network: a stand-in downloader.
#include <fstream>
#include <regex>

#include "deps.h"
#include "sandbox.h"
#include "sha256.h"
#include "test.h"

using namespace vrml;

namespace {

class FakeDownloader : public Downloader {
public:
    std::string content = "the archive";
    bool fail = false;
    int calls = 0;
    std::string last_url;
    bool Fetch(const std::string& url, const fs::path& dest, std::string* error) override {
        ++calls;
        last_url = url;
        if (fail) { *error = "no network (pretend)"; return false; }
        tt::Write(dest, content);
        return true;
    }
};

Pin TestPin() {
    Pin p;
    p.id = "thing";
    p.version = "1.0";
    p.url = "https://example.invalid/thing.zip";
    p.file = "thing.zip";
    p.sha256 = {Sha256Hex("the archive"), Sha256Hex("the repacked archive")};   // current + historical, as with D2RLoader
    return p;
}

std::string Slurp(const fs::path& p) {
    std::string s;
    ReadFile(p, &s);
    return s;
}

}  // namespace

TEST(the_two_reviewed_d2rloader_hashes) {
    const Pin& pin = D2RLoaderPin();
    CHECK_EQ(pin.sha256.size(), size_t(2));
    CHECK(HashAccepted(pin.sha256, "f7bf0287398890b09bcc68b576327e727304bd88f7be80160f9676e8bd6871e2"));     // current
    CHECK(HashAccepted(pin.sha256, "9286c6b5bff7f1043658411dbea305faa9698b6a455e6441668fc6d02c22aad7"));     // historical (before the repack)
    CHECK(HashAccepted(pin.sha256, "F7BF0287398890B09BCC68B576327E727304BD88F7BE80160F9676E8BD6871E2"));     // case does not matter
    CHECK(!HashAccepted(pin.sha256, "f7bf0287398890b09bcc68b576327e727304bd88f7be80160f9676e8bd6871e3"));    // one digit off
    CHECK(!HashAccepted(pin.sha256, ""));
    CHECK(!HashAccepted(pin.sha256, "f7bf"));
    CHECK(!HashAccepted(pin.sha256, std::string(64, '0')));
    CHECK_EQ(pin.url, std::string("https://d2rloader.net/downloads/D2RLoader-1.3.1-beta.zip"));
    for (const std::string& h : pin.sha256) CHECK(IsSha256Hex(h));
}

TEST(the_pin_is_the_one_the_installers_carry) {
    // read the real sources when they are next to this folder (the launcher lives in the mod's repository for now)
    const fs::path repo = fs::path(VRML_SOURCE_DIR).parent_path();
    const Pin& pin = D2RLoaderPin();
    int checked = 0;
    for (const char* rel : {"installer/D2R_VR_Setup.iss", "tools/settings/settings.cpp"}) {
        std::string text;
        if (!ReadFile(repo / rel, &text)) continue;
        ++checked;
        for (const std::string& h : pin.sha256) CHECK(text.find(h) != std::string::npos);
        std::smatch m;
        // no other 64-digit hash on the D2RLoader pin lines
        std::regex line(R"((D2RLoaderSha256\w*|kD2RLoaderSha256\w*)[^\n]*?([0-9a-f]{64}))");
        for (auto it = std::sregex_iterator(text.begin(), text.end(), line); it != std::sregex_iterator(); ++it)
            CHECK(HashAccepted(pin.sha256, (*it)[2].str()));
    }
    if (!checked) std::cerr << "    (the installer sources are not next to the launcher: skipped)\n";
}

TEST(a_good_download_is_kept_and_reused_by_every_mod) {
    tt::Sandbox sb;
    FakeDownloader dl;
    DepsCache cache(sb.home, &dl, &sb.log);
    const Pin pin = TestPin();
    fs::path f = cache.Ensure(pin);
    CHECK(IsFile(f));
    CHECK_EQ(dl.calls, 1);
    CHECK_EQ(dl.last_url, pin.url);
    DepsCache again(sb.home, &dl, &sb.log);       // another mod, another run
    CHECK_EQ(again.Ensure(pin).string(), f.string());
    CHECK_EQ(dl.calls, 1);                          // no second download
}

TEST(a_download_with_another_hash_is_deleted_and_stops_everything) {
    tt::Sandbox sb;
    FakeDownloader dl;
    dl.content = "something else";                  // what a changed or tampered file looks like
    DepsCache cache(sb.home, &dl, &sb.log);
    const Pin pin = TestPin();
    CHECK_ERROR("E_HASH", cache.Ensure(pin));
    CHECK(!Exists(cache.PathOf(pin)));
    fs::path part = cache.PathOf(pin);
    part += ".part";
    CHECK(!Exists(part));
}

TEST(a_cached_file_with_another_hash_is_refetched) {
    tt::Sandbox sb;
    FakeDownloader dl;
    DepsCache cache(sb.home, &dl, &sb.log);
    const Pin pin = TestPin();
    tt::Write(cache.PathOf(pin), "damaged on disk");
    fs::path f = cache.Ensure(pin);
    CHECK_EQ(dl.calls, 1);
    CHECK_EQ(Slurp(f), std::string("the archive"));
}

TEST(the_historical_archive_is_accepted_too) {
    tt::Sandbox sb;
    FakeDownloader dl;
    dl.content = "the repacked archive";
    DepsCache cache(sb.home, &dl, &sb.log);
    CHECK(IsFile(cache.Ensure(TestPin())));
}

TEST(no_network_and_no_cache) {
    tt::Sandbox sb;
    FakeDownloader dl;
    dl.fail = true;
    DepsCache cache(sb.home, &dl, &sb.log);
    CHECK_ERROR("E_IO", cache.Ensure(TestPin()));
    DepsCache offline(sb.home, nullptr, &sb.log);
    CHECK_ERROR("E_NOT_FOUND", offline.Ensure(TestPin()));
    // with a good cache the missing network does not matter
    tt::Write(offline.PathOf(TestPin()), "the archive");
    CHECK(IsFile(offline.Ensure(TestPin())));
}

TEST(requirements_are_checked_against_what_the_selected_mod_needs) {
    tt::Sandbox sb;
    tt::ModSpec s = tt::AlphaSpec();
    const std::string good = Sha256Hex("fake D2RLoader");
    Json req = Json::Array();
    Json r = Json::Object();
    r.set("id", Json::String("d2rloader"));
    Json files = Json::Array();
    Json f1 = Json::Object();
    f1.set("path", Json::String("{game}/D2RLoader.exe"));
    Json h = Json::Array();
    h.push(Json::String(good));
    f1.set("sha256_any_of", h);
    files.push(f1);
    Json f2 = Json::Object();
    f2.set("path", Json::String("{game}/D2RCore.dll"));      // not in the fake game
    files.push(f2);
    r.set("files", files);
    req.push(r);
    Json opt = Json::Object();
    opt.set("id", Json::String("bodywalk"));
    opt.set("optional", Json::Bool(true));
    Json bf = Json::Array();
    Json bf1 = Json::Object();
    bf1.set("path", Json::String("{game}/BodyWalkVR/BodyWalkVR.exe"));
    bf.push(bf1);
    opt.set("files", bf);
    req.push(opt);
    s.extra.set("requires", req);
    tt::Install(sb, s);
    Manifest m = Registry(sb.home, &sb.log).Load("alpha");

    auto res = CheckRequirements(m, sb.game);
    REQUIRE(res.size() == 2);
    CHECK(!res[0].ok);                                         // D2RCore.dll is missing
    CHECK(res[0].problems.size() == 1);
    CHECK(!res[1].ok);
    CHECK(res[1].optional);

    tt::Write(sb.game / "D2RCore.dll", "core");
    CHECK(CheckRequirements(m, sb.game)[0].ok);
    tt::Write(sb.game / "D2RLoader.exe", "a different D2RLoader");
    res = CheckRequirements(m, sb.game);
    CHECK(!res[0].ok);
    CHECK(res[0].problems[0].find("is not the file") != std::string::npos);
}

TEST_MAIN()
