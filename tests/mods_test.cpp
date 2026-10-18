// The launcher's view of D2RLoader mods: discovery (manifest, built-in profile, plain mod), D2RLoader checks, runtimes, a start, settings.
#include <algorithm>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <functional>

#include "mods.h"
#include "releases.h"
#include "sandbox.h"
#include "test.h"
#include "versioninfo.h"

using namespace vrml;

namespace {

struct Recording : LaunchBackend {
    std::vector<LaunchPlan> started;
    std::vector<std::map<std::string, std::string>> envs;
    bool fail = false;
    bool Start(const LaunchPlan& p, const std::map<std::string, std::string>& env, std::string* error) override {
        if (fail) {
            if (error) *error = "refused";
            return false;
        }
        started.push_back(p);
        envs.push_back(env);
        return true;
    }
};

void InstallMod(const fs::path& game, const std::string& folder, const std::string& description = "x") {
    tt::Write(game / "mods" / folder / (folder + ".mpq") / "modinfo.json",
              "{ \"name\": \"" + folder + "\", \"savepath\": \"../\", \"description\": \"" + description + "\" }");
}

const ModEntry* Find(const std::vector<ModEntry>& v, const std::string& id) {
    for (const ModEntry& e : v)
        if (e.id == id) return &e;
    return nullptr;
}

// Both folder and modinfo.json of a mod get the day (a mod put in at that day).
void InstalledOn(const fs::path& game, const std::string& folder, int day) {
    InstallMod(game, folder);
    tt::SetCreated(game / "mods" / folder / (folder + ".mpq") / "modinfo.json", day);
    tt::SetCreated(game / "mods" / folder, day);
}

std::string KnownOrder(const std::vector<ModEntry>& mods) {
    std::string s;
    for (const ModEntry& e : mods)
        if (e.known) s += (s.empty() ? "" : ",") + e.id + (e.installed ? "" : "(-)");
    return s;
}

std::string ExampleText(const char* which) {
    return tt::Read(fs::path(VRML_SOURCE_DIR) / "examples" / "d2rloader-mods" / which / "vrmod-manifest.json");
}

}  // namespace

TEST(the_two_builtin_profiles_are_the_example_manifests) {
    const std::vector<Manifest>& b = BuiltinMods();
    CHECK_EQ(b.size(), size_t(2));
    CHECK_EQ(b[0].id, std::string("d2r-vr"));
    CHECK_EQ(b[0].d2rloader_mod, std::string("D2R-VR"));
    CHECK_EQ(b[0].name, std::string("D2R VR Authentic"));
    CHECK_EQ(b[1].id, std::string("d2r-pcvr"));
    CHECK_EQ(b[1].d2rloader_mod, std::string("D2R-PCVR"));
    CHECK_EQ(b[1].name, std::string("D2R VR Unleashed"));
    // the embedded text and the example files must not drift apart
    CHECK(Json::Parse(ExampleText("d2r-vr")) == b[0].raw);
    CHECK(Json::Parse(ExampleText("d2r-pcvr")) == b[1].raw);
}

TEST(the_launch_contracts_of_the_two_mods) {
    const std::vector<Manifest>& b = BuiltinMods();
    const LaunchOption& vr = b[0].launch.at(0);
    CHECK_EQ(vr.exe, std::string("{game}/D2R_VR_Settings.exe"));
    CHECK_EQ(vr.args.size(), size_t(1));
    CHECK_EQ(vr.args[0], std::string("--start"));
    const LaunchOption& pc = b[1].launch.at(0);
    CHECK_EQ(pc.exe, std::string("{game}/D2RLoader.exe"));
    CHECK_EQ(pc.args.size(), size_t(2));
    CHECK_EQ(pc.args[0], std::string("-mod"));
    CHECK_EQ(pc.args[1], std::string("D2R-PCVR"));
}

TEST(nothing_installed_shows_the_known_mods_as_not_installed) {
    tt::Sandbox sb;
    const std::vector<ModEntry> mods = DiscoverMods(sb.game);
    CHECK_EQ(mods.size(), size_t(2));
    CHECK(!mods[0].installed);
    CHECK(!mods[1].installed);
    CHECK_EQ(mods[0].source, std::string("built-in"));
    CHECK(mods[0].known);
}

TEST(an_installed_mod_without_a_manifest_uses_the_builtin_profile) {
    tt::Sandbox sb;
    InstallMod(sb.game, "D2R-PCVR");
    const std::vector<ModEntry> mods = DiscoverMods(sb.game);
    const ModEntry* e = Find(mods, "d2r-pcvr");
    CHECK(e != nullptr);
    CHECK(e->installed);
    CHECK_EQ(e->source, std::string("built-in"));
    CHECK_EQ(e->name, std::string("D2R VR Unleashed"));
    CHECK(e->notes.empty());
    CHECK(!Find(mods, "d2r-vr")->installed);
}

TEST(a_manifest_in_the_mod_folder_wins_and_its_icon_is_found) {
    tt::Sandbox sb;
    InstallMod(sb.game, "D2R-VR");
    Json j = Json::Parse(ExampleText("d2r-vr"));
    j.set("version", Json::String("9.9.9"));
    j.set("name", Json::String("D2R VR Authentic (from the mod folder)"));
    tt::Write(sb.game / "mods" / "D2R-VR" / "vrmod-manifest.json", j.dump());
    tt::Write(sb.game / "mods" / "D2R-VR" / "launcher_icon.png", "PNG");
    const std::vector<ModEntry> mods = DiscoverMods(sb.game);
    const ModEntry* e = Find(mods, "d2r-vr");
    CHECK(e != nullptr);
    CHECK_EQ(e->source, std::string("manifest"));
    CHECK_EQ(e->version, std::string("9.9.9"));
    CHECK_EQ(e->name, std::string("D2R VR Authentic (from the mod folder)"));
    CHECK(!e->icon.empty());
    CHECK(IsFile(e->icon));
}

