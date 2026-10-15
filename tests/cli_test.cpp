// The command line, end to end on a fake game folder.
#include <cstdlib>
#include <sstream>

#include "cli.h"
#include "sandbox.h"
#include "test.h"

using namespace vrml;

namespace {

struct Run {
    int code;
    std::string out, err;
};

struct RecordingBackend : LaunchBackend {
    std::vector<LaunchPlan> started;
    bool Start(const LaunchPlan& p, const std::map<std::string, std::string>&, std::string*) override {
        started.push_back(p);
        return true;
    }
};

struct World {
    tt::Sandbox sb;
    CliEnv env;
    RecordingBackend backend;
    World() {
        env.probe = &sb.probe;
        env.backend = &backend;
        env.env = {{"PATH", "x"}};
        tt::ModSpec a = tt::AlphaSpec();
        a.extra.set("launch", Json::Parse(R"({"options":[{"id":"go","label":"Go","kind":"exec","exe":"{game}/Alpha_Settings.exe","args":["--start"]}]})"));
        tt::WritePackage(sb.home, a);
        tt::WritePackage(sb.home, tt::BetaSpec());
    }
    Run Cli(std::vector<std::string> args, bool withGame = true) {
        args.insert(args.begin(), {"--home", U(sb.home)});
        if (withGame) args.insert(args.begin(), {"--game", U(sb.game)});
        std::ostringstream out, err;
        int code = RunCli(args, out, err, env);
        return {code, out.str(), err.str()};
    }
    void Register() {
        CHECK_EQ(Cli({"register", U(sb.home / "mods" / "alpha" / "manifest.json")}).code, 0);
        CHECK_EQ(Cli({"register", U(sb.home / "mods" / "beta" / "manifest.json")}).code, 0);
    }
};

bool Has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

}  // namespace

TEST(register_list_activate_status) {
    World w;
    Run r = w.Cli({"list"});
    CHECK_EQ(r.code, 0);
    CHECK(Has(r.out, "no mod is registered"));
    CHECK(Has(r.out, "a package without registration"));       // the installer's last step is missing for both
    w.Register();
    r = w.Cli({"list"});
    CHECK(Has(r.out, "alpha  1.0.0"));
    CHECK(Has(r.out, "beta  2.0.0"));
    CHECK(!Has(r.out, "without registration"));

    r = w.Cli({"activate", "alpha", "--dry-run"});
    CHECK_EQ(r.code, 0);
    CHECK(Has(r.out, "activate alpha"));
    CHECK(Has(r.out, "put in"));
    CHECK(!tt::Snapshot(w.sb.game).count("d2rloader/plugins/d2rl-vrcam.dll"));

    r = w.Cli({"activate", "alpha"});
    CHECK_EQ(r.code, 0);
    CHECK_EQ(tt::Read(w.sb.game / "d2rloader/plugins/d2rl-vrcam.dll"), std::string("ALPHA-DLL-1.0.0 ALPHA-MARKER"));

    r = w.Cli({"status"});
    CHECK_EQ(r.code, 0);
    CHECK(Has(r.out, "active alpha 1.0.0"));
    CHECK(Has(r.out, "build 3.3.93787"));
    CHECK(Has(r.out, "D2RLoader 1.3.1-beta"));
    CHECK(Has(r.out, "steam edition"));

    r = w.Cli({"status", "--json"});
    Json j = Json::Parse(r.out);
    CHECK_EQ(j.find("game")->find("active")->find("vr-plugin")->get_string("id"), std::string("alpha"));

    r = w.Cli({"list"});
    CHECK(Has(r.out, "* alpha"));

    r = w.Cli({"activate", "beta"});
    CHECK_EQ(r.code, 0);
    r = w.Cli({"verify"});
    CHECK_EQ(r.code, 0);
    r = w.Cli({"deactivate", "vr-plugin"});                     // by group
    CHECK_EQ(r.code, 0);
    CHECK(!Has(tt::Read(w.sb.home / "games" / GameKey(w.sb.game) / "state.json"), "\"beta\""));
}

TEST(exit_codes) {
    World w;
    w.Register();
    CHECK_EQ(w.Cli({}).code, 2);                                  // usage
    CHECK_EQ(w.Cli({"frobnicate"}).code, 2);
    CHECK_EQ(w.Cli({"activate"}).code, 2);
    CHECK_EQ(w.Cli({"activate", "alpha"}, false).code, 2);        // no game given
    CHECK_EQ(w.Cli({"activate", "nope"}).code, 3);                // not found
    CHECK_EQ(w.Cli({"register", U(w.sb.root / "nothing.json")}).code, 4);
    w.sb.probe.Start("D2R.exe");
    Run r = w.Cli({"activate", "alpha"});
    CHECK_EQ(r.code, 10);                                         // the game runs
    CHECK(Has(r.err, "E_GAME_RUNNING"));
    w.sb.probe.Stop("D2R.exe");
    CHECK_EQ(w.Cli({"activate", "alpha"}).code, 0);
    CHECK_EQ(w.Cli({"unregister", "alpha"}).code, 12);            // in use
    tt::Write(w.sb.game / "d2rloader/plugins/d2rl-vrcam.dll", "tampered");
    CHECK_EQ(w.Cli({"verify"}).code, 1);
}

