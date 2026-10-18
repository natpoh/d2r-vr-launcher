#include "gui_uninstall.h"

#include <atomic>
#include <memory>

#include <shellapi.h>

namespace vrml {
namespace gui {

namespace {

std::wstring Wide(const std::string& s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

struct Wait {
    HWND target;
    HANDLE process;
    UINT message;
    std::shared_ptr<std::atomic<bool>> done;
};

}  // namespace

bool StartUninstaller(HWND owner, const Model::UninstallOffer& o, HWND notify, UINT message, bool* cancelled, std::function<bool()>* finished) {
    if (cancelled) *cancelled = false;
    auto done = std::make_shared<std::atomic<bool>>(false);
    if (finished) *finished = [done] { return done->load(); };
    std::wstring params;
    for (const std::string& a : o.args) params += (params.empty() ? L"" : L" ") + Wide(a);
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof sei;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.hwnd = owner;
    sei.lpVerb = o.needs_admin ? L"runas" : L"open";
    fs::path exe_path = o.exe;
    exe_path.make_preferred();
    const std::wstring exe = exe_path.wstring();
    sei.lpFile = exe.c_str();
    sei.lpParameters = params.empty() ? nullptr : params.c_str();
    const std::wstring dir = exe_path.parent_path().wstring();
    sei.lpDirectory = dir.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        if (cancelled) *cancelled = GetLastError() == ERROR_CANCELLED;
        done->store(true);
        return false;
    }
    if (!sei.hProcess) {
        done->store(true);              // nothing to wait on: the window cannot tell when it ends
        return true;
    }
    auto* job = new Wait{notify, sei.hProcess, message, done};
    HANDLE th = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        std::unique_ptr<Wait> w(static_cast<Wait*>(p));
        WaitForSingleObject(w->process, INFINITE);
        CloseHandle(w->process);
        w->done->store(true);           // first, so whoever gets the message (or polls) sees it ended
        PostMessageW(w->target, w->message, 0, 0);
        return 0;
    }, job, 0, nullptr);
    if (th) {
        CloseHandle(th);
    } else {
        CloseHandle(sei.hProcess);
        done->store(true);
        delete job;
    }
    return true;
}

}  // namespace gui
}  // namespace vrml
