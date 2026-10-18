#include "launch.h"

#include <algorithm>
#include <memory>
#include <set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

namespace vrml {

namespace {
bool Has(const std::vector<std::string>& v, const std::string& x) { return std::find(v.begin(), v.end(), x) != v.end(); }

std::string Expand(std::string s, const LaunchContext& ctx) {
    const std::pair<std::string, std::string> subs[] = {{"{game}", U(ctx.game)}};
    for (const auto& kv : subs)
        for (size_t at; (at = s.find(kv.first)) != std::string::npos;) s.replace(at, kv.first.size(), kv.second);
    return s;
}
}  // namespace

std::vector<fs::path> SteamLibraries(const fs::path& steamRoot) {
    std::vector<fs::path> out;
    if (IsDir(steamRoot)) out.push_back(steamRoot);
    std::string text;
    if (!ReadFile(steamRoot / "steamapps" / "libraryfolders.vdf", &text)) return out;
    // lines like:   "path"		"D:\\SteamLibrary"
    size_t pos = 0;
    while ((pos = text.find("\"path\"", pos)) != std::string::npos) {
        pos += 6;
        size_t q1 = text.find('"', pos);
        if (q1 == std::string::npos) break;
        std::string val;
        size_t i = q1 + 1;
        for (; i < text.size() && text[i] != '"'; ++i) {
            if (text[i] == '\\' && i + 1 < text.size()) ++i;
            val += text[i];
        }
        pos = i;
        fs::path p = P(val);
        if (IsDir(p) && std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    }
    return out;
}

fs::path FindRuntime(const std::string& kind, const RuntimeRoots& roots) {
    if (kind == "steamvr") {
        for (const fs::path& lib : roots.steam_libraries) {
            fs::path f = lib / "steamapps" / "common" / "SteamVR" / "steamxr_win64.json";
            if (IsFile(f)) return f;
        }
    } else if (kind == "vdxr") {
        if (roots.program_files.empty()) return {};
        fs::path f = roots.program_files / "Virtual Desktop Streamer" / "OpenXR" / "virtualdesktop-openxr.json";
        if (IsFile(f)) return f;
    }
    return {};
}

std::string ResolveVia(const std::string& via, const std::map<std::string, std::string>& env) {
    if (via == "direct" || via == "explorer") return via;
    for (const auto& kv : env) {
        const std::string k = Lower(kv.first);
        if (k == "steamoverlaygameid" || k == "steamgameid") return "explorer";
    }
    return "direct";
}

std::map<std::string, std::string> ApplyEnv(const std::map<std::string, std::string>& base, const LaunchPlan& p) {
    std::map<std::string, std::string> out;
    std::set<std::string> drop;
    for (const std::string& k : p.env_unset) drop.insert(Lower(k));
    for (const auto& kv : base)
        if (!drop.count(Lower(kv.first))) out[kv.first] = kv.second;
    for (const auto& kv : p.env_set) {
        for (auto it = out.begin(); it != out.end();) {   // a set replaces the same name in another case
            if (IEquals(it->first, kv.first)) it = out.erase(it);
            else ++it;
        }
        out[kv.first] = kv.second;
    }
    return out;
}

LaunchPlan BuildLaunchPlan(const Manifest& m, const std::string& optionId, const LaunchContext& ctx) {
    if (m.launch.empty()) Throw("E_NOT_FOUND", "\"" + m.id + "\" declares no launch option");
    const LaunchOption* opt = nullptr;
    for (const LaunchOption& o : m.launch) {
        if (!optionId.empty() && o.id != optionId) continue;
        if (!o.stores.empty() && !Has(o.stores, ctx.store)) {
            if (!optionId.empty()) Throw("E_PRECONDITION", "launch option \"" + o.id + "\" of \"" + m.id + "\" does not support the " + ctx.store + " edition");
            continue;
        }
        opt = &o;
        break;
    }
    if (!opt) {
        if (!optionId.empty()) Throw("E_NOT_FOUND", "\"" + m.id + "\" has no launch option \"" + optionId + "\"");
        Throw("E_PRECONDITION", "\"" + m.id + "\" has no launch option for the " + ctx.store + " edition");
    }

    LaunchPlan p;
    p.option = opt->id;
    p.kind = opt->kind;
    p.store = ctx.store;
    p.mod_id = m.id;
    p.mod_version = m.version;
    p.exe = Expand(opt->exe, ctx);
    p.url = opt->url;
    p.cwd = Expand(opt->cwd.empty() ? "{game}" : opt->cwd, ctx);
    for (const std::string& a : opt->args) p.args.push_back(Expand(a, ctx));
    p.via = ResolveVia(opt->via, ctx.env);

    if (p.kind == "exec" && !IsFile(P(p.exe))) Throw("E_NOT_FOUND", p.exe + " not found - is the mod active, and D2RLoader installed?");

    // the runtime of THIS start
    std::vector<std::string> runtimes = opt->runtimes.empty() ? std::vector<std::string>{"system"} : opt->runtimes;
    std::string rt = ctx.runtime.empty() ? runtimes.front() : ctx.runtime;
    if (!Has(runtimes, rt)) {
        std::string list;
        for (const std::string& r : runtimes) list += (list.empty() ? "" : ", ") + r;
        Throw("E_RUNTIME", "launch option \"" + opt->id + "\" does not support the runtime \"" + rt + "\" (it supports: " + list + ")");
    }
    p.runtime = rt;

    // ReShade's base folder is chosen by each mod INSIDE its own game process. A copy that reaches the process from the player's environment (setx, a wrapper,
    // the other mod's earlier start) would send this mod's ReShade into another mod's folder, where it refuses to start (error 1114, an empty headset). So a mod
    // started by the launcher never inherits it; a manifest rule below may still set it on purpose.
    if (!m.d2rloader_mod.empty() && p.kind == "exec") p.env_unset.push_back(kReShadeBasePathVar);

    // env rules of the store, in order: unset, then set
    for (const EnvRule& r : opt->env) {
        if (!r.stores.empty() && !Has(r.stores, ctx.store)) continue;
        for (const std::string& k : r.unset) {
            p.env_unset.push_back(k);
            p.env_set.erase(k);
        }
        for (const auto& kv : r.set) p.env_set[kv.first] = Expand(kv.second, ctx);
    }
    if (rt == "steamvr" || rt == "vdxr") {
        fs::path f = FindRuntime(rt, ctx.roots);
        if (f.empty()) Throw("E_RUNTIME", std::string(rt == "vdxr" ? "Virtual Desktop's OpenXR runtime (Virtual Desktop Streamer)" : "SteamVR") +
                                              " was not found - the OpenXR runtime manifest is missing");
        p.env_set["XR_RUNTIME_JSON"] = U(f);
    } else if (rt == "file") {
        if (!IsFile(ctx.runtime_file)) Throw("E_RUNTIME", "the OpenXR runtime manifest was not found: " + U(ctx.runtime_file));
        p.env_set["XR_RUNTIME_JSON"] = U(ctx.runtime_file);
    }
    // for the mods' own logs and for support: who was started, in what
    p.env_set["VRMOD_ACTIVE_ID"] = m.id;
    p.env_set["VRMOD_ACTIVE_VERSION"] = m.version;
    p.env_set["VRMOD_GAME_BUILD"] = ctx.game_build;
    p.env_set["VRMOD_STORE"] = ctx.store;
    p.env_set["VRMOD_RUNTIME"] = rt;
    return p;
}

std::string LaunchPlan::Describe() const {
    std::string s = "launch " + mod_id + " " + mod_version + " (option " + option + ", " + store + " edition, runtime " + runtime + ", via " + via + ")\n";
    s += kind == "url" ? "  open  " + url + "\n" : "  run   " + exe + "\n";
    for (const std::string& a : args) s += "  arg   " + a + "\n";
    s += "  in    " + cwd + "\n";
    for (const std::string& k : env_unset) s += "  env - " + k + "\n";
    for (const auto& kv : env_set) s += "  env + " + kv.first + "=" + kv.second + "\n";
    return s;
}

Json LaunchPlan::ToJson() const {
    Json j = Json::Object();
    j.set("option", Json::String(option));
    j.set("kind", Json::String(kind));
    j.set("exe", Json::String(exe));
    j.set("url", Json::String(url));
    j.set("cwd", Json::String(cwd));
    j.set("via", Json::String(via));
    j.set("runtime", Json::String(runtime));
    j.set("store", Json::String(store));
    j.set("mod_id", Json::String(mod_id));
    j.set("mod_version", Json::String(mod_version));
    Json a = Json::Array();
    for (const std::string& s : args) a.push(Json::String(s));
    j.set("args", a);
    Json es = Json::Object();
    for (const auto& kv : env_set) es.set(kv.first, Json::String(kv.second));
    j.set("env_set", es);
    Json eu = Json::Array();
    for (const std::string& s : env_unset) eu.push(Json::String(s));
    j.set("env_unset", eu);
    return j;
}

LaunchPlan LaunchPlan::FromJson(const Json& j) {
    LaunchPlan p;
    p.option = j.get_string("option");
    p.kind = j.get_string("kind");
    p.exe = j.get_string("exe");
    p.url = j.get_string("url");
    p.cwd = j.get_string("cwd");
    p.via = j.get_string("via");
    p.runtime = j.get_string("runtime");
    p.store = j.get_string("store");
    p.mod_id = j.get_string("mod_id");
    p.mod_version = j.get_string("mod_version");
    if (const Json* a = j.find("args"))
        for (const Json& s : a->items()) p.args.push_back(s.as_string());
    if (const Json* e = j.find("env_set"))
        for (const Json::Member& kv : e->members()) p.env_set[kv.first] = kv.second.as_string();
    if (const Json* u = j.find("env_unset"))
        for (const Json& s : u->items()) p.env_unset.push_back(s.as_string());
    return p;
}

#ifdef _WIN32
namespace {
class WinBackend : public LaunchBackend {
public:
    bool Start(const LaunchPlan& plan, const std::map<std::string, std::string>& env, std::string* error) override {
        if (plan.via == "explorer") {
            if (error) *error = "starting through Explorer's desktop is not implemented in the prototype";
            return false;
        }
        if (plan.kind == "url") {
            HINSTANCE r = ShellExecuteW(nullptr, L"open", P(plan.url).wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            if (reinterpret_cast<INT_PTR>(r) <= 32) { if (error) *error = "ShellExecute failed"; return false; }
            return true;
        }
        std::wstring cmd = L"\"" + P(plan.exe).wstring() + L"\"";
        for (const std::string& a : plan.args) cmd += L" \"" + P(a).wstring() + L"\"";
        std::wstring block;
        for (const auto& kv : env) {
            block += P(kv.first).wstring() + L"=" + P(kv.second).wstring();
            block.push_back(L'\0');
        }
        block.push_back(L'\0');
        STARTUPINFOW si{};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> buf(cmd.begin(), cmd.end());
        buf.push_back(0);
        const std::wstring cwd = P(plan.cwd).wstring();
        if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT, block.empty() ? nullptr : &block[0],
                            cwd.c_str(), &si, &pi)) {
            if (error) *error = "CreateProcess failed (" + std::to_string(GetLastError()) + ")";
            return false;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return true;
    }
};
}  // namespace
std::unique_ptr<LaunchBackend> MakeSystemBackend() { return std::make_unique<WinBackend>(); }
#else
namespace {
class NoBackend : public LaunchBackend {
public:
    bool Start(const LaunchPlan&, const std::map<std::string, std::string>&, std::string* error) override {
        if (error) *error = "starting the game is Windows only";
        return false;
    }
};
}  // namespace
std::unique_ptr<LaunchBackend> MakeSystemBackend() { return std::make_unique<NoBackend>(); }
#endif

}  // namespace vrml
