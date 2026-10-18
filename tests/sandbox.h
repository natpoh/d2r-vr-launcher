// sandbox.h - a fake game folder, a launcher home, and small mod packages built from a description. Nothing here touches a real
// game: every test works in its own folder under VRML_TEST_DIR (or the system temp folder) and removes it at the end.
#pragma once

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "engine.h"
#include "json.h"
#include "manifest.h"
#include "process.h"
#include "registry.h"
#include "sha256.h"
#include "util.h"

namespace tt {

using namespace vrml;

inline fs::path TempBase() {
    if (const char* e = std::getenv("VRML_TEST_DIR")) return fs::path(e);
    return fs::temp_directory_path();
}

inline void Write(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << content;
}
// Sets the creation time of a file or folder to day `day` of a fixed year (what the launcher orders the mods by); a no-op off Windows.
inline void SetCreated(const fs::path& p, int day) {
#ifdef _WIN32
    ULARGE_INTEGER t;
    t.QuadPart = 133801632000000000ULL + static_cast<ULONGLONG>(day) * 864000000000ULL;
    FILETIME ft{t.LowPart, t.HighPart};
    HANDLE h = CreateFileW(p.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        SetFileTime(h, &ft, nullptr, nullptr);
        CloseHandle(h);
    }
#else
    (void)p;
    (void)day;
#endif
}
inline std::string Read(const fs::path& p) {
    std::string s;
    ReadFile(p, &s);
    return s;
}

struct Sandbox {
    fs::path root, home, game;
    FakeProbe probe;
    Logger log;

    Sandbox() {
        static std::atomic<int> counter{0};
        root = TempBase() / ("vrml_test_" + std::to_string(counter++) + "_" + std::to_string(std::random_device{}()));
        fs::remove_all(root);
        home = root / "home";
        game = root / "game";
        fs::create_directories(home);
        // a fake game: D2R.exe holding the build as UTF-16 text (what d2r-pcvr's check looks for), the store marker, a few files that
        // are NOT any mod's and must never be touched
        std::string exe = "MZ-fake-D2R.exe ";
        for (char c : std::string("3.3.93787")) { exe += c; exe += '\0'; }
        Write(game / "D2R.exe", exe);
        Write(game / "Data" / ".build.config", "build-config");
        Write(game / "D2RLoader.exe", "fake D2RLoader");
        Write(game / "d2rloader" / "config" / "d2rloader.toml", "[d2rloader.advanced]\nallow_global_extensions = true\n");
        Write(game / "d2rloader" / "logs" / "d2rloader.log", "[x] D2RLoader v1.3.1-beta (Release, Release) started.\n[x] [BUILD] D2R version: 3.3.93787.\n");
        Write(game / "d2rloader" / "plugins" / "other_plugin.dll", "someone else's plugin");
        Write(game / "ReShade64.dll", "reshade");
        Write(game / "Saved" / "Settings.json", "{}");
    }
    ~Sandbox() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    Sandbox(const Sandbox&) = delete;
    Sandbox& operator=(const Sandbox&) = delete;

