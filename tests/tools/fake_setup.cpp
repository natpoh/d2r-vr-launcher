// fake_setup.exe - a stand-in for a mod's Setup in tools/install_smoke.py and tools/install_view_smoke.py. It installs nothing real: what it does is told by
// environment variables
//   FAKE_SETUP_LOG       file the arguments and the variables below are appended to
//   FAKE_SETUP_GAME      the game folder it "installs" into
//   FAKE_SETUP_MOD       the mod folder name ("D2R-VR", "D2R-PCVR"); when absent it follows its own file name: ...D2R_VR... = D2R-VR, ...PCVR... = D2R-PCVR
//   FAKE_SETUP_MANIFEST  a file copied to <game>\mods\<mod>\vrmod-manifest.json (what a real Setup writes)
//   FAKE_SETUP_MODE      "install" (default): write the mod; "nothing": exit without writing (a Setup that installed somewhere else)
//   FAKE_SETUP_EXIT      the exit code (default 0; Inno Setup uses 5 for "cancelled")
//   FAKE_SETUP_DELAY_MS  wait this long before it ends (a Setup wizard that is open for a while)
// Copied to a file named unins000.exe it is a stand-in for a mod's own UNINSTALLER instead (D2R VR Authentic's in the game folder, D2R VR Unleashed's in
// ...\Programs\D2R PCVR; the mod is told by the folder it sits in: "D2R PCVR" = D2R-PCVR, else D2R-VR):
//   FAKE_UNINSTALL_LOG       file the arguments are appended to ("uninstaller started args=N", "uninstaller mod=...", "uninstaller removed" / "uninstaller kept"); default FAKE_SETUP_LOG
//   FAKE_UNINSTALL_MODE      "remove" (default): delete <FAKE_SETUP_GAME>\mods\<mod>; "keep": exit without removing (the player answered No in its own question)
//   FAKE_UNINSTALL_EXIT      the exit code (default 0)
//   FAKE_UNINSTALL_DELAY_MS  wait this long before it removes anything (its wizard is open for a while)
// The three after MOD may be given per mod, with the mod's name and '_' for '-' as a suffix, and then win over the plain one: FAKE_SETUP_MANIFEST_D2R_VR,
// FAKE_SETUP_MODE_D2R_PCVR, FAKE_SETUP_EXIT_D2R_VR ... (one environment serves a run that installs both mods).
#include <windows.h>

#include <cstdio>
#include <cwctype>
#include <string>

namespace {

std::wstring Env(const wchar_t* name) {
    wchar_t buf[4096] = {};
    const DWORD n = GetEnvironmentVariableW(name, buf, 4096);
    return n > 0 && n < 4096 ? std::wstring(buf, n) : std::wstring();
}

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return "";
    std::string s(static_cast<size_t>(WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), static_cast<int>(s.size()), nullptr, nullptr);
    return s;
}

void MakeDirs(const std::wstring& path) {
    for (size_t i = 3; i < path.size(); ++i)
        if (path[i] == L'\\' || path[i] == L'/') CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
    CreateDirectoryW(path.c_str(), nullptr);
}

std::wstring Upper(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(towupper(c));
    return s;
}

void RemoveTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring path = dir + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                RemoveTree(path);
            } else {
                SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(path.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

// unins000.exe: the stand-in uninstaller
int Uninstall(const std::wstring& self, int argc, wchar_t** argv) {
    const size_t slash = self.find_last_of(L"\\/");
    const std::wstring parent = slash == std::wstring::npos ? std::wstring() : self.substr(0, slash);
    const size_t pslash = parent.find_last_of(L"\\/");
    const std::wstring folder = Upper(pslash == std::wstring::npos ? parent : parent.substr(pslash + 1));
    const std::wstring mod = folder == L"D2R PCVR" ? L"D2R-PCVR" : L"D2R-VR";
    std::wstring logPath = Env(L"FAKE_UNINSTALL_LOG");
    if (logPath.empty()) logPath = Env(L"FAKE_SETUP_LOG");
    const std::wstring game = Env(L"FAKE_SETUP_GAME"), mode = Env(L"FAKE_UNINSTALL_MODE");
    auto log = [&](const std::string& line) {
        if (logPath.empty()) return;
        FILE* f = nullptr;
        if (_wfopen_s(&f, logPath.c_str(), L"ab") == 0 && f) {
            fprintf(f, "%s\n", line.c_str());
            fclose(f);
        }
    };
    log("uninstaller started args=" + std::to_string(argc - 1));
    for (int i = 1; i < argc; ++i) log("uninstaller arg" + std::to_string(i) + "=" + Utf8(argv[i]));
    log("uninstaller mod=" + Utf8(mod));
    const std::wstring delay = Env(L"FAKE_UNINSTALL_DELAY_MS");
    if (!delay.empty()) Sleep(static_cast<DWORD>(_wtoi(delay.c_str())));
    if (mode != L"keep" && !game.empty()) {
        RemoveTree(game + L"\\mods\\" + mod);
        log("uninstaller removed");
    } else {
        log("uninstaller kept");
    }
    const std::wstring code = Env(L"FAKE_UNINSTALL_EXIT");
    return code.empty() ? 0 : _wtoi(code.c_str());
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const std::wstring exe = Upper(std::wstring(self));
    {
        const size_t sl = exe.find_last_of(L"\\/");
        if ((sl == std::wstring::npos ? exe : exe.substr(sl + 1)) == L"UNINS000.EXE") return Uninstall(std::wstring(self), argc, argv);
    }
    std::wstring mod = Env(L"FAKE_SETUP_MOD");
    if (mod.empty()) {
        const size_t slash = exe.find_last_of(L"\\/");
        const std::wstring name = slash == std::wstring::npos ? exe : exe.substr(slash + 1);
        if (name.find(L"PCVR") != std::wstring::npos) mod = L"D2R-PCVR";
        else if (name.find(L"D2R_VR") != std::wstring::npos) mod = L"D2R-VR";
    }
    std::wstring suffix = Upper(mod);
    for (wchar_t& c : suffix)
        if (c == L'-') c = L'_';
    auto per = [&](const wchar_t* base) {
        std::wstring v = Env((std::wstring(base) + L"_" + suffix).c_str());
        return v.empty() ? Env(base) : v;
    };
    const std::wstring logPath = Env(L"FAKE_SETUP_LOG");
    const std::wstring game = Env(L"FAKE_SETUP_GAME"), manifest = per(L"FAKE_SETUP_MANIFEST"), mode = per(L"FAKE_SETUP_MODE");
    if (!logPath.empty()) {
        FILE* f = nullptr;
        if (_wfopen_s(&f, logPath.c_str(), L"ab") == 0 && f) {
            fprintf(f, "started args=%d\n", argc - 1);
            for (int i = 1; i < argc; ++i) fprintf(f, "arg%d=%s\n", i, Utf8(argv[i]).c_str());
            wchar_t cwd[MAX_PATH] = {};
            GetCurrentDirectoryW(MAX_PATH, cwd);
            fprintf(f, "cwd=%s\n", Utf8(cwd).c_str());
            fprintf(f, "game=%s\nmod=%s\nmode=%s\n", Utf8(game).c_str(), Utf8(mod).c_str(), Utf8(mode).c_str());
            fclose(f);
        }
    }
    const std::wstring delay = Env(L"FAKE_SETUP_DELAY_MS");
    if (!delay.empty()) Sleep(static_cast<DWORD>(_wtoi(delay.c_str())));
    if (mode != L"nothing" && !game.empty() && !mod.empty()) {
        const std::wstring dir = game + L"\\mods\\" + mod;
        MakeDirs(dir + L"\\" + mod + L".mpq");
        FILE* f = nullptr;
        if (_wfopen_s(&f, (dir + L"\\" + mod + L".mpq\\modinfo.json").c_str(), L"wb") == 0 && f) {
            fputs("{\"name\":\"fake\"}", f);
            fclose(f);
        }
        if (!manifest.empty()) CopyFileW(manifest.c_str(), (dir + L"\\vrmod-manifest.json").c_str(), FALSE);
    }
    const std::wstring code = per(L"FAKE_SETUP_EXIT");
    return code.empty() ? 0 : _wtoi(code.c_str());
}
