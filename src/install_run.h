// install_run.h - installing D2RLoader and the two mods from the launcher's window (and from `--install-run`, which has no window): one place that
// runs the pieces one after the other, so what the install view's Install button does and what the headless run does is the same code.
//
//   Piece              D2RLoader (installed by the launcher itself), D2R VR Authentic (its own Setup, elevated), D2R VR Unleashed (his Setup, per user)
//   PieceJob           what to do with one piece: the release the player saw (or "look it up"), `accept_unverified`, `force` (install again)
//   RunInstallJobs     the pieces in the right order - D2RLoader first - each one: look up the release, download, check the hash, install (D2RLoader) or run
//                      the Setup (mods), look at the game folder afterwards. One outcome per piece, in words. A piece that fails does not stop the others,
//                      except a cancel, a running game and a folder that is not a game. Never throws: everything is in the outcomes.
//   RunHooks           what the caller wants to hear while it runs (log lines, progress, which piece is done) and the cancel question. Called on the thread
//                      that runs RunInstallJobs: the window's worker posts them to the UI thread.
//   test switches      VRML_ALLOW_HTTP_HOST, VRML_URL_LOADER / _AUTHENTIC_API / _UNLEASHED_API, VRML_TEST_NO_ELEVATE, VRML_TEST_CANCEL_AFTER and the pin file
//                      are honoured only together with the loopback switch (net.h); production sets none of them. They are the same as `vrmodlauncher install`'s.
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "cli.h"
#include "install.h"
#include "mods.h"
#include "releases.h"
#include "util.h"

namespace vrml {

enum class Piece { Loader = 0, Authentic = 1, Unleashed = 2 };
constexpr int kPieceCount = 3;
const char* PieceId(Piece p);          // "d2rloader", "d2r-vr", "d2r-pcvr"
const char* PieceName(Piece p);        // "D2RLoader", "D2R VR Authentic", "D2R VR Unleashed"
const char* PieceWord(Piece p);        // the word on the command line: "loader", "authentic", "unleashed"
bool IsMod(Piece p);
// loader | d2rloader, authentic | d2r-vr, unleashed | d2r-pcvr (any case)
bool ParsePiece(const std::string& text, Piece* out);
// "loader,authentic" -> the pieces, in the order they are installed in; false (and *problem) for a word that is none of them or an empty list.
bool ParsePieceList(const std::string& csv, std::vector<Piece>* out, std::string* problem);
// The Piece of a mod id ("d2r-vr" / "d2r-pcvr"); false for any other id.
bool PieceOfModId(const std::string& id, Piece* out);

// ---- sources and the loader's pin, with the test switches
ReleaseSources ReleaseSourcesFor(const std::map<std::string, std::string>& env);
// The HTTP client options of the JSON question for a mod (host list, size cap); the loopback host only with the test switch.
HttpOptions HttpOptionsForQuestion(Piece mod, const std::map<std::string, std::string>& env);

struct LoaderPlan {
    Pin pin;                                    // what is downloaded
    std::vector<ExpectedFile> expected;         // what must be in the game folder afterwards (the three reviewed files)
    bool own_expected = false;                  // from a test pin file: handed to InstallD2RLoader
};
// The built-in pin and reviewed files; with a test pin file (only together with the loopback switch, else E_USAGE) and VRML_URL_LOADER the test ones.
LoaderPlan LoaderPlanFor(const std::map<std::string, std::string>& env, const std::string& pinFile);
// D2RLoader in `game` against `plan.expected` (hashes the three files, ~50 MB): the same answers as CheckLoader, for the plan in use.
LoaderStatus CheckLoaderAgainst(const fs::path& game, const LoaderPlan& plan);

// "What is newest" for one mod (the install view and the quiet check at start): CheckUpdate with the right client and sources. Never throws: u.checked / u.error.
// `http` null = the real client (WinHTTP; the loopback host only with the test switch).
UpdateInfo CheckModRelease(Piece mod, const fs::path& game, const std::map<std::string, std::string>& env, HttpClient* http = nullptr);

// ---- jobs and outcomes
struct PieceJob {
    Piece piece = Piece::Loader;
    bool accept_unverified = false;     // the player said yes to a release that has no published checksum
    bool force = false;                 // install the newest even when the installed version is current (a repair); D2RLoader: nothing to force
    bool has_release = false;           // `release` is what the player saw; else the release is looked up when the job runs
    Release release;
};

enum class JobStatus { Installed, AlreadyThere, Failed, Cancelled, NotRun };
const char* JobStatusWord(JobStatus s);    // "installed", "already-there", "failed", "cancelled", "not-run"

struct PieceOutcome {
    Piece piece = Piece::Loader;
    JobStatus status = JobStatus::NotRun;
    std::string code;                   // "" or E_...
    int exit_code = 0;                  // the command line's exit code for `code` (0 when it went well)
    int setup_exit = -1;                // the Setup's own exit code when one ran
    std::string message;                // one or two sentences for the player
    std::vector<std::string> notes;
    std::string version;                // what is installed afterwards ("" = unknown or nothing)
    std::string sha256;                 // of the installer that was downloaded, for a mod
    bool sha_verified = false;
    bool downloaded = false;
};

struct RunHooks {
    std::function<void(const std::string& line)> log;
    // `stage`: "lookup", "download", "unpack", "verify", "install", "setup", "check". done/total are bytes for "download" (total 0 = not known).
    std::function<void(int index, int count, Piece piece, const std::string& stage, uint64_t done, uint64_t total)> progress;
    std::function<bool()> cancelled;                    // asked often; true stops at the next safe point
    std::function<void(const PieceOutcome&)> piece_done;
};

struct RunContext {
    fs::path game, home;
    CliEnv* env = nullptr;              // probe (required: "is the game running"); env (test switches); null downloader/http/setup = the real ones
    std::string pin_file;               // TEST switch (needs the loopback switch)
};

// The jobs in the order they run (D2RLoader, Authentic, Unleashed; one job per piece), whatever order they were given in.
std::vector<PieceJob> OrderedJobs(std::vector<PieceJob> jobs);
// Runs them. Blocks until the last Setup is closed: call it from a worker thread. Not run because of a cancel or a stop: status NotRun.
std::vector<PieceOutcome> RunInstallJobs(const RunContext& ctx, std::vector<PieceJob> jobs, const RunHooks& hooks);

// The command line's exit code for an error or result code: 0 "", 27 E_UNVERIFIED, 23 E_HASH, 30 E_CANCELLED, 31 E_UAC_DECLINED, 28 E_SETUP, 11 E_MOD_NOT_INSTALLED, ...
int ExitCodeForCode(const std::string& code);
// The exit code of the first failed or cancelled piece; 0 when every piece went well (or was already there).
int FirstFailureExit(const std::vector<PieceOutcome>& outcomes);
// The text `--install-run` prints: one "<id>  <status>[  <code>]" line per piece, the message under it.
std::string FormatOutcomes(const std::vector<PieceOutcome>& outcomes);

}  // namespace vrml
