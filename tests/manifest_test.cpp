#include <fstream>
#include <iostream>

#include "manifest.h"
#include "sandbox.h"
#include "test.h"

using namespace vrml;

static fs::path Src() { return fs::path(VRML_SOURCE_DIR); }

static bool Load(const fs::path& f, Manifest* m, std::vector<Issue>* issues) {
    try {
        return LoadManifest(f, m, issues);
    } catch (const Error& e) {
        issues->push_back({"", e.what()});
        return false;
    }
}

TEST(examples_are_valid) {
    for (const char* name : {"d2r-vr", "d2r-pcvr"}) {
        Manifest m;
        std::vector<Issue> issues;
        const fs::path f = Src() / "examples" / name / "manifest.json";
        CHECK(Load(f, &m, &issues));
        if (!issues.empty()) std::cerr << IssuesText(issues);
        CHECK_EQ(m.id, std::string(name));
        CHECK_EQ(m.group, std::string("d2r-vr-plugin"));
    }
}

TEST(d2r_vr_example_names_everything_the_mod_ships) {
    Manifest m;
    std::vector<Issue> issues;
    REQUIRE(Load(Src() / "examples" / "d2r-vr" / "manifest.json", &m, &issues));
    std::set<std::string> dst;
    for (const FileEntry& f : m.files) dst.insert(f.dst_rel);
    CHECK(dst.count("d2rloader/plugins/d2rl-vrcam.dll"));
    CHECK(dst.count("d2rloader/plugins/d2r_vr.ini"));
    CHECK(dst.count("D2R_VR_Settings.exe"));
    CHECK(dst.count("reshade-shaders/Shaders/D2R_DepthFog.fx"));
    CHECK(dst.count("reshade-shaders/Textures/D2R_Sky_ours/"));
    CHECK(dst.count("ReShade.ini"));
    CHECK(dst.count("FlatVR_DepthProvider.addon64"));
    CHECK_EQ(m.version, std::string("0.165.0"));
    CHECK(m.has_settings_app);
    CHECK(!m.launch.empty());
}

TEST(pcvr_example_has_the_openxr_loader_outside_plugins) {
    Manifest m;
    std::vector<Issue> issues;
    REQUIRE(Load(Src() / "examples" / "d2r-pcvr" / "manifest.json", &m, &issues));
    bool loader = false, reshadeIni = false;
    for (const FileEntry& f : m.files) {
        if (f.dst_rel == "openxr_loader.dll") loader = true;
        if (f.dst_rel == "ReShade.ini" && f.role == Role::Config) reshadeIni = true;
    }
    CHECK(loader);
    CHECK(reshadeIni);
    REQUIRE(!m.launch.empty());
    CHECK_EQ(m.launch[0].runtimes.size(), size_t(2));
    CHECK(!m.has_settings_app);
}

TEST(fixtures_by_name) {
    const fs::path dir = Src() / "tests" / "fixtures" / "manifests";
    int n = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        const std::string name = U(e.path().filename());
        Manifest m;
        std::vector<Issue> issues;
        const bool ok = Load(e.path(), &m, &issues);
        ++n;
        if (StartsWith(name, "ok_")) { CHECK(ok); if (!ok) std::cerr << name << ":\n" << IssuesText(issues); }
        else { CHECK(!ok); if (ok) std::cerr << name << " was accepted\n"; }
    }
    CHECK(n >= 30);
}

TEST(issue_paths_point_at_the_field) {
    Manifest m;
    std::vector<Issue> issues;
    Load(Src() / "tests" / "fixtures" / "manifests" / "bad_dst_traversal.json", &m, &issues);
    REQUIRE(!issues.empty());
    CHECK_EQ(issues[0].path, std::string("/files/0/dst"));
    issues.clear();
    Load(Src() / "tests" / "fixtures" / "manifests" / "bad_unknown_field.json", &m, &issues);
    REQUIRE(!issues.empty());
    CHECK_EQ(issues[0].path, std::string("/colour"));
}

TEST(install_help_is_an_optional_text) {
    Manifest m;
    std::vector<Issue> issues;
    CHECK(Load(Src() / "tests" / "fixtures" / "manifests" / "ok_install_help.json", &m, &issues));
    CHECK_EQ(m.install_help, std::string("Close the game, run the Setup, choose Full install."));
    CHECK(Load(Src() / "tests" / "fixtures" / "manifests" / "ok_d2rloader_mod.json", &m, &issues));
    CHECK(m.install_help.empty());
    issues.clear();
    CHECK(!Load(Src() / "tests" / "fixtures" / "manifests" / "bad_install_help_type.json", &m, &issues));
    REQUIRE(!issues.empty());
    CHECK_EQ(issues[0].path, std::string("/install_help"));
}

