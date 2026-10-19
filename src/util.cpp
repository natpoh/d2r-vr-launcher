#include "util.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <sstream>

#include "sha256.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace vrml {

void Throw(const std::string& code, const std::string& msg) {
    int exit = 1;
    if (code == "E_USAGE") exit = 2;
    else if (code == "E_NOT_FOUND") exit = 3;
    else if (code == "E_MANIFEST") exit = 4;
    else if (code == "E_GAME_RUNNING") exit = 10;
    else if (code == "E_PRECONDITION" || code == "E_HOOK") exit = 11;
    else if (code == "E_COLLISION" || code == "E_IN_USE" || code == "E_IDENTITY") exit = 12;
    else if (code == "E_SCHEMA") exit = 13;
    else if (code == "E_IO") exit = 20;
    else if (code == "E_ROLLBACK") exit = 21;
    else if (code == "E_JOURNAL") exit = 22;
    else if (code == "E_HASH") exit = 23;
    else if (code == "E_RUNTIME") exit = 24;
    else if (code == "E_GAME") exit = 25;
    else if (code == "E_ARCHIVE") exit = 26;
    else if (code == "E_UNVERIFIED") exit = 27;
    else if (code == "E_SETUP") exit = 28;
    else if (code == "E_UNTRUSTED") exit = 29;
    else if (code == "E_CANCELLED") exit = 30;
    else if (code == "E_UAC_DECLINED") exit = 31;
    else if (code == "E_FIRST_START") exit = 32;
    throw Error(code, exit, msg);
}

// ------------------------------------------------------------------ strings and paths

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool IEquals(const std::string& a, const std::string& b) { return Lower(a) == Lower(b); }
bool StartsWith(const std::string& s, const std::string& p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }
bool EndsWith(const std::string& s, const std::string& x) { return s.size() >= x.size() && s.compare(s.size() - x.size(), x.size(), x) == 0; }

std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

fs::path P(const std::string& utf8) { return fs::u8path(utf8); }
std::string U(const fs::path& p) { return p.generic_u8string(); }

std::string NormRel(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    std::vector<std::string> keep;
    for (const std::string& part : Split(s, '/')) {
        if (part.empty() || part == ".") continue;
        keep.push_back(part);
    }
    std::string out;
    for (size_t i = 0; i < keep.size(); ++i) {
        if (i) out += '/';
        out += keep[i];
    }
    // a directory spelled with a trailing slash stays one
    if (!out.empty() && !s.empty() && s.back() == '/') out += '/';
    return out;
}

bool SafeRel(const std::string& rel, std::string* why) {
    auto fail = [&](const char* m) { if (why) *why = m; return false; };
    if (rel.empty()) return fail("empty path");
    if (rel[0] == '/' || rel[0] == '\\') return fail("absolute path");
    if (rel.find(':') != std::string::npos) return fail("a drive or a stream (':') in the path");
    for (const std::string& part : Split(NormRel(rel), '/')) {
        if (part.empty()) continue;
        if (part == "..") return fail("'..' in the path");
        if (part.back() == '.' || part.back() == ' ') return fail("a name ending in a dot or a space");
        for (char c : part)
            if (c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || static_cast<unsigned char>(c) < 0x20)
                return fail("a character Windows does not allow in a name");
    }
    return true;
}

bool GlobMatch(const std::string& pattern, const std::string& name) {
    const std::string p = Lower(pattern), n = Lower(name);
    size_t pi = 0, ni = 0, star = std::string::npos, mark = 0;
    while (ni < n.size()) {
        if (pi < p.size() && (p[pi] == '?' || p[pi] == n[ni])) { ++pi; ++ni; }
        else if (pi < p.size() && p[pi] == '*') { star = pi++; mark = ni; }
        else if (star != std::string::npos) { pi = star + 1; ni = ++mark; }
        else return false;
    }
    while (pi < p.size() && p[pi] == '*') ++pi;
    return pi == p.size();
}

