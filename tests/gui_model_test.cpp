// What the launcher window decides, without the window: the game list, the preselected mod, the runtime per mod, what Play may do and why not,
// what it remembers, the status line, the command line. (The window itself is checked by tools/ui_smoke.py.)
#include "fakes.h"
#include "gui_model.h"
#include "sandbox.h"
#include "test.h"

using namespace vrml;
using namespace vrml::gui;

namespace {

struct Recording : LaunchBackend {
    std::vector<LaunchPlan> started;
    bool Start(const LaunchPlan& p, const std::map<std::string, std::string>&, std::string*) override {
        started.push_back(p);
        return true;
    }
};

// A model over a sandbox: no machine-wide search, a stand-in process list and backend.
struct Rig {
    tt::Sandbox sb;
    Recording backend;
    Machine svc;
    Options opt;
    Rig() {
        svc.probe = &sb.probe;
        svc.backend = &backend;
        opt.game = U(sb.game);
        opt.home = U(sb.home);
    }
    // `day` >= 0: the mod's folder and modinfo.json were created that day (the list is ordered by it: the mod installed first is first)
    void Install(const std::string& folder, int day = -1) {
        tt::Write(sb.game / "mods" / folder / (folder + ".mpq") / "modinfo.json", "{ \"name\": \"" + folder + "\", \"savepath\": \"../\" }");
        if (day >= 0) {
            tt::SetCreated(sb.game / "mods" / folder / (folder + ".mpq") / "modinfo.json", day);
            tt::SetCreated(sb.game / "mods" / folder, day);
        }
        if (folder == "D2R-VR") tt::Write(sb.game / "D2R_VR_Settings.exe", "settings");     // what Authentic's Play starts
    }
    Model Make() { return Model(opt, svc); }
};

}  // namespace

TEST(the_command_line) {
    Options o = ParseOptions({"--game", "D:/g", "--home", "C:/h", "--select", "d2r-pcvr", "--dry-run", "--play", "--screenshot", "x.png", "--dpi", "144"});
    CHECK_EQ(o.game, std::string("D:/g"));
    CHECK_EQ(o.home, std::string("C:/h"));
    CHECK_EQ(o.select, std::string("d2r-pcvr"));
    CHECK_EQ(o.screenshot, std::string("x.png"));
    CHECK(o.dry_run);
    CHECK(o.play);
    CHECK_EQ(o.dpi, 144);
    CHECK(o.problems.empty());
    o = ParseOptions({"--bogus", "--game"});
    CHECK_EQ(o.problems.size(), size_t(2));
    o = ParseOptions({"--dpi", "5"});
    CHECK_EQ(o.dpi, 0);
    CHECK_EQ(o.problems.size(), size_t(1));
    CHECK(ParseOptions({"--help"}).help);
    o = ParseOptions({"--size", "560x600", "--no-search", "--processes", "D2R.exe,D2RLoader.exe"});
    CHECK_EQ(o.width_dp, 560);
    CHECK_EQ(o.height_dp, 600);
    CHECK(o.no_search);
    CHECK(o.fake_processes);
    CHECK_EQ(o.processes.size(), size_t(2));
    o = ParseOptions({"--processes", "-"});
    CHECK(o.fake_processes && o.processes.empty());          // "-": a process list with nothing in it
    o = ParseOptions({"--size", "banana"});
    CHECK_EQ(o.problems.size(), size_t(1));
}

TEST(the_first_installed_mod_is_preselected_else_the_first_known) {
    Rig r;
    {
        Model m = r.Make();
        CHECK_EQ(m.mods().size(), size_t(2));       // both known mods are listed, installed or not
        CHECK_EQ(m.mods()[0].id, std::string("d2r-pcvr"));   // nothing installed: Unleashed is listed before Authentic
        CHECK_EQ(m.mod()->id, std::string("d2r-pcvr"));      // ... and the first one is preselected
        CHECK(!m.mod()->installed);
        CHECK(m.first_run());
    }
    r.Install("D2R-PCVR", 5);
    Model m = r.Make();
    CHECK_EQ(m.mod()->id, std::string("d2r-pcvr"));  // the only installed one
    r.Install("D2R-VR", 1);
    Model both = r.Make();
    CHECK_EQ(both.mods()[0].id, std::string("d2r-vr"));   // installed first on this computer: first in the list
    CHECK_EQ(both.mod()->id, std::string("d2r-vr"));      // and preselected on a first start
    tt::SetCreated(r.sb.game / "mods" / "D2R-VR" / "D2R-VR.mpq" / "modinfo.json", 9);   // an update that makes modinfo.json anew does not move it
    Model after = r.Make();
    CHECK_EQ(after.mods()[0].id, std::string("d2r-vr"));
}

