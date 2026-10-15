// The transaction: plan -> journal -> apply -> verify -> commit, recovery after a crash, refusal while the game runs,
// two mods that write the same file names, update and uninstall of one mod while the other stays.
#include <cstdlib>

#include "engine.h"
#include "sandbox.h"
#include "test.h"

using namespace vrml;
using tt::Read;
using tt::Write;

namespace {

std::string Live(const tt::Sandbox& sb, const std::string& rel) {
    const fs::path p = sb.game / P(rel);
    return IsFile(p) ? Read(p) : std::string("<none>");
}
bool Has(const tt::Sandbox& sb, const std::string& rel) { return IsFile(sb.game / P(rel)); }

const char* kIni = "d2rloader/plugins/d2r_vr.ini";
const char* kDll = "d2rloader/plugins/d2rl-vrcam.dll";
const char* kFx = "reshade-shaders/Shaders/D2R_DepthFog.fx";
const char* kSkyA = "reshade-shaders/Textures/D2R_Sky_ours/sky_a.png";

// the files that belong to no mod: they must be byte-for-byte the same after anything the launcher does
std::map<std::string, std::string> Unrelated(const tt::Sandbox& sb) {
    std::map<std::string, std::string> all = tt::Snapshot(sb.game), out;
    for (const char* k : {"D2R.exe", "Data/.build.config", "D2RLoader.exe", "d2rloader/config/d2rloader.toml", "d2rloader/logs/d2rloader.log",
                          "d2rloader/plugins/other_plugin.dll", "ReShade64.dll", "Saved/Settings.json"})
        out[k] = all[k];
    return out;
}

void TwoMods(tt::Sandbox& sb) {
    tt::Install(sb, tt::AlphaSpec());
    tt::Install(sb, tt::BetaSpec());
}

bool Contains(const std::vector<std::string>& v, const std::string& part) {
    for (const std::string& s : v)
        if (s.find(part) != std::string::npos) return true;
    return false;
}

class RecordingHooks : public HookRunner {
public:
    std::vector<std::string> calls;
    std::string failOn;   // a hook name that fails
    bool Run(const std::string& name, const Manifest& m, const Hook& h, const std::map<std::string, std::string>& env, std::string* msg) override {
        calls.push_back(name + ":" + m.id + ":" + h.exe + ":" + (env.count("VRMOD_ID") ? env.at("VRMOD_ID") : ""));
        if (name == failOn) { *msg = "the hook said no"; return false; }
        return true;
    }
};

}  // namespace

// ------------------------------------------------------------------ the basics

TEST(activate_puts_in_the_complete_setup) {
    tt::Sandbox sb;
    TwoMods(sb);
    const auto before = Unrelated(sb);
    Engine eng = sb.MakeEngine();
    eng.Activate(sb.game, "alpha");
    CHECK_EQ(Live(sb, kDll), std::string("ALPHA-DLL-1.0.0 ALPHA-MARKER"));
    CHECK_EQ(Live(sb, "Alpha_Settings.exe"), std::string("ALPHA-SETTINGS"));
    CHECK_EQ(Live(sb, kFx), std::string("ALPHA-FX-1.0.0"));
    CHECK_EQ(Live(sb, kSkyA), std::string("SKY-A"));
    CHECK_EQ(Live(sb, kIni), std::string("[camera]\nmode=alpha\n"));      // seeded
    CHECK_EQ(Live(sb, "ReShade.ini"), std::string("alpha-reshade\n"));
    CHECK(!Has(sb, "openxr_loader.dll"));
    CHECK(Unrelated(sb) == before);
    GameState st = eng.LoadState(sb.game);
    REQUIRE(st.active.count("vr-plugin") == 1);
    CHECK_EQ(st.active["vr-plugin"].id, std::string("alpha"));
    CHECK(!Exists(eng.Dirs(sb.game).journal));
    CHECK(eng.Verify(sb.game).ok());
}

TEST(switch_swaps_the_whole_setup_and_back) {
    tt::Sandbox sb;
    TwoMods(sb);
    Engine eng = sb.MakeEngine();
    eng.Activate(sb.game, "alpha");
    eng.Activate(sb.game, "beta");
    CHECK_EQ(Live(sb, kDll), std::string("BETA-DLL-2.0.0 BETA-MARKER"));
    CHECK_EQ(Live(sb, "openxr_loader.dll"), std::string("BETA-OPENXR"));     // outside the plugins folder
    CHECK_EQ(Live(sb, kFx), std::string("BETA-FX-2.0.0"));
    CHECK_EQ(Live(sb, kIni), std::string("[camera]\nmode=beta\n"));
    CHECK_EQ(Live(sb, "ReShade.ini"), std::string("beta-reshade\n"));
    CHECK_EQ(Live(sb, "D2R_Frame_ReShade.ini"), std::string("Techniques=beta\n"));
    CHECK(!Has(sb, "Alpha_Settings.exe"));
    CHECK_EQ(Live(sb, kSkyA), std::string("SKY-A"));                           // shared: the same bytes, still there
    CHECK(eng.Verify(sb.game).ok());

    eng.Activate(sb.game, "alpha");
    CHECK_EQ(Live(sb, kDll), std::string("ALPHA-DLL-1.0.0 ALPHA-MARKER"));
    CHECK(!Has(sb, "openxr_loader.dll"));
    CHECK(!Has(sb, "D2R_Frame_ReShade.ini"));
    CHECK_EQ(Live(sb, "Alpha_Settings.exe"), std::string("ALPHA-SETTINGS"));
    CHECK(eng.Verify(sb.game).ok());
    GameState st = eng.LoadState(sb.game);
    CHECK_EQ(st.active.size(), size_t(1));
    CHECK_EQ(st.active["vr-plugin"].id, std::string("alpha"));
}

