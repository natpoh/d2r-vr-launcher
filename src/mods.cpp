#include "mods.h"

#include <algorithm>
#include <cctype>
#include <set>

#include "builtin_mods.h"
#include "deps.h"
#include "game.h"
#include "releases.h"
#include "versioninfo.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#endif

namespace vrml {

namespace {

fs::path ModInfoPath(const fs::path& modDir, const std::string& folder) { return modDir / P(folder + ".mpq") / "modinfo.json"; }

// When the mod was first installed: the older of the creation times of its folder and of its modinfo.json (an update that rewrites modinfo.json
// gets a new one for the file, not for the folder; one that makes the folder anew changes both, and nothing short of that moves a mod in the list).
long long InstalledAt(const fs::path& modDir, const fs::path& modInfo) {
    const long long a = CreationTime(modDir), b = CreationTime(modInfo);
    if (a < 0) return b;
    if (b < 0) return a;
    return std::min(a, b);
}

std::string IdFromFolder(const std::string& folder) {
    std::string id;
    for (char c : folder) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u)) id += static_cast<char>(std::tolower(u));
        else if (!id.empty() && id.back() != '-') id += '-';
    }
    while (!id.empty() && id.back() == '-') id.pop_back();
    return id;
}

// The launcher's data for a D2RLoader mod that has none: start it with -mod.
Manifest PlainManifest(const std::string& folder, const std::string& id, const fs::path& dir) {
    Json j = Json::Object();
    j.set("schema", Json::String(kManifestSchema));
    j.set("id", Json::String(id));
    j.set("name", Json::String(folder));
    j.set("version", Json::String("0.0.0"));
    Json game = Json::Object();
    game.set("id", Json::String("d2r"));
    j.set("game", game);
    Json dm = Json::Object();
    dm.set("name", Json::String(folder));
    j.set("d2rloader_mod", dm);
    Json opt = Json::Object();
    opt.set("id", Json::String("default"));
    opt.set("label", Json::String("Play " + folder));
    opt.set("kind", Json::String("exec"));
    opt.set("exe", Json::String("{game}/D2RLoader.exe"));
    Json args = Json::Array();
    args.push(Json::String("-mod"));
    args.push(Json::String(folder));
    opt.set("args", args);
    opt.set("cwd", Json::String("{game}"));
    Json rts = Json::Array();
    for (const char* r : {"system", "steamvr", "vdxr", "file"}) rts.push(Json::String(r));
    opt.set("runtimes", rts);
    Json opts = Json::Array();
    opts.push(opt);
    Json launch = Json::Object();
    launch.set("options", opts);
    j.set("launch", launch);
    Manifest m;
    std::vector<Issue> issues;
    ParseManifest(j, &m, &issues);
    m.dir = dir;
    return m;
}

void Fill(ModEntry& e, const Manifest& m, const fs::path& dir, const std::string& source) {
    e.id = m.id;
    e.folder = m.d2rloader_mod;
    e.name = m.name;
    e.description = m.description;
    e.version = m.version;
    e.publisher = m.publisher;
    e.homepage = m.homepage;
    e.support = m.support;
    e.dir = dir;
    e.source = source;
    e.launchable = !m.launch.empty();
    e.manifest = m;
    e.manifest.dir = dir;
    e.found_version = source == "manifest" ? m.version : "";       // a manifest of the mod's own says what is installed; the built-in profile only says what the launcher shipped with
    e.found_from = source == "manifest" ? "manifest" : "";
    if (!m.icon.empty() && IsFile(dir / P(m.icon))) e.icon = dir / P(m.icon);
}

// vrmod-manifest.json of a mod folder: true when it was read and names this folder.
bool ReadModManifest(const fs::path& dir, const std::string& folder, Manifest* m, std::vector<std::string>* notes) {
    const fs::path file = dir / "vrmod-manifest.json";
    if (!IsFile(file)) return false;
    std::vector<Issue> issues;
    Manifest got;
    bool ok = false;
    try {
        ok = LoadManifest(file, &got, &issues);
    } catch (const Error& e) {
        notes->push_back(std::string("vrmod-manifest.json: ") + e.what());
        return false;
    }
    if (!ok) {
        notes->push_back("vrmod-manifest.json is not valid: " + IssuesText(issues));
        return false;
    }
    if (!got.is_d2rloader_mod()) {
        notes->push_back("vrmod-manifest.json does not name a D2RLoader mod (d2rloader_mod)");
        return false;
    }
    if (!IEquals(got.d2rloader_mod, folder)) {
        notes->push_back("vrmod-manifest.json names the mod \"" + got.d2rloader_mod + "\" but sits in the folder \"" + folder + "\"");
        return false;
    }
    *m = got;
    return true;
}

