#include "engine.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <memory>

#include "game.h"
#include "sha256.h"

namespace vrml {

namespace {

constexpr int kKeepBackups = 3;

std::string FileHash(const fs::path& p) {
    std::string h;
    if (!Sha256File(p, &h)) Throw("E_IO", "cannot read " + U(p));
    return h;
}

bool Contains(const std::vector<std::string>& v, const std::string& x) { return std::find(v.begin(), v.end(), x) != v.end(); }

std::string ExpandGame(std::string s, const fs::path& game) {
    const std::string tok = "{game}";
    for (size_t at; (at = s.find(tok)) != std::string::npos;) s.replace(at, tok.size(), U(game));
    return s;
}

std::vector<std::string> DefaultProcesses(const Manifest& m) {
    return m.processes.empty() ? std::vector<std::string>{"D2R.exe", "D2RLoader.exe"} : m.processes;
}

// ------------------------------------------------------------------ the files a manifest puts in the game

struct EF {
    std::string dst;      // game-relative
    fs::path src;         // payload file
    Role role = Role::Asset;
    std::string sha;      // asset/program/shared: the payload's hash; config: the seed's (or "")
    fs::path seed;
};

std::vector<EF> Expand(const Manifest& m) {
    std::vector<EF> out;
    std::set<std::string> seen;
    auto add = [&](EF ef) {
        if (!seen.insert(Lower(ef.dst)).second) Throw("E_MANIFEST", "\"" + m.id + "\" names " + ef.dst + " twice (a folder entry overlaps a file entry)");
        out.push_back(std::move(ef));
    };
    for (const FileEntry& f : m.files) {
        if (f.role == Role::Generated) continue;
        const fs::path src = m.dir / P(f.src);
        fs::path seed = f.seed.empty() ? fs::path() : m.dir / P(f.seed);
        if (f.is_dir()) {
            if (!IsDir(src)) {
                if (f.optional) continue;
                Throw("E_MANIFEST", "\"" + m.id + "\": the folder " + f.src + " is missing from the package");
            }
            std::vector<fs::path> files;
            std::error_code ec;
            for (auto it = fs::recursive_directory_iterator(src, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
                if (it->is_regular_file()) files.push_back(it->path());
            std::sort(files.begin(), files.end());
            for (const fs::path& file : files) {
                EF ef;
                ef.dst = f.dst_rel + U(file.lexically_relative(src));
                ef.src = file;
                ef.role = f.role;
                ef.sha = f.role == Role::Config ? "" : FileHash(file);
                add(ef);
            }
        } else {
            if (!IsFile(src)) {
                if (f.optional) continue;
                Throw("E_MANIFEST", "\"" + m.id + "\": " + f.src + " is missing from the package");
            }
            EF ef;
            ef.dst = f.dst_rel;
            ef.src = src;
            ef.role = f.role;
            ef.seed = seed;
            if (f.role == Role::Config) ef.sha = IsFile(seed) ? FileHash(seed) : "";
            else ef.sha = FileHash(src);
            add(ef);
        }
    }
    return out;
}

// A game folder must not be a maze of links: the engine writes through no junction or symlink.
void CheckNoLinks(const fs::path& game, const std::string& rel) {
    fs::path cur = game;
    for (const std::string& part : Split(rel, '/')) {
        if (part.empty()) continue;
        cur /= P(part);
        if (Exists(cur) && IsReparsePoint(cur))
            Throw("E_PRECONDITION", "refusing to write through a link or junction: " + U(cur));
    }
}

// ------------------------------------------------------------------ operations and their undo

void DoOp(const Op& o) {
    const fs::path dst = P(o.dst);
    if (o.type == "capture") {
        if (!IsFile(dst)) return;
        if (o.had_prev) CopyAtomic(P(o.store), P(o.prev));
        CopyAtomic(dst, P(o.store));
    } else if (o.type == "backup") {
        if (!IsFile(dst)) return;
        CopyAtomic(dst, P(o.bak));
        if (FileHash(dst) != FileHash(P(o.bak))) Throw("E_HASH", "the backup of " + o.dst + " does not match the file");
        std::error_code ec;
        fs::remove(dst, ec);
        if (ec) Throw("E_IO", "cannot remove " + o.dst + " (is it in use?): " + ec.message());
    } else if (o.type == "install" || o.type == "restore") {
        CopyAtomic(P(o.src), dst);
        if (FileHash(dst) != o.sha) {
            RemoveQuiet(dst);   // never leave a file we know is wrong where the mod will look for it
            Throw("E_HASH", "after copying, " + o.dst + " does not have the expected hash (the package changed, or the disk or a virus scanner did)");
        }
    } else {
        Throw("E_IO", "unknown operation \"" + o.type + "\" in the journal");
    }
}

void UndoOp(const Op& o) {
    const fs::path dst = P(o.dst);
    auto dropTmp = [](const fs::path& p) {
        fs::path t = p;
        t += kTmpSuffix;
        RemoveQuiet(t);
    };
    if (!o.dst.empty()) dropTmp(dst);
    if (o.type == "capture") {
        dropTmp(P(o.store));
        if (o.had_prev) {
            if (IsFile(P(o.prev))) CopyAtomic(P(o.prev), P(o.store));   // never copied = the store was not touched
        } else {
            RemoveQuiet(P(o.store));
        }
    } else if (o.type == "backup") {
        dropTmp(P(o.bak));
        if (IsFile(P(o.bak)) && !(IsFile(dst) && FileHash(dst) == FileHash(P(o.bak)))) CopyAtomic(P(o.bak), dst);
    } else if (o.type == "install" || o.type == "restore") {
        if (IsFile(dst) && FileHash(dst) == o.sha) RemoveQuiet(dst);
    }
}

}  // namespace

// ------------------------------------------------------------------ JSON of state and ops

Json GameState::ToJson() const {
    Json j = Json::Object();
    j.set("schema", Json::Number(1));
    j.set("path", Json::String(path));
    j.set("store", Json::String(store));
    j.set("build", Json::String(build));
    j.set("last_tx", Json::String(last_tx));
    Json act = Json::Object();
    for (const auto& kv : active) {
        const ActiveMod& a = kv.second;
        Json o = Json::Object();
        o.set("id", Json::String(a.id));
        o.set("version", Json::String(a.version));
        o.set("group", Json::String(a.group));
        o.set("activated_at", Json::String(a.activated_at));
        Json arr = Json::Array();
        for (const AppliedFile& f : a.applied) {
            Json e = Json::Object();
            e.set("dst", Json::String(f.dst));
            e.set("role", Json::String(f.role));
            e.set("sha256", Json::String(f.sha256));
            if (f.pre) e.set("pre", Json::Bool(true));
            if (!f.foreign.empty()) {
                e.set("foreign", Json::String(f.foreign));
                e.set("foreign_sha", Json::String(f.foreign_sha));
            }
            arr.push(e);
        }
        o.set("applied", arr);
        act.set(kv.first, o);
    }
    j.set("active", act);
    if (!last_launch.is_null()) j.set("last_launch", last_launch);
    return j;
}

GameState GameState::FromJson(const Json& j) {
    GameState s;
    s.path = j.get_string("path");
    s.store = j.get_string("store");
    s.build = j.get_string("build");
    s.last_tx = j.get_string("last_tx");
    if (const Json* a = j.find("active"))
        for (const Json::Member& kv : a->members()) {
            ActiveMod m;
            m.id = kv.second.get_string("id");
            m.version = kv.second.get_string("version");
            m.group = kv.second.get_string("group");
            m.activated_at = kv.second.get_string("activated_at");
            if (const Json* ap = kv.second.find("applied"))
                for (const Json& e : ap->items()) {
                    AppliedFile f;
                    f.dst = e.get_string("dst");
                    f.role = e.get_string("role");
                    f.sha256 = e.get_string("sha256");
                    f.pre = e.get_bool("pre");
                    f.foreign = e.get_string("foreign");
                    f.foreign_sha = e.get_string("foreign_sha");
                    m.applied.push_back(f);
                }
            s.active[kv.first] = m;
        }
    if (const Json* l = j.find("last_launch")) s.last_launch = *l;
    return s;
}

Json Op::ToJson() const {
    Json j = Json::Object();
    j.set("type", Json::String(type));
    j.set("dst", Json::String(dst));
    if (!src.empty()) j.set("src", Json::String(src));
    if (!bak.empty()) j.set("bak", Json::String(bak));
    if (!store.empty()) j.set("store", Json::String(store));
    if (!prev.empty()) j.set("prev", Json::String(prev));
    if (had_prev) j.set("had_prev", Json::Bool(true));
    if (!sha.empty()) j.set("sha", Json::String(sha));
    return j;
}

Op Op::FromJson(const Json& j) {
    Op o;
    o.type = j.get_string("type");
    o.dst = j.get_string("dst");
    o.src = j.get_string("src");
    o.bak = j.get_string("bak");
    o.store = j.get_string("store");
    o.prev = j.get_string("prev");
    o.had_prev = j.get_bool("had_prev");
    o.sha = j.get_string("sha");
    return o;
}

std::string Op::Describe() const {
    if (type == "capture") return "capture settings   " + dst + "  ->  " + store;
    if (type == "backup") return "take out          " + dst + "  ->  " + bak;
    if (type == "install") return "put in            " + src + "  ->  " + dst;
    if (type == "restore") return "give back         " + src + "  ->  " + dst;
    return type;
}

std::string Plan::Describe() const {
    std::string s = action + " " + id + (from.empty() ? "" : " (leaving " + from + ")") + ": " + std::to_string(ops.size()) + " operation(s)\n";
    auto shorten = [&](std::string line) {   // the long absolute paths as {game}/..., {state}/..., {mods}/...
        const std::pair<const std::string*, const char*> roots[] = {{&game_root, "{game}"}, {&state_root, "{state}"}, {&mods_root, "{mods}"}};
        for (const auto& r : roots)
            if (!r.first->empty())
                for (size_t at; (at = line.find(*r.first)) != std::string::npos;) line.replace(at, r.first->size(), r.second);
        return line;
    };
    for (const Op& o : ops) s += "  " + shorten(o.Describe()) + "\n";
    for (const std::string& w : warnings) s += "  warning: " + w + "\n";
    return s;
}

// ------------------------------------------------------------------ preconditions

void EvaluatePreconditions(const std::vector<Precondition>& list, const PreContext& ctx, const std::string& modId,
                           std::vector<std::string>* warnings) {
    for (const Precondition& p : list) {
        if (!Contains(p.applies_to, ctx.action)) continue;
        bool met = true;
        std::string why = p.message;
        if (p.type == "process_absent") {
            std::vector<std::string> run = ctx.probe ? ctx.probe->Running(p.names) : std::vector<std::string>();
            met = run.empty();
            if (!met && why.empty()) why = "close " + run.front() + " first";
        } else if (p.type == "file_exists") {
            met = IsFile(P(ExpandGame(p.file, ctx.game)));
            if (!met && why.empty()) why = "missing: " + p.file;
        } else if (p.type == "file_sha256_any_of") {
            std::string h;
            met = Sha256File(P(ExpandGame(p.file, ctx.game)), &h);
            if (met) {
                met = false;
                for (const std::string& want : p.sha256_any_of)
                    if (IEquals(want, h)) met = true;
            }
            if (!met && why.empty()) why = p.file + " is not the file this mod needs";
        } else if (p.type == "toml_value") {
            std::string v;
            met = TomlValue(P(ExpandGame(p.file, ctx.game)), p.key, &v);
            if (met) {
                if (p.equals.is_bool()) met = v == (p.equals.as_bool() ? "true" : "false");
                else if (p.equals.is_string()) met = v == p.equals.as_string();
                else if (p.equals.is_number()) met = std::atof(v.c_str()) == p.equals.as_number();
            }
            if (!met && why.empty()) why = p.key + " in " + p.file + " is not what this mod needs";
        } else if (p.type == "game_build_in") {
            met = ctx.build.empty() || Contains(p.builds, ctx.build);   // an unknown build is not a reason to refuse
            if (!met && why.empty()) why = "game build " + ctx.build + " is not one this mod was made for";
        }
        if (met) continue;
        const std::string line = modId + ": " + why;
        if (p.severity == "block") Throw("E_PRECONDITION", line);
        if (warnings) warnings->push_back(line);
    }
}

// ------------------------------------------------------------------ the engine

Engine::Engine(fs::path home, Services svc) : reg_(std::move(home), svc.log), svc_(std::move(svc)) {}

GameDirs Engine::Dirs(const fs::path& game) const {
    GameDirs d;
    d.root = reg_.GamesDir() / GameKey(game);
    d.state = d.root / "state.json";
    d.journal = d.root / "journal.json";
    d.store = d.root / "store";
    d.foreign = d.root / "foreign";
    d.backups = d.root / "backups";
    d.imports = d.root / "imports";
    return d;
}

void Engine::RequireGame(const fs::path& game) const {
    if (!LooksLikeGame(game)) Throw("E_GAME", U(game) + " is not a Diablo II: Resurrected folder (no D2R.exe)");
}

GameState Engine::LoadState(const fs::path& game) const {
    GameState s;
    std::string text;
    if (ReadFile(Dirs(game).state, &text)) {
        try {
            s = GameState::FromJson(Json::Parse(text));
        } catch (const JsonError& e) {
            Throw("E_IO", std::string("state.json of this game is damaged: ") + e.what());
        }
    }
    std::error_code ec;
    fs::path abs = fs::absolute(game, ec);
    s.path = U(abs.lexically_normal());
    while (s.path.size() > 1 && s.path.back() == '/') s.path.pop_back();
    return s;
}

void Engine::SaveState(const fs::path& game, const GameState& s) const { WriteFileAtomic(Dirs(game).state, s.ToJson().dump()); }

std::vector<fs::path> Engine::KnownGames() const {
    std::vector<fs::path> out;
    std::error_code ec;
    for (auto it = fs::directory_iterator(reg_.GamesDir(), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::string text;
        if (!ReadFile(it->path() / "state.json", &text)) continue;
        try {
            std::string p = Json::Parse(text).get_string("path");
            if (!p.empty()) out.push_back(P(p));
        } catch (const JsonError&) {
        }
    }
    return out;
}

void Engine::CheckClosed(const std::vector<std::string>& processes) const {
    if (!svc_.probe) Throw("E_USAGE", "no process probe configured: cannot tell whether the game is running");
    std::vector<std::string> run = svc_.probe->Running(processes);
    if (!run.empty()) {
        std::string names;
        for (const std::string& n : run) names += (names.empty() ? "" : ", ") + n;
        Throw("E_GAME_RUNNING", "close " + names + " first - the launcher does not touch a game folder while the game runs");
    }
}

void Engine::WriteJournal(const GameDirs& d, const Plan& plan) const {
    Json j = Json::Object();
    j.set("schema", Json::Number(1));
    j.set("tx", Json::String(plan.tx));
    j.set("action", Json::String(plan.action));
    j.set("id", Json::String(plan.id));
    j.set("from", Json::String(plan.from));
    j.set("started", Json::String(NowIso()));
    Json ops = Json::Array();
    for (const Op& o : plan.ops) ops.push(o.ToJson());
    j.set("ops", ops);
    Json gc = Json::Array();
    for (const std::string& g : plan.gc) gc.push(Json::String(g));
    j.set("gc", gc);
    WriteFileAtomic(d.journal, j.dump());
}

void Engine::Rollback(const std::vector<Op>& ops, std::vector<std::string>* errors) const {
    for (size_t i = ops.size(); i-- > 0;) {
        try {
            UndoOp(ops[i]);
        } catch (const std::exception& e) {
            errors->push_back("undo of operation " + std::to_string(i) + " (" + ops[i].type + " " + ops[i].dst + "): " + e.what());
        }
    }
}

void Engine::Trim(const GameDirs& d) const {
    std::vector<fs::path> dirs;
    std::error_code ec;
    for (auto it = fs::directory_iterator(d.backups, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (it->is_directory()) dirs.push_back(it->path());
    std::sort(dirs.begin(), dirs.end());
    while (dirs.size() > static_cast<size_t>(kKeepBackups)) {
        RemoveQuiet(dirs.front());
        dirs.erase(dirs.begin());
    }
}

void Engine::RunHooks(const std::string& name, const Manifest& m, const std::map<std::string, std::string>& env, bool fatal,
                      std::vector<std::string>* warnings) const {
    auto it = m.hooks.find(name);
    if (it == m.hooks.end()) return;
    for (const Hook& h : it->second) {
        if (!svc_.hooks) {
            if (warnings) warnings->push_back(m.id + ": hook " + name + " (" + h.exe + ") not run - no hook runner");
            continue;
        }
        std::string msg;
        if (!svc_.hooks->Run(name, m, h, env, &msg)) {
            if (fatal) Throw("E_HOOK", m.id + ": " + name + " failed: " + msg);
            if (warnings) warnings->push_back(m.id + ": " + name + " failed: " + msg);
        }
    }
}

// ------------------------------------------------------------------ planning

namespace {

struct Build {
    const fs::path& game;
    const GameDirs& d;
    Plan& plan;
    std::set<std::string> removed;                                       // lower-case dsts taken out by this plan
    std::map<std::string, std::pair<std::string, std::string>> inherited; // lower dst -> (foreign path, hash) passed to the next mod
    unsigned n = 0;

    fs::path NextBak() { return d.backups / plan.tx / std::to_string(n++); }
    fs::path NextForeign() { return d.foreign / plan.tx / std::to_string(n++); }
    std::string Rel(const fs::path& p) const { return U(p.lexically_relative(d.root)); }

    // What the old mod put in goes out: settings captured, files taken out, the file that was in the way given back.
    void Remove(const std::string& oldId, const AppliedFile& e, const std::set<std::string>& targetDsts) {
        const std::string key = Lower(e.dst);
        const fs::path live = game / P(e.dst);
        if (e.role == "config") {
            if (IsFile(live)) {
                Op cap;
                cap.type = "capture";
                cap.dst = U(live);
                cap.store = U(d.store / P(oldId) / P(e.dst));
                cap.had_prev = IsFile(P(cap.store));
                cap.prev = U(NextBak());
                plan.ops.push_back(cap);
                Op b;
                b.type = "backup";
                b.dst = U(live);
                b.bak = U(NextBak());
                plan.ops.push_back(b);
                removed.insert(key);
            }
        } else if (e.role != "shared") {
            if (!e.pre && IsFile(live)) {
                if (!e.sha256.empty() && FileHash(live) != e.sha256)
                    plan.warnings.push_back(e.dst + " was changed since it was put in; the changed copy is kept in the backup of this switch");
                Op b;
                b.type = "backup";
                b.dst = U(live);
                b.bak = U(NextBak());
                plan.ops.push_back(b);
                removed.insert(key);
            }
        }
        if (!e.foreign.empty() && e.role != "shared") {
            if (targetDsts.count(key)) {
                inherited[key] = {e.foreign, e.foreign_sha};   // the next mod sits where the same foreign file was: it keeps the duty to give it back
            } else {
                Op r;
                r.type = "restore";
                r.src = U(d.root / P(e.foreign));
                r.dst = U(live);
                r.sha = e.foreign_sha;
                plan.ops.push_back(r);
                plan.gc.push_back(r.src);
            }
        } else if (!e.foreign.empty()) {
            plan.warnings.push_back("a file that was in the way of the shared " + e.dst + " stays in " + U(d.root / P(e.foreign)));
        }
    }

    // What the new mod needs: returns the entry for the state.
    AppliedFile Install(const std::string& modId, const EF& ef) {
        const std::string key = Lower(ef.dst);
        const fs::path live = game / P(ef.dst);
        AppliedFile a;
        a.dst = ef.dst;
        a.role = RoleName(ef.role);
        if (Exists(live) && !IsFile(live)) Throw("E_IO", "a folder is where the file " + ef.dst + " should be");
        const bool exists = IsFile(live) && !removed.count(key);
        auto inh = inherited.find(key);
        auto moveAside = [&]() {
            Op b;
            b.type = "backup";
            b.dst = U(live);
            fs::path f = NextForeign();
            b.bak = U(f);
            plan.ops.push_back(b);
            a.foreign = Rel(f);
            a.foreign_sha = FileHash(live);
            plan.warnings.push_back(ef.dst + " was already in the game folder; moved aside (not lost) - it is given back when this mod is deactivated");
        };
        if (ef.role == Role::Config) {
            const fs::path stored = d.store / P(modId) / P(ef.dst);
            fs::path source = IsFile(stored) ? stored : ef.seed;
            if (exists) moveAside();
            else if (inh != inherited.end()) { a.foreign = inh->second.first; a.foreign_sha = inh->second.second; }
            if (!source.empty() && IsFile(source)) {
                Op in;
                in.type = "install";
                in.src = U(source);
                in.dst = U(live);
                in.sha = FileHash(source);
                plan.ops.push_back(in);
                a.sha256 = in.sha;
            }
            return a;
        }
        a.sha256 = ef.sha;
        bool write = true;
        if (exists) {
            if (FileHash(live) == ef.sha) {
                a.pre = true;   // the same bytes are already there: not ours to remove later
                write = false;
            } else {
                moveAside();
            }
        } else if (inh != inherited.end()) {
            a.foreign = inh->second.first;
            a.foreign_sha = inh->second.second;
        }
        if (write) {
            Op in;
            in.type = "install";
            in.src = U(ef.src);
            in.dst = U(live);
            in.sha = ef.sha;
            plan.ops.push_back(in);
        }
        return a;
    }
};

}  // namespace

Plan Engine::PlanActivate(const fs::path& game, const std::string& id) {
    RequireGame(game);
    const GameDirs d = Dirs(game);
    if (Exists(d.journal)) Throw("E_JOURNAL", "an earlier switch did not finish: run `vrmodlauncher recover` first");
    GameState st = LoadState(game);
    const Manifest m = reg_.Load(id);
    {
        std::vector<Issue> iss;
        if (!CheckPackage(m, &iss)) Throw("E_MANIFEST", "the package of \"" + id + "\" no longer matches its manifest:\n" + IssuesText(iss));
    }
    Plan plan;
    plan.tx = NewTxId();
    plan.id = id;
    plan.group = m.group;
    plan.game_root = U(game);
    plan.state_root = U(d.root);
    plan.mods_root = U(reg_.ModsDir());

    const std::string store = DetectStore(game);
    if (!store.empty() && !m.stores.empty() && !Contains(m.stores, store))
        Throw("E_PRECONDITION", "\"" + id + "\" does not support the " + store + " edition of the game");
    st.store = store;
    st.build = DetectBuild(game, m.builds);
    if (!m.builds.empty() && !st.build.empty() && !Contains(m.builds, st.build))
        plan.warnings.push_back("game build " + st.build + " is not in the list this mod was made for (" + m.builds.front() + (m.builds.size() > 1 ? ", ..." : "") + ")");

    const std::vector<EF> want = Expand(m);
    std::set<std::string> targetDsts;
    for (const EF& ef : want) {
        targetDsts.insert(Lower(ef.dst));
        CheckNoLinks(game, ef.dst);
    }
    // another group's mod must not own the same file
    for (const auto& kv : st.active) {
        if (kv.first == m.group) continue;
        for (const AppliedFile& a : kv.second.applied)
            for (const EF& ef : want)
                if (Lower(a.dst) == Lower(ef.dst) && !(a.role == "shared" && ef.role == Role::Shared && a.sha256 == ef.sha))
                    Throw("E_COLLISION", "\"" + id + "\" and the active \"" + kv.second.id + "\" (group " + kv.first + ") both own " + a.dst);
    }

    Build b{game, d, plan, {}, {}, 0};
    GameState& next = plan.next;
    next = st;
    next.last_tx = plan.tx;
    for (const std::string& p : DefaultProcesses(m)) plan.processes.push_back(p);

    auto old = st.active.find(m.group);
    ActiveMod nm;
    nm.id = id;
    nm.version = m.version;
    nm.group = m.group;
    nm.activated_at = NowIso();

    if (old != st.active.end() && old->second.id == id) {
        // ---- refresh: the same mod, its package changed (an update) - settings stay, changed files are replaced
        plan.action = "refresh";
        plan.from = id;
        const ActiveMod& cur = old->second;
        nm.activated_at = cur.activated_at;
        std::map<std::string, const AppliedFile*> have;
        for (const AppliedFile& a : cur.applied) have[Lower(a.dst)] = &a;
        for (size_t i = cur.applied.size(); i-- > 0;)
            if (!targetDsts.count(Lower(cur.applied[i].dst))) b.Remove(id, cur.applied[i], targetDsts);
        for (const EF& ef : want) {
            const std::string key = Lower(ef.dst);
            const fs::path live = game / P(ef.dst);
            auto h = have.find(key);
            if (ef.role == Role::Config) {
                if (h != have.end()) nm.applied.push_back(*h->second);
                else nm.applied.push_back(b.Install(id, ef));
                continue;
            }
            const bool exists = IsFile(live);
            if (h != have.end() && exists && FileHash(live) == ef.sha) {
                AppliedFile a = *h->second;
                a.sha256 = ef.sha;
                nm.applied.push_back(a);
                continue;
            }
            AppliedFile a;
            if (h != have.end()) a = *h->second;
            a.dst = ef.dst;
            a.role = RoleName(ef.role);
            a.sha256 = ef.sha;
            if (exists) {
                Op bk;
                bk.type = "backup";
                bk.dst = U(live);
                if (h != have.end() && !h->second->pre) {
                    bk.bak = U(b.NextBak());
                    if (FileHash(live) != h->second->sha256) plan.warnings.push_back(ef.dst + " was changed since it was put in; the changed copy is kept in the backup");
                } else {   // not ours (an identical file was there, or the entry is new): it is given back later
                    fs::path f = b.NextForeign();
                    bk.bak = U(f);
                    a.foreign = b.Rel(f);
                    a.foreign_sha = FileHash(live);
                    a.pre = false;
                }
                plan.ops.push_back(bk);
            }
            Op in;
            in.type = "install";
            in.src = U(ef.src);
            in.dst = U(live);
            in.sha = ef.sha;
            plan.ops.push_back(in);
            nm.applied.push_back(a);
        }
    } else {
        plan.action = "activate";
        if (old != st.active.end()) {
            plan.from = old->second.id;
            for (size_t i = old->second.applied.size(); i-- > 0;) b.Remove(old->second.id, old->second.applied[i], targetDsts);
        }
        for (const EF& ef : want) nm.applied.push_back(b.Install(id, ef));
    }
    next.active[m.group] = nm;
    return plan;
}

Plan Engine::PlanDeactivate(const fs::path& game, const std::string& idOrGroup) {
    RequireGame(game);
    const GameDirs d = Dirs(game);
    if (Exists(d.journal)) Throw("E_JOURNAL", "an earlier switch did not finish: run `vrmodlauncher recover` first");
    GameState st = LoadState(game);
    auto it = st.active.end();
    for (auto i = st.active.begin(); i != st.active.end(); ++i)
        if (i->first == idOrGroup || i->second.id == idOrGroup) it = i;
    if (it == st.active.end()) Throw("E_NOT_FOUND", "\"" + idOrGroup + "\" is not active in this game");
    Plan plan;
    plan.tx = NewTxId();
    plan.action = "deactivate";
    plan.game_root = U(game);
    plan.state_root = U(d.root);
    plan.mods_root = U(reg_.ModsDir());
    plan.id = it->second.id;
    plan.from = it->second.id;
    plan.group = it->first;
    plan.processes = {"D2R.exe", "D2RLoader.exe"};
    Build b{game, d, plan, {}, {}, 0};
    plan.next = st;
    plan.next.last_tx = plan.tx;
    for (size_t i = it->second.applied.size(); i-- > 0;) b.Remove(it->second.id, it->second.applied[i], {});
    plan.next.active.erase(it->first);
    return plan;
}

// ------------------------------------------------------------------ applying

void Engine::Apply(const fs::path& game, Plan& plan) {
    RequireGame(game);
    const GameDirs d = Dirs(game);
    if (Exists(d.journal)) Throw("E_JOURNAL", "an earlier switch did not finish: run `vrmodlauncher recover` first");

    // ---- refuse while the game runs; preconditions of both mods; hooks
    std::vector<std::string> procs = plan.processes;
    std::unique_ptr<Manifest> oldM, newM;
    if (!plan.from.empty() && plan.from != plan.id && reg_.Has(plan.from)) {
        try { oldM = std::make_unique<Manifest>(reg_.Load(plan.from)); } catch (const Error&) {}
    }
    if (plan.action != "deactivate" && reg_.Has(plan.id)) newM = std::make_unique<Manifest>(reg_.Load(plan.id));
    if (plan.action == "deactivate" && reg_.Has(plan.id)) {
        try { oldM = std::make_unique<Manifest>(reg_.Load(plan.id)); } catch (const Error&) {}
    }
    for (const Manifest* mm : {oldM.get(), newM.get()})
        if (mm)
            for (const std::string& p : DefaultProcesses(*mm))
                if (!Contains(procs, p)) procs.push_back(p);
    CheckClosed(procs);
    PreContext pc{game, plan.next.build, "activate", svc_.probe};
    if (oldM) { pc.action = "deactivate"; EvaluatePreconditions(oldM->preconditions, pc, oldM->id, &plan.warnings); }
    if (newM) { pc.action = "activate"; EvaluatePreconditions(newM->preconditions, pc, newM->id, &plan.warnings); }
    std::map<std::string, std::string> env = {{"VRMOD_GAME", U(game)}, {"VRMOD_API", "1"}};
    if (oldM) { env["VRMOD_ID"] = oldM->id; RunHooks("pre_deactivate", *oldM, env, true, &plan.warnings); }
    if (newM) { env["VRMOD_ID"] = newM->id; RunHooks("pre_activate", *newM, env, true, &plan.warnings); }

    if (svc_.log)
        svc_.log->Line(plan.action + " " + plan.id + (plan.from.empty() || plan.from == plan.id ? "" : " (from " + plan.from + ")") + " in " + U(game) + ": " +
                       std::to_string(plan.ops.size()) + " operation(s), tx " + plan.tx);

    if (plan.ops.empty()) {   // nothing to do to the files: only the state moves
        SaveState(game, plan.next);
    } else {
        EnsureDir(d.root);
        WriteJournal(d, plan);
        std::vector<std::string> errors;
        try {
            for (size_t i = 0; i < plan.ops.size(); ++i) {
                if (svc_.crash) svc_.crash(static_cast<int>(i), "before");
                DoOp(plan.ops[i]);
                if (svc_.crash) svc_.crash(static_cast<int>(i), "after");
            }
            // verify what was put in
            for (const auto& kv : plan.next.active) {
                if (kv.second.id != plan.id || plan.action == "deactivate") continue;
                for (const AppliedFile& a : kv.second.applied) {
                    if (a.role == "config") continue;
                    const fs::path live = game / P(a.dst);
                    if (!IsFile(live) || FileHash(live) != a.sha256) Throw("E_HASH", "verification failed for " + a.dst);
                }
            }
            if (svc_.crash) svc_.crash(-1, "commit");
        } catch (const SimulatedCrash&) {
            throw;   // the process "died": the journal stays for `recover`
        } catch (const std::exception& e) {
            Rollback(plan.ops, &errors);
            if (!errors.empty()) {
                std::string all;
                for (const std::string& s : errors) all += "\n  " + s;
                if (svc_.log) svc_.log->Line("ROLLBACK INCOMPLETE after: " + std::string(e.what()) + all);
                Throw("E_ROLLBACK", std::string("the switch failed (") + e.what() + ") and could not be fully undone; run `vrmodlauncher recover`:" + all);
            }
            RemoveQuiet(d.journal);
            RemoveQuiet(d.backups / plan.tx);
            RemoveQuiet(d.foreign / plan.tx);
            if (svc_.log) svc_.log->Line(std::string("switch failed and was undone: ") + e.what());
            throw;
        }
        SaveState(game, plan.next);                                  // the commit point
        if (svc_.crash) svc_.crash(-1, "cleanup");
        RemoveQuiet(d.journal);
        for (const std::string& g : plan.gc) RemoveQuiet(P(g));
        Trim(d);
    }
    if (svc_.log) svc_.log->Line("committed " + plan.tx);

    if (oldM) { env["VRMOD_ID"] = oldM->id; RunHooks("post_deactivate", *oldM, env, false, &plan.warnings); }
    if (newM) { env["VRMOD_ID"] = newM->id; RunHooks("post_activate", *newM, env, false, &plan.warnings); }
}

void Engine::Activate(const fs::path& game, const std::string& id) {
    Plan p = PlanActivate(game, id);
    Apply(game, p);
}

void Engine::Deactivate(const fs::path& game, const std::string& idOrGroup) {
    Plan p = PlanDeactivate(game, idOrGroup);
    Apply(game, p);
}

// ------------------------------------------------------------------ recovery

RecoverReport Engine::Recover(const fs::path& game) {
    RecoverReport rep;
    const GameDirs d = Dirs(game);
    std::string text;
    if (!ReadFile(d.journal, &text)) return rep;
    Json j;
    try {
        j = Json::Parse(text);
    } catch (const JsonError& e) {
        Throw("E_JOURNAL", std::string("journal.json is damaged: ") + e.what() + " - keep the folder " + U(d.root) + " and ask for help");
    }
    CheckClosed({"D2R.exe", "D2RLoader.exe"});
    rep.tx = j.get_string("tx");
    std::vector<Op> ops;
    if (const Json* arr = j.find("ops"))
        for (const Json& o : arr->items()) ops.push_back(Op::FromJson(o));
    const GameState st = LoadState(game);
    if (st.last_tx == rep.tx) {
        // the state was committed and only the clean-up was missing: finish it
        if (const Json* gc = j.find("gc"))
            for (const Json& g : gc->items()) RemoveQuiet(P(g.as_string()));
        RemoveQuiet(d.journal);
        Trim(d);
        rep.result = RecoverReport::Result::RolledForward;
        rep.notes.push_back("the switch " + rep.tx + " had been committed; clean-up finished");
    } else {
        std::vector<std::string> errors;
        Rollback(ops, &errors);
        if (!errors.empty()) {
            std::string all;
            for (const std::string& s : errors) all += "\n  " + s;
            if (svc_.log) svc_.log->Line("recover: rollback incomplete" + all);
            Throw("E_ROLLBACK", "could not undo the unfinished switch " + rep.tx + " completely:" + all);
        }
        RemoveQuiet(d.journal);
        RemoveQuiet(d.backups / rep.tx);
        RemoveQuiet(d.foreign / rep.tx);
        rep.result = RecoverReport::Result::RolledBack;
        rep.notes.push_back("the unfinished switch " + rep.tx + " (" + j.get_string("action") + " " + j.get_string("id") + ") was undone; the game folder is as it was before");
    }
    if (svc_.log) svc_.log->Line("recover: " + rep.notes.back());
    return rep;
}

// ------------------------------------------------------------------ verify

VerifyReport Engine::Verify(const fs::path& game) {
    VerifyReport rep;
    auto add = [&](const char* lvl, const char* code, const std::string& msg) { rep.findings.push_back({lvl, code, msg}); };
    const GameDirs d = Dirs(game);
    if (!LooksLikeGame(game)) {
        add("error", "NOT_A_GAME", U(game) + " has no D2R.exe");
        return rep;
    }
    if (Exists(d.journal)) add("error", "JOURNAL_PENDING", "an earlier switch did not finish: run `vrmodlauncher recover`");
    const GameState st = LoadState(game);
    std::set<std::string> ownedPlugins;
    for (const auto& kv : st.active) {
        const ActiveMod& a = kv.second;
        for (const AppliedFile& f : a.applied) {
            const fs::path live = game / P(f.dst);
            if (IsFile(live) && IsReparsePoint(live)) add("error", "LINK", f.dst + " is a link");
            if (f.role == "config") {
                if (!IsFile(live)) add("warn", "CONFIG_MISSING", a.id + ": settings file " + f.dst + " is not there (the mod will make a new one)");
                continue;
            }
            if (!IsFile(live)) add("error", "MISSING", a.id + ": " + f.dst + " is missing");
            else if (FileHash(live) != f.sha256) add("error", "MODIFIED", a.id + ": " + f.dst + " is not what was put in (changed by something else - `activate " + a.id + "` repairs it)");
        }
        if (reg_.Has(a.id)) {
            for (const ModRecord& r : reg_.List())
                if (r.id == a.id && r.version != a.version)
                    add("warn", "REFRESH_PENDING", a.id + ": version " + a.version + " is active, " + r.version + " is registered - `activate " + a.id + "` brings it in");
        } else {
            add("warn", "PACKAGE_MISSING", a.id + " is active but no longer registered");
        }
        ownedPlugins.insert(a.id);
    }
    // files of a mod that is not active, sitting where its plugin goes: a legacy installation or a mix
    for (const ModRecord& r : reg_.List()) {
        if (ownedPlugins.count(r.id)) continue;
        Manifest m;
        try { m = reg_.Load(r.id); } catch (const Error&) { continue; }
        if (m.identity.plugin_file.empty()) continue;
        for (const FileEntry& f : m.files) {
            if (f.role != Role::Program || f.is_dir() || f.dst_rel.empty()) continue;
            const fs::path base = P(f.dst_rel).filename();
            if (!IEquals(U(base), m.identity.plugin_file)) continue;
            const fs::path live = game / P(f.dst_rel);
            if (!IsFile(live)) continue;
            // is it that mod's plugin? by the hashes the mod publishes, or the package's own file
            std::string h = FileHash(live), pay;
            Sha256File(m.dir / P(f.src), &pay);
            bool is = h == pay;
            for (const std::string& x : m.identity.dll_sha256) if (IEquals(x, h)) is = true;
            if (is) add("warn", "UNMANAGED_MOD_FILES", "the plugin of \"" + m.id + "\" is in the game folder but " + m.id + " is not active here (an install from before the launcher, or a mix): `import " + m.id + "` takes it over");
        }
    }
    if (rep.findings.empty()) add("info", "OK", "nothing wrong found");
    return rep;
}

// ------------------------------------------------------------------ import (existing installations and their backups)

ImportReport Engine::Import(const fs::path& game, const std::string& id, bool force) {
    RequireGame(game);
    const GameDirs d = Dirs(game);
    if (Exists(d.journal)) Throw("E_JOURNAL", "an earlier switch did not finish: run `vrmodlauncher recover` first");
    const Manifest m = reg_.Load(id);
    GameState st = LoadState(game);
    auto cur = st.active.find(m.group);
    if (cur != st.active.end()) Throw("E_IN_USE", "\"" + cur->second.id + "\" is active in group " + m.group + ": deactivate it before importing \"" + id + "\"");
    std::vector<std::string> procs = DefaultProcesses(m);
    CheckClosed(procs);

    ImportReport rep;
    rep.id = id;
    const std::vector<EF> files = Expand(m);

    // identity: is the plugin in the game folder this mod's?
    if (!m.identity.plugin_file.empty() && !force) {
        bool found = false, ok = false;
        for (const EF& ef : files) {
            if (ef.role != Role::Program || !IEquals(U(P(ef.dst).filename()), m.identity.plugin_file)) continue;
            const fs::path live = game / P(ef.dst);
            if (!IsFile(live)) continue;
            found = true;
            const std::string h = FileHash(live);
            if (m.identity.dll_sha256.empty() && m.identity.markers.empty()) ok = true;
            for (const std::string& x : m.identity.dll_sha256) if (IEquals(x, h)) ok = true;
            if (!ok && !m.identity.markers.empty()) {
                std::string bytes;
                if (ReadFile(live, &bytes))
                    for (const std::string& mk : m.identity.markers) {
                        std::string wide;   // a string can sit in the binary as UTF-16 as well
                        for (char c : mk) { wide += c; wide += '\0'; }
                        if (bytes.find(mk) != std::string::npos || bytes.find(wide) != std::string::npos) {
                            ok = true;
                            rep.notes.push_back("recognised by the marker \"" + mk + "\" (not by its hash): probably " + id);
                        }
                    }
            }
        }
        if (!found) Throw("E_IDENTITY", "no " + m.identity.plugin_file + " in the game folder: nothing of \"" + id + "\" to import");
        if (!ok) Throw("E_IDENTITY", "the " + m.identity.plugin_file + " in the game folder is not a build of \"" + id + "\" (use --force to take it over anyway)");
    }

    ActiveMod nm;
    nm.id = id;
    nm.version = m.version;
    nm.group = m.group;
    nm.activated_at = NowIso();
    for (const EF& ef : files) {
        const fs::path live = game / P(ef.dst);
        CheckNoLinks(game, ef.dst);
        AppliedFile a;
        a.dst = ef.dst;
        a.role = RoleName(ef.role);
        if (!IsFile(live)) {
            ++rep.missing;
            rep.notes.push_back(ef.dst + " is not in the game folder (`activate " + id + "` puts it in)");
            if (ef.role == Role::Config) { a.sha256 = ef.sha; nm.applied.push_back(a); }
            continue;
        }
        a.sha256 = FileHash(live);
        if (ef.role == Role::Config) {
            const fs::path stored = d.store / P(id) / P(ef.dst);
            if (IsFile(stored)) {   // never lose an older saved copy
                const fs::path old = d.imports / P(id) / "previous-settings" / P(ef.dst);
                CopyAtomic(stored, old);
            }
            CopyAtomic(live, stored);
            ++rep.captured;
        } else {
            if (a.sha256 != ef.sha) rep.notes.push_back(ef.dst + " differs from the package (an older or changed build): `activate " + id + "` brings the package's in");
            ++rep.adopted;
        }
        nm.applied.push_back(a);
    }

    // backups of the old installation: copied beside the launcher's own, the originals stay where they are
    const fs::path bdir = d.imports / P(id) / "backups";
    for (const std::string& g : m.import_.backup_globs) {
        const std::string rel = GameRel(g);
        const fs::path pattern = P(rel);
        const fs::path dir = game / pattern.parent_path();
        const std::string pat = U(pattern.filename());
        std::error_code ec;
        for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            if (!GlobMatch(pat, U(it->path().filename()))) continue;
            if (IsReparsePoint(it->path())) continue;
            const fs::path to = bdir / it->path().lexically_relative(game);
            if (it->is_regular_file()) { CopyAtomic(it->path(), to); ++rep.backups; }
            else if (it->is_directory()) {
                std::error_code ec2;
                EnsureDir(to.parent_path());
                fs::copy(it->path(), to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec2);
                if (!ec2) ++rep.backups;
                else rep.notes.push_back("could not copy the backup " + U(it->path()) + " completely: " + ec2.message() + " (a path over 260 characters?)");
            }
        }
    }
    if (!m.import_.record_file.empty()) {
        std::string rtext;
        const fs::path rf = P(ExpandGame(m.import_.record_file, game));
        if (ReadFile(rf, &rtext)) {
            try {
                const Json rec = Json::Parse(rtext);
                // a JSON pointer of one level: "/snapshots"
                std::string key = m.import_.record_paths_at;
                if (!key.empty() && key[0] == '/') key = key.substr(1);
                if (const Json* arr = rec.find(key)) {
                    int k = 0;
                    for (const Json& e : arr->items()) {
                        const fs::path from = P(e.as_string());
                        if (!e.is_string() || !Exists(from) || IsReparsePoint(from)) { rep.notes.push_back("the backup " + e.as_string() + " listed in " + U(rf.filename()) + " is not there any more"); continue; }
                        const fs::path to = d.imports / P(id) / "record" / std::to_string(k++);
                        std::error_code ec2;
                        EnsureDir(to.parent_path());
                        if (fs::is_directory(from)) fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec2);
                        else { EnsureDir(to); fs::copy_file(from, to / from.filename(), fs::copy_options::overwrite_existing, ec2); }
                        if (!ec2) ++rep.backups;
                        else rep.notes.push_back("could not copy the backup " + U(from) + " completely: " + ec2.message() + " (a path over 260 characters?)");
                    }
                }
            } catch (const JsonError&) {
                rep.notes.push_back(U(rf.filename()) + " could not be read; its backups were not collected");
            }
        }
    }

    st.store = DetectStore(game);
    st.build = DetectBuild(game, m.builds);
    st.last_tx = NewTxId();
    st.active[m.group] = nm;
    SaveState(game, st);
    if (svc_.log) svc_.log->Line("imported " + id + " " + m.version + " in " + U(game) + ": " + std::to_string(rep.adopted) + " files adopted, " + std::to_string(rep.captured) + " settings captured, " + std::to_string(rep.backups) + " backups copied");
    return rep;
}

SettingsImportReport Engine::ImportSettings(const fs::path& game, const std::string& id, const fs::path& from) {
    RequireGame(game);
    const GameDirs d = Dirs(game);
    if (Exists(d.journal)) Throw("E_JOURNAL", "an earlier switch did not finish: run `vrmodlauncher recover` first");
    const Manifest m = reg_.Load(id);
    GameState st = LoadState(game);
    auto cur = st.active.find(m.group);
    if (cur != st.active.end() && cur->second.id == id)
        Throw("E_IN_USE", "\"" + id + "\" is active here: its files in the game folder are its settings (deactivate it first, or copy the file in)");
    if (!IsDir(from) || IsReparsePoint(from)) Throw("E_NOT_FOUND", U(from) + " is not a folder");
    SettingsImportReport rep;
    rep.id = id;
    for (const EF& ef : Expand(m)) {
        if (ef.role != Role::Config) continue;
        const fs::path src = from / P(ef.dst);
        CheckNoLinks(from, ef.dst);
        if (!IsFile(src)) { rep.notes.push_back(ef.dst + " is not in " + U(from)); continue; }
        const fs::path stored = d.store / P(id) / P(ef.dst);
        if (IsFile(stored)) {   // never lose an older saved copy
            CopyAtomic(stored, d.imports / P(id) / "previous-settings" / P(ef.dst));
            ++rep.kept_previous;
        }
        CopyAtomic(src, stored);
        ++rep.captured;
    }
    if (svc_.log) svc_.log->Line("import-settings " + id + " in " + U(game) + " from " + U(from) + ": " + std::to_string(rep.captured) + " settings file(s)");
    return rep;
}

// ------------------------------------------------------------------ removing a mod

void Engine::RemoveMod(const std::string& id, const RemoveOptions& opts) {
    std::vector<fs::path> using_;
    for (const fs::path& g : KnownGames()) {
        GameState st = LoadState(g);
        for (const auto& kv : st.active)
            if (kv.second.id == id) using_.push_back(g);
    }
    if (!using_.empty() && !opts.force) {
        Throw("E_IN_USE", "\"" + id + "\" is active in " + U(using_.front()) + (using_.size() > 1 ? " and others" : "") + ": deactivate it first, or use --force");
    }
    for (const fs::path& g : using_) Deactivate(g, id);   // checks that the game is closed
    reg_.Unregister(id);
    if (opts.purge_package) RemoveQuiet(reg_.ModDir(id));
    if (opts.purge_settings)
        for (const fs::path& g : KnownGames()) RemoveQuiet(Dirs(g).store / P(id));
}

// ------------------------------------------------------------------ status

Json Engine::Status(const fs::path& game) {
    Json j = Json::Object();
    j.set("launcher", Json::String(kLauncherVersion));
    j.set("home", Json::String(U(reg_.home())));
    Json g = Json::Object();
    g.set("path", Json::String(U(game)));
    g.set("key", Json::String(GameKey(game)));
    const bool isGame = LooksLikeGame(game);
    g.set("is_game", Json::Bool(isGame));
    if (isGame) {
        GameState st = LoadState(game);
        std::vector<std::string> cands;
        for (const ModRecord& r : reg_.List()) {
            try { for (const std::string& b : reg_.Load(r.id).builds) cands.push_back(b); } catch (const Error&) {}
        }
        g.set("store", Json::String(DetectStore(game)));
        g.set("build", Json::String(DetectBuild(game, cands)));
        g.set("d2rloader", Json::String(DetectLoaderVersion(game)));
        Json act = Json::Object();
        for (const auto& kv : st.active) {
            Json a = Json::Object();
            a.set("id", Json::String(kv.second.id));
            a.set("version", Json::String(kv.second.version));
            a.set("activated_at", Json::String(kv.second.activated_at));
            for (const ModRecord& r : reg_.List())
                if (r.id == kv.second.id && r.version != kv.second.version) a.set("refresh_pending_to", Json::String(r.version));
            act.set(kv.first, a);
        }
        g.set("active", act);
        g.set("pending_journal", Json::Bool(Exists(Dirs(game).journal)));
        if (!st.last_launch.is_null()) g.set("last_launch", st.last_launch);
    }
    j.set("game", g);
    Json mods = Json::Array();
    for (const ModRecord& r : reg_.List()) {
        Json o = Json::Object();
        o.set("id", Json::String(r.id));
        o.set("version", Json::String(r.version));
        try {
            Manifest m = reg_.Load(r.id);
            o.set("name", Json::String(m.name));
            o.set("group", Json::String(m.group));
        } catch (const Error& e) {
            o.set("problem", Json::String(e.what()));
        }
        mods.push(o);
    }
    j.set("mods", mods);
    Json un = Json::Array();
    for (const std::string& s : reg_.Unregistered()) un.push(Json::String(s));
    j.set("unregistered_packages", un);
    return j;
}

}  // namespace vrml