TEST(the_last_used_mod_is_preselected_and_select_wins_over_it) {
    Rig r;
    r.Install("D2R-VR");
    r.Install("D2R-PCVR");
    LauncherSettings s;
    s.last_mod = "d2r-pcvr";
    SaveSettings(r.sb.home, s);
    {
        Model m = r.Make();
        CHECK_EQ(m.mod()->id, std::string("d2r-pcvr"));
        CHECK(!m.first_run());
    }
    r.opt.select = "D2R-VR";                         // the folder name, any case
    Model m = r.Make();
    CHECK_EQ(m.mod()->id, std::string("d2r-vr"));
    r.opt.select = "nope";
    Model bad = r.Make();
    CHECK_EQ(bad.startup_notes().size(), size_t(1));
    CHECK_EQ(bad.mod()->id, std::string("d2r-pcvr"));  // falls back to the last used
}

TEST(a_last_mod_that_is_gone_is_not_preselected) {
    Rig r;
    r.Install("D2R-VR");
    LauncherSettings s;
    s.last_mod = "d2r-pcvr";                          // not installed any more
    SaveSettings(r.sb.home, s);
    Model m = r.Make();
    CHECK_EQ(m.mod()->id, std::string("d2r-vr"));
}

TEST(other_d2rloader_mods_are_listed_after_the_known_ones) {
    Rig r;
    r.Install("D2R-VR");
    r.Install("SomeOtherMod");
    Model m = r.Make();
    CHECK_EQ(m.mods().size(), size_t(3));
    CHECK(m.mods()[0].known);
    CHECK(m.mods()[1].known);
    CHECK(!m.mods()[2].known);
    CHECK_EQ(m.mods()[2].folder, std::string("SomeOtherMod"));
}

TEST(why_play_is_not_possible) {
    Rig r;
    Model m = r.Make();
    CHECK(!m.CanPlay(false).can);                    // Authentic is not installed
    CHECK(m.CanPlay(false).reason.find("not installed") != std::string::npos);
    r.Install("D2R-VR");
    Model ok = r.Make();
    CHECK(ok.CanPlay(false).can);
    CHECK(ok.CanPlay(false).reason.empty());
    const PlayState running = ok.CanPlay(true);
    CHECK(!running.can);
    CHECK_EQ(running.reason, std::string("The game is running"));
    RemoveQuiet(r.sb.game / "D2RLoader.exe");
    Model noloader = r.Make();
    CHECK(!noloader.CanPlay(false).can);
    CHECK(noloader.CanPlay(false).reason.find("D2RLoader") != std::string::npos);
    r.opt.game = U(r.sb.root / "nothing");
    Model notgame = r.Make();
    CHECK(!notgame.CanPlay(false).can);
    CHECK(notgame.mods().empty());
}

TEST(the_running_game_is_seen_through_the_probe) {
    Rig r;
    r.Install("D2R-VR");
    Model m = r.Make();
    CHECK(!m.GameRunning());
    r.sb.probe.Start("D2RLoader.exe");
    CHECK(m.GameRunning());
    r.sb.probe.Stop("D2RLoader.exe");
    r.sb.probe.Start("d2r.exe");
    CHECK(m.GameRunning());
}