TEST(a_broken_manifest_falls_back_to_the_builtin_profile_with_a_note) {
    tt::Sandbox sb;
    InstallMod(sb.game, "D2R-VR");
    tt::Write(sb.game / "mods" / "D2R-VR" / "vrmod-manifest.json", "{ not json");
    const ModEntry* e = nullptr;
    const std::vector<ModEntry> mods = DiscoverMods(sb.game);
    e = Find(mods, "d2r-vr");
    CHECK(e != nullptr);
    CHECK_EQ(e->source, std::string("built-in"));
    CHECK(!e->notes.empty());
    // a manifest that names another mod
    Json j = Json::Parse(ExampleText("d2r-pcvr"));
    tt::Write(sb.game / "mods" / "D2R-VR" / "vrmod-manifest.json", j.dump());
    const std::vector<ModEntry> mods2 = DiscoverMods(sb.game);
    e = Find(mods2, "d2r-vr");
    CHECK(e != nullptr);
    CHECK_EQ(e->source, std::string("built-in"));
    CHECK(!e->notes.empty());
}

TEST(another_d2rloader_mod_is_listed_and_can_be_started_with_dash_mod) {
    tt::Sandbox sb;
    InstallMod(sb.game, "Median-XL");
    tt::Write(sb.game / "mods" / "NotAMod" / "readme.txt", "no modinfo.json here");
    const std::vector<ModEntry> mods = DiscoverMods(sb.game);
    const ModEntry* e = Find(mods, "median-xl");
    CHECK(e != nullptr);
    CHECK_EQ(e->source, std::string("plain"));
    CHECK(e->launchable);
    CHECK(!e->known);
    CHECK(Find(mods, "notamod") == nullptr);
    CHECK_EQ(mods.size(), size_t(3));
    const LaunchOption& o = e->manifest.launch.at(0);
    CHECK_EQ(o.args.at(1), std::string("Median-XL"));
}

TEST(d2rloader_is_checked_for_presence_and_for_the_reviewed_files) {
    tt::Sandbox sb;
    LoaderStatus st = CheckLoader(sb.game, true);
    CHECK(st.present);
    CHECK(!st.reviewed);                       // the sandbox's D2RLoader.exe is a stand-in
    CHECK(!st.notes.empty());
    st = CheckLoader(sb.game, false);
    CHECK(st.present);
    CHECK(st.notes.empty());
    RemoveQuiet(sb.game / "D2RLoader.exe");
    st = CheckLoader(sb.game, true);
    CHECK(!st.present);
    CHECK(!st.notes.empty());
}

TEST(the_runtimes_a_start_can_use) {
    tt::Sandbox sb;
    RuntimeRoots roots;
    roots.program_files = sb.root / "pf";
    std::vector<RuntimeChoice> v = ListRuntimes(roots, {});
    CHECK_EQ(v.size(), size_t(4));
    CHECK_EQ(v[0].id, std::string("system"));
    CHECK(v[0].available);
    CHECK(!v[2].available);                    // vdxr: not installed
    tt::Write(roots.program_files / "Virtual Desktop Streamer" / "OpenXR" / "virtualdesktop-openxr.json", "{}");
    v = ListRuntimes(roots, {});
    CHECK_EQ(v[2].id, std::string("vdxr"));
    CHECK(v[2].available);
}

TEST(a_start_refuses_when_it_must_and_says_why) {
    tt::Sandbox sb;
    InstallMod(sb.game, "D2R-PCVR");
    Recording rec;
    RuntimeRoots roots;
    StartRequest req;
    req.game = sb.game;
    req.mod = *Find(DiscoverMods(sb.game), "d2r-pcvr");
    // the game runs
    sb.probe.Start("D2R.exe");
    StartOutcome o = StartMod(req, sb.probe, rec, {}, roots);
    CHECK_EQ(o.code, std::string("E_GAME_RUNNING"));
    CHECK(rec.started.empty());
    sb.probe.Stop("D2R.exe");
    // the mod of the other kind is not installed here
    req.mod = *Find(DiscoverMods(sb.game), "d2r-vr");
    o = StartMod(req, sb.probe, rec, {}, roots);
    CHECK_EQ(o.code, std::string("E_MOD_NOT_INSTALLED"));
    // no D2RLoader
    req.mod = *Find(DiscoverMods(sb.game), "d2r-pcvr");
    RemoveQuiet(sb.game / "D2RLoader.exe");
    o = StartMod(req, sb.probe, rec, {}, roots);
    CHECK_EQ(o.code, std::string("E_LOADER_MISSING"));
    // not a game folder
    req.game = sb.root / "nowhere";
    o = StartMod(req, sb.probe, rec, {}, roots);
    CHECK_EQ(o.code, std::string("E_GAME"));
    CHECK(rec.started.empty());
}

TEST(unleashed_is_started_with_dash_mod_and_its_steam_environment) {
    tt::Sandbox sb;
    InstallMod(sb.game, "D2R-PCVR");
    Recording rec;
    RuntimeRoots roots;
    roots.program_files = sb.root / "pf";
    tt::Write(roots.program_files / "Virtual Desktop Streamer" / "OpenXR" / "virtualdesktop-openxr.json", "{}");
    StartRequest req;
    req.game = sb.game;
    req.mod = *Find(DiscoverMods(sb.game), "d2r-pcvr");
    req.runtime = "vdxr";
    std::map<std::string, std::string> env = {{"PATH", "x"}, {"SteamOverlayGameId", "123"}, {"SteamGameId", "999"}};
    StartOutcome o = StartMod(req, sb.probe, rec, env, roots);
    CHECK_EQ(o.code, std::string(""));
    CHECK(o.started);
    CHECK(!o.warnings.empty());                // the sandbox's D2RLoader is not the reviewed one
    CHECK_EQ(rec.started.size(), size_t(1));
    const LaunchPlan& p = rec.started[0];
    CHECK(EndsWith(p.exe, "D2RLoader.exe"));
    CHECK_EQ(p.args.size(), size_t(2));
    CHECK_EQ(p.args[1], std::string("D2R-PCVR"));
    CHECK_EQ(p.runtime, std::string("vdxr"));
    CHECK_EQ(p.via, std::string("direct"));    // the window never hands over to Explorer, even with Steam's variables in the environment
    const auto& e = rec.envs[0];
    CHECK_EQ(e.at("SteamAppId"), std::string("2536520"));
    CHECK_EQ(e.at("SteamGameId"), std::string("2536520"));
    CHECK(e.count("SteamOverlayGameId") == 0);
    CHECK(e.count("XR_RUNTIME_JSON") > 0);
    CHECK_EQ(e.at("VRMOD_ACTIVE_ID"), std::string("d2r-pcvr"));
}