// The `version` of a vrmod-manifest.json that does NOT validate (a field from a newer or older schema, say), read from the raw JSON. Only when the file says it is THIS mod:
// its id is the known id and, if it names a D2RLoader mod, that is this folder. "" when the file is not JSON, names another mod or has no version number.
std::string LenientManifestVersion(const fs::path& dir, const std::string& id, const std::string& folder) {
    std::string text;
    if (!ReadFile(dir / "vrmod-manifest.json", &text)) return "";
    try {
        const Json j = Json::Parse(text);
        if (!j.is_object() || !IEquals(j.get_string("id"), id)) return "";
        if (const Json* dm = j.find("d2rloader_mod"))
            if (dm->is_object() && !dm->get_string("name").empty() && !IEquals(dm->get_string("name"), folder)) return "";
        const std::string v = j.get_string("version");
        return NormalizeVersion(v).empty() ? std::string() : v;
    } catch (const JsonError&) {
        return "";
    }
}

// "0.3.19" / "0.170.0.0" / "v0.3.19-windows-preview" -> "0.3.19" / "0.170.0" / "0.3.19"; a fourth number that is not 0 stays.
std::string PluginVersionText(const std::string& raw) {
    std::string n = NormalizeVersion(raw);
    if (n.empty()) return "";
    const std::vector<std::string> p = Split(n, '.');
    if (p.size() == 4 && p[3] == "0") n = p[0] + "." + p[1] + "." + p[2];
    return n;
}

// The version of the mod's own plugin: the DLL in <mod>/d2rloader/plugins whose version resource says `mod-id=<id>` in its Comments. A match on the mod id only (the plugin's file
// name is not part of it: earlier D2R VR Unleashed builds called it d2rl-vrcam.dll). The newest wins when more than one says so. "" when there is none or it has no version.
std::string PluginVersion(const fs::path& modDir, const std::string& id) {
    const fs::path plugins = modDir / "d2rloader" / "plugins";
    if (!IsDir(plugins)) return "";
    std::vector<fs::path> dlls;
    std::error_code ec;
    for (fs::directory_iterator it(plugins, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2) && IEquals(U(it->path().extension()), ".dll")) dlls.push_back(it->path());
    }
    std::sort(dlls.begin(), dlls.end());
    std::string best;
    for (const fs::path& dll : dlls) {
        const FileVersion fv = ReadFileVersion(dll);
        if (!fv.ok || !IEquals(CommentValue(fv.comments, "mod-id"), id)) continue;
        std::string v = PluginVersionText(fv.product_version);
        if (v.empty()) v = PluginVersionText(fv.file_version);
        if (v.empty() && fv.has_fixed && (fv.product_numbers[0] | fv.product_numbers[1] | fv.product_numbers[2] | fv.product_numbers[3]) != 0)
            v = PluginVersionText(std::to_string(fv.product_numbers[0]) + "." + std::to_string(fv.product_numbers[1]) + "." + std::to_string(fv.product_numbers[2]) +
                                  (fv.product_numbers[3] ? "." + std::to_string(fv.product_numbers[3]) : std::string()));
        if (v.empty()) continue;
        if (best.empty() || IsNewer(v, best)) best = v;
    }
    return best;
}

#ifdef _WIN32
bool RegString(HKEY root, const wchar_t* key, const wchar_t* name, std::wstring* out, REGSAM extra = 0) {
    HKEY k;
    if (RegOpenKeyExW(root, key, 0, KEY_READ | extra, &k) != ERROR_SUCCESS) return false;
    wchar_t buf[2048];
    DWORD n = sizeof buf, type = 0;
    const bool ok = RegQueryValueExW(k, name, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &n) == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ);
    RegCloseKey(k);
    if (ok) *out = std::wstring(buf, wcsnlen(buf, n / sizeof(wchar_t)));
    return ok;
}
#endif

}  // namespace