TEST(each_mods_settings_are_kept_per_mod) {
    tt::Sandbox sb;
    TwoMods(sb);
    Engine eng = sb.MakeEngine();
    eng.Activate(sb.game, "alpha");
    Write(sb.game / kIni, "[camera]\nmode=alpha-tuned\n");
    Write(sb.game / "ReShade.ini", "alpha-tuned-reshade\n");
    eng.Activate(sb.game, "beta");
    CHECK_EQ(Live(sb, kIni), std::string("[camera]\nmode=beta\n"));            // beta starts from its own defaults
    Write(sb.game / kIni, "[camera]\nmode=beta-tuned\n");
    eng.Activate(sb.game, "alpha");
    CHECK_EQ(Live(sb, kIni), std::string("[camera]\nmode=alpha-tuned\n"));
    CHECK_EQ(Live(sb, "ReShade.ini"), std::string("alpha-tuned-reshade\n"));
    eng.Activate(sb.game, "beta");
    CHECK_EQ(Live(sb, kIni), std::string("[camera]\nmode=beta-tuned\n"));
    CHECK_EQ(Live(sb, "ReShade.ini"), std::string("beta-reshade\n"));
}

TEST(unrelated_files_are_never_touched) {
    tt::Sandbox sb;
    TwoMods(sb);
    const auto before = Unrelated(sb);
    Engine eng = sb.MakeEngine();
    for (const char* id : {"alpha", "beta", "alpha", "beta"}) {
        eng.Activate(sb.game, id);
        CHECK(Unrelated(sb) == before);
    }
    eng.Deactivate(sb.game, "beta");
    CHECK(Unrelated(sb) == before);
    CHECK(!Has(sb, kDll));
    CHECK(!Has(sb, "openxr_loader.dll"));
    CHECK(!Has(sb, kIni));
    CHECK(eng.LoadState(sb.game).active.empty());
}

TEST(a_config_without_a_seed_is_captured_and_given_back) {
    tt::Sandbox sb;
    tt::ModSpec s = tt::AlphaSpec();
    s.files.push_back({"payload/user/extra.cfg", "extra.cfg", "config", "", "", {}});   // the package has a file, but no seed
    tt::Install(sb, s);
    tt::Install(sb, tt::BetaSpec());
    Engine eng = sb.MakeEngine();
    eng.Activate(sb.game, "alpha");
    CHECK(!Has(sb, "extra.cfg"));                       // nothing to seed: no file
    Write(sb.game / "extra.cfg", "made by the player");
    eng.Activate(sb.game, "beta");
    CHECK(!Has(sb, "extra.cfg"));
    eng.Activate(sb.game, "alpha");
    CHECK_EQ(Live(sb, "extra.cfg"), std::string("made by the player"));
}

// ------------------------------------------------------------------ files that were already there

TEST(a_foreign_file_in_the_way_is_moved_aside_and_given_back) {
    tt::Sandbox sb;
    TwoMods(sb);
    Write(sb.game / kIni, "the player's own ini from before the launcher\n");
    Write(sb.game / "Alpha_Settings.exe", "some other program with the same name");
    Engine eng = sb.MakeEngine();
    Plan p = eng.PlanActivate(sb.game, "alpha");
    CHECK(Contains(p.warnings, "moved aside"));
    eng.Apply(sb.game, p);
    CHECK_EQ(Live(sb, kIni), std::string("[camera]\nmode=alpha\n"));
    CHECK_EQ(Live(sb, "Alpha_Settings.exe"), std::string("ALPHA-SETTINGS"));
    CHECK(!tt::Snapshot(eng.Dirs(sb.game).foreign).empty());     // the originals are safe in the launcher's folder
    eng.Deactivate(sb.game, "alpha");
    CHECK_EQ(Live(sb, kIni), std::string("the player's own ini from before the launcher\n"));
    CHECK_EQ(Live(sb, "Alpha_Settings.exe"), std::string("some other program with the same name"));
}

TEST(the_foreign_file_waits_through_a_switch) {
    tt::Sandbox sb;
    TwoMods(sb);
    Write(sb.game / kIni, "the player's old ini\n");
    Engine eng = sb.MakeEngine();
    eng.Activate(sb.game, "alpha");
    eng.Activate(sb.game, "beta");                         // beta sits where the same foreign file was
    CHECK_EQ(Live(sb, kIni), std::string("[camera]\nmode=beta\n"));
    eng.Deactivate(sb.game, "beta");
    CHECK_EQ(Live(sb, kIni), std::string("the player's old ini\n"));
}

TEST(an_identical_file_that_was_there_is_not_removed_later) {
    tt::Sandbox sb;
    TwoMods(sb);
    Write(sb.game / "Alpha_Settings.exe", "ALPHA-SETTINGS");     // the same bytes the mod would put in
    Write(sb.game / kSkyA, "SKY-A");
    Engine eng = sb.MakeEngine();
    Plan p = eng.PlanActivate(sb.game, "alpha");
    for (const Op& o : p.ops) CHECK(o.dst.find("Alpha_Settings.exe") == std::string::npos);   // nothing to write
    eng.Apply(sb.game, p);
    eng.Deactivate(sb.game, "alpha");
    CHECK_EQ(Live(sb, "Alpha_Settings.exe"), std::string("ALPHA-SETTINGS"));
    CHECK_EQ(Live(sb, kSkyA), std::string("SKY-A"));
    CHECK(!Has(sb, kDll));
}

TEST(shared_files_stay_when_a_mod_goes) {
    tt::Sandbox sb;
    TwoMods(sb);
    Engine eng = sb.MakeEngine();
    eng.Activate(sb.game, "alpha");
    eng.Deactivate(sb.game, "alpha");
    CHECK_EQ(Live(sb, "reshade-shaders/Textures/D2R_Sky_ours/sky_b.png"), std::string("SKY-B"));
    eng.Activate(sb.game, "beta");
    CHECK(eng.Verify(sb.game).ok());
}

