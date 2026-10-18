// deps.h - the shared dependency the launcher fetches for the mods (D2RLoader), and the checks that the files in the game folder
// are the ones a mod needs. The network itself is NOT here: a downloader is an interface (the prototype has none).
//
// Rules (from the two installers that exist today, which agree): a download is accepted only when its SHA-256 is one of the
// reviewed ones - an unknown hash stops everything before any game file is written; a verified file in the cache is reused
// by every mod; the files in the game folder are still checked against what the SELECTED mod needs.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "manifest.h"
#include "util.h"

namespace vrml {

struct Pin {
    std::string id;                       // "d2rloader"
    std::string version;                  // "1.3.1-beta"
    std::string url;
    std::string file;                     // the cache file name
    std::vector<std::string> sha256;      // every reviewed archive hash (current + historical)
};

// D2RLoader 1.3.1-beta as D2R VR's installer and settings program and d2r-pcvr's install.py all pin it. The author repacked the
// ZIP under the same name on 2026-10-13 (same payload bytes, new archive hash): both hashes are accepted, any other file is not.
const Pin& D2RLoaderPin();

bool HashAccepted(const std::vector<std::string>& accepted, const std::string& hex);   // case-insensitive

// How far a download is: bytes so far and in all (0 = not known). Returning false cancels it.
using ProgressFn = std::function<bool(uint64_t done, uint64_t total)>;

class Downloader {
public:
    virtual ~Downloader() = default;
    // Fetch url into `dest` (any partial file is the downloader's to clean). false + *error on failure.
    virtual bool Fetch(const std::string& url, const fs::path& dest, std::string* error) = 0;
    // Optional: report progress of the next Fetch calls (a downloader that has none ignores it); cancelling makes Fetch fail with WasCancelled().
    virtual void SetProgress(ProgressFn) {}
    virtual bool WasCancelled() const { return false; }
};

class DepsCache {
public:
    DepsCache(fs::path home, Downloader* dl, Logger* log) : home_(std::move(home)), dl_(dl), log_(log) {}
    fs::path PathOf(const Pin& pin) const { return home_ / "deps" / P(pin.id) / P(pin.file); }
    Downloader* downloader() const { return dl_; }
    // The verified archive: a cached file whose hash is accepted is reused (no download); a cached file with another hash is
    // deleted and fetched again; a download with another hash is deleted and E_HASH is thrown (nothing else is touched).
    // Without a downloader and without a good cache: E_NOT_FOUND. A cancelled download: E_CANCELLED.
    fs::path Ensure(const Pin& pin);

private:
    fs::path home_;
    Downloader* dl_;
    Logger* log_;
};

// ---- is the game folder what a mod needs
struct RequirementResult {
    std::string id;
    bool optional = false;
    bool ok = true;
    std::vector<std::string> problems;
};
// Checks every requirement's files (presence, and the hash when the manifest lists some) in `game`.
std::vector<RequirementResult> CheckRequirements(const Manifest& m, const fs::path& game);

}  // namespace vrml
