#include "registry.h"

#include <algorithm>

namespace vrml {

std::vector<ModRecord> Registry::List() const {
    std::vector<ModRecord> out;
    std::string text;
    if (!ReadFile(home_ / "registry.json", &text)) return out;
    Json j;
    try {
        j = Json::Parse(text);
    } catch (const JsonError& e) {
        Throw("E_IO", std::string("registry.json is damaged: ") + e.what());
    }
    const Json* mods = j.find("mods");
    if (!mods || !mods->is_array()) return out;
    for (const Json& m : mods->items()) {
        ModRecord r;
        r.id = m.get_string("id");
        r.version = m.get_string("version");
        r.previous_version = m.get_string("previous_version");
        r.registered_at = m.get_string("registered_at");
        if (!r.id.empty()) out.push_back(r);
    }
    return out;
}

void Registry::Save(const std::vector<ModRecord>& recs) const {
    Json j = Json::Object();
    j.set("schema", Json::Number(1));
    Json arr = Json::Array();
    for (const ModRecord& r : recs) {
        Json o = Json::Object();
        o.set("id", Json::String(r.id));
        o.set("version", Json::String(r.version));
        if (!r.previous_version.empty()) o.set("previous_version", Json::String(r.previous_version));
        o.set("registered_at", Json::String(r.registered_at));
        arr.push(o);
    }
    j.set("mods", arr);
    WriteFileAtomic(home_ / "registry.json", j.dump());
}

bool Registry::Has(const std::string& id) const {
    for (const ModRecord& r : List())
        if (r.id == id) return true;
    return false;
}

Manifest Registry::Load(const std::string& id) const {
    if (!Has(id)) Throw("E_NOT_FOUND", "no mod \"" + id + "\" is registered (vrmodlauncher list)");
    Manifest m;
    std::vector<Issue> issues;
    const fs::path file = ModDir(id) / "manifest.json";
    if (!IsFile(file)) Throw("E_NOT_FOUND", "the package of \"" + id + "\" is gone: " + U(file));
    if (!LoadManifest(file, &m, &issues)) Throw("E_MANIFEST", "the manifest of \"" + id + "\" is no longer valid:\n" + IssuesText(issues));
    if (m.id != id) Throw("E_MANIFEST", "the manifest in mods/" + id + " says id \"" + m.id + "\"");
    return m;
}

ModRecord Registry::Register(const fs::path& manifestFile) {
    Manifest m;
    std::vector<Issue> issues;
    if (!LoadManifest(manifestFile, &m, &issues)) Throw("E_MANIFEST", U(manifestFile) + " is not a valid manifest:\n" + IssuesText(issues));
    // the manifest must sit where the launcher looks: <home>/mods/<id>/manifest.json
    std::error_code ec;
    fs::path want = fs::weakly_canonical(ModDir(m.id), ec);
    fs::path have = fs::weakly_canonical(m.dir, ec);
    if (Lower(U(want)) != Lower(U(have)))
        Throw("E_USAGE", "put the package in " + U(ModDir(m.id)) + " first (the manifest is in " + U(m.dir) + ")");
    if (!CheckPackage(m, &issues)) Throw("E_MANIFEST", "the package of \"" + m.id + "\" does not match its manifest:\n" + IssuesText(issues));

    std::vector<ModRecord> recs = List();
    ModRecord rec;
    rec.id = m.id;
    rec.version = m.version;
    rec.registered_at = NowIso();
    bool replaced = false;
    for (ModRecord& r : recs)
        if (r.id == m.id) {
            rec.previous_version = r.version == m.version ? r.previous_version : r.version;
            r = rec;
            replaced = true;
        }
    if (!replaced) recs.push_back(rec);
    std::sort(recs.begin(), recs.end(), [](const ModRecord& a, const ModRecord& b) { return a.id < b.id; });
    Save(recs);
    if (log_) log_->Line("registered " + m.id + " " + m.version + (replaced ? " (replaces " + rec.previous_version + ")" : ""));
    return rec;
}

void Registry::Unregister(const std::string& id) {
    std::vector<ModRecord> recs = List();
    size_t before = recs.size();
    recs.erase(std::remove_if(recs.begin(), recs.end(), [&](const ModRecord& r) { return r.id == id; }), recs.end());
    if (recs.size() == before) Throw("E_NOT_FOUND", "no mod \"" + id + "\" is registered");
    Save(recs);
    if (log_) log_->Line("unregistered " + id);
}

std::vector<std::string> Registry::Unregistered() const {
    std::vector<std::string> out;
    std::error_code ec;
    for (auto it = fs::directory_iterator(ModsDir(), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (!it->is_directory() || !IsFile(it->path() / "manifest.json")) continue;
        std::string id = U(it->path().filename());
        if (!Has(id)) out.push_back(id);
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace vrml