TEST(a_changed_file_is_kept_in_the_backup_when_the_mod_goes) {
    tt::Sandbox sb;
    TwoMods(sb);
    Engine eng = sb.MakeEngine();
    eng.Activate(sb.game, "alpha");
    Write(sb.game / kFx, "someone edited the shader");
    Plan p = eng.PlanActivate(sb.game, "beta");
    CHECK(Contains(p.warnings, "was changed since it was put in"));
    eng.Apply(sb.game, p);
    CHECK(!tt::Snapshot(eng.Dirs(sb.game).backups).empty());
    CHECK_EQ(Live(sb, kFx), std::string("BETA-FX-2.0.0"));
}

// ------------------------------------------------------------------ refusing

TEST(nothing_changes_while_the_game_runs) {
    tt::Sandbox sb;
    TwoMods(sb);
    Engine eng = sb.MakeEngine();
    eng.Activate(sb.game, "alpha");
    const auto g0 = tt::Snapshot(sb.game);

    for (const char* exe : {"D2R.exe", "D2RLoader.exe", "d2rloader.EXE"}) {
        sb.probe.Start(exe);
        CHECK_ERROR("E_GAME_RUNNING", eng.Activate(sb.game, "beta"));
        CHECK_ERROR("E_GAME_RUNNING", eng.Deactivate(sb.game, "alpha"));
        CHECK(tt::Snapshot(sb.game) == g0);
        CHECK(!Exists(eng.Dirs(sb.game).journal));
        sb.probe.Stop(exe);
    }
    // a process only the mod names
    tt::ModSpec s = tt::AlphaSpec("1.0.1");
    s.processes = {"D2R.exe", "D2RLoader.exe", "Alpha_Settings.exe"};
    tt::Install(sb, s);
    Engine eng2 = sb.MakeEngine();
    eng2.Activate(sb.game, "alpha");   // refresh to 1.0.1 works while it is not running
    sb.probe.Start("Alpha_Settings.exe");
    CHECK_ERROR("E_GAME_RUNNING", eng2.Activate(sb.game, "beta"));
    sb.probe.Stop("Alpha_Settings.exe");
    eng2.Activate(sb.game, "beta");
    CHECK_EQ(Live(sb, kDll), std::string("BETA-DLL-2.0.0 BETA-MARKER"));
}

TEST(a_dry_run_changes_nothing) {
    tt::Sandbox sb;
    TwoMods(sb);
    Engine eng = sb.MakeEngine();
    eng.Activate(sb.game, "alpha");
    const auto g0 = tt::Snapshot(sb.game);
    const auto h0 = tt::Snapshot(sb.home);
    Plan p = eng.PlanActivate(sb.game, "beta");
    CHECK(p.ops.size() > 5);
    CHECK(!p.Describe().empty());
    CHECK(tt::Snapshot(sb.game) == g0);
    CHECK(tt::Snapshot(sb.home) == h0);
    sb.probe.Start("D2R.exe");
    (void)eng.PlanActivate(sb.game, "beta");    // planning is allowed, applying is not
}

TEST(a_link_in_the_way_is_refused) {
    tt::Sandbox sb;
    TwoMods(sb);
    const fs::path target = sb.root / "elsewhere";
    fs::create_directories(target);
    fs::path link = sb.game / "reshade-shaders";
    bool made = false;
#ifdef _WIN32
    made = std::system(("cmd /c mklink /J \"" + link.string() + "\" \"" + target.string() + "\" >nul 2>nul").c_str()) == 0;
#else
    std::error_code ec;
    fs::create_directory_symlink(target, link, ec);
    made = !ec;
#endif
    if (!made) { std::cerr << "    (could not make a junction here: skipped)\n"; return; }
    Engine eng = sb.MakeEngine();
    CHECK_ERROR("E_PRECONDITION", eng.PlanActivate(sb.game, "alpha"));
    CHECK(!Has(sb, kDll));
    CHECK(tt::Snapshot(target).empty());
}

TEST(a_mod_for_the_other_store_is_refused) {
    tt::Sandbox sb;
    const fs::path mf = tt::WritePackage(sb.home, tt::AlphaSpec());
    Json j = Json::Parse(tt::Read(mf));
    Json stores = Json::Array();
    stores.push(Json::String("battlenet"));       // the fake game is a Steam one
    j["game"].set("stores", stores);
    Write(mf, j.dump());
    Registry(sb.home, &sb.log).Register(mf);
    Engine eng = sb.MakeEngine();
    CHECK_ERROR("E_PRECONDITION", eng.PlanActivate(sb.game, "alpha"));
}

TEST(a_folder_where_a_file_goes_is_refused) {
    tt::Sandbox sb;
    TwoMods(sb);
    fs::create_directories(sb.game / "Alpha_Settings.exe");
    Engine eng = sb.MakeEngine();
    CHECK_ERROR("E_IO", eng.PlanActivate(sb.game, "alpha"));
}

TEST(not_a_game_folder) {
    tt::Sandbox sb;
    TwoMods(sb);
    fs::create_directories(sb.root / "empty");
    Engine eng = sb.MakeEngine();
    CHECK_ERROR("E_GAME", eng.Activate(sb.root / "empty", "alpha"));
}

