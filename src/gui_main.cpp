// D2RVRLauncher.exe - the window of the D2R VR launcher (docs/MOD_MANIFEST.md says what a mod is; gui_model.h what the window decides).
//
//   D2RVRLauncher.exe                            the window
//   D2RVRLauncher.exe --game DIR --home DIR      for tests and support: a given game folder, a given launcher folder
//   D2RVRLauncher.exe --play [--dry-run] ...     no window: do Play once, print the outcome, exit with 0 or the refusal's code
//   D2RVRLauncher.exe --screenshot FILE.png      render the window to a PNG and exit
//   D2RVRLauncher.exe --install-view             the window, with the install view (D2RLoader and the two mods: install / update) opened right away
//   D2RVRLauncher.exe --install-run loader,authentic,unleashed --game DIR     (test aid, needs VRML_ALLOW_HTTP_HOST) what the view's Install does, no window
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <string>
#include <vector>

#include "gui_draw.h"
#include "gui_model.h"
#include "gui_window.h"
#include "install_run.h"

namespace {

using namespace vrml;
using namespace vrml::gui;

std::string Utf8(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return "";
    std::string s(static_cast<size_t>(n) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

// A window-subsystem program has no console of its own. Output goes to the handle the caller redirected (a pipe, a file), else to the
// console of the shell that started it. False when there is nowhere to write.
bool WriteOut(const std::string& text) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE) {
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) return false;
        out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (out == INVALID_HANDLE_VALUE) return false;
    }
    DWORD written = 0;
    return WriteFile(out, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) != 0;
}

// --play: what the Play button would do, once, without a window. The outcome goes to stdout and to <home>\logs\gui_last.txt (for a
// caller that cannot read our stdout).
int HeadlessPlay(Model& model) {
    const Options& opt = model.options();
    std::string text;
    int code = 0;
    const ModEntry* mod = model.mod();
    if (!model.game() || !model.game()->is_game) {
        text = "error [E_GAME]: there is no game folder (give --game DIR)\n";
        code = StartExitCode("E_GAME");
    } else if (!opt.select.empty() && !(mod && (IEquals(mod->id, opt.select) || IEquals(mod->folder, opt.select)))) {
        text = "error [E_NOT_FOUND]: there is no mod \"" + opt.select + "\" in this game folder\n";
        code = 3;
    } else {
        const StartOutcome so = model.Play(opt.dry_run);
        text = FormatOutcome(so, opt.dry_run, &code);
    }
    text += "exit " + std::to_string(code) + "\n";
    WriteOut(text);
    try {
        EnsureDir(model.home() / "logs");
        WriteFileAtomic(model.home() / "logs" / "gui_last.txt", text);
    } catch (const std::exception&) {
        // the file is a convenience for tests: the exit code and stdout already said it
    }
    return code;
}

// --install-run: what the install view's Install button does for these pieces, once, without a window (the same RunInstallJobs). One block per piece
// goes to stdout and to <home>\logs\gui_last.txt; the exit code is 0, or the first failed piece's code (27 no published checksum, 23 hash, 30 cancelled ...).
int HeadlessInstall(Model& model) {
    const Options& opt = model.options();
    std::string text;
    int code = 0;
    if (!model.game() || !model.game()->is_game) {
        text = "error [E_GAME]: there is no game folder (give --game DIR)\n";
        code = ExitCodeForCode("E_GAME");
    } else {
        std::vector<PieceJob> jobs;
        for (const std::string& word : opt.install_run) {
            PieceJob j;
            if (!ParsePiece(word, &j.piece)) continue;
            j.accept_unverified = opt.accept_unverified;
            jobs.push_back(j);
        }
        CliEnv env;
        env.env = model.services().env;
        env.probe = model.services().probe;
        RunContext ctx;
        ctx.game = model.game()->install.dir;
        ctx.home = model.home();
        ctx.env = &env;
        ctx.pin_file = opt.pin_file;
        const std::vector<PieceOutcome> outcomes = RunInstallJobs(ctx, jobs, RunHooks());
        text = FormatOutcomes(outcomes);
        code = FirstFailureExit(outcomes);
    }
    text += "exit " + std::to_string(code) + "\n";
    WriteOut(text);
    try {
        EnsureDir(model.home() / "logs");
        WriteFileAtomic(model.home() / "logs" / "gui_last.txt", text);
    } catch (const std::exception&) {
    }
    return code;
}

}  // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
    int argc = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    for (int i = 1; wargv && i < argc; ++i) args.push_back(Utf8(wargv[i]));
    if (wargv) LocalFree(wargv);

    const Options opt = ParseOptions(args);
    if (opt.help || !opt.problems.empty()) {
        std::string text;
        for (const std::string& p : opt.problems) text += p + "\n";
        text += UsageText();
        if (!WriteOut(text)) MessageBoxW(nullptr, fs::u8path(text).wstring().c_str(), L"D2R VR Launcher", MB_OK | MB_ICONINFORMATION);
        return opt.help ? 0 : 2;
    }

    {
        // switches that reach a web server are test aids: only with the loopback test switch (VRML_ALLOW_HTTP_HOST)
        const std::vector<std::string> bad = TestSwitchProblems(opt, CurrentEnvironment());
        if (!bad.empty()) {
            std::string text;
            for (const std::string& p : bad) text += p + "\n";
            if (!WriteOut(text)) MessageBoxW(nullptr, fs::u8path(text).wstring().c_str(), L"D2R VR Launcher", MB_OK | MB_ICONINFORMATION);
            return 2;
        }
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);       // the folder and file pickers are COM
    int code = 0;
    {
        Machine machine = SystemMachine();
        FakeProbe pretend;                            // --processes: a process list of the test's own, the machine's is not read
        if (opt.fake_processes) {
            for (const std::string& n : opt.processes) pretend.Start(n);
            machine.probe = &pretend;
        }
        Model model(opt, machine);
        if (opt.play) {
            code = HeadlessPlay(model);
        } else if (opt.has_install_run) {
            code = HeadlessInstall(model);
        } else {
            GdiplusSession gdiplus;
            code = RunWindow(instance, model);
        }
    }
    CoUninitialize();
    return code;
}