TEST(play_remembers_what_was_used_and_a_dry_run_or_a_refusal_does_not) {
    Rig r;
    r.Install("D2R-VR");
    r.Install("D2R-PCVR");
    r.opt.select = "d2r-pcvr";
    {
        Model m = r.Make();
        const StartOutcome dry = m.Play(true);
        CHECK(dry.code.empty());
        CHECK(!dry.started);
        CHECK(r.backend.started.empty());
        CHECK(!IsFile(r.sb.home / "settings.json"));
        r.sb.probe.Start("D2R.exe");
        const StartOutcome refused = m.Play(false);
        CHECK_EQ(refused.code, std::string("E_GAME_RUNNING"));
        CHECK(!IsFile(r.sb.home / "settings.json"));
        r.sb.probe.Stop("D2R.exe");
        const StartOutcome so = m.Play(false);
        CHECK(so.started);
        CHECK_EQ(r.backend.started.size(), size_t(1));
        CHECK(EndsWith(r.backend.started[0].exe, "D2RLoader.exe"));
    }
    const LauncherSettings s = LoadSettings(r.sb.home);
    CHECK_EQ(s.last_mod, std::string("d2r-pcvr"));
    CHECK(s.game.empty());                           // --game given: the remembered folder is not touched
    CHECK_EQ(s.mods.at("d2r-pcvr").runtime, std::string("system"));
}

TEST(play_remembers_the_game_folder_when_it_was_found_not_given) {
    Rig r;
    r.Install("D2R-VR");
    r.opt.game.clear();
    r.svc.game_roots.extra = {r.sb.game};            // "found"
    Model m = r.Make();
    REQUIRE(m.game());
    CHECK(m.Play(false).started);
    CHECK_EQ(LoadSettings(r.sb.home).game, U(r.sb.game));
}

TEST(the_runtime_is_remembered_per_mod_and_a_gone_one_falls_back) {
    Rig r;
    r.Install("D2R-VR");
    r.Install("D2R-PCVR");
    tt::Write(r.sb.root / "rt" / "my_runtime.json", "{}");
    Model m = r.Make();
    REQUIRE(m.mod());
    CHECK_EQ(m.runtimes()[static_cast<size_t>(m.runtime_index())].id, std::string("system"));
    int file = -1, steamvr = -1;
    for (size_t i = 0; i < m.runtimes().size(); ++i) {
        if (m.runtimes()[i].id == "file") file = static_cast<int>(i);
        if (m.runtimes()[i].id == "steamvr") steamvr = static_cast<int>(i);
    }
    REQUIRE(file >= 0);
    std::string why;
    CHECK(!m.SetRuntime(file, r.sb.root / "rt" / "missing.json", &why));       // a file that is not there
    CHECK(!why.empty());
    CHECK(m.SetRuntime(file, r.sb.root / "rt" / "my_runtime.json", &why));
    CHECK_EQ(m.runtime_index(), file);
    m.SelectMod(1);                                                               // Unleashed: its own choice, still the default
    CHECK_EQ(m.runtimes()[static_cast<size_t>(m.runtime_index())].id, std::string("system"));
    m.SelectMod(0);
    CHECK_EQ(m.runtime_index(), file);
    // the plan of a start uses it
    const StartOutcome so = m.Play(true);
    CHECK_EQ(so.plan.runtime, std::string("file"));
    CHECK_EQ(so.plan.env_set.count("XR_RUNTIME_JSON"), size_t(1));
    // a runtime that is not installed here cannot be chosen
    if (steamvr >= 0 && !m.runtimes()[static_cast<size_t>(steamvr)].available) {
        CHECK(!m.SetRuntime(steamvr, {}, &why));
        CHECK(why.find("SteamVR") != std::string::npos);
    }
    // the runtime file is deleted: the model shows the default again (the remembered text stays until the player chooses)
    RemoveQuiet(r.sb.root / "rt" / "my_runtime.json");
    CHECK_EQ(m.runtimes()[static_cast<size_t>(m.runtime_index())].id, std::string("system"));
}

TEST(browsing_adds_only_a_game_folder) {
    Rig r;
    r.opt.game.clear();
    Model m = r.Make();
    std::string error;
    CHECK_EQ(m.AddGame(r.sb.root / "empty", &error), -1);
    CHECK(error.find("D2R.exe") != std::string::npos);
    r.Install("D2R-VR");
    const int i = m.AddGame(r.sb.game, &error);
    CHECK_EQ(i, 0);
    CHECK_EQ(m.game_index(), 0);
    CHECK(m.game()->is_game);
    CHECK_EQ(m.mods().size(), size_t(2));
    CHECK_EQ(m.AddGame(r.sb.game, &error), 0);        // the same folder again: not listed twice
    CHECK_EQ(m.games().size(), size_t(1));
    CHECK_EQ(m.settings().game, U(r.sb.game));        // and it is the remembered one
}