TEST(preconditions_of_the_manifest) {
    tt::Sandbox sb;
    tt::ModSpec s = tt::AlphaSpec();
    Json pre = Json::Array();
    pre.push(Json::Parse(R"({"type":"toml_value","file":"{game}/d2rloader/config/d2rloader.toml",
        "key":"d2rloader.advanced.allow_global_extensions","equals":true,"severity":"block","message":"global extensions are off"})"));
    pre.push(Json::Parse(R"({"type":"file_exists","file":"{game}/NotThere.dll","severity":"warn","message":"NotThere.dll would help"})"));
    s.extra.set("preconditions", pre);
    tt::Install(sb, s);
    Engine eng = sb.MakeEngine();
    Plan p = eng.PlanActivate(sb.game, "alpha");
    eng.Apply(sb.game, p);                               // the toml says true: fine; the warning is only a warning
    CHECK(Contains(p.warnings, "NotThere.dll would help"));
    eng.Deactivate(sb.game, "alpha");
    Write(sb.game / "d2rloader/config/d2rloader.toml", "[d2rloader.advanced]\nallow_global_extensions = false\n");
    CHECK_ERROR("E_PRECONDITION", eng.Activate(sb.game, "alpha"));
    CHECK(!Has(sb, kDll));
}

TEST(hooks_run_in_order_and_a_failing_pre_hook_stops_the_switch) {
    tt::Sandbox sb;
    tt::ModSpec a = tt::AlphaSpec(), b = tt::BetaSpec();
    a.extra.set("hooks", Json::Parse(R"({"pre_deactivate":[{"exe":"a-pre-out"}],"post_deactivate":[{"exe":"a-post-out"}],"pre_activate":[{"exe":"a-pre-in"}]})"));
    b.extra.set("hooks", Json::Parse(R"({"pre_activate":[{"exe":"b-pre-in"}],"post_activate":[{"exe":"b-post-in"}]})"));
    tt::Install(sb, a);
    tt::Install(sb, b);
    RecordingHooks hooks;
    Services svc;
    svc.probe = &sb.probe;
    svc.log = &sb.log;
    svc.hooks = &hooks;
    Engine eng(sb.home, svc);
    eng.Activate(sb.game, "alpha");
    hooks.calls.clear();
    eng.Activate(sb.game, "beta");
    REQUIRE(hooks.calls.size() == 4);
    CHECK_EQ(hooks.calls[0], std::string("pre_deactivate:alpha:a-pre-out:alpha"));
    CHECK_EQ(hooks.calls[1], std::string("pre_activate:beta:b-pre-in:beta"));
    CHECK_EQ(hooks.calls[2], std::string("post_deactivate:alpha:a-post-out:alpha"));
    CHECK_EQ(hooks.calls[3], std::string("post_activate:beta:b-post-in:beta"));

    const auto g0 = tt::Snapshot(sb.game);
    hooks.failOn = "pre_activate";
    CHECK_ERROR("E_HOOK", eng.Activate(sb.game, "alpha"));
    CHECK(tt::Snapshot(sb.game) == g0);
    hooks.failOn = "";
    eng.Activate(sb.game, "alpha");
    hooks.failOn = "post_activate";                     // beta's post_activate fails on the way back to beta
    Plan p = eng.PlanActivate(sb.game, "beta");
    eng.Apply(sb.game, p);                              // a failing post hook is a warning, the switch stands
    CHECK(Contains(p.warnings, "the hook said no"));
    CHECK_EQ(Live(sb, kDll), std::string("BETA-DLL-2.0.0 BETA-MARKER"));
}

// ------------------------------------------------------------------ collisions between groups

TEST(two_groups_may_not_own_the_same_file) {
    tt::Sandbox sb;
    TwoMods(sb);
    tt::ModSpec g = tt::BetaSpec();
    g.id = "gamma";
    g.group = "other-group";
    tt::Install(sb, g);
    Engine eng = sb.MakeEngine();
    eng.Activate(sb.game, "beta");
    const auto g0 = tt::Snapshot(sb.game);
    CHECK_ERROR("E_COLLISION", eng.Activate(sb.game, "gamma"));
    CHECK(tt::Snapshot(sb.game) == g0);
}

TEST(two_groups_that_share_nothing_are_both_active) {
    tt::Sandbox sb;
    tt::Install(sb, tt::AlphaSpec());
    tt::ModSpec o;
    o.id = "overlay";
    o.group = "overlays";
    o.files = {{"payload/ov.dll", "d2rloader/plugins/overlay.dll", "program", "OVERLAY", "", {}}};
    tt::Install(sb, o);
    Engine eng = sb.MakeEngine();
    eng.Activate(sb.game, "alpha");
    eng.Activate(sb.game, "overlay");
    CHECK_EQ(eng.LoadState(sb.game).active.size(), size_t(2));
    eng.Activate(sb.game, "alpha");                      // a refresh of alpha leaves the overlay alone
    CHECK_EQ(Live(sb, "d2rloader/plugins/overlay.dll"), std::string("OVERLAY"));
    eng.Deactivate(sb.game, "alpha");
    CHECK_EQ(Live(sb, "d2rloader/plugins/overlay.dll"), std::string("OVERLAY"));
    CHECK(!Has(sb, kDll));
}

TEST(each_game_has_its_own_state) {
    tt::Sandbox sb;
    TwoMods(sb);
    const fs::path game2 = sb.root / "game2";
    fs::copy(sb.game, game2, fs::copy_options::recursive);
    Engine eng = sb.MakeEngine();
    eng.Activate(sb.game, "alpha");
    eng.Activate(game2, "beta");
    CHECK_EQ(eng.LoadState(sb.game).active["vr-plugin"].id, std::string("alpha"));
    CHECK_EQ(eng.LoadState(game2).active["vr-plugin"].id, std::string("beta"));
    CHECK(eng.Dirs(sb.game).root != eng.Dirs(game2).root);
    CHECK_EQ(eng.KnownGames().size(), size_t(2));
    CHECK_EQ(Read(sb.game / kDll), std::string("ALPHA-DLL-1.0.0 ALPHA-MARKER"));
    CHECK_EQ(Read(game2 / kDll), std::string("BETA-DLL-2.0.0 BETA-MARKER"));
}

