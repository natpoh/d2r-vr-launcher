#include "versioninfo.h"

#include <cwchar>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace vrml {

std::vector<std::string> CommentTokens(const std::string& comments) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : comments) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ';' || c == ',') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string CommentValue(const std::string& comments, const std::string& key) {
    const std::string prefix = Lower(key) + "=";
    for (const std::string& t : CommentTokens(comments))
        if (StartsWith(Lower(t), prefix)) return t.substr(prefix.size());
    return "";
}

#ifdef _WIN32

namespace {

std::string ToUtf8(const wchar_t* w, size_t n) {
    if (!w || n == 0) return "";
    const int len = WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(n), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string s(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(n), &s[0], len, nullptr, nullptr);
    return s;
}

}  // namespace

FileVersion ReadFileVersion(const fs::path& file) {
    FileVersion v;
    const std::wstring path = file.wstring();
    if (path.empty()) return v;
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0 || size > (16u << 20)) return v;
    std::vector<char> block(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, block.data())) return v;

    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT fixed_len = 0;
    if (VerQueryValueW(block.data(), L"\\", reinterpret_cast<LPVOID*>(&fixed), &fixed_len) && fixed && fixed_len >= sizeof(VS_FIXEDFILEINFO) && fixed->dwSignature == 0xFEEF04BD) {
        v.has_fixed = true;
        v.product_numbers[0] = HIWORD(fixed->dwProductVersionMS);
        v.product_numbers[1] = LOWORD(fixed->dwProductVersionMS);
        v.product_numbers[2] = HIWORD(fixed->dwProductVersionLS);
        v.product_numbers[3] = LOWORD(fixed->dwProductVersionLS);
    }

    // the string tables the file declares, then the usual ones
    std::vector<std::wstring> tables;
    struct LangCodePage {
        WORD lang, codepage;
    };
    LangCodePage* tr = nullptr;
    UINT tr_len = 0;
    if (VerQueryValueW(block.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<LPVOID*>(&tr), &tr_len) && tr) {
        for (UINT i = 0; i < tr_len / sizeof(LangCodePage) && i < 8; ++i) {
            wchar_t name[48];
            swprintf(name, 48, L"\\StringFileInfo\\%04x%04x\\", tr[i].lang, tr[i].codepage);
            tables.push_back(name);
        }
    }
    for (const wchar_t* t : {L"\\StringFileInfo\\040904b0\\", L"\\StringFileInfo\\040904e4\\", L"\\StringFileInfo\\000004b0\\"}) tables.push_back(t);

    auto query = [&](const wchar_t* key) {
        for (const std::wstring& t : tables) {
            const std::wstring q = t + key;
            wchar_t* text = nullptr;
            UINT len = 0;
            if (VerQueryValueW(block.data(), q.c_str(), reinterpret_cast<LPVOID*>(&text), &len) && text && len > 0) {
                const size_t n = wcsnlen(text, len);
                if (n > 0) return ToUtf8(text, n);
            }
        }
        return std::string();
    };
    v.product_version = query(L"ProductVersion");
    v.file_version = query(L"FileVersion");
    v.comments = query(L"Comments");
    v.product_name = query(L"ProductName");
    v.company = query(L"CompanyName");
    v.ok = v.has_fixed || !v.product_version.empty() || !v.file_version.empty() || !v.comments.empty();
    return v;
}

#else

FileVersion ReadFileVersion(const fs::path&) { return FileVersion(); }

#endif

}  // namespace vrml
