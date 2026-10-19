// vrmodlauncher - the command line of the shared VR mod launcher (a draft; no GUI yet - see SPEC.md).
#include <iostream>

#include "cli.h"
#include "mods.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

int main(int argc, char** argv) {
    std::vector<std::string> args;
#ifdef _WIN32
    // UTF-8 arguments from the wide command line
    int wargc = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    for (int i = 1; wargv && i < wargc; ++i) args.push_back(vrml::U(std::filesystem::path(wargv[i])));
    if (wargv) LocalFree(wargv);
    (void)argc;
    (void)argv;
#else
    for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
#endif
    vrml::CliEnv env;
    env.env = vrml::CurrentEnvironment();
    auto probe = vrml::MakeSystemProbe();
    auto backend = vrml::MakeSystemBackend();
    env.probe = probe.get();
    env.backend = backend.get();
    env.default_home = vrml::DefaultHome(env.env);
    env.runtime_roots = vrml::SystemRuntimeRoots(env.env);   // Program Files and the Steam libraries
    env.saved_games = vrml::SavedGamesFolder();               // where the game keeps its Settings.json (`launch`'s first-start check)
    return vrml::RunCli(args, std::cout, std::cerr, env);
}
