// fake_loader.exe - a stand-in for D2RLoader.exe in the launcher's end-to-end tests. It starts nothing: it writes how it was started - the arguments, the
// folder, the launcher's environment (XR_RUNTIME_JSON, VRMOD_*, the Steam variables) - to fake_loader_started.txt beside itself, and exits.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

int wmain(int argc, wchar_t** argv) {
    wchar_t exe[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe, n);
    dir.resize(dir.find_last_of(L'\\') + 1);
    FILE* f = nullptr;
    if (_wfopen_s(&f, (dir + L"fake_loader_started.txt").c_str(), L"wb") != 0 || !f) return 2;
    auto utf8 = [](const std::wstring& w) {
        std::string s(static_cast<size_t>(WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr)), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), (int)s.size(), nullptr, nullptr);
        return s;
    };
    fprintf(f, "args=%d\n", argc - 1);
    for (int i = 1; i < argc; ++i) fprintf(f, "arg%d=%s\n", i, utf8(argv[i]).c_str());
    wchar_t cwd[MAX_PATH] = {};
    GetCurrentDirectoryW(MAX_PATH, cwd);
    fprintf(f, "cwd=%s\n", utf8(cwd).c_str());
    static const wchar_t* const names[] = {L"XR_RUNTIME_JSON", L"VRMOD_ACTIVE_ID", L"VRMOD_ACTIVE_VERSION", L"VRMOD_RUNTIME", L"VRMOD_STORE", L"VRMOD_GAME_BUILD",
                                           L"SteamAppId", L"SteamGameId", L"SteamOverlayGameId"};
    for (const wchar_t* name : names) {
        wchar_t v[2048] = {};
        const DWORD len = GetEnvironmentVariableW(name, v, 2048);
        fprintf(f, "%s=%s\n", utf8(name).c_str(), len > 0 && len < 2048 ? utf8(v).c_str() : "");
    }
    fclose(f);
    return 0;
}
