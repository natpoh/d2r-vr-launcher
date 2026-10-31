// util.h - errors, paths, small file helpers and the log, shared by every part of the launcher.
#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace vrml {

namespace fs = std::filesystem;

// An error with a stable code (what scripts and installers see) and an exit code (the CLI contract).
struct Error : std::runtime_error {
    std::string code;
    int exit_code;
    Error(std::string c, int exit, const std::string& msg) : std::runtime_error(msg), code(std::move(c)), exit_code(exit) {}
};
// the codes (exit code in brackets)
//   E_USAGE[2] E_NOT_FOUND[3] E_MANIFEST[4] E_GAME_RUNNING[10] E_PRECONDITION[11] E_COLLISION[12] E_IN_USE[12]
//   E_IDENTITY[12] E_SCHEMA[13] E_IO[20] E_ROLLBACK[21] E_JOURNAL[22] E_HASH[23] E_RUNTIME[24] E_GAME[25]
//   E_ARCHIVE[26] (a ZIP that is damaged or unsafe) E_UNVERIFIED[27] (no published checksum and the caller did not accept that)
//   E_SETUP[28] (a mod's Setup ended with an error) E_UNTRUSTED[29] (an address or an answer from a server the launcher does not trust) E_CANCELLED[30] (the player or the caller stopped a download) E_UAC_DECLINED[31]
//   E_FIRST_START[32] (the game has never been started on this PC without mods: no Settings.json in Saved Games)
[[noreturn]] void Throw(const std::string& code, const std::string& msg);

constexpr const char* kLauncherVersion = "0.3.8";   // the window's version resource is read from this line (CMakeLists.txt)
constexpr const char* kManifestSchema = "vrmod-manifest/1";

// ---- strings and paths (all paths in files are UTF-8 with '/')
std::string Lower(std::string s);
bool IEquals(const std::string& a, const std::string& b);
bool StartsWith(const std::string& s, const std::string& prefix);
bool EndsWith(const std::string& s, const std::string& suffix);
std::vector<std::string> Split(const std::string& s, char sep);

fs::path P(const std::string& utf8);          // UTF-8 -> path
std::string U(const fs::path& p);             // path -> UTF-8 with '/'

// A game-relative path as written in a manifest ("a\b", "./a//b") -> "a/b". Throws nothing; see SafeRel.
std::string NormRel(std::string s);
// False (with *why) for an empty path, "..", an absolute path, a drive, a stream (':'), or a trailing dot/space part.
bool SafeRel(const std::string& rel, std::string* why = nullptr);
// "*" and "?" only, case-insensitive (Windows); on the file name.
bool GlobMatch(const std::string& pattern, const std::string& name);

// First 16 hex digits of SHA-256 of the lower-cased, normalised, absolute game path: the folder name of a game's state.
std::string GameKey(const fs::path& gameDir);

// ---- files
bool Exists(const fs::path& p);
bool IsFile(const fs::path& p);
bool IsDir(const fs::path& p);
bool IsReparsePoint(const fs::path& p);       // a symlink or a junction (never followed by the engine)
// When a file or folder was created, as the file system says (Windows: FILETIME, 100 ns since 1601); -1 when it cannot be read. A copy or an
// update that rewrites a file's content keeps it; only a delete and create makes a new one.
long long CreationTime(const fs::path& p);
bool ReadFile(const fs::path& p, std::string* out);
void EnsureDir(const fs::path& dir);
// Writes to "<p>.vrml-tmp" and renames over p: a reader sees the old or the whole new file, never half.
void WriteFileAtomic(const fs::path& p, const std::string& data);
// Copies to "<dst>.vrml-tmp" and renames over dst; creates the parents. Throws E_IO.
void CopyAtomic(const fs::path& src, const fs::path& dst);
void RemoveQuiet(const fs::path& p);          // a file or a tree; no error if absent
constexpr const char* kTmpSuffix = ".vrml-tmp";

// Tests only: a stand-in for "the process died here". Thrown by a test hook, never caught by the engine's rollback.
struct SimulatedCrash : std::exception {
    const char* what() const noexcept override { return "simulated crash"; }
};
// Called by CopyAtomic after the temporary file is complete and before the rename (a crash there leaves "<dst>.vrml-tmp").
void SetAfterTmpHookForTests(std::function<void(const fs::path&)> hook);

// ---- time
std::string NowIso();                          // "2026-10-15T10:15:30Z"
std::string NewTxId();                         // sortable and unique within a run: "20261015-101530-0001"
void SetClockForTests(std::function<std::string()> iso);   // empty: the real clock

// ---- the log: one line per event, to a file (the support log) and to an in-memory tail (tests, `status`)
class Logger {
public:
    explicit Logger(fs::path file = {}) : file_(std::move(file)) {}
    void Line(const std::string& msg);
    const std::vector<std::string>& tail() const { return tail_; }
    void SetFile(fs::path f) { file_ = std::move(f); }

private:
    fs::path file_;
    std::vector<std::string> tail_;
};

}  // namespace vrml