TEST(authentic_is_started_through_its_settings_program_and_a_missing_runtime_stops_it) {
    tt::Sandbox sb;
    InstallMod(sb.game, "D2R-VR");
    tt::Write(sb.game / "D2R_VR_Settings.exe", "settings");
    Recording rec;
    RuntimeRoots roots;
    roots.program_files = sb.root / "pf";
    StartRequest req;
    req.game = sb.game;
    req.mod = *Find(DiscoverMods(sb.game), "d2r-vr");
    req.runtime = "steamvr";                   // not installed in the sandbox
    StartOutcome o = StartMod(req, sb.probe, rec, {}, roots);
    CHECK_EQ(o.code, std::string("E_RUNTIME"));
    CHECK(rec.started.empty());
    req.runtime = "system";
    o = StartMod(req, sb.probe, rec, {}, roots);
    CHECK(o.started);
    CHECK(EndsWith(rec.started[0].exe, "D2R_VR_Settings.exe"));
    CHECK_EQ(rec.started[0].args.at(0), std::string("--start"));
    CHECK(rec.envs[0].count("XR_RUNTIME_JSON") == 0);   // the system runtime is never overridden
    // a dry run starts nothing
    rec.started.clear();
    req.dry_run = true;
    o = StartMod(req, sb.probe, rec, {}, roots);
    CHECK(!o.started);
    CHECK(rec.started.empty());
    CHECK(!o.plan.exe.empty());
    // a backend that cannot start the process
    req.dry_run = false;
    rec.fail = true;
    o = StartMod(req, sb.probe, rec, {}, roots);
    CHECK_EQ(o.code, std::string("E_IO"));
}

TEST(another_mods_reshade_folder_never_reaches_the_game_that_is_started) {
    // A player case: after trying one mod, RESHADE_BASE_PATH_OVERRIDE stayed in his Windows environment and pointed the other mod's ReShade into the first
    // mod's folder (ReShade64.dll did not load, error 1114, the headset stayed empty). Each mod sets the variable itself, inside its game process.
    const std::string leaked = "C:\\game\\mods\\D2R-PCVR\\reshade";
    for (const char* folder : {"D2R-VR", "D2R-PCVR"}) {
        tt::Sandbox sb;
        InstallMod(sb.game, folder);
        tt::Write(sb.game / "D2R_VR_Settings.exe", "settings");
        Recording rec;
        StartRequest req;
        req.game = sb.game;
        req.mod = *Find(DiscoverMods(sb.game), std::string(folder) == "D2R-VR" ? "d2r-vr" : "d2r-pcvr");
        // the name is compared the way Windows does, in any case
        std::map<std::string, std::string> env = {{"PATH", "x"}, {"Reshade_Base_Path_Override", leaked}, {"RESHADE_DISABLE_LOADING_CHECK", "1"}};
        StartOutcome o = StartMod(req, sb.probe, rec, env, RuntimeRoots());
        CHECK(o.started);
        CHECK_EQ(rec.envs.size(), size_t(1));
        const auto& e = rec.envs[0];
        bool has = false;
        for (const auto& kv : e) has = has || IEquals(kv.first, "RESHADE_BASE_PATH_OVERRIDE");
        CHECK(!has);                                         // not handed on
        CHECK(e.count("RESHADE_DISABLE_LOADING_CHECK") > 0); // other ReShade variables are the player's business: passed on unchanged
        CHECK(e.count("PATH") > 0);
        bool said = false, saidOthers = false;
        for (const std::string& w : o.warnings) {
            said = said || (w.find("RESHADE_BASE_PATH_OVERRIDE is set in your Windows environment") != std::string::npos && w.find(leaked) != std::string::npos);
            saidOthers = saidOthers || w.find("RESHADE_DISABLE_LOADING_CHECK") != std::string::npos;
        }
        CHECK(said);                                         // and the player is told, with the value and how to remove it
        CHECK(saidOthers);
        // the plan lists the removal, a clean environment gets no warning about it
        CHECK(std::find(o.plan.env_unset.begin(), o.plan.env_unset.end(), std::string("RESHADE_BASE_PATH_OVERRIDE")) != o.plan.env_unset.end());
        CHECK(ReShadeEnvironmentNotes({{"PATH", "x"}}).empty());
        CHECK_EQ(ReShadeEnvironmentNotes({{"RESHADE_BASE_PATH_OVERRIDE", "x"}}).size(), size_t(1));
    }
}

TEST(a_manifest_may_set_the_reshade_folder_on_purpose) {
    tt::Sandbox sb;
    InstallMod(sb.game, "D2R-PCVR");
    std::string text = ExampleText("d2r-pcvr");
    const size_t at = text.find("\"env\": [");
    CHECK(at != std::string::npos);
    text.insert(at + 8, "{ \"set\": { \"RESHADE_BASE_PATH_OVERRIDE\": \"{game}/mods/D2R-PCVR/reshade\" } },");
    tt::Write(sb.game / "mods" / "D2R-PCVR" / "vrmod-manifest.json", text);
    Recording rec;
    StartRequest req;
    req.game = sb.game;
    req.mod = *Find(DiscoverMods(sb.game), "d2r-pcvr");
    const StartOutcome o = StartMod(req, sb.probe, rec, {{"RESHADE_BASE_PATH_OVERRIDE", "C:/elsewhere"}}, RuntimeRoots());
    CHECK(o.started);
    const auto& e = rec.envs[0];
    CHECK_EQ(e.count("RESHADE_BASE_PATH_OVERRIDE"), size_t(1));
    CHECK(e.at("RESHADE_BASE_PATH_OVERRIDE").find("mods/D2R-PCVR/reshade") != std::string::npos);   // the mod's own value wins over the inherited one
}