TEST(a_given_folder_that_is_not_a_game_is_listed_and_named_as_such) {
    Rig r;
    r.opt.game = U(r.sb.root);                         // no D2R.exe here
    Model m = r.Make();
    REQUIRE(m.game());
    CHECK(!m.game()->is_game);
    Tone tone = Tone::Normal;
    const std::string line = m.GameLine(&tone);
    CHECK(tone == Tone::Error);
    CHECK(line.find("no D2R.exe") != std::string::npos);
}

TEST(the_game_line_tells_edition_build_and_loader) {
    Rig r;
    Model m = r.Make();
    Tone tone = Tone::Normal;
    std::string line = m.GameLine(&tone);
    CHECK_EQ(line, std::string("Steam edition - build 3.3.93787 - D2RLoader found, checking its files..."));
    CHECK(tone == Tone::Normal);
    LoaderStatus st = CheckLoader(r.sb.game, true);        // the sandbox's D2RLoader.exe is a stand-in: not the reviewed files
    m.SetLoaderStatus(st);
    line = m.GameLine(&tone);
    CHECK(line.find("(not the reviewed files)") != std::string::npos);
    CHECK(tone == Tone::Warn);
    st.reviewed = true;
    st.version = "1.3.1-beta";
    m.SetLoaderStatus(st);
    CHECK_EQ(m.GameLine(&tone), std::string("Steam edition - build 3.3.93787 - D2RLoader 1.3.1-beta (reviewed files)"));
    CHECK(tone == Tone::Normal);
    RemoveQuiet(r.sb.game / "D2RLoader.exe");
    Model gone = r.Make();
    CHECK(gone.GameLine(&tone).find("D2RLoader is missing") != std::string::npos);
    CHECK(tone == Tone::Warn);
}

TEST(no_game_at_all_is_said_in_words) {
    Rig r;
    r.opt.game.clear();
    Model m = r.Make();
    Tone tone = Tone::Normal;
    CHECK(m.game() == nullptr);
    CHECK(m.GameLine(&tone).find("Browse") != std::string::npos);
    CHECK(tone == Tone::Warn);
    CHECK(!m.CanPlay(false).can);
    const StartOutcome so = m.Play(false);
    CHECK_EQ(so.code, std::string("E_GAME"));
}

TEST(a_mod_installed_while_the_window_is_open_is_seen) {
    Rig r;
    Model m = r.Make();
    CHECK(m.SelectModById("d2r-vr"));                 // (nothing installed: the preselected one would be Unleashed, the first in the list)
    CHECK(!m.Refresh());                              // nothing changed
    r.Install("D2R-VR");
    CHECK(m.Refresh());
    CHECK(m.mod()->installed);
    CHECK(!m.Refresh());
    r.Install("Another");
    CHECK(m.Refresh());
    CHECK_EQ(m.mods().size(), size_t(3));
    CHECK_EQ(m.mod()->id, std::string("d2r-vr"));     // the selection stays on the same mod
}

TEST(the_outcome_text_of_a_headless_play) {
    Rig r;
    r.Install("D2R-VR");
    Model m = r.Make();
    int code = -1;
    std::string text = FormatOutcome(m.Play(true), true, &code);
    CHECK_EQ(code, 0);
    CHECK(text.find("D2R_VR_Settings.exe") != std::string::npos);
    CHECK(text.find("arg   --start") != std::string::npos);
    r.sb.probe.Start("D2R.exe");
    text = FormatOutcome(m.Play(true), true, &code);
    CHECK_EQ(code, 10);
    CHECK(text.find("error [E_GAME_RUNNING]") != std::string::npos);
    r.sb.probe.Stop("D2R.exe");
    r.opt.select = "d2r-pcvr";
    Model pc = r.Make();
    text = FormatOutcome(pc.Play(true), true, &code);
    CHECK_EQ(code, 11);
    CHECK(text.find("E_MOD_NOT_INSTALLED") != std::string::npos);
}

