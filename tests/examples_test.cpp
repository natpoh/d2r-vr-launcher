// The two example manifests (D2R VR 0.165 and D2R PCVR 0.3.13) are not only valid: with a package made to match each one
// (dummy files where the real ones would be), the engine switches a fake game between them, keeps each one's settings,
// and the launch plans come out as the two mods' installers start them today.
#include "engine.h"
#include "launch.h"
#include "sandbox.h"
#include "test.h"
#include "deps.h"

using namespace vrml;

namespace {

// A package for the manifest in `exampleDir`: the manifest itself and a dummy for every src it names. The content of a folder's files
// depends on the file's name only, so two mods that ship "the same pictures" ship the same bytes (as the real ones do).
void MakePackage(const fs::path& home, const std::string& id) {
    const fs::path dir = home / "mods" / id;
    fs::create_directories(dir);
    std::string text;
    REQUIRE(ReadFile(fs::path(VRML_SOURCE_DIR) / "examples" / P(id) / "manifest.json", &text));
    tt::Write(dir / "manifest.json", text);
    Json j = Json::Parse(text);
    for (const Json& f : j.find("files")->items()) {
        if (f.get_string("role") == "generated") continue;
        if (f.get_bool("optional") && f.get_string("tag") == "vr") continue;   // a flat-only package: the VR files are not in it
        const std::string src = f.get_string("src");
        const bool sky = src.find("sky") != std::string::npos;
        if (src.back() == '/') {
            if (src.find("sky_ours") != std::string::npos) {
                tt::Write(dir / P(src) / "D2R_Ceiling_act1_caves.png", "ceiling");      // what only this mod has
            } else {
                for (const char* n : {"D2R_Sky_act1.png", "D2R_Sky_act2.png", "D2R_Crosshair.png"})
                    tt::Write(dir / P(src) / n, std::string("picture:") + n);
            }
        } else {
            tt::Write(dir / P(src), sky ? std::string("x") : id + ":" + src);
        }
        if (f.has("seed")) tt::Write(dir / P(f.get_string("seed")), id + " defaults for " + f.get_string("dst") + "\n");
    }
}

std::string Live(const tt::Sandbox& sb, const std::string& rel) {
    const fs::path p = sb.game / P(rel);
    return IsFile(p) ? tt::Read(p) : std::string("<none>");
}

}  // namespace

TEST(the_two_example_mods_switch_a_fake_game) {
    tt::Sandbox sb;
    MakePackage(sb.home, "d2r-vr");
    MakePackage(sb.home, "d2r-pcvr");
    Registry reg(sb.home, &sb.log);
    reg.Register(sb.home / "mods" / "d2r-vr" / "manifest.json");
    reg.Register(sb.home / "mods" / "d2r-pcvr" / "manifest.json");
    Engine eng = sb.MakeEngine();

    eng.Activate(sb.game, "d2r-vr");
    CHECK(IsFile(sb.game / "d2rloader/plugins/d2rl-vrcam.dll"));
    CHECK(IsFile(sb.game / "D2R_VR_Settings.exe"));
    CHECK(IsFile(sb.game / "reshade-shaders/Textures/D2R_Sky_ours/D2R_Ceiling_act1_caves.png"));    // only this mod has them
    CHECK(!IsFile(sb.game / "FlatVR_DepthProvider.addon64"));                                       // optional: not in this package
    CHECK(!IsFile(sb.game / "openxr_loader.dll"));
    CHECK(eng.Verify(sb.game).ok());
    // the player tunes D2R VR
    tt::Write(sb.game / "d2rloader/plugins/d2r_vr.ini", "[openxr]\non=1\n");

    eng.Activate(sb.game, "d2r-pcvr");
    CHECK(IsFile(sb.game / "openxr_loader.dll"));                                                   // outside the plugins folder
    CHECK(IsFile(sb.game / "D2R_Sky_ours/D2R_Sky_act1.png"));
    CHECK(!IsFile(sb.game / "D2R_VR_Settings.exe"));
    CHECK(!IsFile(sb.game / "reshade-shaders/Textures/D2R_Sky_ours/D2R_Ceiling_act1_caves.png"));
    CHECK(IsFile(sb.game / "reshade-shaders/Textures/D2R_Sky_ours/D2R_Sky_act1.png"));              // shared: stayed
    CHECK_EQ(Live(sb, "d2rloader/plugins/d2r_vr.ini"), std::string("d2r-pcvr defaults for {game}/d2rloader/plugins/d2r_vr.ini\n"));
    CHECK(IsFile(sb.game / "D2R_Frame_ReShade.ini"));
    CHECK(eng.Verify(sb.game).ok());
    tt::Write(sb.game / "d2rloader/plugins/d2r_vr.ini", "[calibration]\nversion=1\n");

    eng.Activate(sb.game, "d2r-vr");
    CHECK_EQ(Live(sb, "d2rloader/plugins/d2r_vr.ini"), std::string("[openxr]\non=1\n"));            // each mod's settings came back
    CHECK(!IsFile(sb.game / "openxr_loader.dll"));
    CHECK(!IsFile(sb.game / "D2R_Frame_ReShade.ini"));
    CHECK(!IsFile(sb.game / "D2R_Sky_ours/D2R_Sky_act1.png"));
    eng.Activate(sb.game, "d2r-pcvr");
    CHECK_EQ(Live(sb, "d2rloader/plugins/d2r_vr.ini"), std::string("[calibration]\nversion=1\n"));
    CHECK(eng.Verify(sb.game).ok());
}