TEST(games_are_found_in_the_steam_libraries_and_the_battlenet_folder) {
    tt::Sandbox sb;
    GameRoots roots;
    roots.steam_libraries = {sb.root / "steam", sb.root / "lib2"};
    roots.battlenet = {sb.root / "bnet", sb.root / "none"};
    tt::Write(sb.root / "steam" / "steamapps" / "common" / "Diablo II Resurrected" / "D2R.exe", "x");
    tt::Write(sb.root / "steam" / "steamapps" / "common" / "Diablo II Resurrected" / "Data" / ".build.config", "x");
    tt::Write(sb.root / "bnet" / "D2R.exe", "x");
    tt::Write(sb.root / "bnet" / ".build.info", "x");
    roots.extra = {sb.root / "bnet"};          // the same folder again: listed once
    const std::vector<GameInstall> v = FindGames(roots);
    CHECK_EQ(v.size(), size_t(2));
    CHECK_EQ(v[0].store, std::string("steam"));
    CHECK_EQ(v[0].source, std::string("Steam"));
    CHECK_EQ(v[1].store, std::string("battlenet"));
}

TEST(an_earlier_global_install_is_pointed_out) {
    tt::Sandbox sb;
    CHECK(GlobalPluginNotes(sb.game).empty());
    InstallMod(sb.game, "D2R-PCVR");
    tt::Write(sb.game / "d2rloader" / "plugins" / "d2rl-vrcam.dll", "an earlier D2R VR");
    CHECK_EQ(GlobalPluginNotes(sb.game).size(), size_t(1));
    const std::vector<ModEntry> mods = DiscoverMods(sb.game);
    CHECK(!Find(mods, "d2r-pcvr")->notes.empty());          // shown on the installed mod
    CHECK(!Find(mods, "d2r-vr")->notes.empty());            // and on the one that is not installed (an earlier install is the reason)
    tt::Write(sb.game / "d2rloader" / "plugins" / "d2rl-pcvr.dll", "an earlier d2r-pcvr");
    CHECK_EQ(GlobalPluginNotes(sb.game).size(), size_t(2));
    // d2r-pcvr up to 0.3.15 had D2R VR's file name: told apart by the cache folder name in its DLL (UTF-16)
    std::string wide;
    for (char c : std::string("d2r_vr_icons")) { wide += c; wide += '\0'; }
    tt::Write(sb.game / "d2rloader" / "plugins" / "d2rl-vrcam.dll", "MZ" + wide);
    const std::vector<std::string> notes = GlobalPluginNotes(sb.game);
    CHECK(notes.at(0).find("D2R VR Unleashed install") != std::string::npos);
    tt::Write(sb.game / "d2rloader" / "plugins" / "d2rl-vrcam.dll", "an earlier D2R VR");
    // and a start says so (the start itself goes on)
    Recording rec;
    StartRequest req;
    req.game = sb.game;
    req.mod = *Find(mods, "d2r-pcvr");
    const StartOutcome o = StartMod(req, sb.probe, rec, {}, RuntimeRoots());
    CHECK(o.started);
    CHECK(o.warnings.size() >= 3);                          // two global plugins and the stand-in D2RLoader
}

TEST(a_file_the_manifest_lists_but_the_folder_lacks_is_pointed_out) {
    tt::Sandbox sb;
    InstallMod(sb.game, "D2R-VR");
    Json j = Json::Parse(ExampleText("d2r-vr"));
    Json files = Json::Array();
    for (const char* name : {"d2rloader/plugins/d2rl-vrcam.dll", "d2rloader/plugins/d2r_vr_weight.dll"}) {
        Json f = Json::Object();
        f.set("path", Json::String(name));
        f.set("sha256", Json::String(std::string(64, 'a')));
        files.push(f);
    }
    Json opt = Json::Object();
    opt.set("path", Json::String("d2rloader/plugins/d2r_vr_grip.dll"));
    opt.set("optional", Json::Bool(true));
    files.push(opt);
    Json evil = Json::Object();
    evil.set("path", Json::String("../../outside.dll"));
    files.push(evil);
    j.set("x-mod-files", files);
    tt::Write(sb.game / "mods" / "D2R-VR" / "vrmod-manifest.json", j.dump());
    tt::Write(sb.game / "mods" / "D2R-VR" / "d2rloader" / "plugins" / "d2rl-vrcam.dll", "plugin");
    const std::vector<ModEntry> mods = DiscoverMods(sb.game);
    const ModEntry* e = Find(mods, "d2r-vr");
    CHECK(e != nullptr);
    CHECK_EQ(e->notes.size(), size_t(1));                  // the weight DLL: listed, not optional, not there
    CHECK(e->notes.at(0).find("d2r_vr_weight.dll") != std::string::npos);
    tt::Write(sb.game / "mods" / "D2R-VR" / "d2rloader" / "plugins" / "d2r_vr_weight.dll", "weight");
    CHECK(Find(DiscoverMods(sb.game), "d2r-vr")->notes.empty());
}

TEST(what_the_launcher_remembers) {
    tt::Sandbox sb;
    LauncherSettings s = LoadSettings(sb.home);
    CHECK(s.game.empty());
    s.game = "D:/Games/D2R";
    s.last_mod = "d2r-pcvr";
    s.mods["d2r-vr"].runtime = "vdxr";
    s.mods["d2r-pcvr"].runtime = "file";
    s.mods["d2r-pcvr"].runtime_file = "C:/rt/x.json";
    SaveSettings(sb.home, s);
    const LauncherSettings t = LoadSettings(sb.home);
    CHECK_EQ(t.game, std::string("D:/Games/D2R"));
    CHECK_EQ(t.last_mod, std::string("d2r-pcvr"));
    CHECK_EQ(t.mods.at("d2r-vr").runtime, std::string("vdxr"));
    CHECK_EQ(t.mods.at("d2r-pcvr").runtime_file, std::string("C:/rt/x.json"));
    tt::Write(sb.home / "settings.json", "garbage");
    CHECK(LoadSettings(sb.home).game.empty());
}