TEST(the_install_switches_of_the_command_line) {
    Options o = ParseOptions({"--install-view", "--install-tick", "unleashed,loader", "--no-update-check", "--shot-now", "--install-now", "--accept-unverified",
                              "--pin-file", "p.json", "--install-run", "authentic,loader"});
    CHECK(o.install_view && o.no_update_check && o.shot_now && o.install_now && o.accept_unverified);
    CHECK_EQ(o.pin_file, std::string("p.json"));
    REQUIRE(o.install_tick.size() == 2);
    CHECK(o.install_tick[0] == "loader" && o.install_tick[1] == "unleashed");           // in the order they are installed in
    CHECK(o.has_install_run);
    REQUIRE(o.install_run.size() == 2);
    CHECK(o.install_run[0] == "loader" && o.install_run[1] == "authentic");
    CHECK(o.problems.empty());
    o = ParseOptions({"--install-run", "bogus"});
    CHECK(!o.has_install_run);
    CHECK_EQ(o.problems.size(), size_t(1));
    o = ParseOptions({"--install-tick"});
    CHECK_EQ(o.problems.size(), size_t(1));
    o = ParseOptions({});
    CHECK(!o.install_view && !o.has_install_run && !o.no_update_check && !o.accept_unverified);
    CHECK(std::string(UsageText()).find("--install-view") != std::string::npos);
}

TEST(test_switches_that_reach_a_server_need_the_loopback_switch) {
    Options o = ParseOptions({"--install-run", "loader", "--game", "G:/g", "--accept-unverified", "--pin-file", "x", "--install-now"});
    CHECK_EQ(TestSwitchProblems(o, {}).size(), size_t(4));
    CHECK(TestSwitchProblems(o, {{"VRML_ALLOW_HTTP_HOST", "127.0.0.1:5"}}).empty());
    CHECK(TestSwitchProblems(o, {{"VRML_ALLOW_HTTP_HOST", "example.com:5"}}).size() == 4);      // only a loopback host counts
    o = ParseOptions({"--install-run", "loader"});
    const std::vector<std::string> p = TestSwitchProblems(o, {{"VRML_ALLOW_HTTP_HOST", "127.0.0.1:5"}});
    REQUIRE(p.size() == 1);
    CHECK(p[0].find("--game") != std::string::npos);
    CHECK(TestSwitchProblems(ParseOptions({"--install-view", "--screenshot", "x.png", "--shot-now"}), {}).empty());     // nothing here reaches a server by itself
}

TEST(the_settings_remember_the_update_check_and_the_first_run_offer) {
    tt::Sandbox sb;
    LauncherSettings d;
    CHECK(d.check_updates_on_start);                          // on by default
    CHECK(!d.install_view_offered);
    CHECK(LoadSettings(sb.home).check_updates_on_start);       // no file: the defaults
    LauncherSettings s;
    s.check_updates_on_start = false;
    s.install_view_offered = true;
    SaveSettings(sb.home, s);
    const LauncherSettings back = LoadSettings(sb.home);
    CHECK(!back.check_updates_on_start);
    CHECK(back.install_view_offered);
    // a file from before these settings existed: the update check is on
    tt::Write(sb.home / "settings.json", R"({"schema":"vrmod-launcher-settings/1","game":"","last_mod":"d2r-vr","close_on_start":true,"mods":{}})");
    const LauncherSettings old = LoadSettings(sb.home);
    CHECK(old.check_updates_on_start && !old.install_view_offered && old.close_on_start);
}

namespace {
Release Newest(const std::string& id, const std::string& version, bool upToDate = false) {
    Release r;
    r.id = id;
    r.version = version;
    r.up_to_date = upToDate;
    if (!upToDate) r.url = "https://example.invalid/x.exe";
    return r;
}
UpdateInfo Info(const std::string& id, const Release& r) {
    UpdateInfo u;
    u.id = id;
    u.checked = true;
    u.latest = r;
    return u;
}
}  // namespace