const std::vector<Manifest>& BuiltinMods() {
    static const std::vector<Manifest> mods = [] {
        std::vector<Manifest> v;
        for (const char* text : {kBuiltinD2rVr, kBuiltinD2rPcvr}) {
            Manifest m;
            std::vector<Issue> issues;
            if (ParseManifest(Json::Parse(text), &m, &issues)) v.push_back(m);
        }
        return v;
    }();
    return mods;
}

// ------------------------------------------------------------------ the game

GameInstall DescribeGame(const fs::path& dir, const std::string& source) {
    GameInstall g;
    g.dir = dir;
    g.source = source;
    g.store = DetectStore(dir);
    std::vector<std::string> builds;
    for (const Manifest& m : BuiltinMods())
        for (const std::string& b : m.builds)
            if (std::find(builds.begin(), builds.end(), b) == builds.end()) builds.push_back(b);
    g.build = DetectBuild(dir, builds);
    return g;
}

RuntimeRoots SystemRuntimeRoots(const std::map<std::string, std::string>& env) {
    RuntimeRoots r;
    for (const char* v : {"ProgramFiles", "ProgramW6432"}) {
        for (const auto& kv : env)
            if (IEquals(kv.first, v) && !kv.second.empty() && r.program_files.empty()) r.program_files = P(kv.second);
    }
#ifdef _WIN32
    std::wstring steam;
    if (RegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", &steam)) r.steam_libraries = SteamLibraries(fs::path(steam));
#endif
    return r;
}

GameRoots SystemGameRoots(const std::map<std::string, std::string>& env) {
    GameRoots g;
    const RuntimeRoots rr = SystemRuntimeRoots(env);
    g.steam_libraries = rr.steam_libraries;
#ifdef _WIN32
    // Battle.net registers the game's uninstall key (d2r-pcvr's game_install.py reads the same one, never account data)
    const wchar_t* bases[] = {L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall"};
    for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER})
        for (const wchar_t* base : bases) {
            std::wstring key = std::wstring(base) + L"\\Diablo II Resurrected", loc;
            if (RegString(root, key.c_str(), L"InstallLocation", &loc)) g.battlenet.push_back(fs::path(loc));
        }
#endif
    if (!rr.program_files.empty()) g.battlenet.push_back(rr.program_files / "Diablo II Resurrected");
    return g;
}

std::vector<GameInstall> FindGames(const GameRoots& roots) {
    std::vector<GameInstall> out;
    std::set<std::string> seen;
    auto add = [&](const fs::path& dir, const std::string& source) {
        if (dir.empty() || !LooksLikeGame(dir)) return;
        std::error_code ec;
        const fs::path canon = fs::weakly_canonical(dir, ec);
        const std::string key = Lower(U(ec ? dir : canon));
        if (!seen.insert(key).second) return;
        out.push_back(DescribeGame(dir, source));
    };
    for (const fs::path& lib : roots.steam_libraries) add(lib / "steamapps" / "common" / "Diablo II Resurrected", "Steam");
    for (const fs::path& d : roots.battlenet) add(d, "Battle.net");
    for (const fs::path& d : roots.extra) add(d, "chosen");
    return out;
}

// ------------------------------------------------------------------ D2RLoader

LoaderStatus CheckLoader(const fs::path& game, bool hash) {
    LoaderStatus st;
    st.present = IsFile(game / "D2RLoader.exe");
    if (!st.present) {
        st.notes.push_back("D2RLoader.exe is not in the game folder: the mods run only under D2RLoader");
        return st;
    }
    st.version = DetectLoaderVersion(game);
    if (!hash || BuiltinMods().empty()) return st;
    for (const RequirementResult& r : CheckRequirements(BuiltinMods().front(), game)) {
        if (r.id != "d2rloader") continue;
        st.reviewed = r.ok;
        for (const std::string& p : r.problems) st.notes.push_back(p);
        if (r.ok) st.version = "1.3.1-beta";
    }
    return st;
}

// ------------------------------------------------------------------ mods