TEST(the_window_remembers_close_on_start) {
    tt::Sandbox sb;
    CHECK(!LoadSettings(sb.home).close_on_start);
    LauncherSettings s;
    s.close_on_start = true;
    SaveSettings(sb.home, s);
    CHECK(LoadSettings(sb.home).close_on_start);
}

TEST(placeholders_in_a_mods_strings_are_expanded) {
    const std::map<std::string, std::string> env = {{"LOCALAPPDATA", "C:/Users/p/AppData/Local"}, {"ProgramFiles", "C:/Program Files"}};
    const fs::path game = P("D:/Games/D2R");
    CHECK_EQ(ExpandModPath("{game}/unins000.exe", game, env), std::string("D:/Games/D2R/unins000.exe"));
    CHECK_EQ(ExpandModPath("{localappdata}/Programs/x/u.exe", game, env), std::string("C:/Users/p/AppData/Local/Programs/x/u.exe"));
    CHECK_EQ(ExpandModPath("{game}|{game}", game, env), std::string("D:/Games/D2R|D:/Games/D2R"));
    CHECK_EQ(ExpandModPath("{appdata}/x", game, env), std::string("{appdata}/x"));      // unknown folder: left as written
    CHECK_EQ(ExpandModPath("plain", game, env), std::string("plain"));
}

TEST(what_the_buttons_of_a_mod_run) {
    tt::Sandbox sb;
    InstallMod(sb.game, "D2R-VR");
    InstallMod(sb.game, "D2R-PCVR");
    InstallMod(sb.game, "Other");
    const std::map<std::string, std::string> env = {{"LOCALAPPDATA", "C:/Users/p/AppData/Local"}};
    const std::vector<ModEntry> mods = DiscoverMods(sb.game);
    const ModTools vr = ResolveModTools(*Find(mods, "d2r-vr"), sb.game, env);
    CHECK_EQ(vr.settings_exe, P(U(sb.game) + "/D2R_VR_Settings.exe"));
    CHECK_EQ(vr.uninstall_exe, P(U(sb.game) + "/unins000.exe"));
    CHECK_EQ(vr.uninstall_args.size(), size_t(1));
    CHECK(vr.uninstall_needs_admin);
    CHECK_EQ(vr.logs.size(), size_t(9));        // the mod's own logs, D2RLoader's, and ReShade's in the mod and (an earlier install's) in the game folder
    CHECK_EQ(vr.logs[0], P(U(sb.game) + "/mods/D2R-VR/d2rloader/logs/d2r-vr-vrcam.log"));
    CHECK_EQ(vr.logs.back(), P(U(sb.game) + "/ReShade.log"));
    CHECK_EQ(vr.settings_label, std::string("D2R VR Authentic Settings"));   // the big button beside Play: the mod's name + " Settings"
    const ModTools pc = ResolveModTools(*Find(mods, "d2r-pcvr"), sb.game, env);
    CHECK(pc.settings_exe.empty());
    CHECK(pc.settings_label.empty());                                       // no settings app: no name (the window shows a greyed "Settings")
    CHECK_EQ(pc.uninstall_exe, P("C:/Users/p/AppData/Local/Programs/D2R PCVR/unins000.exe"));
    const ModTools other = ResolveModTools(*Find(mods, "other"), sb.game, env);
    CHECK(other.settings_exe.empty());
    CHECK(other.uninstall_exe.empty());
    CHECK(other.logs.empty());
}

TEST(the_mod_installed_first_on_this_computer_is_listed_first) {
#ifdef _WIN32
    {   // Authentic first, Unleashed later
        tt::Sandbox sb;
        InstalledOn(sb.game, "D2R-VR", 1);
        InstalledOn(sb.game, "D2R-PCVR", 5);
        const std::vector<ModEntry> mods = DiscoverMods(sb.game);
        CHECK_EQ(KnownOrder(mods), std::string("d2r-vr,d2r-pcvr"));
        CHECK(mods[0].installed_at > 0 && mods[0].installed_at < mods[1].installed_at);
    }
    {   // Unleashed first
        tt::Sandbox sb;
        InstalledOn(sb.game, "D2R-VR", 7);
        InstalledOn(sb.game, "D2R-PCVR", 3);
        CHECK_EQ(KnownOrder(DiscoverMods(sb.game)), std::string("d2r-pcvr,d2r-vr"));
    }
    {   // the same time: Unleashed before Authentic
        tt::Sandbox sb;
        InstalledOn(sb.game, "D2R-VR", 4);
        InstalledOn(sb.game, "D2R-PCVR", 4);
        CHECK_EQ(KnownOrder(DiscoverMods(sb.game)), std::string("d2r-pcvr,d2r-vr"));
    }
    {   // a mod's own update does not move it to the end: modinfo.json is made anew (a new creation time), its folder is the older one
        tt::Sandbox sb;
        InstalledOn(sb.game, "D2R-VR", 1);
        InstalledOn(sb.game, "D2R-PCVR", 5);
        tt::SetCreated(sb.game / "mods" / "D2R-VR" / "D2R-VR.mpq" / "modinfo.json", 9);
        CHECK_EQ(KnownOrder(DiscoverMods(sb.game)), std::string("d2r-vr,d2r-pcvr"));
        // and when only the folder is newer (the file kept by a copy), the file's older time counts
        tt::SetCreated(sb.game / "mods" / "D2R-VR" / "D2R-VR.mpq" / "modinfo.json", 1);
        tt::SetCreated(sb.game / "mods" / "D2R-VR", 9);
        CHECK_EQ(KnownOrder(DiscoverMods(sb.game)), std::string("d2r-vr,d2r-pcvr"));
    }
    {   // the one not installed comes after the installed one, whichever it is
        tt::Sandbox sb;
        InstalledOn(sb.game, "D2R-VR", 2);
        CHECK_EQ(KnownOrder(DiscoverMods(sb.game)), std::string("d2r-vr,d2r-pcvr(-)"));
    }
    {
        tt::Sandbox sb;
        InstalledOn(sb.game, "D2R-PCVR", 2);
        CHECK_EQ(KnownOrder(DiscoverMods(sb.game)), std::string("d2r-pcvr,d2r-vr(-)"));
    }
    {   // nothing installed: Unleashed first, and other D2RLoader mods still come after both
        tt::Sandbox sb;
        InstallMod(sb.game, "Other");
        const std::vector<ModEntry> mods = DiscoverMods(sb.game);
        CHECK_EQ(KnownOrder(mods), std::string("d2r-pcvr(-),d2r-vr(-)"));
        CHECK_EQ(mods.back().id, std::string("other"));
    }
#endif
}