TEST(a_card_offers_install_or_update_and_a_badge_when_the_servers_said_so) {
    Rig r;
    Model m = r.Make();
    // not installed: the card says Install, before and after any answer
    ModEntry a = m.mods()[0], u = m.mods()[1];
    CHECK(m.OfferFor(a).kind == Model::CardOffer::Kind::Install && m.OfferFor(a).label == "Install" && m.OfferFor(a).badge.empty());
    CHECK(m.OfferFor(u).kind == Model::CardOffer::Kind::Install);
    // installed through a Setup that wrote its manifest: 0.167.0; the newest is 0.168.0
    tt::LeaveMod(r.sb.game, "d2r-vr", "0.167.0");
    Model with = r.Make();
    a = with.mods()[0];
    CHECK(a.installed && a.source == "manifest");
    CHECK(with.OfferFor(a).kind == Model::CardOffer::Kind::None);                       // nothing asked yet: no button, no badge
    with.SetLatest(Info("d2r-vr", Newest("d2r-vr", "0.168.0")));
    Model::CardOffer o = with.OfferFor(a);
    CHECK(o.kind == Model::CardOffer::Kind::Update);
    CHECK_EQ(o.label, std::string("Update"));
    CHECK_EQ(o.badge, std::string("Update available: 0.167.0 -> 0.168.0"));
    REQUIRE(with.LatestOf("d2r-vr"));
    CHECK_EQ(with.LatestOf("d2r-vr")->version, std::string("0.168.0"));
    // current: nothing
    with.SetLatest(Info("d2r-vr", Newest("d2r-vr", "0.167.0", true)));
    CHECK(with.OfferFor(a).kind == Model::CardOffer::Kind::None);
    with.SetLatest(Info("d2r-vr", Newest("d2r-vr", "0.167.0")));
    CHECK(with.OfferFor(a).kind == Model::CardOffer::Kind::None);                       // the same version: not newer
    // a question that failed (offline for a moment) leaves what was known: no flicker, and the badge is still right for the installed version
    with.SetLatest(Info("d2r-vr", Newest("d2r-vr", "0.169.0")));
    CHECK(with.OfferFor(a).kind == Model::CardOffer::Kind::Update);
    UpdateInfo failed;
    failed.id = "d2r-vr";
    failed.checked = false;
    failed.error = "no network";
    with.SetLatest(failed);
    REQUIRE(with.LatestOf("d2r-vr"));
    CHECK_EQ(with.LatestOf("d2r-vr")->version, std::string("0.169.0"));
    CHECK(with.OfferFor(a).kind == Model::CardOffer::Kind::Update);
    // and a mod nobody has answered about has nothing to offer
    CHECK(with.LatestOf("d2r-pcvr") == nullptr);
    // an installed mod whose version is not known (the built-in profile: no manifest of its own) counts as older than anything newer
    tt::Write(r.sb.game / "mods" / "D2R-PCVR" / "D2R-PCVR.mpq" / "modinfo.json", "{}");
    Model builtin = r.Make();
    ModEntry b = builtin.mods()[1];
    CHECK(b.installed && b.source != "manifest");
    builtin.SetLatest(Info("d2r-pcvr", Newest("d2r-pcvr", "0.3.19")));
    CHECK(builtin.OfferFor(b).kind == Model::CardOffer::Kind::Update);
    CHECK_EQ(builtin.OfferFor(b).badge, std::string("Update available: 0.3.19"));
    // another D2RLoader mod has neither
    r.Install("SomeOtherMod");
    Model other = r.Make();
    CHECK(other.OfferFor(other.mods().back()).kind == Model::CardOffer::Kind::None);
}

TEST(the_quiet_check_at_start_and_the_first_run_offer_follow_the_settings) {
    Rig r;
    {
        Model m = r.Make();
        CHECK(m.UpdateCheckWanted());                      // on by default
        CHECK(!m.ShouldOfferInstallView());                // the folder was given with --game: no offer
    }
    r.opt.no_update_check = true;
    CHECK(!r.Make().UpdateCheckWanted());
    r.opt.no_update_check = false;
    LauncherSettings s;
    s.check_updates_on_start = false;
    SaveSettings(r.sb.home, s);
    CHECK(!r.Make().UpdateCheckWanted());
    s.check_updates_on_start = true;
    SaveSettings(r.sb.home, s);
    CHECK(r.Make().UpdateCheckWanted());
    // no game, nothing to look for
    r.opt.game = U(r.sb.root / "nothing");
    CHECK(!r.Make().UpdateCheckWanted());

    // the offer: a game that was found (not given), none of the two VR mods installed, not offered before
    Rig f;
    f.opt.game.clear();
    f.svc.game_roots.extra = {f.sb.game};
    CHECK(f.Make().ShouldOfferInstallView());
    f.Install("SomeOtherMod");
    CHECK(f.Make().ShouldOfferInstallView());              // a third-party mod is not a VR mod
    f.Install("D2R-PCVR");
    CHECK(!f.Make().ShouldOfferInstallView());             // one VR mod is there
    RemoveQuiet(f.sb.game / "mods" / "D2R-PCVR");
    LauncherSettings offered;
    offered.install_view_offered = true;
    SaveSettings(f.sb.home, offered);
    CHECK(!f.Make().ShouldOfferInstallView());             // offered once: not again
    Rig none;
    none.opt.game.clear();
    CHECK(!none.Make().ShouldOfferInstallView());          // no game found at all
}