std::string GameKey(const fs::path& gameDir) {
    std::error_code ec;
    fs::path abs = fs::weakly_canonical(gameDir, ec);
    if (ec) abs = fs::absolute(gameDir).lexically_normal();
    std::string s = Lower(U(abs));
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return Sha256Hex(s).substr(0, 16);
}

// ------------------------------------------------------------------ files

bool Exists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(fs::symlink_status(p, ec));
}
bool IsFile(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}
bool IsDir(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}

bool IsReparsePoint(const fs::path& p) {
    std::error_code ec;
    if (fs::is_symlink(fs::symlink_status(p, ec))) return true;
#ifdef _WIN32
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    return false;
#endif
}

long long CreationTime(const fs::path& p) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return -1;
    ULARGE_INTEGER t;
    t.LowPart = d.ftCreationTime.dwLowDateTime;
    t.HighPart = d.ftCreationTime.dwHighDateTime;
    return static_cast<long long>(t.QuadPart);
#else
    (void)p;
    return -1;
#endif
}

bool ReadFile(const fs::path& p, std::string* out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    if (out) *out = ss.str();
    return true;
}

void EnsureDir(const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec && !IsDir(dir)) Throw("E_IO", "cannot create the folder " + U(dir) + ": " + ec.message());
}

void WriteFileAtomic(const fs::path& p, const std::string& data) {
    EnsureDir(p.parent_path());
    fs::path tmp = p;
    tmp += kTmpSuffix;
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) Throw("E_IO", "cannot write " + U(tmp));
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        f.flush();
        if (!f) Throw("E_IO", "cannot write " + U(tmp));
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);   // replaces an existing file
    if (ec) {
        RemoveQuiet(tmp);
        Throw("E_IO", "cannot replace " + U(p) + ": " + ec.message());
    }
}

static std::function<void(const fs::path&)> g_afterTmp;
void SetAfterTmpHookForTests(std::function<void(const fs::path&)> hook) { g_afterTmp = std::move(hook); }

void CopyAtomic(const fs::path& src, const fs::path& dst) {
    EnsureDir(dst.parent_path());
    fs::path tmp = dst;
    tmp += kTmpSuffix;
    std::error_code ec;
    fs::copy_file(src, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) Throw("E_IO", "cannot copy " + U(src) + " to " + U(dst) + ": " + ec.message());
    if (g_afterTmp) g_afterTmp(dst);
    fs::rename(tmp, dst, ec);
    if (ec) {
        RemoveQuiet(tmp);
        Throw("E_IO", "cannot place " + U(dst) + ": " + ec.message());
    }
}

void RemoveQuiet(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
}

// ------------------------------------------------------------------ time

static std::function<std::string()> g_clock;
static std::atomic<unsigned> g_txCounter{0};

void SetClockForTests(std::function<std::string()> iso) { g_clock = std::move(iso); }

std::string NowIso() {
    if (g_clock) return g_clock();
    std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string NewTxId() {
    std::string iso = NowIso();   // 2026-10-15T10:15:30Z
    std::string compact;
    for (char c : iso)
        if (c >= '0' && c <= '9') compact += c;
    if (compact.size() >= 14) compact.insert(8, "-");
    char n[16];
    std::snprintf(n, sizeof n, "-%04u", ++g_txCounter);
    return compact + n;
}

// ------------------------------------------------------------------ log

void Logger::Line(const std::string& msg) {
    std::string line = NowIso() + " " + msg;
    tail_.push_back(line);
    if (tail_.size() > 500) tail_.erase(tail_.begin(), tail_.begin() + 100);
    if (file_.empty()) return;
    std::error_code ec;
    fs::create_directories(file_.parent_path(), ec);
    std::ofstream f(file_, std::ios::app);
    if (f) f << line << "\n";
}

}  // namespace vrml