TEST(a_manifest_may_name_its_settings_button) {
    tt::Sandbox sb;
    InstallMod(sb.game, "D2R-VR");
    std::string text = ExampleText("d2r-vr");
    const size_t at = text.find("\"settings_app\": {");
    CHECK(at != std::string::npos);
    text.insert(at + 17, "\"label\": \"Our Own Settings\",");
    tt::Write(sb.game / "mods" / "D2R-VR" / "vrmod-manifest.json", text);
    const std::vector<ModEntry> mods = DiscoverMods(sb.game);
    const ModTools t = ResolveModTools(*Find(mods, "d2r-vr"), sb.game, {});
    CHECK_EQ(t.settings_label, std::string("Our Own Settings"));            // the manifest's word wins over "<name> Settings"
    CHECK(t.settings_exe.string().find("D2R_VR_Settings.exe") != std::string::npos);
}

TEST(a_refusal_has_an_exit_code) {
    CHECK_EQ(StartExitCode(""), 0);
    CHECK_EQ(StartExitCode("E_GAME_RUNNING"), 10);
    CHECK_EQ(StartExitCode("E_MOD_NOT_INSTALLED"), 11);
    CHECK_EQ(StartExitCode("E_LOADER_MISSING"), 11);
    CHECK_EQ(StartExitCode("E_RUNTIME"), 24);
    CHECK_EQ(StartExitCode("E_GAME"), 25);
    CHECK_EQ(StartExitCode("E_IO"), 20);
}

// ================================================================== which version is installed (found_version): manifest, a manifest that does not validate, the plugin's version resource

namespace {

#ifdef VRML_FAKE_PLUGIN_DIR
// A stand-in plugin (tests/tools/fake_plugin.cpp + a version resource) copied to <game>\mods\<folder>\d2rloader\plugins\<as>.
fs::path PutPlugin(const fs::path& game, const std::string& folder, const char* fake, const std::string& as) {
    const fs::path dst = game / "mods" / folder / "d2rloader" / "plugins" / as;
    fs::create_directories(dst.parent_path());
    fs::copy_file(fs::path(VRML_FAKE_PLUGIN_DIR) / (std::string(fake) + ".dll"), dst, fs::copy_options::overwrite_existing);
    return dst;
}
#endif

// vrmod-manifest.json of a known mod at `version`; `edit` changes the parsed JSON first
void PutManifest(const fs::path& game, const std::string& which, const std::string& folder, const std::string& version, const std::function<void(Json&)>& edit = {}) {
    Json j = Json::Parse(ExampleText(which.c_str()));
    j.set("version", Json::String(version));
    if (edit) edit(j);
    tt::Write(game / "mods" / folder / "vrmod-manifest.json", j.dump());
}

// the manifest of a mod from another schema: a field this launcher does not know (the report from the field: /settings_app/label was unknown to an earlier build)
void UnknownField(Json& j) { j.set("not_a_field_of_this_schema", Json::Bool(true)); }

}  // namespace

TEST(the_installed_version_is_the_version_of_the_mods_own_valid_manifest) {
    tt::Sandbox sb;
    ModEntry e = *Find(DiscoverMods(sb.game), "d2r-vr");
    CHECK(!e.installed && e.found_version.empty() && e.found_from.empty());              // not installed: nothing
    InstallMod(sb.game, "D2R-VR");
    e = *Find(DiscoverMods(sb.game), "d2r-vr");
    CHECK(e.installed && e.source == "built-in");
    CHECK(e.found_version.empty() && e.found_from.empty());                              // the built-in profile's version is a guess about the release shipped with the launcher
    CHECK(!e.version.empty());                                                           // (it is still what a launch plan carries)
    PutManifest(sb.game, "d2r-vr", "D2R-VR", "0.170.0");
    e = *Find(DiscoverMods(sb.game), "d2r-vr");
    CHECK_EQ(e.source, std::string("manifest"));
    CHECK_EQ(e.found_version, std::string("0.170.0"));
    CHECK_EQ(e.found_from, std::string("manifest"));
    CHECK_EQ(e.version, std::string("0.170.0"));
    PutManifest(sb.game, "d2r-vr", "D2R-VR", "0.170.0-launcher-test");
    e = *Find(DiscoverMods(sb.game), "d2r-vr");
    CHECK_EQ(e.found_version, std::string("0.170.0-launcher-test"));                     // as written; it is compared by its base version
    CHECK_EQ(NormalizeVersion(e.found_version), std::string("0.170.0"));
    // another D2RLoader mod with a valid manifest has it too; one without has none
    InstallMod(sb.game, "Median-XL");
    CHECK(Find(DiscoverMods(sb.game), "median-xl")->found_version.empty());
}