// ------------------------------------------------------------------ a crash in the middle

namespace {

struct CrashWorld {
    tt::Sandbox sb;
    std::map<std::string, std::string> g0, h0;
    CrashWorld() {
        TwoMods(sb);
        Engine eng = sb.MakeEngine();
        eng.Activate(sb.game, "alpha");
        Write(sb.game / kIni, "[camera]\nmode=alpha-tuned\n");            // settings that must survive
        Write(sb.game / "Foreign.txt", "not ours");
        g0 = tt::Snapshot(sb.game);
        h0 = tt::Snapshot(sb.home);
    }
};

// Runs "switch to beta" with a death at the given point; then the next start: refuse, recover, and check.
// kind: 0 = before op `at`, 1 = after op `at`, 2 = between a copy's temp file and its rename (the `at`-th copy), 3 = commit, 4 = cleanup
void CrashAt(int kind, int at, std::string* label) {
    CrashWorld w;
    int copies = 0;
    auto crash = [&](int op, const char* phase) {
        const std::string ph = phase;
        if ((kind == 0 && ph == "before" && op == at) || (kind == 1 && ph == "after" && op == at) ||
            (kind == 3 && ph == "commit") || (kind == 4 && ph == "cleanup"))
            throw SimulatedCrash();
    };
    if (kind == 2)
        SetAfterTmpHookForTests([&](const fs::path&) {
            if (copies++ == at) throw SimulatedCrash();
        });
    Engine eng = w.sb.MakeEngine(crash);
    bool died = false;
    try {
        eng.Activate(w.sb.game, "beta");
    } catch (const SimulatedCrash&) {
        died = true;
    }
    SetAfterTmpHookForTests({});
    *label = "kind " + std::to_string(kind) + " at " + std::to_string(at);
    if (!died) {   // the point was beyond the work: the switch simply finished
        CHECK(eng.Verify(w.sb.game).ok());
        return;
    }
    // ---- the next start
    Engine next = w.sb.MakeEngine();
    CHECK(Exists(next.Dirs(w.sb.game).journal));
    CHECK_ERROR("E_JOURNAL", next.Activate(w.sb.game, "alpha"));
    CHECK_ERROR("E_JOURNAL", next.Deactivate(w.sb.game, "alpha"));
    CHECK(!next.Verify(w.sb.game).ok());                                   // verify says so too
    w.sb.probe.Start("D2R.exe");
    CHECK_ERROR("E_GAME_RUNNING", next.Recover(w.sb.game));                // not while the game runs
    w.sb.probe.Stop("D2R.exe");
    RecoverReport rep = next.Recover(w.sb.game);
    CHECK(!Exists(next.Dirs(w.sb.game).journal));
    if (kind == 4) {   // the state had been committed
        CHECK(rep.result == RecoverReport::Result::RolledForward);
        CHECK_EQ(next.LoadState(w.sb.game).active["vr-plugin"].id, std::string("beta"));
        CHECK_EQ(Read(w.sb.game / kDll), std::string("BETA-DLL-2.0.0 BETA-MARKER"));
        CHECK(next.Verify(w.sb.game).ok());
        next.Activate(w.sb.game, "alpha");                                  // and the way back works
        CHECK_EQ(Read(w.sb.game / kIni), std::string("[camera]\nmode=alpha-tuned\n"));
        return;
    }
    CHECK(rep.result == RecoverReport::Result::RolledBack);
    const auto g1 = tt::Snapshot(w.sb.game);
    if (g1 != w.g0) std::cerr << "    game folder differs after recovery (" << *label << "):\n" << tt::DiffText(w.g0, g1);
    CHECK(g1 == w.g0);                                                      // the game folder is exactly as it was
    const auto h1 = tt::Snapshot(w.sb.home);
    if (h1 != w.h0) std::cerr << "    launcher folder differs after recovery (" << *label << "):\n" << tt::DiffText(w.h0, h1);
    CHECK(h1 == w.h0);
    CHECK_EQ(next.LoadState(w.sb.game).active["vr-plugin"].id, std::string("alpha"));
    // and the switch can be done now
    next.Activate(w.sb.game, "beta");
    CHECK_EQ(Read(w.sb.game / kDll), std::string("BETA-DLL-2.0.0 BETA-MARKER"));
    CHECK(next.Verify(w.sb.game).ok());
    next.Activate(w.sb.game, "alpha");
    CHECK_EQ(Read(w.sb.game / kIni), std::string("[camera]\nmode=alpha-tuned\n"));   // the tuned settings were never lost
    CHECK_EQ(Read(w.sb.game / "Foreign.txt"), std::string("not ours"));
}

}  // namespace

TEST(a_crash_at_every_point_is_recovered) {
    int ops = 0, copies = 0;
    {
        CrashWorld w;
        Engine eng = w.sb.MakeEngine();
        ops = static_cast<int>(eng.PlanActivate(w.sb.game, "beta").ops.size());
        SetAfterTmpHookForTests([&](const fs::path&) { ++copies; });
        eng.Activate(w.sb.game, "beta");
        SetAfterTmpHookForTests({});
    }
    REQUIRE(ops >= 8);
    REQUIRE(copies >= ops);
    std::string label;
    int points = 0;
    for (int i = 0; i < ops; ++i) { CrashAt(0, i, &label); CrashAt(1, i, &label); points += 2; }
    for (int c = 0; c < copies; ++c) { CrashAt(2, c, &label); ++points; }
    CrashAt(3, 0, &label);
    CrashAt(4, 0, &label);
    points += 2;
    std::cerr << "    " << points << " crash points tried (" << ops << " operations, " << copies << " copies)\n";
}