TEST(unparseable_json_is_an_error) {
    tt::Sandbox sb;
    tt::Write(sb.root / "m.json", "{ \"schema\": ");
    Manifest m;
    std::vector<Issue> issues;
    CHECK_ERROR("E_MANIFEST", LoadManifest(sb.root / "m.json", &m, &issues));
    CHECK_ERROR("E_MANIFEST", LoadManifest(sb.root / "missing.json", &m, &issues));
}

TEST(versions) {
    CHECK(IsVersion("1.2.3"));
    CHECK(IsVersion("0.165.0-rc.1"));
    CHECK(!IsVersion("1.2"));
    CHECK(!IsVersion("v1.2.3"));
    CHECK(!IsVersion("1.2.3-"));
    CHECK(CompareVersions("0.165.0", "0.164.9") > 0);
    CHECK(CompareVersions("1.3.1", "1.3.1") == 0);
    CHECK(CompareVersions("1.3.1-beta", "1.3.1") < 0);   // a pre-release is below its release
    CHECK(CompareVersions("0.3.13", "0.3.2") > 0);       // numbers, not text
    CHECK(CompareVersions("2.0.0", "10.0.0") < 0);
}

TEST(paths) {
    std::string why;
    CHECK(SafeRel("d2rloader/plugins/x.dll"));
    CHECK(SafeRel("a\\b\\c.txt"));
    CHECK(!SafeRel("", &why));
    CHECK(!SafeRel("../x", &why));
    CHECK(!SafeRel("a/../../x", &why));
    CHECK(!SafeRel("/abs", &why));
    CHECK(!SafeRel("C:/x", &why));
    CHECK(!SafeRel("a/b.", &why));
    CHECK(!SafeRel("a/b ", &why));
    CHECK(!SafeRel("a/b?.txt", &why));
    CHECK_EQ(NormRel(".\\a\\\\b/./c"), std::string("a/b/c"));
    CHECK_EQ(NormRel("dir/"), std::string("dir/"));
    CHECK_EQ(GameRel("{game}\\d2rloader/plugins/x.dll"), std::string("d2rloader/plugins/x.dll"));
    CHECK_EQ(GameRel("{localappdata}/x"), std::string(""));
    CHECK(GlobMatch("*.before_*", "d2r_vr.ini.before_test3"));
    CHECK(GlobMatch("D2R_VR_*", "d2r_vr_settings.ini"));
    CHECK(!GlobMatch("*.bak", "x.dll"));
}

TEST(game_key_ignores_case_and_slashes) {
    tt::Sandbox sb;
    const std::string a = GameKey(sb.game);
    CHECK_EQ(a.size(), size_t(16));
    CHECK_EQ(GameKey(sb.game / "."), a);
    CHECK(GameKey(sb.root / "other") != a);
}

// ---- the package on disk

TEST(package_check_finds_missing_and_wrong_files) {
    tt::Sandbox sb;
    tt::ModSpec s = tt::AlphaSpec();
    const fs::path mf = tt::WritePackage(sb.home, s);
    Manifest m;
    std::vector<Issue> issues;
    REQUIRE(LoadManifest(mf, &m, &issues));
    CHECK(CheckPackage(m, &issues));
    fs::remove(sb.home / "mods" / "alpha" / "payload" / "Alpha_Settings.exe");
    issues.clear();
    CHECK(!CheckPackage(m, &issues));
    CHECK(!issues.empty());
    CHECK(issues[0].message.find("missing in the package") != std::string::npos);
}

TEST(package_check_compares_listed_hashes) {
    tt::Sandbox sb;
    tt::ModSpec s = tt::AlphaSpec();
    const fs::path mf = tt::WritePackage(sb.home, s);
    Json j = Json::Parse(tt::Read(mf));
    j["files"].items()[0].set("sha256", Json::String(std::string(64, '0')));
    tt::Write(mf, j.dump());
    Manifest m;
    std::vector<Issue> issues;
    REQUIRE(LoadManifest(mf, &m, &issues));
    CHECK(!CheckPackage(m, &issues));
    CHECK(issues[0].path == "/files/0/sha256");
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--validate") {   // for tools/check_schema.py: "OK <file>" / "BAD <file>"
        for (int i = 2; i < argc; ++i) {
            Manifest m;
            std::vector<Issue> issues;
            const bool ok = Load(fs::path(argv[i]), &m, &issues);
            std::cout << (ok ? "OK " : "BAD ") << argv[i] << "\n";
        }
        return 0;
    }
    return t::RunAll(argc, argv);
}