TEST(the_examples_ask_the_game_for_what_they_need) {
    tt::Sandbox sb;                                        // the fake D2RLoader.exe is not the real file
    MakePackage(sb.home, "d2r-vr");
    Registry(sb.home, &sb.log).Register(sb.home / "mods" / "d2r-vr" / "manifest.json");
    Manifest m = Registry(sb.home, &sb.log).Load("d2r-vr");
    auto res = CheckRequirements(m, sb.game);
    REQUIRE(res.size() == 3);
    CHECK_EQ(res[0].id, std::string("d2rloader"));
    CHECK(!res[0].ok);                                      // wrong bytes, and D2RCore.dll / d2rloader.mpq are not there
    CHECK(res[0].problems.size() == 3);
    CHECK(res[1].ok);                                       // ReShade64.dll is in the fake game
}

TEST(the_launch_plans_are_what_the_installers_start_today) {
    tt::Sandbox sb;
    MakePackage(sb.home, "d2r-vr");
    MakePackage(sb.home, "d2r-pcvr");
    Registry reg(sb.home, &sb.log);
    reg.Register(sb.home / "mods" / "d2r-vr" / "manifest.json");
    reg.Register(sb.home / "mods" / "d2r-pcvr" / "manifest.json");
    Engine eng = sb.MakeEngine();

    LaunchContext ctx;
    ctx.game = sb.game;
    ctx.store = "steam";
    ctx.game_build = "3.3.93787";
    ctx.roots.program_files = sb.root / "PF";
    tt::Write(sb.root / "PF/Virtual Desktop Streamer/OpenXR/virtualdesktop-openxr.json", "{}");
    fs::create_directories(sb.root / "Steam/steamapps/common/SteamVR");
    tt::Write(sb.root / "Steam/steamapps/common/SteamVR/steamxr_win64.json", "{}");
    ctx.roots.steam_libraries = {sb.root / "Steam"};

    // D2R VR: its own START (--start) with the system runtime; the launcher's runtime choice, when given, reaches it as XR_RUNTIME_JSON
    eng.Activate(sb.game, "d2r-vr");
    ctx.mod_id = "d2r-vr";
    ctx.mod_version = "0.165.0";
    LaunchPlan v = BuildLaunchPlan(reg.Load("d2r-vr"), "", ctx);
    CHECK_EQ(v.runtime, std::string("system"));
    CHECK(v.exe.find("D2R_VR_Settings.exe") != std::string::npos);
    CHECK_EQ(v.args.size(), size_t(1));
    CHECK(!v.env_set.count("XR_RUNTIME_JSON"));

    // D2R PCVR: the loader, the runtime chosen for this start
    eng.Activate(sb.game, "d2r-pcvr");
    ctx.mod_id = "d2r-pcvr";
    ctx.mod_version = "0.3.13";
    ctx.runtime = "vdxr";
    LaunchPlan p = BuildLaunchPlan(reg.Load("d2r-pcvr"), "", ctx);
    CHECK(p.exe.find("D2RLoader.exe") != std::string::npos);
    CHECK(p.env_set.at("XR_RUNTIME_JSON").find("virtualdesktop-openxr.json") != std::string::npos);
    CHECK_EQ(p.env_set.at("SteamAppId"), std::string("2536520"));
    ctx.store = "battlenet";
    ctx.runtime = "steamvr";
    LaunchPlan b = BuildLaunchPlan(reg.Load("d2r-pcvr"), "", ctx);
    CHECK(!b.env_set.count("SteamAppId"));
}

TEST_MAIN()
