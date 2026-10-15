// registry.h - the mods the launcher knows: <home>/mods/<id>/manifest.json (the package a mod's installer put there) and
// <home>/registry.json (what was registered, when, which version replaced which).
//
// The contract for an installer:  1. put the package in <home>/mods/<id>/   2. run  vrmodlauncher register <that manifest>.
// Register validates the manifest and the package (every src is there, listed hashes match, no links) and only then records it.
#pragma once

#include <string>
#include <vector>

#include "manifest.h"
#include "util.h"

namespace vrml {

struct ModRecord {
    std::string id, version, previous_version, registered_at;
};

class Registry {
public:
    Registry(fs::path home, Logger* log) : home_(std::move(home)), log_(log) {}

    const fs::path& home() const { return home_; }
    fs::path ModsDir() const { return home_ / "mods"; }
    fs::path ModDir(const std::string& id) const { return ModsDir() / P(id); }
    fs::path GamesDir() const { return home_ / "games"; }

    std::vector<ModRecord> List() const;
    bool Has(const std::string& id) const;
    // The registered mod's manifest, validated again (E_NOT_FOUND, E_MANIFEST).
    Manifest Load(const std::string& id) const;
    // E_MANIFEST with the issues when the manifest or the package is wrong; E_USAGE when the manifest is not in <home>/mods/<id>/.
    ModRecord Register(const fs::path& manifestFile);
    // Drops the record only; the package folder is the installer's (or `unregister --purge`'s).
    void Unregister(const std::string& id);
    // Folders in mods/ that hold a manifest.json nobody registered (an installer that skipped the last step).
    std::vector<std::string> Unregistered() const;

private:
    void Save(const std::vector<ModRecord>& recs) const;
    fs::path home_;
    Logger* log_;
};

}  // namespace vrml
