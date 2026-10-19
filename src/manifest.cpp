#include "manifest.h"

#include <cctype>
#include <cstdlib>
#include <set>

#include "sha256.h"

namespace vrml {

const char* RoleName(Role r) {
    switch (r) {
        case Role::Program: return "program";
        case Role::Asset: return "asset";
        case Role::Config: return "config";
        case Role::Shared: return "shared";
        case Role::Generated: return "generated";
    }
    return "?";
}

std::string GameRel(const std::string& dst) {
    std::string d = dst;
    for (char& c : d)
        if (c == '\\') c = '/';
    const std::string pre = "{game}/";
    if (!StartsWith(d, pre)) return "";
    return NormRel(d.substr(pre.size()));
}

bool IsVersion(const std::string& v) {
    // major.minor.patch with an optional -prerelease (letters, digits, dots, dashes)
    size_t i = 0;
    for (int part = 0; part < 3; ++part) {
        size_t b = i;
        while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) ++i;
        if (i == b) return false;
        if (part < 2) {
            if (i >= v.size() || v[i] != '.') return false;
            ++i;
        }
    }
    if (i == v.size()) return true;
    if (v[i] != '-' || i + 1 >= v.size()) return false;
    for (++i; i < v.size(); ++i) {
        char c = v[i];
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-')) return false;
    }
    return true;
}

int CompareVersions(const std::string& a, const std::string& b) {
    auto split = [](const std::string& v, std::vector<long long>* nums, std::string* pre) {
        size_t dash = v.find('-');
        std::string core = dash == std::string::npos ? v : v.substr(0, dash);
        *pre = dash == std::string::npos ? "" : v.substr(dash + 1);
        for (const std::string& p : Split(core, '.')) nums->push_back(std::atoll(p.c_str()));
        while (nums->size() < 3) nums->push_back(0);
    };
    std::vector<long long> na, nb;
    std::string pa, pb;
    split(a, &na, &pa);
    split(b, &nb, &pb);
    for (size_t i = 0; i < na.size() && i < nb.size(); ++i)
        if (na[i] != nb[i]) return na[i] < nb[i] ? -1 : 1;
    if (pa == pb) return 0;
    if (pa.empty()) return 1;    // the release is above its pre-release
    if (pb.empty()) return -1;
    return pa < pb ? -1 : 1;
}

