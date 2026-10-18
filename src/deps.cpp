#include "deps.h"

#include "sha256.h"

namespace vrml {

const Pin& D2RLoaderPin() {
    static const Pin pin = {
        "d2rloader",
        "1.3.1-beta",
        "https://d2rloader.net/downloads/D2RLoader-1.3.1-beta.zip",
        "D2RLoader-1.3.1-beta.zip",
        // installer/D2R_VR_Setup.iss (D2RLoaderSha256, D2RLoaderSha256Historical), tools/settings/settings.cpp (kD2RLoaderSha256*),
        // d2r-pcvr steamvr/install.py (LOADER_HASHES)
        {"f7bf0287398890b09bcc68b576327e727304bd88f7be80160f9676e8bd6871e2", "9286c6b5bff7f1043658411dbea305faa9698b6a455e6441668fc6d02c22aad7"},
    };
    return pin;
}

bool HashAccepted(const std::vector<std::string>& accepted, const std::string& hex) {
    if (!IsSha256Hex(hex)) return false;
    for (const std::string& a : accepted)
        if (IEquals(a, hex)) return true;
    return false;
}

fs::path DepsCache::Ensure(const Pin& pin) {
    const fs::path file = PathOf(pin);
    std::string h;
    if (IsFile(file)) {
        if (Sha256File(file, &h) && HashAccepted(pin.sha256, h)) {
            if (log_) log_->Line("deps: " + pin.file + " reused from the cache (hash ok)");
            return file;
        }
        if (log_) log_->Line("deps: the cached " + pin.file + " has another hash; deleted");
        RemoveQuiet(file);
    }
    if (!dl_) Throw("E_NOT_FOUND", pin.file + " is not in the cache and no downloader is available");
    EnsureDir(file.parent_path());
    fs::path tmp = file;
    tmp += ".part";
    RemoveQuiet(tmp);
    std::string err;
    if (!dl_->Fetch(pin.url, tmp, &err)) {
        RemoveQuiet(tmp);
        if (dl_->WasCancelled()) Throw("E_CANCELLED", "the download of " + pin.file + " was cancelled");
        Throw("E_IO", "could not download " + pin.url + ": " + err);
    }
    if (!Sha256File(tmp, &h) || !HashAccepted(pin.sha256, h)) {
        RemoveQuiet(tmp);
        Throw("E_HASH", "the file downloaded from " + pin.url + " is not the " + pin.id + " " + pin.version + " archive that was reviewed (SHA-256 " +
                            (h.empty() ? std::string("unreadable") : h) + "); nothing was installed");
    }
    std::error_code ec;
    fs::rename(tmp, file, ec);
    if (ec) Throw("E_IO", "cannot place " + U(file) + ": " + ec.message());
    if (log_) log_->Line("deps: " + pin.file + " downloaded and verified");
    return file;
}

std::vector<RequirementResult> CheckRequirements(const Manifest& m, const fs::path& game) {
    std::vector<RequirementResult> out;
    for (const Requirement& r : m.requires_) {
        RequirementResult res;
        res.id = r.id;
        res.optional = r.optional;
        for (const RequiredFile& f : r.files) {
            const std::string rel = GameRel(f.path);
            const fs::path file = game / P(rel);
            if (!IsFile(file)) {
                res.ok = false;
                res.problems.push_back(f.path + " is missing");
                continue;
            }
            if (f.sha256_any_of.empty()) continue;
            std::string h;
            Sha256File(file, &h);
            if (!HashAccepted(f.sha256_any_of, h)) {
                res.ok = false;
                res.problems.push_back(f.path + " is not the file " + m.name + " needs (SHA-256 " + h + ")");
            }
        }
        out.push_back(res);
    }
    return out;
}

}  // namespace vrml