TEST(an_error_in_the_middle_undoes_itself) {
    for (int failAt = 0; failAt < 12; ++failAt) {
        CrashWorld w;
        Engine eng = w.sb.MakeEngine([&](int op, const char* phase) {
            if (op == failAt && std::string(phase) == "after") throw std::runtime_error("disk full (pretend)");
        });
        bool threw = false;
        try { eng.Activate(w.sb.game, "beta"); } catch (const std::runtime_error&) { threw = true; }
        CHECK(threw);
        CHECK(!Exists(eng.Dirs(w.sb.game).journal));            // it cleaned up after itself - no `recover` needed
        CHECK(tt::Snapshot(w.sb.game) == w.g0);
        CHECK(tt::Snapshot(w.sb.home) == w.h0);
        CHECK_EQ(eng.LoadState(w.sb.game).active["vr-plugin"].id, std::string("alpha"));
    }
}

TEST(a_damaged_payload_is_caught_and_undone) {
    tt::Sandbox sb;
    TwoMods(sb);
    Engine eng = sb.MakeEngine();
    eng.Activate(sb.game, "alpha");
    const auto g0 = tt::Snapshot(sb.game);
    // the package changes between plan and apply (an antivirus eats a file, a disk error): the copy then has another hash
    Plan p = eng.PlanActivate(sb.game, "beta");
    Write(sb.home / "mods" / "beta" / "payload" / "openxr_loader.dll", "CORRUPT");
    CHECK_ERROR("E_HASH", eng.Apply(sb.game, p));
    CHECK(tt::Snapshot(sb.game) == g0);
    CHECK(!Exists(eng.Dirs(sb.game).journal));
}

// ------------------------------------------------------------------ update and uninstall

TEST(updating_the_active_mod_replaces_only_what_changed) {
    tt::Sandbox sb;
    TwoMods(sb);
    Engine eng = sb.MakeEngine();
    eng.Activate(sb.game, "alpha");
    Write(sb.game / kIni, "[camera]\nmode=alpha-tuned\n");

    tt::ModSpec v2 = tt::AlphaSpec("1.1.0");
    v2.files.erase(v2.files.begin() + 1);                                       // the settings exe is gone in 1.1.0
    v2.files.push_back({"payload/New.txt", "New.txt", "asset", "NEW-IN-1.1", "", {}});
    tt::Install(sb, v2);                                                        // the installer: new package + register
    Json st = eng.Status(sb.game);
    CHECK_EQ(st.find("game")->find("active")->find("vr-plugin")->get_string("refresh_pending_to"), std::string("1.1.0"));
    bool pending = false;
    for (const Finding& f : eng.Verify(sb.game).findings) pending = pending || f.code == "REFRESH_PENDING";
    CHECK(pending);

    Plan p = eng.PlanActivate(sb.game, "alpha");
    CHECK_EQ(p.action, std::string("refresh"));
    for (const Op& o : p.ops) {
        CHECK(o.dst.find("sky_") == std::string::npos);                         // unchanged shared files are not rewritten
        CHECK(o.dst.find("d2r_vr.ini") == std::string::npos);                   // settings are not touched
    }
    eng.Apply(sb.game, p);
    CHECK_EQ(Live(sb, kDll), std::string("ALPHA-DLL-1.1.0 ALPHA-MARKER"));
    CHECK_EQ(Live(sb, kFx), std::string("ALPHA-FX-1.1.0"));
    CHECK_EQ(Live(sb, "New.txt"), std::string("NEW-IN-1.1"));
    CHECK(!Has(sb, "Alpha_Settings.exe"));
    CHECK_EQ(Live(sb, kIni), std::string("[camera]\nmode=alpha-tuned\n"));
    CHECK_EQ(eng.LoadState(sb.game).active["vr-plugin"].version, std::string("1.1.0"));
    CHECK(eng.Verify(sb.game).ok());
    CHECK(eng.PlanActivate(sb.game, "alpha").ops.empty());                      // nothing left to do
}

TEST(updating_a_mod_that_is_not_active_leaves_the_game_alone) {
    tt::Sandbox sb;
    TwoMods(sb);
    Engine eng = sb.MakeEngine();
    eng.Activate(sb.game, "beta");
    Write(sb.game / kIni, "[camera]\nmode=beta-tuned\n");
    eng.Activate(sb.game, "alpha");
    const auto g0 = tt::Snapshot(sb.game);
    tt::Install(sb, tt::BetaSpec("2.0.1"));                                     // beta updates while alpha is active
    CHECK(tt::Snapshot(sb.game) == g0);
    CHECK(eng.Verify(sb.game).ok());
    eng.Activate(sb.game, "beta");
    CHECK_EQ(Live(sb, kDll), std::string("BETA-DLL-2.0.1 BETA-MARKER"));       // the new package
    CHECK_EQ(Live(sb, kIni), std::string("[camera]\nmode=beta-tuned\n"));      // its old settings
}