TEST(a_registration_needs_the_package_in_place) {
    World w;
    tt::Write(w.sb.root / "elsewhere" / "manifest.json", tt::Read(w.sb.home / "mods" / "alpha" / "manifest.json"));
    Run r = w.Cli({"register", U(w.sb.root / "elsewhere" / "manifest.json")});
    CHECK_EQ(r.code, 2);
    CHECK(Has(r.err, "put the package in"));
    fs::remove(w.sb.home / "mods" / "alpha" / "payload" / "Alpha_Settings.exe");
    r = w.Cli({"register", U(w.sb.home / "mods" / "alpha" / "manifest.json")});
    CHECK_EQ(r.code, 4);
    CHECK(Has(r.err, "missing in the package"));
}

TEST(an_unfinished_switch_is_recovered_first) {
    World w;
    w.Register();
    CHECK_EQ(w.Cli({"activate", "alpha"}).code, 0);
    // die in the middle of the switch to beta, through the library (the CLI has no crash hook)
    {
        Services svc;
        svc.probe = &w.sb.probe;
        svc.log = &w.sb.log;
        svc.crash = [](int op, const char* phase) { if (op == 4 && std::string(phase) == "after") throw SimulatedCrash(); };
        Engine eng(w.sb.home, svc);
        try { eng.Activate(w.sb.game, "beta"); CHECK(false); } catch (const SimulatedCrash&) {}
    }
    Run r = w.Cli({"status"});
    CHECK(Has(r.out, "did not finish"));
    r = w.Cli({"verify"});
    CHECK_EQ(r.code, 1);
    r = w.Cli({"activate", "beta"});                               // recovers, then switches
    CHECK_EQ(r.code, 0);
    CHECK(Has(r.out, "recover:"));
    CHECK_EQ(tt::Read(w.sb.game / "d2rloader/plugins/d2rl-vrcam.dll"), std::string("BETA-DLL-2.0.0 BETA-MARKER"));
    CHECK_EQ(w.Cli({"recover"}).code, 0);
}

TEST(launch_activates_first_and_starts) {
    World w;
    w.Register();
    Run r = w.Cli({"launch", "alpha", "--dry-run"});
    CHECK_EQ(r.code, 0);
    CHECK(Has(r.out, "would first activate alpha"));
    CHECK(w.backend.started.empty());

    r = w.Cli({"launch", "alpha"});
    CHECK_EQ(r.code, 0);
    CHECK(Has(r.out, "activating alpha first"));
    REQUIRE(w.backend.started.size() == 1);
    CHECK_EQ(w.backend.started[0].mod_id, std::string("alpha"));
    CHECK(Has(w.backend.started[0].exe, "Alpha_Settings.exe"));
    CHECK_EQ(w.backend.started[0].args.size(), size_t(1));
    CHECK(Has(tt::Read(w.sb.home / "games" / GameKey(w.sb.game) / "state.json"), "last_launch"));
    r = w.Cli({"status"});
    CHECK(Has(r.out, "last start alpha 1.0.0"));

    r = w.Cli({"launch", "alpha"});                                 // already active: no switch
    CHECK(!Has(r.out, "activating"));
    CHECK_EQ(w.backend.started.size(), size_t(2));

    // a runtime the option does not offer: nothing starts
    r = w.Cli({"launch", "alpha", "--runtime", "vdxr"});
    CHECK_EQ(r.code, 24);
    CHECK_EQ(w.backend.started.size(), size_t(2));

    // beta has no launch option: the error says so, and the game was switched to beta already (activation is the first step)
    r = w.Cli({"launch", "beta"});
    CHECK_EQ(r.code, 3);
}

TEST(requirements_and_deps_commands) {
    World w;
    w.Register();
    CHECK_EQ(w.Cli({"requirements", "alpha"}).code, 0);          // alpha declares none
    tt::Write(w.sb.root / "fake.zip", "not the archive");
    Run r = w.Cli({"deps", "verify", U(w.sb.root / "fake.zip")});
    CHECK_EQ(r.code, 23);
    CHECK(Has(r.err, "not the reviewed"));
    CHECK_EQ(w.Cli({"deps", "verify", U(w.sb.root / "missing.zip")}).code, 3);
}

TEST(import_through_the_cli) {
    World w;
    w.Register();
    tt::Write(w.sb.game / "d2rloader/plugins/d2rl-vrcam.dll", "ALPHA-DLL-0.9 ALPHA-MARKER");
    tt::Write(w.sb.game / "d2rloader/plugins/d2r_vr.ini", "[camera]\nmode=mine\n");
    Run r = w.Cli({"import", "alpha"});
    CHECK_EQ(r.code, 0);
    CHECK(Has(r.out, "imported alpha"));
    r = w.Cli({"import", "beta"});
    CHECK_EQ(r.code, 12);                                          // the group is taken
    r = w.Cli({"verify"});
    CHECK_EQ(r.code, 0);
}

TEST(version_and_help) {
    World w;
    Run r = w.Cli({"version"}, false);
    CHECK_EQ(r.code, 0);
    CHECK(Has(r.out, "vrmodlauncher"));
    CHECK(Has(r.out, "vrmod-manifest/1"));
}

TEST_MAIN()