void MissingModFiles(const fs::path& modDir, const Manifest& m, std::vector<std::string>* notes) {
    // `x-mod-files`: what the mod's Setup put into its folder ([{ "path": "d2rloader/plugins/d2rl-vrcam.dll", "sha256": "...", "optional": false }, ...]).
    // Only the presence is checked here (hashing the 10 MB plugin on every refresh is not worth it); a missing file is why a mod does not start.
    const Json* list = m.raw.find("x-mod-files");
    if (!list || !list->is_array()) return;
    for (const Json& f : list->items()) {
        const std::string rel = f.get_string("path");
        if (rel.empty() || !SafeRel(NormRel(rel)) || f.get_bool("optional")) continue;
        if (!IsFile(modDir / P(NormRel(rel)))) notes->push_back(m.name + ": " + rel + " is missing from its folder. Run its Setup again.");
    }
}

std::vector<std::string> GlobalPluginNotes(const fs::path& game) {
    std::vector<std::string> v;
    const fs::path plugins = game / "d2rloader" / "plugins";
    if (IsFile(plugins / "d2rl-vrcam.dll")) {
        // d2r-pcvr up to 0.3.15 used the same file name as D2R VR: its DLL has the cache folder name d2r_vr_icons as UTF-16, D2R VR's has d2r_vr_skill_icons
        std::string bytes, wide;
        for (char c : std::string("d2r_vr_icons")) { wide += c; wide += '\0'; }
        const bool his = ReadFile(plugins / "d2rl-vrcam.dll", &bytes) && bytes.find(wide) != std::string::npos;
        if (his)
            v.push_back("An earlier D2R VR Unleashed install (d2rloader/plugins/d2rl-vrcam.dll, a global plugin) is in this game folder. It loads into every mod and keeps D2R VR Authentic "
                        "from starting properly. Run the current D2R VR Unleashed Setup: it moves it into mods/D2R-PCVR.");
        else
            v.push_back("An earlier D2R VR install (d2rloader/plugins/d2rl-vrcam.dll, a global plugin) is in this game folder. It loads into every mod and keeps D2R VR Unleashed "
                        "from starting. Run the current D2R VR Setup: it moves D2R VR into its own mod folder.");
    }
    if (IsFile(plugins / "d2rl-pcvr.dll"))
        v.push_back("An earlier D2R VR Unleashed install (d2rloader/plugins/d2rl-pcvr.dll, a global plugin) is in this game folder. It loads into every mod. "
                    "Run the current D2R VR Unleashed Setup: it moves it into mods/D2R-PCVR.");
    return v;
}

std::vector<std::string> ReShadeEnvironmentNotes(const std::map<std::string, std::string>& env) {
    std::vector<std::string> v;
    std::vector<std::string> others;
    for (const auto& kv : env) {
        if (Lower(kv.first.substr(0, 8)) != "reshade_") continue;
        if (IEquals(kv.first, kReShadeBasePathVar)) {
            v.push_back(std::string(kReShadeBasePathVar) + " is set in your Windows environment (" + kv.second +
                        "). It points ReShade at one folder for every game and mod, which can leave a mod's ReShade unable to start (an empty headset). "
                        "The launcher removes it for the game it starts; remove it from Windows too (System Properties > Environment Variables) if you did not set it on purpose. "
                        "Starting the game any other way (Steam, a shortcut) still passes it on.");
        } else {
            others.push_back(kv.first);
        }
    }
    if (!others.empty()) {
        std::string list;
        for (const std::string& n : others) list += (list.empty() ? "" : ", ") + n;
        v.push_back("These ReShade variables are set in your Windows environment: " + list + ". The launcher passes them on unchanged.");
    }
    return v;
}

