// cli.h - the command line: `vrmodlauncher <command> ...`. A library function so the tests call it with a fake process probe.
//
//   vrmodlauncher [--home DIR] [--game DIR] [--json] <command>
//     list                          the registered mods (and which is active in --game)
//     status                        launcher, game (store, build, D2RLoader), active mods, pending work
//     register <manifest>           an installer's last step: validate the package in <home>/mods/<id>/ and record it
//     unregister <id> [--force] [--purge] [--purge-settings]
//     activate <id> [--dry-run]     make <id> the active mod of its group (also: bring an updated package in)
//     deactivate <id|group> [--dry-run]
//     verify                        are the active mods' files what was put in
//     recover                       undo (or finish) a switch a crash interrupted
//     import <id> [--force]         take over an installation made before the launcher (and collect its backups)
//     requirements <id>             does the game folder have what <id> needs (D2RLoader, ...)
//     launch <id> [--option ID] [--runtime steamvr|vdxr|system|file] [--runtime-file F] [--store steam|battlenet] [--dry-run]
//     deps verify <zip>             is this file the reviewed D2RLoader archive
//     version
//
// Exit codes: 0 ok; 1 verify found errors; 2 usage; 3 not found; 4 invalid manifest; 10 the game runs; 11 a precondition blocks;
// 12 a decision is needed (collision, in use, wrong identity); 13 newer schema; 20 I/O error (rolled back); 21 rollback incomplete;
// 22 an unfinished switch needs `recover`; 23 hash mismatch; 24 OpenXR runtime; 25 not a game folder.
#pragma once

#include <iosfwd>
#include <map>
#include <string>
#include <vector>

#include "process.h"
#include "launch.h"
#include "util.h"

namespace vrml {

struct CliEnv {
    fs::path default_home;                         // used when --home and VRMODLAUNCHER_HOME are not given
    std::map<std::string, std::string> env;        // the environment (tests pass their own)
    ProcessProbe* probe = nullptr;
    LaunchBackend* backend = nullptr;              // null: `launch` can only --dry-run
    RuntimeRoots runtime_roots;
};

int RunCli(const std::vector<std::string>& args, std::ostream& out, std::ostream& err, CliEnv& env);

std::string EnvGet(const std::map<std::string, std::string>& env, const std::string& name);   // case-insensitive, like Windows
fs::path DefaultHome(const std::map<std::string, std::string>& env);   // %LOCALAPPDATA%\VRModLauncher (or ~/.local/share/VRModLauncher)
std::map<std::string, std::string> CurrentEnvironment();

}  // namespace vrml
