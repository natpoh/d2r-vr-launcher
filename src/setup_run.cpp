#include "setup_run.h"

#include <algorithm>
#include <cctype>

#include "game.h"
#include "mods.h"
#include "sha256.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

namespace vrml {

namespace {

constexpr uint64_t kMaxInstallerBytes = 512ull << 20;

std::string SafeDirName(const std::string& s) {
    std::string out;
    for (char c : s) out += (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_') ? c : '_';
    if (out.empty() || out == "." || out == "..") out = "unknown";
    return out;
}

uint64_t FileBytes(const fs::path& f) {
    std::error_code ec;
    const auto n = fs::file_size(f, ec);
    return ec ? 0 : static_cast<uint64_t>(n);
}

}  // namespace

HttpOptions HttpOptionsForRelease(const Release& r) {
    HttpOptions o;
    o.allow_hosts = DownloadHostsFor(r.id);
    o.max_bytes = r.size ? r.size : kMaxInstallerBytes;
    o.receive_timeout_ms = 60000;
    return o;
}

VerifiedInstaller DownloadAndVerify(const Release& r, const fs::path& cacheDir, Downloader& dl, const ProgressFn& progress, bool acceptUnverified,
                                    const std::string& testHttpHost) {
    std::string why;
    if (!ReleaseUrlOk(r, testHttpHost, &why)) Throw("E_UNTRUSTED", "the installer address is refused: " + why);
    if (!r.sha256.empty() && !IsSha256Hex(r.sha256)) Throw("E_UNTRUSTED", "the SHA-256 given for the installer is not a SHA-256");
    if (r.sha256.empty() && !acceptUnverified)
        Throw("E_UNVERIFIED", "no SHA-256 is published for " + r.asset_name + " (" + r.source + "), so the launcher cannot prove that the download is the file that was announced. "
                              "Nothing was downloaded. Install it anyway only if you trust " + r.source + ".");
    const std::string want = Lower(r.sha256);
    const fs::path root = cacheDir / "installers" / P(SafeDirName(r.id));
    const fs::path dir = root / P(SafeDirName(r.version));
    const fs::path file = dir / P(r.asset_name);
    VerifiedInstaller v;
    v.path = file;

    std::string h;
    if (IsFile(file)) {
        // only a copy that matches a published hash (and size) is trusted again
        if (!want.empty() && Sha256File(file, &h) && h == want && (r.size == 0 || FileBytes(file) == r.size)) {
            v.sha256 = h;
            v.size = FileBytes(file);
            v.verified = true;
            return v;
        }
        RemoveQuiet(file);
    }
    EnsureDir(dir);
    dl.SetProgress(progress);
    struct Reset { Downloader& d; ~Reset() { d.SetProgress({}); } } reset{dl};
    std::string err;
    if (!dl.Fetch(r.url, file, &err)) {
        RemoveQuiet(file);
        if (dl.WasCancelled()) Throw("E_CANCELLED", "the download of " + r.asset_name + " was cancelled");
        Throw("E_IO", "could not download " + r.asset_name + ": " + err);
    }
    v.size = FileBytes(file);
    if (!Sha256File(file, &h)) {
        RemoveQuiet(file);
        Throw("E_IO", "cannot read the downloaded " + U(file));
    }
    if (r.size != 0 && v.size != r.size) {
        RemoveQuiet(file);
        Throw("E_HASH", "the downloaded " + r.asset_name + " is " + std::to_string(v.size) + " bytes, not the " + std::to_string(r.size) + " that " + r.source + " announced; it was deleted.");
    }
    if (!want.empty() && h != want) {
        RemoveQuiet(file);
        Throw("E_HASH", "the downloaded " + r.asset_name + " has SHA-256 " + h + ", not the " + want + " that " + r.source + " announced; it was deleted and nothing was run.");
    }
    v.sha256 = h;
    v.verified = !want.empty();
    v.downloaded = true;
    // older installers of this mod are not needed again
    std::error_code ec;
    std::vector<fs::path> old;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
        if (it->path() != dir) old.push_back(it->path());
    for (const fs::path& p : old) RemoveQuiet(p);
    return v;
}

// ------------------------------------------------------------------ running

std::string InnoDirArg(const fs::path& dir) {
    std::string s = dir.u8string();
    std::replace(s.begin(), s.end(), '/', '\\');
    while (s.size() > 3 && s.back() == '\\') s.pop_back();
    for (char c : s)
        if (c == '"' || static_cast<unsigned char>(c) < 0x20) Throw("E_PRECONDITION", "the game folder's path has a character that cannot be passed to a Setup");
    if (s.empty()) Throw("E_PRECONDITION", "no game folder to give the Setup");
    return "/DIR=\"" + s + "\"";
}

bool SetupExitIsCancel(int code) { return code == 2 || code == 5; }

std::string DescribeSetupExit(int code) {
    switch (code) {
        case 0: return "";
        case 1: return "the Setup could not start";
        case 2: return "the Setup was cancelled before anything was installed";
        case 3: return "the Setup stopped with an error while starting";
        case 4: return "the Setup stopped with an error while installing";
        case 5: return "the Setup was cancelled while installing";
        case 6: return "the Setup was ended from outside";
        case 7: return "the Setup could not close the programs it needed to";
        case 8: return "the Setup could not close the programs it needed to";
        default: return "the Setup ended with exit code " + std::to_string(code);
    }
}

int RunSetup(const fs::path& exe, bool elevate, const std::vector<std::string>& args, SetupBackend& backend) {
    if (!IsFile(exe)) Throw("E_NOT_FOUND", "the installer " + U(exe) + " is not there");
    SetupCommand cmd;
    cmd.exe = exe;
    cmd.args = args;
    cmd.elevate = elevate;
    int code = -1;
    std::string err;
    switch (backend.Run(cmd, &code, &err)) {
        case SetupStatus::Finished: return code;
        case SetupStatus::UacDeclined: Throw("E_UAC_DECLINED", "Windows asked for administrator rights to run the Setup and the answer was No. Nothing was installed.");
        case SetupStatus::FailedToStart: break;
    }
    Throw("E_IO", "could not start " + U(exe.filename()) + ": " + err);
}

#ifdef _WIN32

namespace {

std::wstring Wide(const std::string& s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

class ShellSetupBackend : public SetupBackend {
public:
    SetupStatus Run(const SetupCommand& cmd, int* exitCode, std::string* error) override {
        std::wstring params;
        for (const std::string& a : cmd.args) {
            if (!params.empty()) params += L' ';
            params += Wide(a);
        }
        SHELLEXECUTEINFOW sei = {};
        sei.cbSize = sizeof sei;
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        sei.lpVerb = cmd.elevate ? L"runas" : L"open";
        const std::wstring file = cmd.exe.wstring();
        const std::wstring dir = cmd.exe.parent_path().wstring();
        sei.lpFile = file.c_str();
        sei.lpParameters = params.empty() ? nullptr : params.c_str();
        sei.lpDirectory = dir.empty() ? nullptr : dir.c_str();
        sei.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&sei)) {
            const DWORD e = GetLastError();
            if (e == ERROR_CANCELLED) return SetupStatus::UacDeclined;
            if (error) *error = "Windows error " + std::to_string(e);
            return SetupStatus::FailedToStart;
        }
        if (!sei.hProcess) {
            if (error) *error = "Windows gave no process to wait for";
            return SetupStatus::FailedToStart;
        }
        WaitForSingleObject(sei.hProcess, INFINITE);
        DWORD code = 0;
        const BOOL got = GetExitCodeProcess(sei.hProcess, &code);
        CloseHandle(sei.hProcess);
        if (!got) {
            if (error) *error = "the exit code of the Setup could not be read";
            return SetupStatus::FailedToStart;
        }
        *exitCode = static_cast<int>(code);
        return SetupStatus::Finished;
    }
};

}  // namespace

std::unique_ptr<SetupBackend> MakeSystemSetupBackend() { return std::make_unique<ShellSetupBackend>(); }

#else

namespace {
class NoSetupBackend : public SetupBackend {
public:
    SetupStatus Run(const SetupCommand&, int*, std::string* error) override {
        if (error) *error = "running a Setup is implemented for Windows only";
        return SetupStatus::FailedToStart;
    }
};
}  // namespace
std::unique_ptr<SetupBackend> MakeSystemSetupBackend() { return std::make_unique<NoSetupBackend>(); }

#endif

// ------------------------------------------------------------------ all of it

ModInstallResult InstallMod(const fs::path& game, const std::string& id, const Release& release, Downloader& dl, ProcessProbe& probe, SetupBackend& backend,
                            const ModInstallOptions& opt) {
    ModInstallResult res;
    res.id = id;
    res.release = release;
    if (id != "d2r-vr" && id != "d2r-pcvr") Throw("E_USAGE", "unknown mod \"" + id + "\" (d2r-vr or d2r-pcvr)");
    if (release.id != id) Throw("E_USAGE", "the release is for " + release.id + ", not for " + id);
    const std::string name = id == "d2r-vr" ? "D2R VR Authentic" : "D2R VR Unleashed";
    if (!LooksLikeGame(game)) Throw("E_GAME", U(game) + " has no D2R.exe. Pick the folder Diablo II: Resurrected is installed in.");
    const std::vector<std::string> running = probe.Running({"D2R.exe", "D2RLoader.exe"});
    if (!running.empty()) Throw("E_GAME_RUNNING", "Close " + running.front() + " first: the game is running.");
    if (release.up_to_date || release.url.empty()) Throw("E_PRECONDITION", name + " has nothing newer to install.");

    for (const ModEntry& e : DiscoverMods(game))
        if (e.id == id) {
            res.was_installed = e.installed;
            res.previous_version = e.installed && e.source == "manifest" ? NormalizeVersion(e.version) : "";
        }

    res.installer = DownloadAndVerify(release, opt.cache_dir, dl, opt.progress, opt.accept_unverified, opt.test_http_host);
    if (!res.installer.verified)
        res.notes.push_back("No checksum is published for this installer; the file's SHA-256 is " + res.installer.sha256 + ".");
    if (!opt.run_setup) {
        res.message = name + " " + release.version + " was downloaded and checked" + std::string(res.installer.verified ? "" : " (no published checksum to compare with)") +
                      "; the Setup was not run. It is " + U(res.installer.path) + ".";
        res.installed = res.was_installed;
        res.installed_version = res.previous_version;
        return res;
    }

    // Authentic's Setup needs administrator rights (it writes into the game folder, often in Program Files); Unleashed's is per user and has its own wizard
    const bool elevate = id == "d2r-vr" && !opt.test_never_elevate;
    std::vector<std::string> args;
    if (id == "d2r-vr") args.push_back(InnoDirArg(game));
    res.elevated = elevate;
    res.exit_code = RunSetup(res.installer.path, elevate, args, backend);
    res.setup_ran = true;

    for (const ModEntry& e : DiscoverMods(game))
        if (e.id == id) {
            res.installed = e.installed;
            res.installed_version = e.installed && e.source == "manifest" ? NormalizeVersion(e.version) : "";
        }
    const std::string ver = res.installed_version.empty() ? std::string() : " " + res.installed_version;
    if (res.exit_code == 0 && res.installed) {
        res.message = name + ver + " is installed in " + U(game) + ".";
        if (!res.installed_version.empty() && res.installed_version != NormalizeVersion(release.version))
            res.notes.push_back("The folder now holds version " + res.installed_version + ", the announced one was " + release.version + ".");
    } else if (res.exit_code == 0) {
        res.code = "E_MOD_NOT_INSTALLED";
        res.message = "The Setup finished, but " + name + " is not in " + U(game) + ". Did you choose another folder in the Setup?";
    } else if (SetupExitIsCancel(res.exit_code)) {
        res.code = "E_CANCELLED";
        res.message = "The Setup was cancelled" + std::string(res.installed ? "; " + name + (res.was_installed ? " stays as it was." : " is installed anyway.") : "; nothing was installed.");
    } else {
        res.code = "E_SETUP";
        res.message = DescribeSetupExit(res.exit_code) + " (exit code " + std::to_string(res.exit_code) + ")." + (res.installed ? std::string() : " " + name + " is not installed.");
    }
    if (!CheckLoader(game, false).present) res.notes.push_back("D2RLoader.exe is not in the game folder yet: run `install loader` (the mods start only under D2RLoader).");
    return res;
}

}  // namespace vrml
