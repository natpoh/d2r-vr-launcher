// vrmodlauncher - the command line of the shared VR mod launcher (a draft; no GUI yet - see SPEC.md).
#include <iostream>

#include "cli.h"

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
#ifdef _WIN32
    // where the OpenXR runtimes are looked for
    for (const char* v : {"ProgramFiles", "ProgramW6432"}) {
        const std::string pf = vrml::EnvGet(env.env, v);
        if (!pf.empty() && env.runtime_roots.program_files.empty()) env.runtime_roots.program_files = vrml::P(pf);
    }
    // Steam's folder from the registry
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        wchar_t buf[MAX_PATH];
        DWORD n = sizeof buf, type = 0;
        if (RegQueryValueExW(k, L"SteamPath", nullptr, &type, reinterpret_cast<LPBYTE>(buf), &n) == ERROR_SUCCESS && type == REG_SZ)
            env.runtime_roots.steam_libraries = vrml::SteamLibraries(std::filesystem::path(buf));
        RegCloseKey(k);
    }
#endif
    return vrml::RunCli(args, std::cout, std::cerr, env);
}
