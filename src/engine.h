// engine.h - switching a game folder from one mod's complete setup to another's, as a transaction.
//
//   plan -> journal -> apply -> verify -> commit            (and recover on the next start after a crash)
//
// What the engine may touch: only the files the manifests name (dst under {game}), and its own folders under
// <home>/games/<key>/. Nothing else in the game folder is read for any other reason than "is a file there already".
//
// Per game (<home>/games/<GameKey(path)>/):
//   state.json     which mod is active in each exclusive group, and exactly what was written for it (paths, hashes, foreign backups)
//   journal.json   exists only while a transaction runs (or died): the operations, in order, each undoable
//   store/<id>/    the mod's own settings (its config files) for THIS game, captured when it is switched away
//   foreign/<tx>/  files that were in the way when a mod was activated (not ours): restored when it is deactivated
//   backups/<tx>/  what a switch took out (the previous active set): kept for the last few transactions
//   imports/<id>/  backups collected from an existing installation by `import`
#pragma once

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "json.h"
#include "manifest.h"
#include "process.h"
#include "registry.h"
#include "util.h"

namespace vrml {

class HookRunner {
public:
    virtual ~HookRunner() = default;
    // hookName: pre_activate | post_activate | pre_deactivate | post_deactivate. false (+ message) = the hook failed.
    virtual bool Run(const std::string& hookName, const Manifest& m, const Hook& h, const std::map<std::string, std::string>& env,
                     std::string* message) = 0;
};

struct Services {
    ProcessProbe* probe = nullptr;   // required for anything that changes a game folder
    HookRunner* hooks = nullptr;     // optional; without it hooks are only listed
    Logger* log = nullptr;
    // Tests: called around every operation ("before", "after") and at "commit" / "cleanup" (op = -1). Throw SimulatedCrash to die there.
    std::function<void(int op, const char* phase)> crash;
};

// ------------------------------------------------------------------ state

struct AppliedFile {
    std::string dst;                 // game-relative, '/'
    std::string role;                // program | asset | config | shared
    std::string sha256;              // what was written (config: what was seeded/restored; later edits are expected)
    bool pre = false;                // an identical file was already there: not ours to remove
    std::string foreign;             // "foreign/<tx>/<n>" relative to the game's state folder: the file that was in the way
    std::string foreign_sha;
};

struct ActiveMod {
    std::string id, version, group, activated_at;
    std::vector<AppliedFile> applied;
};

struct GameState {
    std::string path, store, build, last_tx;
    std::map<std::string, ActiveMod> active;   // by exclusive group
    Json last_launch;                          // {mod, option, runtime, at} - for status and the support log

    Json ToJson() const;
    static GameState FromJson(const Json& j);
};

// ------------------------------------------------------------------ plan and journal

struct Op {
    std::string type;                // capture | backup | install | restore
    std::string dst;                 // absolute path in the game folder (capture: the live file)
    std::string src;                 // install, restore: what to copy in
    std::string bak;                 // backup: where the old file goes
    std::string store;               // capture: the mod's settings file
    std::string prev;                // capture: where the previous settings file is kept while the capture runs
    bool had_prev = false;
    std::string sha;                 // install, restore: the hash the result must have
    Json ToJson() const;
    static Op FromJson(const Json& j);
    std::string Describe() const;
};

struct Plan {
    std::string tx, action;          // activate | refresh | deactivate
    std::string id, group, from;
    std::vector<Op> ops;
    GameState next;
    std::vector<std::string> gc;     // absolute paths deleted after the commit
    std::vector<std::string> warnings;
    std::vector<std::string> processes;      // exe names that must not run
    std::string game_root, state_root, mods_root;   // for Describe(): paths are printed as {game}/..., {state}/..., {mods}/...
    bool changes_anything() const { return !ops.empty(); }
    std::string Describe() const;
};

struct Finding {
    std::string level;               // error | warn | info
    std::string code;
    std::string message;
};
struct VerifyReport {
    std::vector<Finding> findings;
    bool ok() const {
        for (const Finding& f : findings)
            if (f.level == "error") return false;
        return true;
    }
};

struct RecoverReport {
    enum class Result { Nothing, RolledBack, RolledForward } result = Result::Nothing;
    std::string tx;
    std::vector<std::string> notes;
};

struct ImportReport {
    std::string id;
    int adopted = 0, captured = 0, missing = 0, backups = 0;
    std::vector<std::string> notes;
};

struct RemoveOptions {
    bool force = false;              // deactivate it in every game first (the games must be closed)
    bool purge_package = false;      // delete <home>/mods/<id>
    bool purge_settings = false;     // delete the mod's saved settings in every game
};

struct GameDirs {
    fs::path root, state, journal, store, foreign, backups, imports;
};

// ------------------------------------------------------------------ the engine

class Engine {
public:
    Engine(fs::path home, Services svc);

    Registry& registry() { return reg_; }
    const Registry& registry() const { return reg_; }
    const fs::path& home() const { return reg_.home(); }
    Logger* log() { return svc_.log; }

    GameDirs Dirs(const fs::path& game) const;
    GameState LoadState(const fs::path& game) const;
    std::vector<fs::path> KnownGames() const;       // from games/*/state.json

    // Plans are pure reads (they hash files and look at the disk) - nothing changes until Apply.
    Plan PlanActivate(const fs::path& game, const std::string& id);        // also a refresh when `id` is already active
    Plan PlanDeactivate(const fs::path& game, const std::string& idOrGroup);
    void Apply(const fs::path& game, Plan& plan);

    void Activate(const fs::path& game, const std::string& id);
    void Deactivate(const fs::path& game, const std::string& idOrGroup);

    VerifyReport Verify(const fs::path& game);
    RecoverReport Recover(const fs::path& game);
    ImportReport Import(const fs::path& game, const std::string& id, bool force);
    // Unregisters a mod. Active somewhere: E_IN_USE unless opts.force (then it is deactivated there first).
    void RemoveMod(const std::string& id, const RemoveOptions& opts);

    // The status as a document (the CLI prints it, `--json` gives it as is): launcher, game, active mods, pending work.
    Json Status(const fs::path& game);

private:
    void RequireGame(const fs::path& game) const;
    void SaveState(const fs::path& game, const GameState& s) const;
    void CheckClosed(const std::vector<std::string>& processes) const;
    void WriteJournal(const GameDirs& d, const Plan& plan) const;
    void Rollback(const std::vector<Op>& ops, std::vector<std::string>* errors) const;
    void RunHooks(const std::string& name, const Manifest& m, const std::map<std::string, std::string>& env, bool fatal,
                  std::vector<std::string>* warnings) const;
    void Trim(const GameDirs& d) const;

    Registry reg_;
    Services svc_;
};

// Preconditions of a manifest for `action`, evaluated against a game folder. Blocking ones throw E_PRECONDITION; the rest
// are appended to *warnings.
struct PreContext {
    fs::path game;
    std::string build;
    std::string action;
    ProcessProbe* probe = nullptr;
};
void EvaluatePreconditions(const std::vector<Precondition>& list, const PreContext& ctx, const std::string& modId,
                           std::vector<std::string>* warnings);

}  // namespace vrml
