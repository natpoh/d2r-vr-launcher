#include "cli.h"

#include <cstdlib>
#include <iostream>
#include <set>
#include <sstream>

#include "cli_install.h"
#include "deps.h"
#include "engine.h"
#include "game.h"
#include "mods.h"
#include "registry.h"
#include "sha256.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#ifndef _WIN32
extern char** environ;     // at file scope: a declaration inside namespace vrml would name vrml::environ, which nothing defines
#endif

namespace vrml {

std::map<std::string, std::string> CurrentEnvironment() {
    std::map<std::string, std::string> env;
#ifdef _WIN32
    LPWCH block = GetEnvironmentStringsW();
    if (block) {
        for (const wchar_t* p = block; *p; p += wcslen(p) + 1) {
            std::wstring kv(p);
            size_t eq = kv.find(L'=', 1);   // a name may start with '=' (drive cwd entries)
            if (eq == std::wstring::npos) continue;
            env[U(fs::path(kv.substr(0, eq)))] = U(fs::path(kv.substr(eq + 1)));
        }
        FreeEnvironmentStringsW(block);
    }
#else
    for (char** e = ::environ; e && *e; ++e) {
        std::string kv(*e);
        size_t eq = kv.find('=');
        if (eq != std::string::npos) env[kv.substr(0, eq)] = kv.substr(eq + 1);
    }
#endif
    return env;
}

std::string EnvGet(const std::map<std::string, std::string>& env, const std::string& name) {
    for (const auto& kv : env)
        if (IEquals(kv.first, name)) return kv.second;
    return "";
}

fs::path DefaultHome(const std::map<std::string, std::string>& env) {
    std::string p = EnvGet(env, "LOCALAPPDATA");
    if (!p.empty()) return P(p) / "VRModLauncher";
    p = EnvGet(env, "HOME");
    if (!p.empty()) return P(p) / ".local" / "share" / "VRModLauncher";
    return fs::current_path() / "VRModLauncher";
}

namespace {

struct Args {
    std::vector<std::string> pos;
    std::map<std::string, std::string> opt;   // --name value
    std::set<std::string> flags;              // --name
    bool flag(const std::string& f) const { return flags.count(f) > 0; }
    std::string get(const std::string& k, const std::string& def = "") const {
        auto it = opt.find(k);
        return it == opt.end() ? def : it->second;
    }
};

Args ParseArgs(const std::vector<std::string>& a) {
    static const std::set<std::string> kValue = {"--home", "--game", "--option", "--runtime", "--runtime-file", "--store", "--from", "--pin-file"};
    Args r;
    for (size_t i = 0; i < a.size(); ++i) {
        if (StartsWith(a[i], "--")) {
            if (kValue.count(a[i])) {
                if (i + 1 >= a.size()) Throw("E_USAGE", a[i] + " needs a value");
                const std::string name = a[i];   // (C++17 evaluates the right side of = first: a[++i] must not run before the key)
                r.opt[name] = a[++i];
            } else {
                r.flags.insert(a[i]);
            }
        } else {
            r.pos.push_back(a[i]);
        }
    }
    return r;
}

struct NoBackend : LaunchBackend {
    bool Start(const LaunchPlan&, const std::map<std::string, std::string>&, std::string* error) override {
        if (error) *error = "no launch backend";
        return false;
    }
};

const char* kUsage =
    "usage: vrmodlauncher [--home DIR] [--game DIR] [--json] <command>\n"
    "  mods | launch <mod> | list | status | register <manifest> | unregister <id> | activate <id> | deactivate <id|group> | verify | recover |\n"
    "  import <id> | import-settings <id> --from DIR | requirements <id> | launch <id> | deps verify <zip> | version |\n"
    "  releases [--json] | install loader|authentic|unleashed [--dry-run] [--no-run] [--accept-unverified] [--force]\n";

fs::path NeedGame(const Args& a, const CliEnv& env) {
    std::string g = a.get("--game");
    if (g.empty()) g = EnvGet(env.env, "VRMOD_GAME");
    if (g.empty()) Throw("E_USAGE", "which game folder? give --game DIR (or set VRMOD_GAME)");
    return P(g);
}

void PrintWarnings(std::ostream& out, const std::vector<std::string>& w) {
    for (const std::string& s : w) out << "  warning: " << s << "\n";
}

}  // namespace

int RunCli(const std::vector<std::string>& args, std::ostream& out, std::ostream& err, CliEnv& env) {
    try {
        Args a = ParseArgs(args);
        if (a.pos.empty() || a.flag("--help")) {
            (a.pos.empty() ? err : out) << kUsage;
            return a.pos.empty() ? 2 : 0;
        }
        const std::string cmd = a.pos[0];
        const bool json = a.flag("--json");
        if (cmd == "version") {
            out << "vrmodlauncher " << kLauncherVersion << " (manifest " << kManifestSchema << ")\n";
            return 0;
        }

        fs::path home;
        if (!a.get("--home").empty()) home = P(a.get("--home"));
        else if (!EnvGet(env.env, "VRMODLAUNCHER_HOME").empty()) home = P(EnvGet(env.env, "VRMODLAUNCHER_HOME"));
        else home = env.default_home.empty() ? DefaultHome(env.env) : env.default_home;

        Logger log(home / "logs" / "launcher.log");

        if (cmd == "releases" || cmd == "install") {      // the network commands (cli_install.cpp)
            InstallCliArgs ia;
            ia.pos = a.pos;
            ia.game = a.get("--game").empty() ? EnvGet(env.env, "VRMOD_GAME") : a.get("--game");
            if (cmd == "install" && ia.game.empty()) Throw("E_USAGE", "which game folder? give --game DIR (or set VRMOD_GAME)");
            ia.pin_file = a.get("--pin-file");
            ia.json = json;
            ia.dry_run = a.flag("--dry-run");
            ia.no_run = a.flag("--no-run");
            ia.accept_unverified = a.flag("--accept-unverified");
            ia.force = a.flag("--force");
            return cmd == "releases" ? RunReleasesCommand(ia, out, err, env) : RunInstallCommand(ia, out, err, env, home, log);
        }

        Services svc;
        svc.probe = env.probe;
        svc.log = &log;
        Engine eng(home, svc);

        if (cmd == "deps") {
            if (a.pos.size() < 3 || a.pos[1] != "verify") Throw("E_USAGE", "deps verify <zip>");
            std::string h;
            if (!Sha256File(P(a.pos[2]), &h)) Throw("E_NOT_FOUND", "cannot read " + a.pos[2]);
            const Pin& pin = D2RLoaderPin();
            if (!HashAccepted(pin.sha256, h)) Throw("E_HASH", "not the reviewed " + pin.id + " " + pin.version + " archive (SHA-256 " + h + ")");
            out << a.pos[2] << ": the reviewed " << pin.id << " " << pin.version << " archive (SHA-256 " << h << ")\n";
            return 0;
        }

        if (cmd == "list") {
            Json arr = Json::Array();
            const std::vector<ModRecord> recs = eng.registry().List();
            std::map<std::string, std::string> active;   // id -> group
            if (!a.get("--game").empty() && LooksLikeGame(P(a.get("--game"))))
                for (const auto& kv : eng.LoadState(P(a.get("--game"))).active) active[kv.second.id] = kv.first;
            for (const ModRecord& r : recs) {
                std::string name, group;
                try { Manifest m = eng.registry().Load(r.id); name = m.name; group = m.group; } catch (const Error&) { name = "(package problem)"; }
                Json o = Json::Object();
                o.set("id", Json::String(r.id));
                o.set("name", Json::String(name));
                o.set("version", Json::String(r.version));
                o.set("group", Json::String(group));
                o.set("active", Json::Bool(active.count(r.id) > 0));
                arr.push(o);
                if (!json) out << (active.count(r.id) ? "* " : "  ") << r.id << "  " << r.version << "  " << name << "  [" << group << "]\n";
            }
            if (json) out << arr.dump();
            else if (recs.empty()) out << "no mod is registered\n";
            for (const std::string& u : eng.registry().Unregistered())
                if (!json) out << "  (" << u << ": a package without registration - the installer's last step `register` is missing)\n";
            return 0;
        }

        if (cmd == "mods") {
            const fs::path game = NeedGame(a, env);
            if (!LooksLikeGame(game)) Throw("E_GAME", U(game) + " has no D2R.exe");
            const GameInstall gi = DescribeGame(game, "given");
            const LoaderStatus loader = CheckLoader(game, true);
            Json arr = Json::Array();
            const std::vector<ModEntry> mods = DiscoverMods(game);
            for (const ModEntry& e : mods) {
                Json o = Json::Object();
                o.set("id", Json::String(e.id));
                o.set("folder", Json::String(e.folder));
                o.set("name", Json::String(e.name));
                o.set("version", Json::String(e.version));
                o.set("installed_version", Json::String(e.found_version));            // what is installed ("" = not known): manifest, a manifest that does not validate, or the mod's plugin
                o.set("installed_version_from", Json::String(e.found_from));
                o.set("installed", Json::Bool(e.installed));
                o.set("source", Json::String(e.source));
                o.set("launchable", Json::Bool(e.launchable));
                Json notes = Json::Array();
                for (const std::string& n : e.notes) notes.push(Json::String(n));
                o.set("notes", notes);
                arr.push(o);
            }
            if (json) {
                Json r = Json::Object();
                r.set("game", Json::String(U(game)));
                r.set("store", Json::String(gi.store));
                r.set("build", Json::String(gi.build));
                Json l = Json::Object();
                l.set("present", Json::Bool(loader.present));
                l.set("reviewed", Json::Bool(loader.reviewed));
                l.set("version", Json::String(loader.version));
                r.set("d2rloader", l);
                r.set("mods", arr);
                out << r.dump();
                return 0;
            }
            out << "game   " << U(game) << "  (" << (gi.store.empty() ? "store unknown" : gi.store) << ", build " << (gi.build.empty() ? "unknown" : gi.build) << ")\n";
            out << "loader " << (loader.present ? (loader.reviewed ? "D2RLoader " + loader.version + " (reviewed files)" : std::string("D2RLoader present, files not the reviewed 1.3.1-beta ones")) : std::string("D2RLoader.exe is missing")) << "\n";
            for (const ModEntry& e : mods) {
                // an installed mod shows the version found in its folder (never the built-in profile's), and where it was read from when that is not its manifest
                std::string have = e.installed ? (e.found_version.empty() ? "version unknown  " : e.found_version + (e.found_from == "manifest" ? "" : " (" + e.found_from + ")") + "  ") : "";
                out << (e.installed ? "* " : "  ") << e.id << "  " << e.name << "  " << have << "["
                    << (e.installed ? "installed, " : "not installed, ") << e.source << "]\n";
                for (const std::string& n : e.notes) out << "    note: " << n << "\n";
            }
            return 0;
        }

        if (cmd == "register") {
            if (a.pos.size() < 2) Throw("E_USAGE", "register <manifest>");
            ModRecord r = eng.registry().Register(P(a.pos[1]));
            out << "registered " << r.id << " " << r.version << (r.previous_version.empty() ? "" : " (was " + r.previous_version + ")") << "\n";
            return 0;
        }

        if (cmd == "unregister") {
            if (a.pos.size() < 2) Throw("E_USAGE", "unregister <id>");
            RemoveOptions o;
            o.force = a.flag("--force");
            o.purge_package = a.flag("--purge");
            o.purge_settings = a.flag("--purge-settings");
            eng.RemoveMod(a.pos[1], o);
            out << "unregistered " << a.pos[1] << "\n";
            return 0;
        }

        if (cmd == "status") {
            fs::path game = a.get("--game").empty() && EnvGet(env.env, "VRMOD_GAME").empty() ? fs::path() : NeedGame(a, env);
            Json st = eng.Status(game.empty() ? fs::path("-") : game);
            if (json) { out << st.dump(); return 0; }
            out << "VR Mod Launcher " << kLauncherVersion << "   home " << st.get_string("home") << "\n";
            if (!game.empty()) {
                const Json& g = *st.find("game");
                out << "game   " << g.get_string("path") << "\n";
                if (g.get_bool("is_game")) {
                    out << "       " << (g.get_string("store").empty() ? "store unknown" : g.get_string("store") + " edition") << ", build "
                        << (g.get_string("build").empty() ? "unknown" : g.get_string("build")) << ", D2RLoader "
                        << (g.get_string("d2rloader").empty() ? "unknown" : g.get_string("d2rloader")) << "\n";
                    const Json* act = g.find("active");
                    if (!act || act->members().empty()) out << "active none (the game runs without a VR mod)\n";
                    else
                        for (const Json::Member& kv : act->members()) {
                            out << "active " << kv.second.get_string("id") << " " << kv.second.get_string("version") << "  [" << kv.first << "]  since "
                                << kv.second.get_string("activated_at") << "\n";
                            if (kv.second.has("refresh_pending_to")) out << "       a newer package is registered: " << kv.second.get_string("refresh_pending_to") << " (activate it to bring it in)\n";
                        }
                    if (g.get_bool("pending_journal")) out << "WARNING an earlier switch did not finish: run `recover`\n";
                    if (const Json* ll = g.find("last_launch"))
                        out << "last start " << ll->get_string("mod") << ", runtime " << ll->get_string("runtime") << ", " << ll->get_string("at") << "\n";
                } else {
                    out << "       (not a Diablo II: Resurrected folder)\n";
                }
            }
            out << "mods   ";
            const Json* mods = st.find("mods");
            if (!mods || mods->items().empty()) out << "none registered\n";
            else {
                out << "\n";
                for (const Json& m : mods->items()) out << "       " << m.get_string("id") << " " << m.get_string("version") << "  " << m.get_string("name") << "\n";
            }
            return 0;
        }

        // ---- everything below works on one game folder
        const fs::path game = NeedGame(a, env);

        // an unfinished switch is dealt with first, before any command that would change or trust the folder
        auto recoverFirst = [&]() {
            if (Exists(eng.Dirs(game).journal)) {
                RecoverReport r = eng.Recover(game);
                for (const std::string& n : r.notes) out << "recover: " << n << "\n";
            }
        };

        if (cmd == "recover") {
            RecoverReport r = eng.Recover(game);
            if (r.result == RecoverReport::Result::Nothing) out << "nothing to recover\n";
            for (const std::string& n : r.notes) out << n << "\n";
            return 0;
        }

        if (cmd == "verify") {
            VerifyReport r = eng.Verify(game);
            for (const Finding& f : r.findings) out << f.level << " " << f.code << ": " << f.message << "\n";
            return r.ok() ? 0 : 1;
        }

        if (cmd == "activate" || cmd == "deactivate") {
            if (a.pos.size() < 2) Throw("E_USAGE", cmd + " <id>");
            recoverFirst();
            Plan p = cmd == "activate" ? eng.PlanActivate(game, a.pos[1]) : eng.PlanDeactivate(game, a.pos[1]);
            if (a.flag("--dry-run")) {
                out << p.Describe();
                return 0;
            }
            eng.Apply(game, p);
            out << p.action << " " << p.id << ": done (" << p.ops.size() << " operation(s))\n";
            PrintWarnings(out, p.warnings);
            return 0;
        }

        if (cmd == "import") {
            if (a.pos.size() < 2) Throw("E_USAGE", "import <id>");
            recoverFirst();
            ImportReport r = eng.Import(game, a.pos[1], a.flag("--force"));
            out << "imported " << r.id << ": " << r.adopted << " file(s) taken over, " << r.captured << " settings file(s) captured, " << r.backups << " backup(s) copied\n";
            for (const std::string& n : r.notes) out << "  note: " << n << "\n";
            return 0;
        }

        if (cmd == "import-settings") {
            if (a.pos.size() < 2 || a.get("--from").empty()) Throw("E_USAGE", "import-settings <id> --from DIR");
            recoverFirst();
            SettingsImportReport r = eng.ImportSettings(game, a.pos[1], P(a.get("--from")));
            out << "imported settings of " << r.id << ": " << r.captured << " file(s) saved for this game" << (r.kept_previous ? " (" + std::to_string(r.kept_previous) + " earlier saved copy kept in imports/)" : std::string()) << "\n";
            for (const std::string& n : r.notes) out << "  note: " << n << "\n";
            return 0;
        }

        if (cmd == "requirements") {
            if (a.pos.size() < 2) Throw("E_USAGE", "requirements <id>");
            Manifest m = eng.registry().Load(a.pos[1]);
            bool bad = false;
            for (const RequirementResult& r : CheckRequirements(m, game)) {
                out << (r.ok ? "ok      " : (r.optional ? "optional " : "MISSING ")) << r.id << "\n";
                for (const std::string& p : r.problems) out << "          " << p << "\n";
                if (!r.ok && !r.optional) bad = true;
            }
            return bad ? 11 : 0;
        }

        if (cmd == "launch") {
            if (a.pos.size() < 2) Throw("E_USAGE", "launch <id>");
            // a D2RLoader mod found in the game folder (or a built-in profile): started with -mod, nothing is activated or swapped
            if (!a.get("--game").empty() || !EnvGet(env.env, "VRMOD_GAME").empty()) {
                const fs::path g = NeedGame(a, env);
                for (const ModEntry& e : DiscoverMods(g)) {
                    if (!IEquals(e.id, a.pos[1]) && !IEquals(e.folder, a.pos[1])) continue;
                    if (!env.probe) Throw("E_USAGE", "no process probe: cannot check that the game is closed");
                    LauncherSettings ls = LoadSettings(home);
                    StartRequest req;
                    req.game = g;
                    req.mod = e;
                    req.option = a.get("--option");
                    req.runtime = a.get("--runtime").empty() ? ls.mods[e.id].runtime : a.get("--runtime");
                    req.runtime_file = P(a.get("--runtime-file").empty() ? ls.mods[e.id].runtime_file : a.get("--runtime-file"));
                    req.store = a.get("--store");
                    req.dry_run = a.flag("--dry-run") || !env.backend;
                    NoBackend nb;
                    StartOutcome so = StartMod(req, *env.probe, env.backend ? *env.backend : nb, env.env, env.runtime_roots);
                    for (const std::string& w : so.warnings) out << "  warning: " << w << "\n";
                    if (!so.code.empty()) {
                        err << "error [" << so.code << "]: " << so.message << "\n";
                        return StartExitCode(so.code);
                    }
                    if (req.dry_run) {
                        out << so.plan.Describe();
                        return 0;
                    }
                    ls.game = U(g);
                    ls.last_mod = e.id;
                    SaveSettings(home, ls);
                    log.Line("started " + e.id + " " + e.version + " (D2RLoader mod " + e.folder + ", option " + so.plan.option + ", runtime " + so.plan.runtime + ", store " + so.plan.store + ")");
                    out << "started " << e.id << " " << e.version << "\n";
                    return 0;
                }
            }
            recoverFirst();
            Manifest m = eng.registry().Load(a.pos[1]);
            GameState st = eng.LoadState(game);
            auto act = st.active.find(m.group);
            const bool isActive = act != st.active.end() && act->second.id == m.id;
            LaunchContext ctx;
            ctx.mod_id = m.id;
            ctx.mod_version = m.version;
            ctx.game = game;
            ctx.store = a.get("--store").empty() ? DetectStore(game) : a.get("--store");
            if (ctx.store.empty()) ctx.store = "steam";
            ctx.game_build = DetectBuild(game, m.builds);
            ctx.env = env.env;
            ctx.roots = env.runtime_roots;
            ctx.runtime = a.get("--runtime");
            ctx.runtime_file = P(a.get("--runtime-file"));
            if (!isActive) {
                if (a.flag("--dry-run")) out << "would first activate " << m.id << " (it is not the active mod)\n";
                else {
                    out << "activating " << m.id << " first\n";
                    eng.Activate(game, m.id);
                }
            }
            // the game folder after activation is what the launch plan checks (exe present), so a dry run of an inactive mod cannot
            LaunchPlan lp;
            try {
                lp = BuildLaunchPlan(m, a.get("--option"), ctx);
            } catch (const Error& e) {
                if (!isActive && a.flag("--dry-run") && e.code == "E_NOT_FOUND") {
                    out << "(the launch plan cannot be shown before the mod is active: " << e.what() << ")\n";
                    return 0;
                }
                throw;
            }
            if (a.flag("--dry-run") || !env.backend) {
                out << lp.Describe();
                return 0;
            }
            std::string error;
            if (!env.backend->Start(lp, ApplyEnv(env.env, lp), &error)) Throw("E_IO", "could not start: " + error);
            st = eng.LoadState(game);
            Json ll = Json::Object();
            ll.set("mod", Json::String(m.id + " " + m.version));
            ll.set("option", Json::String(lp.option));
            ll.set("runtime", Json::String(lp.runtime));
            ll.set("at", Json::String(NowIso()));
            st.last_launch = ll;
            WriteFileAtomic(eng.Dirs(game).state, st.ToJson().dump());
            log.Line("started " + m.id + " " + m.version + " (option " + lp.option + ", runtime " + lp.runtime + ", store " + lp.store + ", build " + ctx.game_build + ")");
            out << "started " << m.id << " " << m.version << "\n";
            return 0;
        }

        Throw("E_USAGE", "unknown command \"" + cmd + "\"\n" + kUsage);
    } catch (const Error& e) {
        err << "error [" << e.code << "]: " << e.what() << "\n";
        return e.exit_code;
    } catch (const SimulatedCrash&) {
        throw;
    } catch (const std::exception& e) {
        err << "error: " << e.what() << "\n";
        return 20;
    }
}

}  // namespace vrml
