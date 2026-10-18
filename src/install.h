// install.h - putting D2RLoader into a game folder (the one thing the launcher installs itself; the two mods bring their own Setup, see setup_run.h).
//
// The order, and why:
//   1. refuse unless the folder has D2R.exe (E_GAME) and neither D2R.exe nor D2RLoader.exe runs (E_GAME_RUNNING);
//   2. the three reviewed files (D2RLoader.exe, D2RCore.dll, d2rloader.mpq) already there -> "already installed", nothing downloaded or written;
//   3. the archive: from the cache when its hash is one of the reviewed ones, else downloaded and checked (a wrong hash deletes it: E_HASH);
//   4. unpacked into a temporary folder INSIDE the game folder (same disk: the last step is renames), only names the D2RLoader archive may
//      hold (the top-level names d2r-pcvr's install.py accepts: mods, d2rloader, d2rloader.exe, d2rcore.dll, d2rloader.mpq);
//   5. the three unpacked files must hash to the reviewed ones (the manifests' requires[d2rloader].files) - otherwise E_HASH and NOTHING
//      has been written to the game folder;
//   6. every file it would REPLACE is first copied to <backupDir>\d2rloader-<stamp>\...; then the new files are moved in. A file under
//      d2rloader\config\ or mods\ that already exists is the player's and is never replaced (d2rloader.toml is written only when absent);
//   7. if a move fails half way the files already moved are put back from the backup and the new ones removed: the folder is as it was.
// A folder that cannot be written (Program Files, ...) is reported as "administrator rights are needed for this folder" (E_IO).
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "deps.h"
#include "process.h"
#include "util.h"

namespace vrml {

struct ExpectedFile {
    std::string rel;                            // game-relative, '/': "D2RLoader.exe"
    std::vector<std::string> sha256_any_of;     // the reviewed hashes of the UNPACKED file
};
// The three reviewed 1.3.1-beta files, from the built-in manifest (requires[d2rloader].files).
std::vector<ExpectedFile> LoaderExpectedFiles();

// stage: "download" (bytes done / total), "unpack", "verify", "install" (files done / total). Returning false cancels (before the first
// file is moved in; later it is ignored): E_CANCELLED.
using InstallProgress = std::function<bool(const std::string& stage, uint64_t done, uint64_t total)>;

struct InstallResult {
    bool already_installed = false;
    std::vector<std::string> written;           // game-relative, '/'
    std::vector<std::string> kept;              // already there and the player's: not replaced
    std::vector<std::string> unchanged;         // already there with the same content
    std::vector<std::string> backed_up;         // what was copied to backup_dir before being replaced
    fs::path backup_dir;                        // "" when nothing was replaced
    bool downloaded = false;                    // false: the archive came from the cache (or nothing was needed)
    std::vector<std::string> notes;             // for the player
};

// Errors: E_GAME, E_GAME_RUNNING, E_HASH (the archive, or the unpacked files), E_ARCHIVE (damaged or unsafe ZIP), E_IO (download, disk, rights),
// E_CANCELLED. `expected` null = LoaderExpectedFiles(). `backupDir` is the folder the d2rloader-<stamp> folder goes into.
InstallResult InstallD2RLoader(const fs::path& game, const Pin& pin, DepsCache& cache, ProcessProbe& probe, const InstallProgress& progress,
                               const fs::path& backupDir, const std::vector<ExpectedFile>* expected = nullptr, Logger* log = nullptr);

// Are all of `expected` in `game` with an accepted hash.
bool FilesMatch(const fs::path& root, const std::vector<ExpectedFile>& expected, std::vector<std::string>* problems = nullptr);

// Can files be created in `dir`? Tried by creating and removing one empty file (what "needs administrator rights" comes down to).
bool FolderWritable(const fs::path& dir);

// The words for an error code of the file system: "administrator rights are needed for this folder" for access denied, "in use by another program" ...
std::string DescribeIoError(const std::error_code& ec, const fs::path& where);

}  // namespace vrml
