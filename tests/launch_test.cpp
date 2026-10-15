// The launch plan: option, store, the OpenXR runtime of this start, the environment rules, "via".
#include "launch.h"
#include "sandbox.h"
#include "test.h"

using namespace vrml;

namespace {

Manifest PcvrLike() {
    Manifest m;
    std::vector<Issue> issues;
    REQUIRE(LoadManifest(fs::path(VRML_SOURCE_DIR) / "examples" / "d2r-pcvr" / "manifest.json", &m, &issues));
    return m;
}

struct World {
    tt::Sandbox sb;
    fs::path steam, steamvr, programFiles;
    LaunchContext ctx;
    World() {
        steam = sb.root / "Steam";
        fs::create_directories(steam / "steamapps");
        fs::path lib2 = sb.root / "SteamLibrary";
        fs::create_directories(lib2 / "steamapps" / "common" / "SteamVR");
        tt::Write(lib2 / "steamapps" / "common" / "SteamVR" / "steamxr_win64.json", "{}");
        tt::Write(steam / "steamapps" / "libraryfolders.vdf",
                  "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"" + std::string(U(steam)) + "\"\n\t}\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\"" + U(lib2) + "\"\n\t}\n"
                  "\t\"2\"\n\t{\n\t\t\"path\"\t\t\"" + U(sb.root / "NoSuchLibrary") + "\"\n\t}\n}\n");
        programFiles = sb.root / "Program Files";
        ctx.mod_id = "d2r-pcvr";
        ctx.mod_version = "0.3.13";
        ctx.game = sb.game;
        ctx.game_build = "3.3.93787";
        ctx.store = "steam";
        ctx.roots.steam_libraries = SteamLibraries(steam);
        ctx.roots.program_files = programFiles;
    }
    void InstallVdxr() { tt::Write(programFiles / "Virtual Desktop Streamer" / "OpenXR" / "virtualdesktop-openxr.json", "{}"); }
};

}  // namespace

TEST(steam_libraries_are_read_from_the_vdf) {
    World w;
    REQUIRE(w.ctx.roots.steam_libraries.size() == 2);      // the Steam folder + one existing library (the third does not exist)
    CHECK(FindRuntime("steamvr", w.ctx.roots) == w.sb.root / "SteamLibrary" / "steamapps" / "common" / "SteamVR" / "steamxr_win64.json");
    CHECK(FindRuntime("vdxr", w.ctx.roots).empty());
    w.InstallVdxr();
    CHECK(!FindRuntime("vdxr", w.ctx.roots).empty());
    CHECK(FindRuntime("system", w.ctx.roots).empty());
}

TEST(the_runtime_is_chosen_for_this_start_only) {
    World w;
    w.InstallVdxr();
    Manifest m = PcvrLike();
    w.ctx.runtime = "steamvr";
    LaunchPlan a = BuildLaunchPlan(m, "", w.ctx);
    CHECK(a.env_set.at("XR_RUNTIME_JSON").find("steamxr_win64.json") != std::string::npos);
    w.ctx.runtime = "vdxr";
    LaunchPlan b = BuildLaunchPlan(m, "", w.ctx);
    CHECK(b.env_set.at("XR_RUNTIME_JSON").find("virtualdesktop-openxr.json") != std::string::npos);
    CHECK_EQ(b.env_set.at("VRMOD_RUNTIME"), std::string("vdxr"));
    // no default given: the option's first
    w.ctx.runtime = "";
    CHECK_EQ(BuildLaunchPlan(m, "", w.ctx).runtime, std::string("steamvr"));
}

TEST(a_missing_runtime_stops_the_launch) {
    World w;                                        // SteamVR is there, Virtual Desktop is not
    Manifest m = PcvrLike();
    w.ctx.runtime = "vdxr";
    CHECK_ERROR("E_RUNTIME", BuildLaunchPlan(m, "", w.ctx));
    w.ctx.runtime = "system";                      // the option does not offer it
    CHECK_ERROR("E_RUNTIME", BuildLaunchPlan(m, "", w.ctx));
}

TEST(steam_and_battlenet_differ_in_the_environment) {
    World w;
    Manifest m = PcvrLike();
    w.ctx.runtime = "steamvr";
    LaunchPlan s = BuildLaunchPlan(m, "", w.ctx);
    CHECK_EQ(s.env_set.at("SteamAppId"), std::string("2536520"));           // the Steam edition initialises against the owned game
    CHECK_EQ(s.env_set.at("SteamGameId"), std::string("2536520"));
    w.ctx.store = "battlenet";
    LaunchPlan b = BuildLaunchPlan(m, "", w.ctx);
    CHECK(!b.env_set.count("SteamAppId"));                                    // a Battle.net copy must not pick Steam's ownership
    bool unsetsApp = false, unsetsOverlay = false;
    for (const std::string& k : b.env_unset) { unsetsApp = unsetsApp || k == "SteamAppId"; unsetsOverlay = unsetsOverlay || k == "SteamOverlayGameId"; }
    CHECK(unsetsApp);
    CHECK(unsetsOverlay);

    // the environment the child gets
    std::map<std::string, std::string> base = {{"PATH", "x"}, {"steamoverlaygameid", "123"}, {"SteamAppId", "999"}};
    auto env = ApplyEnv(base, s);
    CHECK_EQ(env.at("SteamAppId"), std::string("2536520"));
    CHECK(!env.count("steamoverlaygameid"));
    CHECK_EQ(env.at("PATH"), std::string("x"));
    auto envB = ApplyEnv(base, b);
    CHECK(!envB.count("SteamAppId"));
}