std::vector<ModEntry> DiscoverMods(const fs::path& game) {
    std::vector<ModEntry> out;
    std::set<std::string> seen;
    const fs::path mods = game / "mods";

    for (const Manifest& b : BuiltinMods()) {
        ModEntry e;
        const std::string folder = b.d2rloader_mod;
        const fs::path dir = mods / P(folder);
        seen.insert(Lower(folder));
        e.installed = IsFile(ModInfoPath(dir, folder));
        if (e.installed) e.installed_at = InstalledAt(dir, ModInfoPath(dir, folder));
        e.known = true;
        Manifest use = b;
        std::string source = "built-in";
        std::string found, found_from;
        if (e.installed) {
            Manifest m;
            if (ReadModManifest(dir, folder, &m, &e.notes)) {
                use = m;
                source = "manifest";
            } else {
                // a manifest that does not validate still names its version (the note says it is not valid); else the mod's own plugin says which release it is
                found = LenientManifestVersion(dir, b.id, folder);
                if (!found.empty()) found_from = "manifest (not valid)";
                if (found.empty()) {
                    found = PluginVersion(dir, b.id);
                    if (!found.empty()) found_from = "plugin";
                }
            }
        }
        Fill(e, use, dir, source);
        if (source != "manifest") {
            e.found_version = found;
            e.found_from = found_from;
        }
        if (e.installed) MissingModFiles(dir, use, &e.notes);
        for (const std::string& n : GlobalPluginNotes(game)) e.notes.push_back(n);   // also when the mod is not installed: an earlier install is why
        out.push_back(std::move(e));
    }
    // the order the player sees: the mod installed first on this computer first (an update does not move it), then the one installed later, then
    // the ones that are not installed; equal or unreadable times, or nothing installed: D2R VR Unleashed before D2R VR Authentic
    std::stable_sort(out.begin(), out.end(), [](const ModEntry& a, const ModEntry& b) {
        if (a.installed != b.installed) return a.installed;
        if (a.installed && a.installed_at >= 0 && b.installed_at >= 0 && a.installed_at != b.installed_at) return a.installed_at < b.installed_at;
        const int ra = a.id == "d2r-pcvr" ? 0 : 1, rb = b.id == "d2r-pcvr" ? 0 : 1;
        return ra < rb;
    });

    std::error_code ec;
    if (IsDir(mods)) {
        std::vector<fs::path> dirs;
        for (fs::directory_iterator it(mods, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_directory(ec) && !IsReparsePoint(it->path())) dirs.push_back(it->path());
        std::sort(dirs.begin(), dirs.end());
        for (const fs::path& dir : dirs) {
            const std::string folder = U(dir.filename());
            if (seen.count(Lower(folder))) continue;
            if (!IsFile(ModInfoPath(dir, folder))) continue;     // not a D2RLoader mod
            seen.insert(Lower(folder));
            ModEntry e;
            e.installed = true;
            e.installed_at = InstalledAt(dir, ModInfoPath(dir, folder));
            Manifest m;
            std::string source = "manifest";
            if (!ReadModManifest(dir, folder, &m, &e.notes)) {
                const std::string id = IdFromFolder(folder);
                if (id.size() < 2) {
                    e.notes.push_back("the folder name cannot be turned into a mod id");
                    continue;
                }
                m = PlainManifest(folder, id, dir);
                source = "plain";
            }
            Fill(e, m, dir, source);
            out.push_back(std::move(e));
        }
    }
    return out;
}

// ------------------------------------------------------------------ what a mod's buttons run

namespace {
std::string EnvValue(const std::map<std::string, std::string>& env, const std::string& name) {
    for (const auto& kv : env)
        if (IEquals(kv.first, name)) return kv.second;
    return "";
}
}  // namespace

std::string ExpandModPath(const std::string& text, const fs::path& game, const std::map<std::string, std::string>& env) {
    const std::pair<std::string, std::string> subs[] = {
        {"{game}", U(game)},
        {"{localappdata}", EnvValue(env, "LOCALAPPDATA")},
        {"{appdata}", EnvValue(env, "APPDATA")},
        {"{programfiles}", EnvValue(env, "ProgramFiles")},
    };
    std::string s = text;
    for (const auto& kv : subs) {
        // a placeholder whose folder is unknown stays as written (the path then simply does not exist)
        if (kv.second.empty()) continue;
        for (size_t at = 0; (at = s.find(kv.first, at)) != std::string::npos; at += kv.second.size()) s.replace(at, kv.first.size(), kv.second);
    }
    return s;
}

ModTools ResolveModTools(const ModEntry& mod, const fs::path& game, const std::map<std::string, std::string>& env) {
    ModTools t;
    const Manifest& m = mod.manifest;
    if (m.has_settings_app && !m.settings_exe.empty()) {
        t.settings_exe = P(ExpandModPath(m.settings_exe, game, env));
        t.settings_label = !m.settings_label.empty() ? m.settings_label : mod.name + " Settings";
        if (const Json* sa = m.raw.find("settings_app"))
            if (const Json* a = sa->find("args"))
                for (const Json& s : a->items()) t.settings_args.push_back(ExpandModPath(s.as_string(), game, env));
    }
    if (const Json* un = m.raw.find("uninstall")) {
        const std::string exe = un->get_string("exe");
        if (!exe.empty()) t.uninstall_exe = P(ExpandModPath(exe, game, env));
        if (const Json* a = un->find("args"))
            for (const Json& s : a->items()) t.uninstall_args.push_back(ExpandModPath(s.as_string(), game, env));
        t.uninstall_needs_admin = un->get_bool("needs_admin");
    }
    for (const std::string& l : m.logs) t.logs.push_back(P(ExpandModPath(l, game, env)));
    return t;
}

// ------------------------------------------------------------------ runtimes

std::vector<RuntimeChoice> ListRuntimes(const RuntimeRoots& roots, const std::map<std::string, std::string>&) {
    std::vector<RuntimeChoice> v;
    RuntimeChoice sys;
    sys.id = "system";
    sys.label = "System default";
    sys.available = true;
    sys.detail = "the OpenXR runtime Windows has set";
#ifdef _WIN32
    std::wstring active;
    if (RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime", &active, KEY_WOW64_64KEY)) sys.detail = U(fs::path(active));
#endif
    v.push_back(sys);
    for (const char* kind : {"steamvr", "vdxr"}) {
        RuntimeChoice c;
        c.id = kind;
        c.label = std::string(kind) == "steamvr" ? "SteamVR" : "Virtual Desktop (VDXR)";
        const fs::path p = FindRuntime(kind, roots);
        c.available = !p.empty();
        c.detail = c.available ? U(p) : (std::string(kind) == "steamvr" ? "SteamVR is not installed" : "Virtual Desktop Streamer is not installed");
        v.push_back(c);
    }
    RuntimeChoice f;
    f.id = "file";
    f.label = "A runtime manifest file...";
    f.available = true;
    f.detail = "any OpenXR runtime's .json";
    v.push_back(f);
    return v;
}

// ------------------------------------------------------------------ has the game been started once

fs::path SavedGamesFolder() {
#ifdef _WIN32
    PWSTR p = nullptr;
    fs::path r;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_SavedGames, 0, nullptr, &p)) && p) r = fs::path(p);
    CoTaskMemFree(p);
    return r;