TEST(uninstalling_one_mod_keeps_the_other) {
    tt::Sandbox sb;
    TwoMods(sb);
    Engine eng = sb.MakeEngine();
    eng.Activate(sb.game, "beta");
    Write(sb.game / kIni, "[camera]\nmode=beta-tuned\n");
    eng.Activate(sb.game, "alpha");
    Write(sb.game / kIni, "[camera]\nmode=alpha-tuned\n");
    const auto g0 = tt::Snapshot(sb.game);

    RemoveOptions purge;
    purge.purge_package = true;
    eng.RemoveMod("beta", purge);                                               // beta is not active: its removal does not touch the game
    CHECK(tt::Snapshot(sb.game) == g0);
    CHECK(!eng.registry().Has("beta"));
    CHECK(!IsDir(sb.home / "mods" / "beta"));
    CHECK(eng.Verify(sb.game).ok());
    CHECK_EQ(Live(sb, kIni), std::string("[camera]\nmode=alpha-tuned\n"));

    // beta comes back (a reinstall): its saved settings are still there
    tt::Install(sb, tt::BetaSpec());
    eng.Activate(sb.game, "beta");
    CHECK_EQ(Live(sb, kIni), std::string("[camera]\nmode=beta-tuned\n"));
    eng.Activate(sb.game, "alpha");

    // alpha is active: the removal asks for --force; then the game is as before alpha, and beta is intact
    CHECK_ERROR("E_IN_USE", eng.RemoveMod("alpha", RemoveOptions()));
    RemoveOptions force;
    force.force = true;
    force.purge_package = true;
    eng.RemoveMod("alpha", force);
    CHECK(!Has(sb, kDll));
    CHECK(!Has(sb, "Alpha_Settings.exe"));
    CHECK(!Has(sb, kIni));
    CHECK(eng.LoadState(sb.game).active.empty());
    eng.Activate(sb.game, "beta");
    CHECK_EQ(Live(sb, kIni), std::string("[camera]\nmode=beta-tuned\n"));
    CHECK(eng.Verify(sb.game).ok());
    // a removal refuses while the game runs
    sb.probe.Start("D2RLoader.exe");
    CHECK_ERROR("E_GAME_RUNNING", eng.RemoveMod("beta", force));
    CHECK(eng.registry().Has("beta"));
}

TEST(removing_with_purge_settings_forgets_the_saved_settings) {
    tt::Sandbox sb;
    TwoMods(sb);
    Engine eng = sb.MakeEngine();
    eng.Activate(sb.game, "beta");
    Write(sb.game / kIni, "[camera]\nmode=beta-tuned\n");
    eng.Activate(sb.game, "alpha");
    RemoveOptions o;
    o.purge_package = true;
    o.purge_settings = true;
    eng.RemoveMod("beta", o);
    tt::Install(sb, tt::BetaSpec());
    eng.Activate(sb.game, "beta");
    CHECK_EQ(Live(sb, kIni), std::string("[camera]\nmode=beta\n"));            // defaults again
}

// ------------------------------------------------------------------ verify and import

TEST(verify_sees_what_was_changed_behind_its_back) {
    tt::Sandbox sb;
    TwoMods(sb);
    Engine eng = sb.MakeEngine();
    eng.Activate(sb.game, "alpha");
    CHECK(eng.Verify(sb.game).ok());
    Write(sb.game / kDll, "the other mod's installer was here");
    fs::remove(sb.game / kFx);
    fs::remove(sb.game / kIni);
    VerifyReport r = eng.Verify(sb.game);
    CHECK(!r.ok());
    bool modified = false, missing = false, cfg = false;
    for (const Finding& f : r.findings) {
        if (f.code == "MODIFIED") modified = true;
        if (f.code == "MISSING") missing = true;
        if (f.code == "CONFIG_MISSING") { cfg = true; CHECK_EQ(f.level, std::string("warn")); }
    }
    CHECK(modified);
    CHECK(missing);
    CHECK(cfg);
    eng.Activate(sb.game, "alpha");                          // "activate" repairs
    CHECK(eng.Verify(sb.game).ok());
    CHECK_EQ(Live(sb, kDll), std::string("ALPHA-DLL-1.0.0 ALPHA-MARKER"));
}

TEST(verify_notices_a_mod_installed_without_the_launcher) {
    tt::Sandbox sb;
    TwoMods(sb);
    Write(sb.game / kDll, "ALPHA-DLL-1.0.0 ALPHA-MARKER");     // an old-style installer put alpha's plugin here
    Engine eng = sb.MakeEngine();
    VerifyReport r = eng.Verify(sb.game);
    bool found = false;
    for (const Finding& f : r.findings)
        if (f.code == "UNMANAGED_MOD_FILES" && f.message.find("\"alpha\"") != std::string::npos) found = true;
    CHECK(found);
}