    Engine MakeEngine() {
        Services s;
        s.probe = &probe;
        s.log = &log;
        return Engine(home, s);
    }
    Engine MakeEngine(std::function<void(int, const char*)> crash) {
        Services s;
        s.probe = &probe;
        s.log = &log;
        s.crash = std::move(crash);
        return Engine(home, s);
    }
};

struct F {
    std::string src;       // in the package, e.g. "payload/plugins/x.dll" (a trailing '/' = a folder: then `files` holds its contents)
    std::string dst;       // game-relative
    std::string role;
    std::string content;   // the file (for a folder: unused)
    std::string seed;      // config: the default content ("" = none)
    std::map<std::string, std::string> folder;   // folder entries: relative name -> content
};

struct ModSpec {
    std::string id, version = "1.0.0", group = "vr-plugin", name;
    std::vector<F> files;
    std::vector<std::string> processes;           // game.processes ("" = the default)
    std::vector<std::string> plugin_hashes;       // identity.dll_sha256
    std::string plugin_file;
    std::vector<std::string> markers;
    Json extra = Json::Object();                  // merged into the manifest (launch, hooks, preconditions, ...)
};

// Writes <home>/mods/<id>/ (manifest + payload) - an installer's first step. Returns the manifest path.
inline fs::path WritePackage(const fs::path& home, const ModSpec& s) {
    const fs::path dir = home / "mods" / s.id;
    fs::remove_all(dir);
    Json m = Json::Object();
    m.set("schema", Json::String("vrmod-manifest/1"));
    m.set("id", Json::String(s.id));
    m.set("name", Json::String(s.name.empty() ? s.id : s.name));
    m.set("version", Json::String(s.version));
    Json game = Json::Object();
    game.set("id", Json::String("d2r"));
    Json builds = Json::Array();
    builds.push(Json::String("3.3.93787"));
    game.set("builds", builds);
    if (!s.processes.empty()) {
        Json pr = Json::Array();
        for (const std::string& p : s.processes) pr.push(Json::String(p));
        game.set("processes", pr);
    }
    m.set("game", game);
    m.set("exclusive_group", Json::String(s.group));
    if (!s.plugin_file.empty()) {
        Json id = Json::Object();
        id.set("plugin_file", Json::String(s.plugin_file));
        if (!s.plugin_hashes.empty()) {
            Json h = Json::Array();
            for (const std::string& x : s.plugin_hashes) h.push(Json::String(x));
            id.set("dll_sha256", h);
        }
        if (!s.markers.empty()) {
            Json mk = Json::Array();
            for (const std::string& x : s.markers) mk.push(Json::String(x));
            id.set("markers", mk);
        }
        m.set("identity", id);
    }
    Json files = Json::Array();
    for (const F& f : s.files) {
        Json e = Json::Object();
        e.set("src", Json::String(f.src));
        e.set("dst", Json::String("{game}/" + f.dst));
        e.set("role", Json::String(f.role));
        if (!f.seed.empty()) {
            e.set("seed", Json::String(f.src + ".default"));
            Write(dir / P(f.src + ".default"), f.seed);
        }
        files.push(e);
        if (!f.src.empty() && f.src.back() == '/') {
            for (const auto& kv : f.folder) Write(dir / P(f.src) / P(kv.first), kv.second);
        } else {
            Write(dir / P(f.src), f.content);
        }
    }
    m.set("files", files);
    for (const Json::Member& kv : s.extra.members()) m.set(kv.first, kv.second);
    const fs::path mf = dir / "manifest.json";
    Write(mf, m.dump());
    return mf;
}

// WritePackage + register
inline void Install(Sandbox& sb, const ModSpec& s) {
    Registry r(sb.home, &sb.log);
    r.Register(WritePackage(sb.home, s));
}

// A stand-in for D2R VR: the plugin, a settings exe, a shader, shared sky pictures, two config files.
inline ModSpec AlphaSpec(const std::string& version = "1.0.0") {
    ModSpec s;
    s.id = "alpha";
    s.name = "Alpha VR";
    s.version = version;
    s.plugin_file = "d2rl-vrcam.dll";
    s.markers = {"ALPHA-MARKER"};
    s.files = {
        {"payload/plugins/d2rl-vrcam.dll", "d2rloader/plugins/d2rl-vrcam.dll", "program", "ALPHA-DLL-" + version + " ALPHA-MARKER", "", {}},
        {"payload/Alpha_Settings.exe", "Alpha_Settings.exe", "program", "ALPHA-SETTINGS", "", {}},
        {"payload/Shaders/D2R_DepthFog.fx", "reshade-shaders/Shaders/D2R_DepthFog.fx", "asset", "ALPHA-FX-" + version, "", {}},
        {"payload/sky/", "reshade-shaders/Textures/D2R_Sky_ours/", "shared", "", "", {{"sky_a.png", "SKY-A"}, {"sky_b.png", "SKY-B"}}},
        {"payload/user/d2r_vr.ini", "d2rloader/plugins/d2r_vr.ini", "config", "", "[camera]\nmode=alpha\n", {}},
        {"payload/user/ReShade.ini", "ReShade.ini", "config", "", "alpha-reshade\n", {}},
    };
    return s;
}

// A stand-in for d2r-pcvr: same plugin name, same ini, same shader name (different bytes), an OpenXR loader outside the plugins
// folder, its own ReShade files, the same sky pictures.
inline ModSpec BetaSpec(const std::string& version = "2.0.0") {
    ModSpec s;
    s.id = "beta";
    s.name = "Beta PCVR";
    s.version = version;
    s.plugin_file = "d2rl-vrcam.dll";
    s.markers = {"BETA-MARKER"};
    s.files = {
        {"payload/plugins/d2rl-vrcam.dll", "d2rloader/plugins/d2rl-vrcam.dll", "program", "BETA-DLL-" + version + " BETA-MARKER", "", {}},
        {"payload/openxr_loader.dll", "openxr_loader.dll", "program", "BETA-OPENXR", "", {}},
        {"payload/Shaders/D2R_DepthFog.fx", "reshade-shaders/Shaders/D2R_DepthFog.fx", "asset", "BETA-FX-" + version, "", {}},
        {"payload/sky/", "reshade-shaders/Textures/D2R_Sky_ours/", "shared", "", "", {{"sky_a.png", "SKY-A"}, {"sky_b.png", "SKY-B"}}},
        {"payload/user/d2r_vr.ini", "d2rloader/plugins/d2r_vr.ini", "config", "", "[camera]\nmode=beta\n", {}},
        {"payload/user/ReShade.ini", "ReShade.ini", "config", "", "beta-reshade\n", {}},
        {"payload/user/D2R_Frame_ReShade.ini", "D2R_Frame_ReShade.ini", "config", "", "Techniques=beta\n", {}},
    };
    return s;
}

// every file under dir -> its SHA-256 (relative paths, '/'); for "the folder is exactly as it was"
inline std::map<std::string, std::string> Snapshot(const fs::path& dir) {
    std::map<std::string, std::string> out;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file()) {
            std::string h;
            Sha256File(it->path(), &h);
            out[U(it->path().lexically_relative(dir))] = h;
        }
    return out;
}

inline std::string DiffText(const std::map<std::string, std::string>& a, const std::map<std::string, std::string>& b) {
    std::string s;
    for (const auto& kv : a) {
        auto it = b.find(kv.first);
        if (it == b.end()) s += "  gone: " + kv.first + "\n";
        else if (it->second != kv.second) s += "  changed: " + kv.first + "\n";
    }
    for (const auto& kv : b)
        if (!a.count(kv.first)) s += "  new: " + kv.first + "\n";
    return s;
}

}  // namespace tt