namespace {

struct V {
    std::vector<Issue>* out;
    void add(const std::string& path, const std::string& msg) { out->push_back({path, msg}); }
};

bool IdOk(const std::string& s, size_t minLen, size_t maxLen) {
    if (s.size() < minLen || s.size() > maxLen) return false;
    if (!(std::islower(static_cast<unsigned char>(s[0])) || std::isdigit(static_cast<unsigned char>(s[0])))) return false;
    for (char c : s)
        if (!(std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '-')) return false;
    return true;
}

// Rejects unknown keys (typos) except "x-..." extensions.
void KnownKeys(V& v, const Json& o, const std::string& path, std::initializer_list<const char*> allowed) {
    for (const Json::Member& m : o.members()) {
        if (StartsWith(m.first, "x-")) continue;
        bool ok = false;
        for (const char* a : allowed)
            if (m.first == a) { ok = true; break; }
        if (!ok) v.add(path + "/" + m.first, "unknown field");
    }
}

bool Obj(V& v, const Json* j, const std::string& path, bool required) {
    if (!j) {
        if (required) v.add(path, "required");
        return false;
    }
    if (!j->is_object()) { v.add(path, "must be an object"); return false; }
    return true;
}

std::string Str(V& v, const Json& o, const char* key, const std::string& path, bool required, bool nonEmpty = true) {
    const Json* j = o.find(key);
    if (!j) {
        if (required) v.add(path + "/" + key, "required");
        return "";
    }
    if (!j->is_string()) { v.add(path + "/" + key, "must be a string"); return ""; }
    if (nonEmpty && j->as_string().empty()) v.add(path + "/" + key, "must not be empty");
    return j->as_string();
}

bool Bool(V& v, const Json& o, const char* key, const std::string& path, bool def) {
    const Json* j = o.find(key);
    if (!j) return def;
    if (!j->is_bool()) { v.add(path + "/" + key, "must be true or false"); return def; }
    return j->as_bool();
}

std::vector<std::string> StrList(V& v, const Json& o, const char* key, const std::string& path) {
    std::vector<std::string> out;
    const Json* j = o.find(key);
    if (!j) return out;
    if (!j->is_array()) { v.add(path + "/" + key, "must be an array of strings"); return out; }
    for (size_t i = 0; i < j->items().size(); ++i) {
        const Json& e = j->items()[i];
        if (!e.is_string() || e.as_string().empty()) v.add(path + "/" + key + "/" + std::to_string(i), "must be a non-empty string");
        else out.push_back(e.as_string());
    }
    return out;
}

std::vector<std::string> EnumList(V& v, const Json& o, const char* key, const std::string& path, std::initializer_list<const char*> allowed) {
    std::vector<std::string> out = StrList(v, o, key, path);
    for (size_t i = 0; i < out.size(); ++i) {
        bool ok = false;
        for (const char* a : allowed)
            if (out[i] == a) ok = true;
        if (!ok) v.add(path + "/" + key + "/" + std::to_string(i), "unexpected value \"" + out[i] + "\"");
    }
    return out;
}

std::vector<std::string> HashList(V& v, const Json& o, const char* key, const std::string& path) {
    std::vector<std::string> out = StrList(v, o, key, path);
    for (size_t i = 0; i < out.size(); ++i)
        if (!IsSha256Hex(out[i])) v.add(path + "/" + key + "/" + std::to_string(i), "must be 64 hex digits (SHA-256)");
    return out;
}

// a path under {game}/ (placeholders: only {game} here)
std::string GamePath(V& v, const std::string& dst, const std::string& path, bool allowGlob) {
    std::string rel = GameRel(dst);
    if (rel.empty()) { v.add(path, "must start with {game}/"); return ""; }
    std::string why;
    std::string probe = rel;
    if (allowGlob)
        for (char& c : probe)
            if (c == '*' || c == '?') c = 'x';
    if (!SafeRel(probe, &why)) { v.add(path, why); return ""; }
    return rel;
}

Precondition ParsePrecondition(V& v, const Json& j, const std::string& path) {
    Precondition p;
    if (!Obj(v, &j, path, true)) return p;
    KnownKeys(v, j, path, {"type", "names", "file", "sha256_any_of", "key", "equals", "builds", "severity", "applies_to", "message"});
    p.type = Str(v, j, "type", path, true);
    static const std::set<std::string> kTypes = {"process_absent", "file_exists", "file_sha256_any_of", "toml_value", "game_build_in"};
    if (!p.type.empty() && !kTypes.count(p.type)) v.add(path + "/type", "unknown precondition type \"" + p.type + "\"");
    p.names = StrList(v, j, "names", path);
    p.file = Str(v, j, "file", path, false);
    p.sha256_any_of = HashList(v, j, "sha256_any_of", path);
    p.key = Str(v, j, "key", path, false);
    if (const Json* e = j.find("equals")) p.equals = *e;
    p.builds = StrList(v, j, "builds", path);
    p.severity = Str(v, j, "severity", path, false);
    if (p.severity.empty()) p.severity = "block";
    if (p.severity != "block" && p.severity != "warn") v.add(path + "/severity", "must be \"block\" or \"warn\"");
    p.applies_to = EnumList(v, j, "applies_to", path, {"install", "repair", "activate", "deactivate", "start"});
    if (p.applies_to.empty()) p.applies_to = {"activate", "start"};
    p.message = Str(v, j, "message", path, false);
    if (p.type == "process_absent" && p.names.empty()) v.add(path + "/names", "required for process_absent");
    if ((p.type == "file_exists" || p.type == "file_sha256_any_of" || p.type == "toml_value") && p.file.empty())
        v.add(path + "/file", "required for " + p.type);
    if (p.type == "file_sha256_any_of" && p.sha256_any_of.empty()) v.add(path + "/sha256_any_of", "required for file_sha256_any_of");
    if (p.type == "toml_value" && (p.key.empty() || !j.has("equals"))) v.add(path + "/key", "toml_value needs key and equals");
    if (p.type == "game_build_in" && p.builds.empty()) v.add(path + "/builds", "required for game_build_in");
    return p;
}

Hook ParseHook(V& v, const Json& j, const std::string& path) {
    Hook h;
    if (!Obj(v, &j, path, true)) return h;
    KnownKeys(v, j, path, {"exe", "args", "timeout_s"});
    h.exe = Str(v, j, "exe", path, true);
    h.args = StrList(v, j, "args", path);
    h.timeout_s = static_cast<int>(j.get_int("timeout_s", 30));
    if (h.timeout_s < 1 || h.timeout_s > 600) v.add(path + "/timeout_s", "must be 1..600");
    return h;
}

}  // namespace