namespace {
tt::ModSpec AlphaWithBackups() {
    tt::ModSpec s = tt::AlphaSpec();
    s.extra.set("import", Json::Parse(R"({"backups":{"globs":["{game}/d2rloader/plugins/*.before_*"]}})"));
    return s;
}
void LegacyAlpha(tt::Sandbox& sb) {
    Write(sb.game / kDll, "ALPHA-DLL-0.9.0 ALPHA-MARKER");     // an older build
    Write(sb.game / "Alpha_Settings.exe", "ALPHA-SETTINGS");
    Write(sb.game / kFx, "ALPHA-FX-0.9.0");
    Write(sb.game / kIni, "[camera]\nmode=the-players-own-tuning\n");
    Write(sb.game / "ReShade.ini", "alpha-reshade-tuned\n");
    Write(sb.game / "d2rloader/plugins/d2r_vr.ini.before_test1", "an older ini");
    Write(sb.game / "d2rloader/plugins/d2rl-vrcam.dll.before_bear", "an older dll");
}
}  // namespace

TEST(import_takes_over_an_installation_made_before_the_launcher) {
    tt::Sandbox sb;
    tt::Install(sb, AlphaWithBackups());
    tt::Install(sb, tt::BetaSpec());
    LegacyAlpha(sb);
    const auto g0 = tt::Snapshot(sb.game);
    Engine eng = sb.MakeEngine();
    ImportReport r = eng.Import(sb.game, "alpha", false);
    CHECK(tt::Snapshot(sb.game) == g0);                                         // importing moves nothing in the game folder
    CHECK_EQ(r.adopted, 3);                                                      // the plugin, the settings exe, the shader
    CHECK_EQ(r.missing, 2);                                                      // the two sky pictures are not there (activate puts them in)
    CHECK_EQ(r.captured, 2);                                                     // the ini and ReShade.ini
    CHECK_EQ(r.backups, 2);                                                      // the two .before_* copies
    CHECK(Exists(eng.Dirs(sb.game).imports / "alpha" / "backups" / "d2rloader" / "plugins" / "d2r_vr.ini.before_test1"));
    CHECK_EQ(eng.LoadState(sb.game).active["vr-plugin"].id, std::string("alpha"));
    CHECK(eng.Verify(sb.game).ok());     // an older build than the package: what was adopted is what is there
    eng.Activate(sb.game, "beta");
    CHECK_EQ(Live(sb, kIni), std::string("[camera]\nmode=beta\n"));
    eng.Activate(sb.game, "alpha");
    CHECK_EQ(Live(sb, kIni), std::string("[camera]\nmode=the-players-own-tuning\n"));   // the player's tuning came through the round trip
    CHECK_EQ(Live(sb, "ReShade.ini"), std::string("alpha-reshade-tuned\n"));
    CHECK_EQ(Live(sb, kDll), std::string("ALPHA-DLL-1.0.0 ALPHA-MARKER"));
}

TEST(import_refuses_a_plugin_that_is_not_this_mods) {
    tt::Sandbox sb;
    tt::Install(sb, tt::AlphaSpec());
    tt::Install(sb, tt::BetaSpec());
    Write(sb.game / kDll, "BETA-DLL-2.0.0 BETA-MARKER");        // beta's plugin is in the folder
    Engine eng = sb.MakeEngine();
    CHECK_ERROR("E_IDENTITY", eng.Import(sb.game, "alpha", false));
    ImportReport ok = eng.Import(sb.game, "beta", false);       // the right one is taken
    CHECK(ok.adopted >= 1);
    CHECK_ERROR("E_IN_USE", eng.Import(sb.game, "alpha", true));  // beta holds the group now
}

TEST(import_without_the_plugin_has_nothing_to_take) {
    tt::Sandbox sb;
    tt::Install(sb, tt::AlphaSpec());
    Engine eng = sb.MakeEngine();
    CHECK_ERROR("E_IDENTITY", eng.Import(sb.game, "alpha", false));
}

TEST(import_collects_backups_named_by_a_record_file) {
    tt::Sandbox sb;
    tt::ModSpec s = tt::BetaSpec();
    s.extra.set("import", Json::Parse(R"({"backups":{"record":{"file":"{game}/d2rloader/pcvr-install.json","paths_at":"/snapshots"}}})"));
    tt::Install(sb, s);
    const fs::path snap1 = sb.root / "helper" / "setup-backups" / "game" / "20261013-100000";
    const fs::path snap2 = sb.root / "helper" / "setup-backups" / "game" / "20261014-100000";
    Write(snap1 / "manifest.json", "{}");
    Write(snap1 / "previous" / "d2rloader" / "plugins" / "d2r_vr.ini", "the ini before his first install");
    Write(snap2 / "manifest.json", "{}");
    Json rec = Json::Object();
    Json arr = Json::Array();
    arr.push(Json::String(U(snap1)));
    arr.push(Json::String(U(snap2)));
    arr.push(Json::String(U(sb.root / "helper" / "gone")));       // a backup that was deleted since
    rec.set("snapshots", arr);
    Write(sb.game / "d2rloader" / "pcvr-install.json", rec.dump());
    Write(sb.game / kDll, "BETA-DLL-2.0.0 BETA-MARKER");
    Engine eng = sb.MakeEngine();
    ImportReport r = eng.Import(sb.game, "beta", false);
    CHECK_EQ(r.backups, 2);
    CHECK(Exists(eng.Dirs(sb.game).imports / "beta" / "record" / "0" / "previous" / "d2rloader" / "plugins" / "d2r_vr.ini"));
    CHECK(Exists(snap1 / "manifest.json"));                         // the originals stay where they are
    bool noted = false;
    for (const std::string& n : r.notes) noted = noted || n.find("is not there any more") != std::string::npos;
    CHECK(noted);
}

// ------------------------------------------------------------------ status, retention, log

TEST(status_names_the_active_mod_the_build_and_the_loader) {
    tt::Sandbox sb;
    TwoMods(sb);
    Engine eng = sb.MakeEngine();
    eng.Activate(sb.game, "beta");
    Json st = eng.Status(sb.game);
    const Json* g = st.find("game");
    REQUIRE(g != nullptr);
    CHECK_EQ(g->get_string("store"), std::string("steam"));
    CHECK_EQ(g->get_string("build"), std::string("3.3.93787"));
    CHECK_EQ(g->get_string("d2rloader"), std::string("1.3.1-beta"));
    CHECK_EQ(g->find("active")->find("vr-plugin")->get_string("id"), std::string("beta"));
    CHECK_EQ(g->find("active")->find("vr-plugin")->get_string("version"), std::string("2.0.0"));
    CHECK_EQ(st.find("mods")->items().size(), size_t(2));
}

TEST(the_log_says_what_happened) {
    tt::Sandbox sb;
    TwoMods(sb);
    Engine eng = sb.MakeEngine();
    eng.Activate(sb.game, "alpha");
    eng.Activate(sb.game, "beta");
    CHECK(Contains(sb.log.tail(), "activate alpha"));
    CHECK(Contains(sb.log.tail(), "activate beta (from alpha)"));
    CHECK(Contains(sb.log.tail(), "committed"));
}

TEST(only_the_last_backups_are_kept) {
    tt::Sandbox sb;
    TwoMods(sb);
    Engine eng = sb.MakeEngine();
    for (int i = 0; i < 6; ++i) eng.Activate(sb.game, i % 2 ? "alpha" : "beta");
    int dirs = 0;
    for (const auto& e : fs::directory_iterator(eng.Dirs(sb.game).backups)) dirs += e.is_directory();
    CHECK(dirs <= 3);
    CHECK(dirs >= 1);
}

TEST_MAIN()
