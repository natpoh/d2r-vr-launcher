// launch.h - starting the active mod: which option, which store, which OpenXR runtime for THIS start only, and how the
// process is started. The planner is pure (tested); starting a process is a backend (Windows only, not covered by tests).
//
// From d2r-pcvr's launch_windows.py, moved here:
//   * the OpenXR runtime is chosen per launch with XR_RUNTIME_JSON (SteamVR, Virtual Desktop's VDXR, or a file; "system" removes an inherited one) - the system-wide
//     active runtime is never changed;
//   * a start from inside a Steam-launched process must go through Explorer's desktop (Steam's overlay injected into the loader
//     bootstrap crashed the game with 0xC0000005); `via: auto` picks that when Steam's variables are in the environment;
//   * Steam and Battle.net differ: Steam's ids are set for the Steam edition and removed for Battle.net - the mod declares it
//     in `launch.options[].env`.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "json.h"
#include "manifest.h"
#include "util.h"

namespace vrml {

// The variable that tells ReShade where its ini, log, presets and shaders live. Both mods set it themselves, in their own game process; the launcher never
// sets it for the user and removes an inherited copy from the game it starts (see BuildLaunchPlan).
inline constexpr const char* kReShadeBasePathVar = "RESHADE_BASE_PATH_OVERRIDE";

struct RuntimeRoots {
    std::vector<fs::path> steam_libraries;   // the Steam folder and the library folders (SteamLibraries)
    fs::path program_files;                  // C:\Program Files
};

// Steam's own folder first, then the "path" entries of steamapps/libraryfolders.vdf that exist.
std::vector<fs::path> SteamLibraries(const fs::path& steamRoot);
// "steamvr": <library>/steamapps/common/SteamVR/steamxr_win64.json; "vdxr": <Program Files>/Virtual Desktop Streamer/OpenXR/virtualdesktop-openxr.json.
// Empty path when it is not installed.
fs::path FindRuntime(const std::string& kind, const RuntimeRoots& roots);

struct LaunchContext {
    std::string mod_id, mod_version, game_build;
    std::string store;                       // steam | battlenet
    fs::path game;
    std::map<std::string, std::string> env;  // the launcher's own environment (for `via: auto`)
    RuntimeRoots roots;
    std::string runtime;                     // "" = the option's first | system | steamvr | vdxr | file
    fs::path runtime_file;                   // for runtime "file"
};

struct LaunchPlan {
    std::string option, kind, exe, url, cwd, via;     // via: direct | explorer (never "auto" here)
    std::vector<std::string> args;
    std::map<std::string, std::string> env_set;
    std::vector<std::string> env_unset;
    std::string runtime, store;
    std::string mod_id, mod_version;

    std::string Describe() const;
    Json ToJson() const;                              // the request an Explorer-started child reads
    static LaunchPlan FromJson(const Json& j);
};

// The launch plan for `optionId` ("" = the first option that fits the store). Errors: E_NOT_FOUND (no such option, no exe),
// E_RUNTIME (the option does not support the runtime, or the runtime is not installed), E_PRECONDITION (store not supported).
LaunchPlan BuildLaunchPlan(const Manifest& m, const std::string& optionId, const LaunchContext& ctx);

// "auto" -> "explorer" when the environment shows Steam started us, else "direct".
std::string ResolveVia(const std::string& via, const std::map<std::string, std::string>& env);

// The environment a child gets: `base` minus env_unset plus env_set (names compare case-insensitively, as on Windows).
std::map<std::string, std::string> ApplyEnv(const std::map<std::string, std::string>& base, const LaunchPlan& p);

class LaunchBackend {
public:
    virtual ~LaunchBackend() = default;
    virtual bool Start(const LaunchPlan& plan, const std::map<std::string, std::string>& env, std::string* error) = 0;
};
// Windows: CreateProcess for `direct`; `explorer` is NOT implemented in the prototype (see SPEC: the port of launch_from_desktop).
std::unique_ptr<LaunchBackend> MakeSystemBackend();

}  // namespace vrml
