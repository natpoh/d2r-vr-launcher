// game.h - what the launcher can tell about a game folder: is it one, which store, which build, which D2RLoader.
#pragma once

#include <string>
#include <vector>

#include "util.h"

namespace vrml {

// D2R.exe is there.
bool LooksLikeGame(const fs::path& gameDir);
// "steam" (Data\.build.config), "battlenet" (.build.info) or "" - the same test as d2r-pcvr's game_install.py.
std::string DetectStore(const fs::path& gameDir);
// The game build, e.g. "3.3.93787": the line "D2R version: X." that D2RLoader writes to d2rloader\logs\d2rloader.log, else one of
// `candidates` found as UTF-16 text inside D2R.exe (d2r-pcvr's check). "" when neither tells.
std::string DetectBuild(const fs::path& gameDir, const std::vector<std::string>& candidates);
// "1.3.1-beta" from the log line "D2RLoader v1.3.1-beta (...) started." or "".
std::string DetectLoaderVersion(const fs::path& gameDir);
// The setting `key` ("section.name") of a TOML file; false when the file or the key is not there. Only what D2RLoader's
// d2rloader.toml uses: [section.sub] headers and key = true|false|number|"text" lines.
bool TomlValue(const fs::path& file, const std::string& dottedKey, std::string* value);

}  // namespace vrml