#else
    return {};
#endif
}

fs::path GameSettingsFile(const fs::path& savedGames) { return savedGames / "Diablo II Resurrected" / "Settings.json"; }

bool GameStartedOnce(const fs::path& savedGames) { return IsFile(GameSettingsFile(savedGames)); }

bool FirstStartCheck(const fs::path& savedGames, bool skip, std::string* line) {
    if (savedGames.empty()) {
        *line = "first-start check: not made (Windows did not say where Saved Games is)";
        return true;
    }
    const bool once = GameStartedOnce(savedGames);
    *line = "first-start check: " + U(GameSettingsFile(savedGames)) + (once ? " is there" : skip ? " is missing (started anyway)" : " is missing: not started");
    return once || skip;
}

// ------------------------------------------------------------------ a start

StartOutcome StartMod(const StartRequest& req, ProcessProbe& probe, LaunchBackend& backend, const std::map<std::string, std::string>& env,
                      const RuntimeRoots& roots) {
    StartOutcome out;
    auto refuse = [&](const std::string& code, const std::string& msg) {
        out.code = code;
        out.message = msg;
        return out;
    };
    const ModEntry& mod = req.mod;
    if (!LooksLikeGame(req.game)) return refuse("E_GAME", "There is no D2R.exe in this folder. Pick the folder Diablo II: Resurrected is installed in.");
    std::vector<std::string> names = mod.manifest.processes;
    if (names.empty()) names = {"D2R.exe", "D2RLoader.exe"};
    const std::vector<std::string> running = probe.Running(names);
    if (!running.empty()) return refuse("E_GAME_RUNNING", "Close " + running.front() + " first: the game is running.");
    if (!mod.installed) return refuse("E_MOD_NOT_INSTALLED", mod.name + " is not installed in this game folder.");
    if (!mod.launchable) return refuse("E_PRECONDITION", mod.name + " has no way to be started.");
    for (const std::string& n : GlobalPluginNotes(req.game)) out.warnings.push_back(n);
    for (const std::string& n : ReShadeEnvironmentNotes(env)) out.warnings.push_back(n);
    const LoaderStatus loader = CheckLoader(req.game, true);
    if (!loader.present) return refuse("E_LOADER_MISSING", "D2RLoader.exe is not in the game folder. The mods run only under D2RLoader 1.3.1.");
    if (!loader.reviewed) {
        // (the file names and hashes are in loader.notes: `mods` and the log have them; a player needs the one sentence)
        out.warnings.push_back("D2RLoader's files are not the reviewed 1.3.1-beta ones. The mods were made for 1.3.1-beta.");
    }

    LaunchContext ctx;
    ctx.mod_id = mod.id;
    ctx.mod_version = mod.version;
    ctx.game = req.game;
    ctx.store = req.store.empty() ? DetectStore(req.game) : req.store;
    if (ctx.store.empty()) ctx.store = "steam";
    ctx.game_build = DetectBuild(req.game, mod.manifest.builds);
    ctx.env = {};          // the window starts the game from a clean desktop session: nothing to hand over to Explorer
    ctx.roots = roots;
    ctx.runtime = req.runtime;
    ctx.runtime_file = req.runtime_file;
    if (!mod.manifest.builds.empty() && !ctx.game_build.empty() &&
        std::find(mod.manifest.builds.begin(), mod.manifest.builds.end(), ctx.game_build) == mod.manifest.builds.end())
        out.warnings.push_back("The game is build " + ctx.game_build + "; " + mod.name + " was made for " + mod.manifest.builds.front() + ".");
    try {
        out.plan = BuildLaunchPlan(mod.manifest, req.option, ctx);
    } catch (const Error& e) {
        return refuse(e.code, e.what());
    }
    if (!FirstStartCheck(req.saved_games, req.skip_first_start_check, &out.first_start)) return refuse("E_FIRST_START", kFirstStartMessage);
    if (req.dry_run) return out;
    std::string error;
    if (!backend.Start(out.plan, ApplyEnv(env, out.plan), &error)) return refuse("E_IO", "Could not start the game: " + error);
    out.started = true;
    return out;
}