bool ParseManifest(const Json& j, Manifest* out, std::vector<Issue>* issues) {
    Manifest local;
    Manifest& m = out ? *out : local;
    m = Manifest();
    std::vector<Issue> sink;
    std::vector<Issue>& iss = issues ? *issues : sink;
    const size_t before = iss.size();
    V v{&iss};
    m.raw = j;

    if (!Obj(v, &j, "", true)) return false;
    KnownKeys(v, j, "", {"schema", "id", "name", "version", "edition", "publisher", "homepage", "support", "license", "description", "build",
                         "game", "exclusive_group", "d2rloader_mod", "icon", "identity", "requires", "files", "external", "settings_app",
                         "settings_hint", "launch", "hooks", "preconditions", "import", "logs", "update", "uninstall", "install_help"});

    m.schema = Str(v, j, "schema", "", true);
    if (!m.schema.empty() && m.schema != kManifestSchema)
        v.add("/schema", "this launcher reads \"" + std::string(kManifestSchema) + "\", the manifest says \"" + m.schema + "\"");
    m.id = Str(v, j, "id", "", true);
    if (!m.id.empty() && !IdOk(m.id, 2, 40)) v.add("/id", "must be 2-40 characters: lower-case letters, digits and '-', starting with a letter or digit");
    m.name = Str(v, j, "name", "", true);
    m.version = Str(v, j, "version", "", true);
    if (!m.version.empty() && !IsVersion(m.version)) v.add("/version", "must be major.minor.patch with an optional -prerelease");
    m.edition = Str(v, j, "edition", "", false);
    m.publisher = Str(v, j, "publisher", "", false);
    m.homepage = Str(v, j, "homepage", "", false);
    m.support = Str(v, j, "support", "", false);
    m.license = Str(v, j, "license", "", false);
    m.description = Str(v, j, "description", "", false);
    m.install_help = Str(v, j, "install_help", "", false);
    // A D2RLoader mod is started with `D2RLoader.exe -mod <name>`: it has no files to swap and no exclusive group.
    if (const Json* dm = j.find("d2rloader_mod")) {
        if (Obj(v, dm, "/d2rloader_mod", false)) {
            KnownKeys(v, *dm, "/d2rloader_mod", {"name"});
            m.d2rloader_mod = Str(v, *dm, "name", "/d2rloader_mod", true);
            const std::string& n = m.d2rloader_mod;
            bool ok = !n.empty() && n.size() <= 64 && std::isalnum(static_cast<unsigned char>(n[0]));
            for (char c : n) ok = ok && (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.');
            if (!n.empty() && !ok) v.add("/d2rloader_mod/name", "a D2RLoader mod folder name: letters, digits, '-', '_' and '.', up to 64 characters");
        }
    }
    const bool isMod = !m.d2rloader_mod.empty();
    m.group = Str(v, j, "exclusive_group", "", !isMod);
    if (!m.group.empty() && !IdOk(m.group, 1, 60)) v.add("/exclusive_group", "lower-case letters, digits and '-'");
    if (isMod && !m.group.empty()) v.add("/exclusive_group", "a D2RLoader mod is not swapped: leave it out");
    m.icon = Str(v, j, "icon", "", false);
    if (!m.icon.empty()) {
        const bool bad = m.icon.find("..") != std::string::npos || m.icon[0] == '/' || m.icon[0] == '\\' || m.icon.find(':') != std::string::npos;
        if (bad) v.add("/icon", "a file name or a relative path inside the mod's folder");
    }

    if (const Json* b = j.find("build")) {
        if (Obj(v, b, "/build", false)) {
            KnownKeys(v, *b, "/build", {"commit", "date", "channel"});
            Str(v, *b, "commit", "/build", false);
            Str(v, *b, "date", "/build", false);
            Str(v, *b, "channel", "/build", false);
        }
    }

    // ---- game
    if (Obj(v, j.find("game"), "/game", true)) {
        const Json& g = *j.find("game");
        KnownKeys(v, g, "/game", {"id", "builds", "stores", "processes"});
        m.game_id = Str(v, g, "id", "/game", true);
        m.builds = StrList(v, g, "builds", "/game");
        m.stores = EnumList(v, g, "stores", "/game", {"steam", "battlenet"});
        m.processes = StrList(v, g, "processes", "/game");
    }

    // ---- identity
    if (const Json* idn = j.find("identity")) {
        if (Obj(v, idn, "/identity", false)) {
            KnownKeys(v, *idn, "/identity", {"plugin_id", "plugin_file", "product_name", "dll_sha256", "markers", "about_file", "log"});
            m.identity.plugin_id = Str(v, *idn, "plugin_id", "/identity", false);
            m.identity.plugin_file = Str(v, *idn, "plugin_file", "/identity", false);
            m.identity.product_name = Str(v, *idn, "product_name", "/identity", false);
            m.identity.dll_sha256 = HashList(v, *idn, "dll_sha256", "/identity");
            m.identity.markers = StrList(v, *idn, "markers", "/identity");
            m.identity.about_file = Str(v, *idn, "about_file", "/identity", false);
            m.identity.log = Str(v, *idn, "log", "/identity", false);
        }
    }

    // ---- requires
    if (const Json* rq = j.find("requires")) {
        if (!rq->is_array()) v.add("/requires", "must be an array");
        else
            for (size_t i = 0; i < rq->items().size(); ++i) {
                const std::string path = "/requires/" + std::to_string(i);
                const Json& r = rq->items()[i];
                if (!Obj(v, &r, path, true)) continue;
                KnownKeys(v, r, path, {"id", "version", "provider", "optional", "note", "files"});
                Requirement req;
                req.id = Str(v, r, "id", path, true);
                req.version = Str(v, r, "version", path, false);
                req.provider = Str(v, r, "provider", path, false);
                if (!req.provider.empty() && req.provider != "launcher" && req.provider != "mod" && req.provider != "user")
                    v.add(path + "/provider", "must be launcher, mod or user");
                req.optional = Bool(v, r, "optional", path, false);
                req.note = Str(v, r, "note", path, false);
                if (const Json* fl = r.find("files")) {
                    if (!fl->is_array()) v.add(path + "/files", "must be an array");
                    else
                        for (size_t k = 0; k < fl->items().size(); ++k) {
                            const std::string fp = path + "/files/" + std::to_string(k);
                            const Json& f = fl->items()[k];
                            if (!Obj(v, &f, fp, true)) continue;
                            KnownKeys(v, f, fp, {"path", "sha256_any_of"});
                            RequiredFile rf;
                            rf.path = Str(v, f, "path", fp, true);
                            if (!rf.path.empty()) GamePath(v, rf.path, fp + "/path", false);
                            rf.sha256_any_of = HashList(v, f, "sha256_any_of", fp);
                            req.files.push_back(rf);
                        }
                }
                m.requires_.push_back(req);
            }
    }

    // ---- files
    const Json* fl = j.find("files");
    if (!fl && !isMod) v.add("/files", "required");
    else if (fl && isMod) v.add("/files", "a D2RLoader mod is installed by its own installer and not swapped: leave files out");
    else if (!fl) {}
    else if (!fl->is_array()) v.add("/files", "must be an array");
    else {
        if (fl->items().empty()) v.add("/files", "a mod without files has nothing to activate");
        std::set<std::string> seen;
        for (size_t i = 0; i < fl->items().size(); ++i) {
            const std::string path = "/files/" + std::to_string(i);
            const Json& f = fl->items()[i];
            if (!Obj(v, &f, path, true)) continue;
            KnownKeys(v, f, path, {"src", "dst", "role", "seed", "sha256", "optional", "tag", "note"});
            FileEntry e;
            const std::string role = Str(v, f, "role", path, true);
            if (role == "program") e.role = Role::Program;
            else if (role == "asset") e.role = Role::Asset;
            else if (role == "config") e.role = Role::Config;
            else if (role == "shared") e.role = Role::Shared;
            else if (role == "generated") e.role = Role::Generated;
            else if (!role.empty()) v.add(path + "/role", "must be program, asset, config, shared or generated");
            e.dst = Str(v, f, "dst", path, true);
            e.src = Str(v, f, "src", path, e.role != Role::Generated);
            e.seed = Str(v, f, "seed", path, false);
            e.sha256 = Str(v, f, "sha256", path, false);
            e.optional = Bool(v, f, "optional", path, false);
            e.tag = Str(v, f, "tag", path, false);
            Str(v, f, "note", path, false, false);
            if (!e.dst.empty()) e.dst_rel = GamePath(v, e.dst, path + "/dst", e.role == Role::Generated);
            if (!e.src.empty()) {
                if (e.role == Role::Generated) v.add(path + "/src", "a generated entry has no src");
                std::string why;
                if (!SafeRel(e.src, &why)) v.add(path + "/src", why);
                const bool srcDir = e.src.back() == '/' || e.src.back() == '\\';
                const bool dstDir = !e.dst.empty() && (e.dst.back() == '/' || e.dst.back() == '\\');
                if (e.role != Role::Generated && srcDir != dstDir) v.add(path + "/dst", "src and dst must both be folders (end with '/') or both files");
                e.src = NormRel(e.src);
                if (srcDir && e.src.back() != '/') e.src += '/';
                if (dstDir && !e.dst_rel.empty() && e.dst_rel.back() != '/') e.dst_rel += '/';
            }
            if (!e.seed.empty()) {
                if (e.role != Role::Config) v.add(path + "/seed", "only a config entry has a seed");
                std::string why;
                if (!SafeRel(e.seed, &why)) v.add(path + "/seed", why);
                e.seed = NormRel(e.seed);
            }
            if (!e.sha256.empty()) {
                if (!IsSha256Hex(e.sha256)) v.add(path + "/sha256", "must be 64 hex digits (SHA-256)");
                if (e.is_dir()) v.add(path + "/sha256", "a folder entry has no single hash");
            }
            if (!e.dst_rel.empty() && !e.is_dir() && !seen.insert(Lower(e.dst_rel)).second) v.add(path + "/dst", "listed twice in this manifest");
            m.files.push_back(e);
        }
    }

    // ---- external (informational)
    if (const Json* ex = j.find("external")) {
        if (!ex->is_array()) v.add("/external", "must be an array");
        else
            for (size_t i = 0; i < ex->items().size(); ++i) {
                const std::string path = "/external/" + std::to_string(i);
                const Json& e = ex->items()[i];
                if (!Obj(v, &e, path, true)) continue;
                KnownKeys(v, e, path, {"dst", "note"});
                std::string dst = Str(v, e, "dst", path, true);
                if (!dst.empty()) {
                    static const char* roots[] = {"{game}/", "{localappdata}/", "{steam}/", "{saved_games}/", "{appdata}/", "{programfiles}/"};
                    bool ok = false;
                    for (const char* r : roots)
                        if (StartsWith(dst, r)) ok = true;
                    if (!ok) v.add(path + "/dst", "must start with {game}/, {localappdata}/, {appdata}/, {programfiles}/, {steam}/ or {saved_games}/");
                    m.external.push_back(dst);
                }
                Str(v, e, "note", path, false, false);
            }
    }

    // ---- settings app
    if (const Json* sa = j.find("settings_app")) {
        if (!sa->is_null()) {
            if (Obj(v, sa, "/settings_app", false)) {
                KnownKeys(v, *sa, "/settings_app", {"exe", "args", "requires_active", "hint", "label"});
                m.settings_exe = Str(v, *sa, "exe", "/settings_app", true);
                StrList(v, *sa, "args", "/settings_app");
                Bool(v, *sa, "requires_active", "/settings_app", true);
                m.settings_label = Str(v, *sa, "label", "/settings_app", false);
                m.has_settings_app = true;
            }
        }
    }
    m.settings_hint = Str(v, j, "settings_hint", "", false);

    // ---- launch
    if (const Json* ln = j.find("launch")) {
        if (Obj(v, ln, "/launch", false)) {
            KnownKeys(v, *ln, "/launch", {"options"});
            const Json* opts = ln->find("options");
            if (!opts || !opts->is_array() || opts->items().empty()) v.add("/launch/options", "required: a non-empty array");
            else {
                std::set<std::string> ids;
                for (size_t i = 0; i < opts->items().size(); ++i) {
                    const std::string path = "/launch/options/" + std::to_string(i);
                    const Json& o = opts->items()[i];
                    if (!Obj(v, &o, path, true)) continue;
                    KnownKeys(v, o, path, {"id", "label", "kind", "exe", "url", "args", "cwd", "via", "stores", "runtimes", "env", "needs"});
                    LaunchOption lo;
                    lo.id = Str(v, o, "id", path, true);
                    if (!lo.id.empty() && !IdOk(lo.id, 1, 40)) v.add(path + "/id", "lower-case letters, digits and '-'");
                    if (!lo.id.empty() && !ids.insert(lo.id).second) v.add(path + "/id", "duplicate launch option id");
                    lo.label = Str(v, o, "label", path, true);
                    lo.kind = Str(v, o, "kind", path, true);
                    if (!lo.kind.empty() && lo.kind != "exec" && lo.kind != "url") v.add(path + "/kind", "must be exec or url");
                    lo.exe = Str(v, o, "exe", path, false);
                    lo.url = Str(v, o, "url", path, false);
                    if (lo.kind == "exec" && lo.exe.empty()) v.add(path + "/exe", "required for kind exec");
                    if (lo.kind == "url" && lo.url.empty()) v.add(path + "/url", "required for kind url");
                    lo.cwd = Str(v, o, "cwd", path, false);
                    lo.via = Str(v, o, "via", path, false);
                    if (lo.via.empty()) lo.via = "auto";
                    if (lo.via != "auto" && lo.via != "direct" && lo.via != "explorer") v.add(path + "/via", "must be auto, direct or explorer");
                    lo.args = StrList(v, o, "args", path);
                    lo.stores = EnumList(v, o, "stores", path, {"steam", "battlenet"});
                    lo.runtimes = EnumList(v, o, "runtimes", path, {"system", "steamvr", "vdxr", "file"});
                    if (const Json* ev = o.find("env")) {
                        if (!ev->is_array()) v.add(path + "/env", "must be an array");
                        else
                            for (size_t k = 0; k < ev->items().size(); ++k) {
                                const std::string ep = path + "/env/" + std::to_string(k);
                                const Json& r = ev->items()[k];
                                if (!Obj(v, &r, ep, true)) continue;
                                KnownKeys(v, r, ep, {"stores", "set", "unset"});
                                EnvRule er;
                                er.stores = EnumList(v, r, "stores", ep, {"steam", "battlenet"});
                                er.unset = StrList(v, r, "unset", ep);
                                if (const Json* st = r.find("set")) {
                                    if (!st->is_object()) v.add(ep + "/set", "must be an object of strings");
                                    else
                                        for (const Json::Member& mm : st->members()) {
                                            if (!mm.second.is_string()) v.add(ep + "/set/" + mm.first, "must be a string");
                                            else er.set.emplace_back(mm.first, mm.second.as_string());
                                        }
                                }
                                lo.env.push_back(er);
                            }
                    }
                    if (const Json* nd = o.find("needs")) {
                        if (!nd->is_array()) v.add(path + "/needs", "must be an array");
                        else
                            for (size_t k = 0; k < nd->items().size(); ++k)
                                lo.needs.push_back(ParsePrecondition(v, nd->items()[k], path + "/needs/" + std::to_string(k)));
                    }
                    m.launch.push_back(lo);
                }
            }
        }
    }

    // ---- hooks
    if (const Json* hk = j.find("hooks")) {
        if (Obj(v, hk, "/hooks", false)) {
            KnownKeys(v, *hk, "/hooks", {"pre_activate", "post_activate", "pre_deactivate", "post_deactivate"});
            for (const Json::Member& mm : hk->members()) {
                if (StartsWith(mm.first, "x-")) continue;
                const std::string path = "/hooks/" + mm.first;
                if (!mm.second.is_array()) { v.add(path, "must be an array of hooks"); continue; }
                for (size_t k = 0; k < mm.second.items().size(); ++k)
                    m.hooks[mm.first].push_back(ParseHook(v, mm.second.items()[k], path + "/" + std::to_string(k)));
            }
        }
    }

    // ---- preconditions
    if (const Json* pc = j.find("preconditions")) {
        if (!pc->is_array()) v.add("/preconditions", "must be an array");
        else
            for (size_t i = 0; i < pc->items().size(); ++i)
                m.preconditions.push_back(ParsePrecondition(v, pc->items()[i], "/preconditions/" + std::to_string(i)));
    }

    // ---- import
    if (const Json* im = j.find("import")) {
        if (Obj(v, im, "/import", false)) {
            KnownKeys(v, *im, "/import", {"backups"});
            if (const Json* bk = im->find("backups")) {
                if (Obj(v, bk, "/import/backups", false)) {
                    KnownKeys(v, *bk, "/import/backups", {"globs", "record"});
                    m.import_.backup_globs = StrList(v, *bk, "globs", "/import/backups");
                    for (size_t i = 0; i < m.import_.backup_globs.size(); ++i)
                        GamePath(v, m.import_.backup_globs[i], "/import/backups/globs/" + std::to_string(i), true);
                    if (const Json* rc = bk->find("record")) {
                        if (Obj(v, rc, "/import/backups/record", false)) {
                            KnownKeys(v, *rc, "/import/backups/record", {"file", "paths_at"});
                            m.import_.record_file = Str(v, *rc, "file", "/import/backups/record", true);
                            m.import_.record_paths_at = Str(v, *rc, "paths_at", "/import/backups/record", true);
                        }
                    }
                }
            }
        }
    }

    m.logs = StrList(v, j, "logs", "");

    if (const Json* up = j.find("update")) {
        if (Obj(v, up, "/update", false)) {
            KnownKeys(v, *up, "/update", {"kind", "url", "repo", "tag", "opens"});
            std::string kind = Str(v, *up, "kind", "/update", true);
            if (!kind.empty() && kind != "http-json" && kind != "github-release") v.add("/update/kind", "must be http-json or github-release");
            Str(v, *up, "url", "/update", false);
            Str(v, *up, "repo", "/update", false);
            Str(v, *up, "tag", "/update", false);
            Str(v, *up, "opens", "/update", false);
        }
    }
    if (const Json* un = j.find("uninstall")) {
        if (Obj(v, un, "/uninstall", false)) {
            KnownKeys(v, *un, "/uninstall", {"exe", "args", "needs_admin"});
            Str(v, *un, "exe", "/uninstall", true);
            StrList(v, *un, "args", "/uninstall");
            Bool(v, *un, "needs_admin", "/uninstall", false);
        }
    }

    return iss.size() == before;
}

bool LoadManifest(const fs::path& file, Manifest* m, std::vector<Issue>* issues) {
    std::string text;
    if (!ReadFile(file, &text)) Throw("E_MANIFEST", "cannot read the manifest " + U(file));
    Json j;
    try {
        j = Json::Parse(text);
    } catch (const JsonError& e) {
        Throw("E_MANIFEST", U(file) + ": not valid JSON: " + e.what());
    }
    std::vector<Issue> local;
    std::vector<Issue>& iss = issues ? *issues : local;
    Manifest tmp;
    bool ok = ParseManifest(j, &tmp, &iss);
    tmp.dir = file.parent_path();
    if (m) *m = tmp;
    return ok;
}

std::string IssuesText(const std::vector<Issue>& issues) {
    std::string out;
    for (const Issue& i : issues) out += "  " + (i.path.empty() ? std::string("/") : i.path) + ": " + i.message + "\n";
    return out;
}

bool CheckPackage(const Manifest& m, std::vector<Issue>* issues) {
    const size_t before = issues->size();
    auto link = [&](const fs::path& p, const std::string& at) {
        if (IsReparsePoint(p)) issues->push_back({at, "the package must not contain links or junctions: " + U(p)});
    };
    for (size_t i = 0; i < m.files.size(); ++i) {
        const FileEntry& f = m.files[i];
        if (f.role == Role::Generated) continue;
        const std::string at = "/files/" + std::to_string(i);
        fs::path src = m.dir / P(f.src);
        auto missing = [&](const fs::path& what, const std::string& where) {
            if (!f.optional) issues->push_back({where, "missing in the package: " + U(what)});
        };
        if (f.is_dir()) {
            if (!IsDir(src)) { missing(src, at + "/src"); continue; }
            link(src, at + "/src");
            std::error_code ec;
            bool any = false;
            for (auto it = fs::recursive_directory_iterator(src, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (IsReparsePoint(it->path())) issues->push_back({at + "/src", "link inside the package: " + U(it->path())});
                if (it->is_regular_file()) any = true;
            }
            if (!any && !f.optional) issues->push_back({at + "/src", "the folder is empty: " + U(src)});
        } else {
            if (!IsFile(src)) { missing(src, at + "/src"); continue; }
            link(src, at + "/src");
            if (!f.sha256.empty()) {
                std::string h;
                Sha256File(src, &h);
                if (!IEquals(h, f.sha256)) issues->push_back({at + "/sha256", "the file in the package has " + h + ", the manifest says " + f.sha256});
            }
        }
        if (!f.seed.empty() && !IsFile(m.dir / P(f.seed))) issues->push_back({at + "/seed", "missing in the package: " + f.seed});
    }
    return issues->size() == before;
}

}  // namespace vrml