TEST(a_manifest_that_does_not_validate_still_names_the_installed_version) {
    tt::Sandbox sb;
    InstallMod(sb.game, "D2R-VR");
    PutManifest(sb.game, "d2r-vr", "D2R-VR", "0.170.0", UnknownField);
    ModEntry e = *Find(DiscoverMods(sb.game), "d2r-vr");
    CHECK_EQ(e.source, std::string("built-in"));                                        // the profile for the launch is still the built-in one
    CHECK_EQ(e.found_version, std::string("0.170.0"));
    CHECK_EQ(e.found_from, std::string("manifest (not valid)"));
    bool noted = false;
    for (const std::string& n : e.notes) noted = noted || n.find("not valid") != std::string::npos;
    CHECK(noted);                                                                       // the "not valid" note stays
    CHECK(e.version != "0.170.0");                                                      // `version` is not touched: what a launch plan carries
    // a suffix is kept as written
    PutManifest(sb.game, "d2r-vr", "D2R-VR", "0.169.0-modlayout-candidate", UnknownField);
    e = *Find(DiscoverMods(sb.game), "d2r-vr");
    CHECK_EQ(e.found_version, std::string("0.169.0-modlayout-candidate"));
    // the real report: settings_app.label is not known to an earlier schema
    PutManifest(sb.game, "d2r-vr", "D2R-VR", "0.170.0", [](Json& j) {
        Json app = Json::Object();
        app.set("exe", Json::String("{game}/D2R_VR_Settings.exe"));
        app.set("unheard_of", Json::String("x"));
        j.set("settings_app", app);
    });
    e = *Find(DiscoverMods(sb.game), "d2r-vr");
    CHECK_EQ(e.found_version, std::string("0.170.0"));
    // it must be THIS mod's manifest: Unleashed's in Authentic's folder says nothing about Authentic
    tt::Write(sb.game / "mods" / "D2R-VR" / "vrmod-manifest.json", [&] {
        Json j = Json::Parse(ExampleText("d2r-pcvr"));
        j.set("version", Json::String("0.3.19"));
        j.set("not_a_field_of_this_schema", Json::Bool(true));
        return j.dump();
    }());
    e = *Find(DiscoverMods(sb.game), "d2r-vr");
    CHECK(e.found_version.empty() && e.found_from.empty());
    // not JSON, or no number in the version: nothing
    tt::Write(sb.game / "mods" / "D2R-VR" / "vrmod-manifest.json", "{ not json");
    CHECK(Find(DiscoverMods(sb.game), "d2r-vr")->found_version.empty());
    PutManifest(sb.game, "d2r-vr", "D2R-VR", "latest", UnknownField);
    CHECK(Find(DiscoverMods(sb.game), "d2r-vr")->found_version.empty());
    PutManifest(sb.game, "d2r-vr", "D2R-VR", "0.170.0", [](Json& j) {
        UnknownField(j);
        j.set("id", Json::String("d2r-pcvr"));
    });
    CHECK(Find(DiscoverMods(sb.game), "d2r-vr")->found_version.empty());                 // names another mod's id
}

#ifdef VRML_FAKE_PLUGIN_DIR
TEST(a_version_resource_is_read_with_its_comments) {
    // Bear's real plugin: ProductName "D2R PCVR", ProductVersion "0.3.19", Comments "mod-id=d2r-pcvr; plugin-id=d2r-pcvr-vrcam; Based on D2R VR by BodyWalkVR (MIT).", company bearstonem
    const FileVersion v = ReadFileVersion(fs::path(VRML_FAKE_PLUGIN_DIR) / "fake_plugin_pcvr.dll");
    CHECK(v.ok);
    CHECK_EQ(v.product_version, std::string("0.3.19"));
    CHECK_EQ(v.file_version, std::string("0.3.19"));
    CHECK_EQ(v.product_name, std::string("D2R PCVR"));
    CHECK_EQ(v.company, std::string("bearstonem"));
    CHECK_EQ(v.comments, std::string("mod-id=d2r-pcvr; plugin-id=d2r-pcvr-vrcam; Based on D2R VR by BodyWalkVR (MIT)."));
    CHECK(v.has_fixed && v.product_numbers[0] == 0 && v.product_numbers[1] == 3 && v.product_numbers[2] == 19 && v.product_numbers[3] == 0);
    CHECK_EQ(CommentValue(v.comments, "mod-id"), std::string("d2r-pcvr"));
    CHECK_EQ(CommentValue(v.comments, "plugin-id"), std::string("d2r-pcvr-vrcam"));
    CHECK_EQ(CommentValue(v.comments, "MOD-ID"), std::string("d2r-pcvr"));              // the key is not case sensitive
    CHECK_EQ(CommentValue(v.comments, "edition"), std::string());
    // ours: the pairs are separated by a space
    const FileVersion ours = ReadFileVersion(fs::path(VRML_FAKE_PLUGIN_DIR) / "fake_plugin_vr.dll");
    CHECK(ours.ok);
    CHECK_EQ(ours.comments, std::string("mod-id=d2r-vr edition=full"));
    CHECK_EQ(CommentValue(ours.comments, "mod-id"), std::string("d2r-vr"));
    CHECK_EQ(CommentValue(ours.comments, "edition"), std::string("full"));
    CHECK_EQ(CommentTokens("a=1; b=2,c=3  d").size(), size_t(4));
    // the id must match as a whole token: d2r-vr-extra is not d2r-vr
    CHECK(CommentValue("mod-id=d2r-vr-extra", "mod-id") != "d2r-vr");
    // not a PE file, a missing file, a DLL without Comments
    tt::Sandbox sb;
    tt::Write(sb.root / "not_a_dll.dll", "this is text");
    CHECK(!ReadFileVersion(sb.root / "not_a_dll.dll").ok);
    CHECK(!ReadFileVersion(sb.root / "missing.dll").ok);
    const FileVersion plain = ReadFileVersion(fs::path(VRML_FAKE_PLUGIN_DIR) / "fake_plugin_plain.dll");
    CHECK(plain.ok && plain.comments.empty() && plain.product_version == "1.2.3");
}

