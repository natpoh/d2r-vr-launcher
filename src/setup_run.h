// setup_run.h - installing a mod by running ITS OWN Setup: download the installer, prove it is the file that was announced, run it,
// then look in the game folder to see whether the mod is there.
//
//   DownloadAndVerify   the installer into a cache folder. A published SHA-256 is enforced (a wrong file is deleted: E_HASH), a published size
//                       too. A release without a published SHA-256 is downloaded ONLY when the caller says accept_unverified (E_UNVERIFIED
//                       before any byte is fetched otherwise); the SHA-256 that was computed is returned so the player can see it.
//   RunSetup            start the file (UAC when `elevate`) and wait until it is closed; its exit code is returned. A declined UAC prompt is
//                       its own error (E_UAC_DECLINED). The process itself is a SetupBackend, so tests run a stand-in or a fake.
//   InstallMod          all of it for D2R VR Authentic (elevated, /DIR="<game folder>") or D2R VR Unleashed (his Setup, per user, its own wizard).
//                       The Setup may be cancelled by the player or may install somewhere else: the result reports the exit code AND whether
//                       the mod is now in this game folder.
// InstallMod blocks until the Setup is closed: the window calls it from a worker thread.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "deps.h"
#include "net.h"
#include "process.h"
#include "releases.h"
#include "util.h"

namespace vrml {

// ---- the installer file
struct VerifiedInstaller {
    fs::path path;
    std::string sha256;             // of the file on disk
    uint64_t size = 0;
    bool verified = false;          // a published SHA-256 was there and matched
    bool downloaded = false;        // false: a verified copy was in the cache already
};
// `cacheDir`\installers\<id>\<version>\<asset>. Errors: E_UNTRUSTED (address not allowed), E_UNVERIFIED, E_IO, E_CANCELLED, E_HASH (hash or size differs; file deleted).
VerifiedInstaller DownloadAndVerify(const Release& r, const fs::path& cacheDir, Downloader& dl, const ProgressFn& progress, bool acceptUnverified,
                                    const std::string& testHttpHost = TestHttpHostFromProcessEnv());
// The limits and the host list a download of `r` needs (size cap = the published size, else 512 MB).
HttpOptions HttpOptionsForRelease(const Release& r);

// ---- running a Setup
struct SetupCommand {
    fs::path exe;
    std::vector<std::string> args;      // already in the form the installer reads them: `/DIR="C:\Games\Diablo II Resurrected"`
    bool elevate = false;
};
enum class SetupStatus { Finished, UacDeclined, FailedToStart };
class SetupBackend {
public:
    virtual ~SetupBackend() = default;
    // Starts, waits for the process to end, gives its exit code. For Finished only *exitCode is set; otherwise *error says why.
    virtual SetupStatus Run(const SetupCommand& cmd, int* exitCode, std::string* error) = 0;
};
// Windows: ShellExecuteExW ("runas" when elevating, "open" otherwise: a Setup whose manifest asks for administrator still gets its prompt).
std::unique_ptr<SetupBackend> MakeSystemSetupBackend();

// The `/DIR="<folder>"` argument of an Inno Setup (no trailing backslash; E_PRECONDITION for a path with a quote or a control character).
std::string InnoDirArg(const fs::path& dir);
// Errors: E_UAC_DECLINED, E_IO (could not start). Returns the exit code.
int RunSetup(const fs::path& exe, bool elevate, const std::vector<std::string>& args, SetupBackend& backend);
// Inno Setup's exit codes in words ("the player cancelled the Setup"); "" for 0.
std::string DescribeSetupExit(int code);
bool SetupExitIsCancel(int code);          // 2 (cancelled before installing) and 5 (cancelled while installing)

// ---- all of it
struct ModInstallOptions {
    fs::path cache_dir;
    bool accept_unverified = false;
    bool run_setup = true;                  // false: download and verify only (--no-run)
    ProgressFn progress;
    std::string test_http_host = TestHttpHostFromProcessEnv();
    bool test_never_elevate = false;        // TESTS ONLY: the stand-in Setup must not raise a UAC prompt
};
struct ModInstallResult {
    std::string id;
    Release release;
    VerifiedInstaller installer;
    bool setup_ran = false;
    bool elevated = false;
    int exit_code = -1;
    bool installed = false;                 // after: the mod is in this game folder
    bool was_installed = false;             // before
    std::string installed_version;          // after ("" = unknown)
    std::string previous_version;
    std::string code;                       // "" = done; E_CANCELLED (the player cancelled the Setup), E_SETUP (it failed), E_MOD_NOT_INSTALLED (finished but the mod is not here)
    std::string message;                    // one or two sentences for the player
    std::vector<std::string> notes;
};
// id: "d2r-vr" (Authentic) or "d2r-pcvr" (Unleashed). Errors before the Setup: E_GAME, E_GAME_RUNNING, E_USAGE, E_UNTRUSTED, E_UNVERIFIED, E_HASH,
// E_IO, E_CANCELLED (download), E_UAC_DECLINED. After the Setup nothing throws: the result says how it went (`code`).
ModInstallResult InstallMod(const fs::path& game, const std::string& id, const Release& release, Downloader& dl, ProcessProbe& probe, SetupBackend& backend,
                            const ModInstallOptions& opt);

}  // namespace vrml