TEST(an_installed_card_offers_uninstall_runs_the_mods_own_or_explains_or_nothing) {
    Rig r;
    r.Install("D2R-VR");
    r.Install("D2R-PCVR");
    r.Install("Another");
    const fs::path la = r.sb.root / "la";
    r.svc.env["LOCALAPPDATA"] = U(la);                       // where Unleashed's Setup puts its uninstaller: a folder of this test
    auto offer = [&](const std::string& id) {
        Model m = r.Make();
        for (const ModEntry& e : m.mods())
            if (e.id == id) return m.UninstallFor(e);
        return Model::UninstallOffer();
    };
    // no uninstaller anywhere (a ZIP install): the button explains and names the folder; it never removes anything itself
    Model::UninstallOffer a = offer("d2r-vr"), u = offer("d2r-pcvr");
    CHECK(a.kind == Model::UninstallOffer::Kind::Explain && a.label == "Uninstall");
    CHECK(u.kind == Model::UninstallOffer::Kind::Explain && u.label == "Uninstall");
    CHECK(u.explanation.find("D2R VR Unleashed has no uninstaller") != std::string::npos);
    CHECK(u.explanation.find("delete the folder") != std::string::npos && u.explanation.find("D2R-PCVR") != std::string::npos);
    CHECK(u.explanation.find("never deletes a mod's folder") != std::string::npos);
    // a D2RLoader mod the launcher does not know: nothing (it never removes what it did not install)
    CHECK(offer("another").kind == Model::UninstallOffer::Kind::None && offer("another").label.empty());
    // Authentic's own unins000.exe in the game folder: it is run (it asks Windows for rights itself)
    tt::Write(r.sb.game / "unins000.exe", "uninstaller");
    a = offer("d2r-vr");
    CHECK(a.kind == Model::UninstallOffer::Kind::Run);
    CHECK_EQ(a.exe, P(U(r.sb.game) + "/unins000.exe"));
    CHECK(a.needs_admin);
    // Unleashed's own: {localappdata}\Programs\D2R PCVR\unins000.exe, started with no arguments (no /SILENT: his own question shows)
    tt::Write(la / "Programs" / "D2R PCVR" / "unins000.exe", "his uninstaller");
    u = offer("d2r-pcvr");
    CHECK(u.kind == Model::UninstallOffer::Kind::Run);
    CHECK_EQ(u.exe, P(U(la) + "/Programs/D2R PCVR/unins000.exe"));
    CHECK(u.args.empty() && !u.needs_admin);
    // a mod that is not installed has no button
    fs::remove_all(r.sb.game / "mods" / "D2R-PCVR");
    CHECK(offer("d2r-pcvr").kind == Model::UninstallOffer::Kind::None);
}

TEST(uninstall_is_refused_while_the_game_runs_and_without_a_game_folder) {
    Rig r;
    r.Install("D2R-VR");
    Model m = r.Make();
    CHECK(m.UninstallRefusal().empty());
    r.sb.probe.Start("D2R.exe");
    const std::string why = m.UninstallRefusal();
    CHECK(why.find("The game is running") != std::string::npos && why.find("Save & Exit") != std::string::npos && why.find("does not close it") != std::string::npos);
    r.sb.probe.Stop("D2R.exe");
    r.sb.probe.Start("D2RLoader.exe");
    CHECK(!m.UninstallRefusal().empty());                    // D2RLoader counts too
    r.sb.probe.Stop("D2RLoader.exe");
    CHECK(m.UninstallRefusal().empty());
    // no game folder: nothing to uninstall from
    Rig none;
    none.opt.game = "";
    none.opt.no_search = true;
    Model nm = none.Make();
    CHECK(nm.UninstallRefusal().find("Pick the folder") != std::string::npos);
}

TEST_MAIN()
