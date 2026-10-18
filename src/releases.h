// releases.h - what the newest D2R VR Authentic and D2R VR Unleashed are, and where their installers are. Read-only: asks two web services
// for a few kilobytes of JSON; nothing is downloaded or installed here.
//
//   Authentic (ours)   GET https://bodywalkvr.com/api/version?product=d2r_vr&v=<installed version>
//                      -> {"version","download_url","changelog"[, "sha256", "size"]}; "version":"0.0.0" means "nothing newer than v".
//                      The download_url must be https on bodywalkvr.com (the Settings program refuses anything else too). sha256 and size are
//                      optional today: when present they are enforced, when absent the installer can only be installed on the player's say-so.
//   Unleashed (Bear)   GET https://api.github.com/repos/bearstonem/d2r-pcvr/releases?per_page=5   (all are pre-releases: no /releases/latest)
//                      -> the newest non-draft release with an asset D2R-PCVR-Setup-<version>.exe; its checksum is the asset's `digest`
//                      ("sha256:<hex>") and/or the tiny D2R-PCVR-Setup-<version>.exe.sha256 asset; when both exist they must agree.
//                      The asset must live in github.com/bearstonem/d2r-pcvr/releases/download/.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "mods.h"
#include "net.h"
#include "util.h"

namespace vrml {

struct Release {
    std::string id;             // "d2r-vr" (Authentic) | "d2r-pcvr" (Unleashed)
    std::string version;        // normalised: "0.168.0", "0.3.19"
    std::string tag;            // GitHub's tag ("v0.3.19-windows-preview"); "" for Authentic
    std::string url;            // the installer
    std::string asset_name;     // its file name ("D2R_VR_Setup_v0.168.0.exe")
    std::string sha256;         // lower case; "" = the source publishes none
    std::string sha256_source;  // "" | "api" | "digest" | "digest + .sha256" | "published"  (where sha256 comes from)
    uint64_t size = 0;          // bytes; 0 = not published
    bool prerelease = false;
    bool up_to_date = false;    // Authentic: the server says nothing newer than the installed version (then url/asset are empty)
    std::string current;        // the installed version that was asked about
    std::string notes;          // the changelog / release text (cut at 20 KB)
    std::string published;      // ISO time, or ""
    std::string source;         // "bodywalkvr.com" | "github.com/bearstonem/d2r-pcvr"
    std::vector<std::string> warnings;     // e.g. "the .sha256 file could not be fetched"
};

// "v0.3.19-windows-preview" -> "0.3.19"; "0.168" -> "0.168.0"; "" when there is no number in it.
std::string NormalizeVersion(const std::string& v);
// a is newer than b ("0.168" == "0.168.0"; an empty b is older than anything, an empty a is not newer than anything).
bool IsNewer(const std::string& a, const std::string& b);

struct ReleaseSources {
    std::string authentic_api = "https://bodywalkvr.com/api/version";     // ?product=d2r_vr&v=<current> is added
    std::string unleashed_api = "https://api.github.com/repos/bearstonem/d2r-pcvr/releases?per_page=5";
    std::string test_http_host;                                           // the loopback test switch (net.h); "" = https only
    ReleaseSources();
};

// Authentic: `current` is what the player has ("" = nothing: asks for the newest). Errors: E_IO (cannot ask / unreadable answer),
// E_UNTRUSTED (the answer names a server the launcher does not download from).
Release LatestAuthentic(HttpClient& http, const std::string& current, const ReleaseSources& src = ReleaseSources());
// Unleashed. Errors: E_IO, E_NOT_FOUND (no release with an installer), E_UNTRUSTED, E_HASH (digest and .sha256 disagree).
Release LatestUnleashed(HttpClient& http, const std::string& current, const ReleaseSources& src = ReleaseSources());

// The two parsers, for tests and for callers that fetched the text themselves.
Release ParseAuthenticAnswer(const std::string& json, const std::string& current, const ReleaseSources& src = ReleaseSources());
// Needs `http` only for the small .sha256 asset (may be null: then only the digest counts).
Release ParseUnleashedAnswer(const std::string& json, const std::string& current, HttpClient* http, const ReleaseSources& src = ReleaseSources());

// May this release be downloaded: https on the host its source is allowed to use (or the test host), a plain .exe name. *why says what is wrong.
bool ReleaseUrlOk(const Release& r, const std::string& testHttpHost, std::string* why);
// The hosts a download of this release may use, redirects included.
std::vector<std::string> DownloadHostsFor(const std::string& id);

// What is installed: the `version` of mods\<folder>\vrmod-manifest.json (not the built-in profile's), "" when not installed or unknown.
std::string InstalledVersion(const fs::path& game, const std::string& id);

struct UpdateInfo {
    std::string id, name;
    bool installed = false;
    std::string installed_version;      // "" = unknown
    bool checked = false;               // false: the question could not be asked (error says why)
    std::string error;
    Release latest;
    bool update_available = false;      // installed and the latest is newer (or the installed version is unknown)
    bool can_install = false;           // not installed and a release is known
};
UpdateInfo CheckUpdate(HttpClient& http, const fs::path& game, const std::string& id, const ReleaseSources& src = ReleaseSources());

}  // namespace vrml