TEST(an_installed_mod_without_a_usable_manifest_takes_its_version_from_its_plugin) {
    tt::Sandbox sb;
    InstallMod(sb.game, "D2R-PCVR");
    PutPlugin(sb.game, "D2R-PCVR", "fake_plugin_pcvr", "d2rl-pcvr.dll");
    ModEntry e = *Find(DiscoverMods(sb.game), "d2r-pcvr");
    CHECK_EQ(e.source, std::string("built-in"));
    CHECK_EQ(e.found_version, std::string("0.3.19"));
    CHECK_EQ(e.found_from, std::string("plugin"));
    // an earlier build of his called the file d2rl-vrcam.dll: the mod id in the Comments is what counts, not the file name
    fs::remove(sb.game / "mods" / "D2R-PCVR" / "d2rloader" / "plugins" / "d2rl-pcvr.dll");
    PutPlugin(sb.game, "D2R-PCVR", "fake_plugin_pcvr", "d2rl-vrcam.dll");
    CHECK_EQ(Find(DiscoverMods(sb.game), "d2r-pcvr")->found_version, std::string("0.3.19"));
    // ours
    InstallMod(sb.game, "D2R-VR");
    PutPlugin(sb.game, "D2R-VR", "fake_plugin_vr", "d2rl-vrcam.dll");
    e = *Find(DiscoverMods(sb.game), "d2r-vr");
    CHECK_EQ(e.found_version, std::string("0.170.0"));
    CHECK_EQ(e.found_from, std::string("plugin"));
    // a plugin that names another mod, or none, or is not a DLL at all, says nothing; neither does another mod's plugin in this folder
    fs::remove(sb.game / "mods" / "D2R-VR" / "d2rloader" / "plugins" / "d2rl-vrcam.dll");
    PutPlugin(sb.game, "D2R-VR", "fake_plugin_other", "other.dll");
    PutPlugin(sb.game, "D2R-VR", "fake_plugin_plain", "plain.dll");
    PutPlugin(sb.game, "D2R-VR", "fake_plugin_pcvr", "wrong_mod.dll");                  // Unleashed's DLL in Authentic's folder: mod-id=d2r-pcvr
    tt::Write(sb.game / "mods" / "D2R-VR" / "d2rloader" / "plugins" / "text.dll", "text");
    tt::Write(sb.game / "mods" / "D2R-VR" / "d2rloader" / "plugins" / "d2r_vr.ini", "[x]");
    e = *Find(DiscoverMods(sb.game), "d2r-vr");
    CHECK(e.found_version.empty() && e.found_from.empty());
    // a four-part ProductVersion loses its trailing ".0"; a DLL with only the numeric version resource is read from the numbers; the newest of two wins
    fs::remove(sb.game / "mods" / "D2R-PCVR" / "d2rloader" / "plugins" / "d2rl-vrcam.dll");
    PutPlugin(sb.game, "D2R-PCVR", "fake_plugin_four", "four.dll");
    CHECK_EQ(Find(DiscoverMods(sb.game), "d2r-pcvr")->found_version, std::string("0.3.20"));
    PutPlugin(sb.game, "D2R-PCVR", "fake_plugin_numeric", "numeric.dll");
    CHECK_EQ(Find(DiscoverMods(sb.game), "d2r-pcvr")->found_version, std::string("0.3.21"));
    fs::remove(sb.game / "mods" / "D2R-PCVR" / "d2rloader" / "plugins" / "four.dll");
    PutPlugin(sb.game, "D2R-PCVR", "fake_plugin_pcvr", "d2rl-pcvr.dll");
    CHECK_EQ(Find(DiscoverMods(sb.game), "d2r-pcvr")->found_version, std::string("0.3.21"));      // 0.3.21 (numbers) beats 0.3.19
    // not installed (no modinfo.json): a stray plugin file counts for nothing
    tt::Sandbox empty;
    PutPlugin(empty.game, "D2R-PCVR", "fake_plugin_pcvr", "d2rl-pcvr.dll");
    CHECK(!Find(DiscoverMods(empty.game), "d2r-pcvr")->installed && Find(DiscoverMods(empty.game), "d2r-pcvr")->found_version.empty());
}

TEST(the_order_of_the_places_a_version_is_found) {
    tt::Sandbox sb;
    InstallMod(sb.game, "D2R-PCVR");
    PutPlugin(sb.game, "D2R-PCVR", "fake_plugin_pcvr", "d2rl-pcvr.dll");                 // plugin: 0.3.19
    // a manifest that does not validate beats the plugin
    PutManifest(sb.game, "d2r-pcvr", "D2R-PCVR", "0.3.17", UnknownField);
    ModEntry e = *Find(DiscoverMods(sb.game), "d2r-pcvr");
    CHECK_EQ(e.found_version, std::string("0.3.17"));
    CHECK_EQ(e.found_from, std::string("manifest (not valid)"));
    // a valid manifest beats both
    PutManifest(sb.game, "d2r-pcvr", "D2R-PCVR", "0.3.18");
    e = *Find(DiscoverMods(sb.game), "d2r-pcvr");
    CHECK_EQ(e.found_version, std::string("0.3.18"));
    CHECK_EQ(e.found_from, std::string("manifest"));
    // ... and when the manifest goes, the plugin is left
    fs::remove(sb.game / "mods" / "D2R-PCVR" / "vrmod-manifest.json");
    e = *Find(DiscoverMods(sb.game), "d2r-pcvr");
    CHECK_EQ(e.found_version, std::string("0.3.19"));
    CHECK_EQ(e.found_from, std::string("plugin"));
    // and nothing: unknown
    fs::remove(sb.game / "mods" / "D2R-PCVR" / "d2rloader" / "plugins" / "d2rl-pcvr.dll");
    e = *Find(DiscoverMods(sb.game), "d2r-pcvr");
    CHECK(e.installed && e.found_version.empty() && e.found_from.empty());
}
#endif

TEST_MAIN()
