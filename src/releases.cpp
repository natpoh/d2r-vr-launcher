#include "releases.h"

#include <algorithm>
#include <cctype>

#include "json.h"
#include "manifest.h"
#include "sha256.h"

namespace vrml {

namespace {

constexpr size_t kMaxNotes = 20000;
constexpr uint64_t kMaxJson = 4ull << 20;           // GitHub's answer for 5 releases is about 50 KB; 4 MB is a limit, not a size

const char* const kAuthenticRepo = "natpoh/diablo2-vr-port";
const char* const kUnleashedRepo = "bearstonem/d2r-pcvr";

// the repository whose release assets a mod's installer may be; "" = none
std::string GithubRepoFor(const std::string& id) {
    if (id == "d2r-vr") return kAuthenticRepo;
    if (id == "d2r-pcvr") return kUnleashedRepo;
    return "";
}

std::string Cut(const std::string& s) { return s.size() <= kMaxNotes ? s : s.substr(0, kMaxNotes) + "\n(cut)"; }

bool SafeInstallerName(const std::string& n) {
    if (n.size() < 5 || n.size() > 120 || !EndsWith(Lower(n), ".exe")) return false;
    for (char c : n)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-')) return false;
    return n[0] != '.' && n.find("..") == std::string::npos;
}

std::string LastSegment(const std::string& url) {
    const UrlParts u = ParseUrl(url);
    std::string path = u.ok ? u.path : url;
    path = path.substr(0, path.find('?'));
    return path.substr(path.find_last_of('/') + 1);
}

Json ParseOrThrow(const std::string& text, const char* what) {
    try {
        return Json::Parse(text);
    } catch (const JsonError& e) {
        Throw("E_IO", std::string("the answer about ") + what + " is not readable (" + e.what() + ")");
    }
}

// "D2R-PCVR-Setup-0.3.19.exe" with pre "D2R-PCVR-Setup-" -> "0.3.19"; "D2R_VR_Setup_v0.188.0.exe" with "D2R_VR_Setup_v" -> "0.188.0"
bool SetupAssetVersion(const std::string& name, const std::string& pre, std::string* ver) {
    const std::string post = ".exe";
    if (!StartsWith(name, pre) || !EndsWith(name, post) || name.size() <= pre.size() + post.size()) return false;
    const std::string mid = name.substr(pre.size(), name.size() - pre.size() - post.size());
    bool lastDot = true;
    for (char c : mid) {
        if (c == '.') {
            if (lastDot) return false;
            lastDot = true;
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            lastDot = false;
        } else {
            return false;
        }
    }
    if (lastDot) return false;
    *ver = mid;
    return true;
}

bool ParseHashFile(const std::string& body, const std::string& assetName, std::string* hex, std::string* problem) {
    // "<hex>  <name>" (sha256sum text mode "<hex> *<name>" too), possibly with a trailing newline
    std::string line = body.substr(0, body.find_first_of("\r\n"));
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    size_t j = i;
    while (j < line.size() && std::isxdigit(static_cast<unsigned char>(line[j]))) ++j;
    const std::string h = line.substr(i, j - i);
    if (!IsSha256Hex(h)) {
        *problem = "the .sha256 file does not start with a SHA-256";
        return false;
    }
    std::string rest = line.substr(j);
    while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t' || rest.front() == '*')) rest.erase(rest.begin());
    while (!rest.empty() && (rest.back() == ' ' || rest.back() == '\t')) rest.pop_back();
    if (!rest.empty() && rest != assetName) {
        *problem = "the .sha256 file is for \"" + rest + "\", not for \"" + assetName + "\"";
        return false;
    }
    *hex = Lower(h);
    return true;
}

std::string LowerHex(const std::string& s) { return Lower(s); }

// "v*" against "v0.188.0": '*' is any run of characters; an empty pattern matches every tag
bool TagMatches(const std::string& tag, const std::string& glob) {
    if (glob.empty()) return true;
    size_t t = 0, g = 0, star = std::string::npos, mark = 0;
    while (t < tag.size()) {
        if (g < glob.size() && glob[g] == '*') {
            star = g++;
            mark = t;
        } else if (g < glob.size() && glob[g] == tag[t]) {
            ++g;
            ++t;
        } else if (star != std::string::npos) {
            g = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (g < glob.size() && glob[g] == '*') ++g;
    return g == glob.size();
}

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

// An ini value read as GetPrivateProfileInt reads it (what D2R VR Authentic Settings does): its leading digits, anything after them ignored;
// 0 when the key is not there or has no number.
long IniInt(const std::string& text, const std::string& section, const std::string& key) {
    std::string cur;
    size_t at = StartsWith(text, "\xEF\xBB\xBF") ? 3 : 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const std::string line = Trim(text.substr(at, end - at));
        at = end + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            const size_t close = line.find(']');
            cur = close == std::string::npos ? std::string() : Lower(Trim(line.substr(1, close - 1)));
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos || cur != Lower(section) || Lower(Trim(line.substr(0, eq))) != Lower(key)) continue;
        const std::string v = Trim(line.substr(eq + 1));
        long n = 0;
        size_t d = !v.empty() && (v[0] == '-' || v[0] == '+') ? 1 : 0;
        for (; d < v.size() && std::isdigit(static_cast<unsigned char>(v[d])) && n < 100000000; ++d) n = n * 10 + (v[d] - '0');
        return !v.empty() && v[0] == '-' ? -n : n;
    }
    return 0;
}

}  // namespace

ReleaseSources::ReleaseSources() : test_http_host(TestHttpHostFromProcessEnv()) {}

std::string NormalizeVersion(const std::string& v) {
    size_t i = 0;
    while (i < v.size() && !std::isdigit(static_cast<unsigned char>(v[i]))) {
        if (i > 0 || !(v[i] == 'v' || v[i] == 'V')) return "";
        ++i;
    }
    std::vector<std::string> parts;
    std::string cur;
    for (; i < v.size(); ++i) {
        const char c = v[i];
        if (std::isdigit(static_cast<unsigned char>(c))) cur += c;
        else if (c == '.' && !cur.empty()) { parts.push_back(cur); cur.clear(); }
        else break;
    }
    if (!cur.empty()) parts.push_back(cur);
    if (parts.empty()) return "";
    for (std::string& p : parts) {                       // "0168" -> "168"; a part is a number, not text
        while (p.size() > 1 && p[0] == '0') p.erase(p.begin());
    }
    while (parts.size() < 3) parts.push_back("0");
    std::string out;
    for (size_t k = 0; k < parts.size(); ++k) out += (k ? "." : "") + parts[k];
    return out;
}

bool IsNewer(const std::string& a, const std::string& b) {
    const std::string na = NormalizeVersion(a), nb = NormalizeVersion(b);
    if (na.empty()) return false;
    if (nb.empty()) return true;
    return CompareVersions(na, nb) > 0;
}

std::vector<std::string> DownloadHostsFor(const std::string& id) {
    if (id == "d2r-vr") return {"bodywalkvr.com", "github.com", "*.githubusercontent.com"};
    if (id == "d2r-pcvr") return {"github.com", "*.githubusercontent.com"};
    return {};
}

std::vector<std::string> DownloadHostsFor(const Release& r) {
    // Authentic is in both places: an installer on bodywalkvr.com stays there, one on GitHub may be redirected to GitHub's file host only
    if (r.id == "d2r-vr") {
        if (ParseUrl(r.url).host == "bodywalkvr.com") return {"bodywalkvr.com"};
        return {"github.com", "*.githubusercontent.com"};
    }
    return DownloadHostsFor(r.id);
}

bool ReleaseUrlOk(const Release& r, const std::string& testHttpHost, std::string* why) {
    auto no = [&](const std::string& m) {
        if (why) *why = m;
        return false;
    };
    if (r.url.empty()) return no("there is no installer address");
    std::string w;
    if (!UrlAllowed(r.url, DownloadHostsFor(r), testHttpHost, &w)) return no(w);
    if (!SafeInstallerName(r.asset_name)) return no("\"" + r.asset_name + "\" is not a plain installer file name");
    const UrlParts u = ParseUrl(r.url);
    // a GitHub address is on github.com itself (the CDN hosts are only where GitHub may redirect it to) and inside the mod's own repository's releases
    const std::string repo = GithubRepoFor(r.id);
    const bool onOurSite = r.id == "d2r-vr" && u.host == "bodywalkvr.com";
    if (!repo.empty() && !onOurSite && u.scheme == "https" && (u.host != "github.com" || !StartsWith(u.path, "/" + repo + "/releases/download/")))
        return no("the installer is not a release asset of github.com/" + repo);
    return true;
}

// ------------------------------------------------------------------ Authentic

Release ParseAuthenticAnswer(const std::string& text, const std::string& current, const ReleaseSources& src) {
    const Json j = ParseOrThrow(text, "the newest D2R VR Authentic");
    if (!j.is_object()) Throw("E_IO", "the answer about the newest D2R VR Authentic is not what was expected");
    Release r;
    r.id = "d2r-vr";
    r.source = "bodywalkvr.com";
    r.current = NormalizeVersion(current);
    r.notes = Cut(j.get_string("changelog"));
    const std::string ver = j.get_string("version");
    const std::string nv = NormalizeVersion(ver);
    if (ver.empty() || nv.empty()) Throw("E_IO", "the answer about the newest D2R VR Authentic has no version");
    if (nv == "0.0.0" || !IsNewer(nv, current)) {
        r.up_to_date = true;
        r.version = r.current;
        if (nv != "0.0.0") r.announced = nv;            // the site's newest is older than (or the same as) what is installed: remembered, so the window can say "you have a newer version"
        return r;
    }
    r.announced = nv;
    r.version = nv;
    r.url = j.get_string("download_url");
    std::string why;
    if (r.url.empty()) Throw("E_IO", "the answer about the newest D2R VR Authentic has no download address");
    if (!UrlAllowed(r.url, {"bodywalkvr.com"}, src.test_http_host, &why))
        Throw("E_UNTRUSTED", "the download address D2R VR Authentic's server gave is refused: " + why);
    r.asset_name = LastSegment(r.url);
    if (!SafeInstallerName(r.asset_name)) Throw("E_UNTRUSTED", "the download address D2R VR Authentic's server gave does not end in a plain installer name (\"" + r.asset_name + "\")");
    if (const Json* h = j.find("sha256")) {
        if (!h->is_string() || !IsSha256Hex(h->as_string())) Throw("E_UNTRUSTED", "the SHA-256 in D2R VR Authentic's answer is not a SHA-256; refusing it");
        r.sha256 = LowerHex(h->as_string());
        r.sha256_source = "published";
    }
    if (const Json* s = j.find("size")) {
        if (!s->is_integer() || s->as_int() <= 0) Throw("E_UNTRUSTED", "the size in D2R VR Authentic's answer is not a number of bytes; refusing it");
        r.size = static_cast<uint64_t>(s->as_int());
    }
    return r;
}

Release LatestAuthenticSite(HttpClient& http, const std::string& current, const ReleaseSources& src) {
    const std::string v = NormalizeVersion(current).empty() ? "0.0.0" : NormalizeVersion(current);
    const std::string url = src.authentic_api + (src.authentic_api.find('?') == std::string::npos ? "?" : "&") + "product=d2r_vr&v=" + v;
    std::string body, err;
    if (!http.GetText(url, &body, &err)) Throw("E_IO", "could not ask bodywalkvr.com for the newest D2R VR Authentic: " + err);
    return ParseAuthenticAnswer(body, current, src);
}

Release LatestAuthentic(HttpClient& http, const std::string& current, const ReleaseSources& src) {
    // GitHub first; bodywalkvr.com when GitHub cannot be asked or lists no installer for this channel (yet). An answer GitHub gave and the
    // launcher refused (E_UNTRUSTED, E_HASH) is reported as it is: the site does not paper over it.
    std::string githubProblem, body, err;
    if (src.authentic_github_api.empty()) {
        githubProblem = "not asked";
    } else if (!http.GetText(src.authentic_github_api, &body, &err)) {
        githubProblem = "could not ask GitHub (" + err + ")";
    } else {
        try {
            return ParseGithubReleases(body, AuthenticGithub(src.develop), current, &http, src);
        } catch (const Error& e) {
            if (e.code != "E_NOT_FOUND" && e.code != "E_IO") throw;
            githubProblem = e.what();
        }
    }
    Release r;
    try {
        r = LatestAuthenticSite(http, current, src);
    } catch (const Error& e) {
        Throw(e.code, std::string(e.what()) + "; GitHub: " + githubProblem);
    }
    if (!src.authentic_github_api.empty()) r.warnings.push_back("GitHub: " + githubProblem + "; bodywalkvr.com's answer is used");
    return r;
}

bool AuthenticDevelopWanted(const fs::path& game) {
    if (game.empty()) return false;
    const fs::path inis[] = {game / "mods" / "D2R-VR" / "d2rloader" / "plugins" / "d2r_vr.ini", game / "d2rloader" / "plugins" / "d2r_vr.ini"};
    for (const fs::path& ini : inis) {
        std::string text;
        if (IsFile(ini) && ReadFile(ini, &text)) return IniInt(text, "update", "develop") != 0;
    }
    return false;
}

// ------------------------------------------------------------------ GitHub releases (Unleashed; Authentic since 0.3.7)

GithubSource AuthenticGithub(bool develop) {
    GithubSource g;
    g.id = "d2r-vr";
    g.name = "D2R VR Authentic";
    g.repo = kAuthenticRepo;
    g.asset_prefix = "D2R_VR_Setup_v";
    g.tag_glob = "v*";
    g.prereleases = develop;
    g.require_hash = true;
    return g;
}

GithubSource UnleashedGithub() {
    GithubSource g;
    g.id = "d2r-pcvr";
    g.name = "D2R VR Unleashed";
    g.repo = kUnleashedRepo;
    g.asset_prefix = "D2R-PCVR-Setup-";
    return g;
}

Release ParseGithubReleases(const std::string& text, const GithubSource& g, const std::string& current, HttpClient* http, const ReleaseSources& src) {
    const Json root = ParseOrThrow(text, ("the newest " + g.name).c_str());
    if (!root.is_array()) Throw("E_IO", "GitHub's answer about the newest " + g.name + " is not a list of releases");
    struct Cand { const Json* rel; const Json* asset; std::string ver; std::string published; };
    std::vector<Cand> cands;
    bool skippedPre = false;
    for (const Json& rel : root.items()) {
        if (!rel.is_object() || rel.get_bool("draft")) continue;
        if (!TagMatches(rel.get_string("tag_name"), g.tag_glob)) continue;
        const Json* assets = rel.find("assets");
        if (!assets || !assets->is_array()) continue;
        for (const Json& a : assets->items()) {
            std::string ver;
            if (!a.is_object() || !SetupAssetVersion(a.get_string("name"), g.asset_prefix, &ver)) continue;
            const std::string state = a.get_string("state", "uploaded");
            if (state != "uploaded") continue;
            if (!g.prereleases && rel.get_bool("prerelease")) {     // a develop build, and the player did not ask for those
                skippedPre = true;
                continue;
            }
            cands.push_back({&rel, &a, NormalizeVersion(ver), rel.get_string("published_at")});
        }
    }
    if (cands.empty())
        Throw("E_NOT_FOUND", "no release of " + g.name + " with an installer (" + g.asset_prefix + "<version>.exe) was found" +
                                 (skippedPre ? " (its pre-releases are develop builds: they count only when develop builds are asked for)" : ""));
    const Cand* best = &cands[0];
    for (const Cand& c : cands) {
        const int cmp = CompareVersions(c.ver, best->ver);
        if (cmp > 0 || (cmp == 0 && c.published > best->published)) best = &c;
    }

    Release r;
    r.id = g.id;
    r.source = "github.com/" + g.repo;
    r.current = NormalizeVersion(current);
    r.version = best->ver;
    r.announced = best->ver;
    r.tag = best->rel->get_string("tag_name");
    r.prerelease = best->rel->get_bool("prerelease");
    r.published = best->published;
    r.notes = Cut(best->rel->get_string("body"));
    r.asset_name = best->asset->get_string("name");
    r.url = best->asset->get_string("browser_download_url");
    r.size = best->asset->get_int("size") > 0 ? static_cast<uint64_t>(best->asset->get_int("size")) : 0;
    r.up_to_date = !IsNewer(r.version, current);
    {
        Release probe = r;
        std::string why;
        if (!ReleaseUrlOk(probe, src.test_http_host, &why)) Throw("E_UNTRUSTED", "the installer address in GitHub's answer is refused: " + why);
    }

    // the checksum: GitHub's own `digest` of the asset, and/or the .sha256 file published next to it
    std::string apiHash;
    const std::string digest = best->asset->get_string("digest");
    if (StartsWith(Lower(digest), "sha256:")) {
        const std::string h = digest.substr(7);
        if (!IsSha256Hex(h)) Throw("E_UNTRUSTED", "the digest GitHub lists for " + r.asset_name + " is not a SHA-256; refusing it");
        apiHash = Lower(h);
    }
    std::string fileHash;
    const Json* assets = best->rel->find("assets");
    for (const Json& a : assets->items()) {
        if (a.get_string("name") != r.asset_name + ".sha256") continue;
        const std::string url = a.get_string("browser_download_url");
        Release probe;
        probe.id = g.id;
        probe.url = url;
        probe.asset_name = "x.exe";
        std::string why;
        if (!ReleaseUrlOk(probe, src.test_http_host, &why)) {
            r.warnings.push_back("the .sha256 address is refused (" + why + "); not used");
            break;
        }
        if (!http) break;
        std::string body, err, problem;
        if (!http->GetText(url, &body, &err)) {
            r.warnings.push_back("the .sha256 file could not be fetched (" + err + ")" + (apiHash.empty() ? "" : "; GitHub's digest alone is used"));
        } else if (!ParseHashFile(body, r.asset_name, &fileHash, &problem)) {
            r.warnings.push_back(problem + (apiHash.empty() ? "" : "; GitHub's digest alone is used"));
        }
        break;
    }
    if (!apiHash.empty() && !fileHash.empty()) {
        if (apiHash != fileHash)
            Throw("E_HASH", "GitHub's digest for " + r.asset_name + " (" + apiHash + ") and the published .sha256 file (" + fileHash + ") disagree; refusing this release.");
        r.sha256 = apiHash;
        r.sha256_source = "digest + .sha256";
    } else if (!apiHash.empty()) {
        r.sha256 = apiHash;
        r.sha256_source = "digest";
    } else if (!fileHash.empty()) {
        r.sha256 = fileHash;
        r.sha256_source = ".sha256 file";
    } else if (g.require_hash) {
        Throw("E_UNTRUSTED", "GitHub lists no SHA-256 for " + r.asset_name + " (no digest, no usable .sha256 file); refusing this release");
    }
    return r;
}

Release ParseUnleashedAnswer(const std::string& text, const std::string& current, HttpClient* http, const ReleaseSources& src) {
    return ParseGithubReleases(text, UnleashedGithub(), current, http, src);
}

Release LatestUnleashed(HttpClient& http, const std::string& current, const ReleaseSources& src) {
    std::string body, err;
    if (!http.GetText(src.unleashed_api, &body, &err)) Throw("E_IO", "could not ask GitHub for the newest D2R VR Unleashed: " + err);
    return ParseUnleashedAnswer(body, current, &http, src);
}

// ------------------------------------------------------------------ what is installed, and the two together

std::string InstalledVersion(const fs::path& game, const std::string& id) {
    for (const ModEntry& e : DiscoverMods(game))
        if (e.id == id && e.installed) return NormalizeVersion(e.found_version);
    return "";
}

UpdateInfo CheckUpdate(HttpClient& http, const fs::path& game, const std::string& id, const ReleaseSources& src) {
    UpdateInfo u;
    u.id = id;
    u.name = id == "d2r-vr" ? "D2R VR Authentic" : id == "d2r-pcvr" ? "D2R VR Unleashed" : id;
    if (!game.empty() && IsDir(game)) {
        for (const ModEntry& e : DiscoverMods(game))
            if (e.id == id) {
                u.installed = e.installed;
                u.installed_version = e.installed ? NormalizeVersion(e.found_version) : "";
            }
    }
    ReleaseSources s = src;
    if (id == "d2r-vr" && AuthenticDevelopWanted(game)) s.develop = true;
    try {
        if (id == "d2r-vr") u.latest = LatestAuthentic(http, u.installed_version, s);
        else if (id == "d2r-pcvr") u.latest = LatestUnleashed(http, u.installed_version, s);
        else Throw("E_USAGE", "unknown mod \"" + id + "\"");
        u.checked = true;
        u.update_available = u.installed && !u.latest.up_to_date && (u.installed_version.empty() || IsNewer(u.latest.version, u.installed_version));
        u.can_install = !u.installed && !u.latest.url.empty();
    } catch (const Error& e) {
        u.error = e.what();
    }
    return u;
}

}  // namespace vrml
