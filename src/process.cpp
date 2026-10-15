#include "process.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace vrml {

namespace {

#ifdef _WIN32
class SystemProbe : public ProcessProbe {
public:
    std::vector<std::string> Running(const std::vector<std::string>& names) override {
        std::vector<std::string> out;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return out;
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof pe;
        std::set<std::string> alive;
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
            alive.insert(Lower(U(fs::path(pe.szExeFile))));
        CloseHandle(snap);
        for (const std::string& n : names)
            if (alive.count(Lower(n))) out.push_back(n);
        return out;
    }
};
#else
class SystemProbe : public ProcessProbe {
public:
    std::vector<std::string> Running(const std::vector<std::string>&) override { return {}; }
};
#endif

}  // namespace

std::unique_ptr<ProcessProbe> MakeSystemProbe() { return std::make_unique<SystemProbe>(); }

}  // namespace vrml