int StartExitCode(const std::string& code) {
    if (code.empty()) return 0;
    if (code == "E_GAME_RUNNING") return 10;
    if (code == "E_RUNTIME") return 24;
    if (code == "E_GAME") return 25;
    if (code == "E_IO") return 20;
    if (code == "E_FIRST_START") return 32;
    return 11;      // E_MOD_NOT_INSTALLED, E_LOADER_MISSING, E_PRECONDITION and anything else the start is not allowed to go on for
}

// ------------------------------------------------------------------ settings

LauncherSettings LoadSettings(const fs::path& home) {
    LauncherSettings s;
    std::string text;
    if (!ReadFile(home / "settings.json", &text)) return s;
    try {
        const Json j = Json::Parse(text);
        s.game = j.get_string("game");
        s.last_mod = j.get_string("last_mod");
        s.close_on_start = j.get_bool("close_on_start");
        s.check_updates_on_start = j.get_bool("check_updates_on_start", true);
        s.install_view_offered = j.get_bool("install_view_offered");
        if (const Json* mods = j.find("mods"))
            for (const auto& kv : mods->members()) {
                ModSettings ms;
                ms.runtime = kv.second.get_string("runtime");
                ms.runtime_file = kv.second.get_string("runtime_file");
                s.mods[kv.first] = ms;
            }
    } catch (const JsonError&) {
        return LauncherSettings();
    }
    return s;
}

void SaveSettings(const fs::path& home, const LauncherSettings& s) {
    Json j = Json::Object();
    j.set("schema", Json::String("vrmod-launcher-settings/1"));
    j.set("game", Json::String(s.game));
    j.set("last_mod", Json::String(s.last_mod));
    j.set("close_on_start", Json::Bool(s.close_on_start));
    j.set("check_updates_on_start", Json::Bool(s.check_updates_on_start));
    j.set("install_view_offered", Json::Bool(s.install_view_offered));
    Json mods = Json::Object();
    for (const auto& kv : s.mods) {
        Json m = Json::Object();
        m.set("runtime", Json::String(kv.second.runtime));
        m.set("runtime_file", Json::String(kv.second.runtime_file));
        mods.set(kv.first, m);
    }
    j.set("mods", mods);
    EnsureDir(home);
    WriteFileAtomic(home / "settings.json", j.dump());
}

}  // namespace vrml
