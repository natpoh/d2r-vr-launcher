#include "game.h"

#include <fstream>
#include <sstream>

namespace vrml {

bool LooksLikeGame(const fs::path& gameDir) { return IsFile(gameDir / "D2R.exe"); }

std::string DetectStore(const fs::path& gameDir) {
    if (IsFile(gameDir / "Data" / ".build.config") || IsFile(gameDir / "data" / ".build.config")) return "steam";
    if (IsFile(gameDir / ".build.info")) return "battlenet";
    return "";
}

static std::string LogValue(const fs::path& log, const std::string& marker, char endChar) {
    std::ifstream f(log, std::ios::binary);
    if (!f) return "";
    std::string line, found;
    while (std::getline(f, line)) {
        size_t at = line.find(marker);
        if (at == std::string::npos) continue;
        std::string rest = line.substr(at + marker.size());
        size_t e = endChar ? rest.find(endChar) : rest.find_first_of(" \r\n");
        found = rest.substr(0, e);
        while (!found.empty() && (found.back() == '.' || found.back() == '\r')) found.pop_back();
    }
    return found;   // the last one wins
}

std::string DetectBuild(const fs::path& gameDir, const std::vector<std::string>& candidates) {
    std::string b = LogValue(gameDir / "d2rloader" / "logs" / "d2rloader.log", "D2R version: ", 0);
    if (!b.empty()) return b;
    std::string exe;
    if (candidates.empty() || !ReadFile(gameDir / "D2R.exe", &exe)) return "";
    for (const std::string& c : candidates) {
        std::string wide;
        for (char ch : c) { wide += ch; wide += '\0'; }
        if (exe.find(wide) != std::string::npos) return c;
    }
    return "";
}

std::string DetectLoaderVersion(const fs::path& gameDir) {
    std::string v = LogValue(gameDir / "d2rloader" / "logs" / "d2rloader.log", "D2RLoader v", ' ');
    return v;
}

bool TomlValue(const fs::path& file, const std::string& dottedKey, std::string* value) {
    std::ifstream f(file, std::ios::binary);
    if (!f) return false;
    size_t dot = dottedKey.rfind('.');
    const std::string wantSection = dot == std::string::npos ? "" : dottedKey.substr(0, dot);
    const std::string wantKey = dot == std::string::npos ? dottedKey : dottedKey.substr(dot + 1);
    std::string line, section;
    while (std::getline(f, line)) {
        size_t b = line.find_first_not_of(" \t\r");
        if (b == std::string::npos || line[b] == '#') continue;
        line = line.substr(b);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        if (line[0] == '[') {
            size_t e = line.find(']');
            section = e == std::string::npos ? "" : line.substr(1, e - 1);
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
        size_t vb = v.find_first_not_of(" \t");
        v = vb == std::string::npos ? "" : v.substr(vb);
        if (!v.empty() && v[0] == '"') {
            size_t e = v.find('"', 1);
            v = v.substr(1, e == std::string::npos ? std::string::npos : e - 1);
        } else {
            size_t c = v.find('#');
            if (c != std::string::npos) v = v.substr(0, c);
            while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
        }
        if (section == wantSection && k == wantKey) {
            if (value) *value = v;
            return true;
        }
    }
    return false;
}

}  // namespace vrml