TEST(a_launch_option_for_another_store_is_refused) {
    World w;
    tt::ModSpec s = tt::AlphaSpec();
    s.extra.set("launch", Json::Parse(R"({"options":[{"id":"go","label":"Go","kind":"exec","exe":"{game}/D2RLoader.exe","stores":["battlenet"]}]})"));
    tt::Install(w.sb, s);
    Manifest m = Registry(w.sb.home, &w.sb.log).Load("alpha");
    w.ctx.store = "steam";
    CHECK_ERROR("E_PRECONDITION", BuildLaunchPlan(m, "", w.ctx));
    CHECK_ERROR("E_PRECONDITION", BuildLaunchPlan(m, "go", w.ctx));
    w.ctx.store = "battlenet";
    CHECK_EQ(BuildLaunchPlan(m, "", w.ctx).option, std::string("go"));
    CHECK_ERROR("E_NOT_FOUND", BuildLaunchPlan(m, "nope", w.ctx));
}

TEST(explorer_is_used_when_steam_started_us) {
    CHECK_EQ(ResolveVia("auto", {{"PATH", "x"}}), std::string("direct"));
    CHECK_EQ(ResolveVia("auto", {{"SteamGameId", "1"}}), std::string("explorer"));
    CHECK_EQ(ResolveVia("auto", {{"SteamOverlayGameId", "1"}}), std::string("explorer"));
    CHECK_EQ(ResolveVia("direct", {{"SteamGameId", "1"}}), std::string("direct"));
    CHECK_EQ(ResolveVia("explorer", {}), std::string("explorer"));
    World w;
    w.ctx.runtime = "steamvr";
    CHECK_EQ(BuildLaunchPlan(PcvrLike(), "", w.ctx).via, std::string("direct"));
    w.ctx.env["SteamOverlayGameId"] = "2536520";
    CHECK_EQ(BuildLaunchPlan(PcvrLike(), "", w.ctx).via, std::string("explorer"));
}

TEST(the_exe_must_be_there_and_is_expanded) {
    World w;
    w.ctx.runtime = "steamvr";
    LaunchPlan p = BuildLaunchPlan(PcvrLike(), "", w.ctx);
    CHECK_EQ(p.exe, U(w.sb.game) + "/D2RLoader.exe");
    CHECK_EQ(p.cwd, U(w.sb.game));
    fs::remove(w.sb.game / "D2RLoader.exe");
    CHECK_ERROR("E_NOT_FOUND", BuildLaunchPlan(PcvrLike(), "", w.ctx));
}

TEST(the_support_variables_name_the_active_mod) {
    World w;
    w.ctx.runtime = "steamvr";
    LaunchPlan p = BuildLaunchPlan(PcvrLike(), "", w.ctx);
    CHECK_EQ(p.env_set.at("VRMOD_ACTIVE_ID"), std::string("d2r-pcvr"));
    CHECK_EQ(p.env_set.at("VRMOD_ACTIVE_VERSION"), std::string("0.3.13"));
    CHECK_EQ(p.env_set.at("VRMOD_GAME_BUILD"), std::string("3.3.93787"));
    CHECK_EQ(p.env_set.at("VRMOD_STORE"), std::string("steam"));
    CHECK(p.Describe().find("runtime steamvr") != std::string::npos);
}

TEST(a_runtime_file_can_be_given) {
    World w;
    tt::ModSpec s = tt::AlphaSpec();
    s.extra.set("launch", Json::Parse(R"({"options":[{"id":"go","label":"Go","kind":"exec","exe":"{game}/D2RLoader.exe","runtimes":["file"]}]})"));
    tt::Install(w.sb, s);
    Manifest m = Registry(w.sb.home, &w.sb.log).Load("alpha");
    w.ctx.runtime = "file";
    w.ctx.runtime_file = w.sb.root / "my-runtime.json";
    CHECK_ERROR("E_RUNTIME", BuildLaunchPlan(m, "", w.ctx));
    tt::Write(w.ctx.runtime_file, "{}");
    CHECK(BuildLaunchPlan(m, "", w.ctx).env_set.at("XR_RUNTIME_JSON").find("my-runtime.json") != std::string::npos);
}

TEST(the_plan_survives_the_explorer_hand_off) {
    World w;
    w.ctx.runtime = "steamvr";
    LaunchPlan p = BuildLaunchPlan(PcvrLike(), "", w.ctx);
    LaunchPlan q = LaunchPlan::FromJson(Json::Parse(p.ToJson().dump()));   // written to a request file, read by the child
    CHECK_EQ(q.exe, p.exe);
    CHECK_EQ(q.runtime, p.runtime);
    CHECK(q.env_set == p.env_set);
    CHECK(q.env_unset == p.env_unset);
    CHECK_EQ(q.via, p.via);
}

TEST_MAIN()
