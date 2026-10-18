// versioninfo.h - what a DLL or EXE says about itself in its version resource (VERSIONINFO): the launcher reads it from a mod's own plugin when the mod
// has no usable vrmod-manifest.json (docs/MOD_MANIFEST.md, "What is installed"). Windows only; elsewhere nothing is read.
#pragma once

#include <string>

#include "util.h"

namespace vrml {

struct FileVersion {
    bool ok = false;                 // the file has a version resource and it could be read
    std::string product_version;     // ProductVersion, e.g. "0.3.19"
    std::string file_version;        // FileVersion
    std::string comments;            // Comments, e.g. "mod-id=d2r-pcvr; plugin-id=d2r-pcvr-vrcam; Based on D2R VR by BodyWalkVR (MIT)."
    std::string product_name;        // ProductName, e.g. "D2R PCVR"
    std::string company;             // CompanyName
    bool has_fixed = false;          // the numeric version (VS_FIXEDFILEINFO) is there
    unsigned product_numbers[4] = {0, 0, 0, 0};
};

// Reads the version resource of `file` (UTF-8 strings). Never throws and never runs the file; a file without a version resource, a text file named .dll or a file that
// cannot be opened gives ok = false.
FileVersion ReadFileVersion(const fs::path& file);

// The tokens of a Comments text: split at whitespace, ';' and ','. "mod-id=d2r-vr edition=full" -> {"mod-id=d2r-vr", "edition=full"}.
std::vector<std::string> CommentTokens(const std::string& comments);
// The value of `key=` among the tokens ("" when absent): CommentValue("mod-id=d2r-vr edition=full", "mod-id") == "d2r-vr".
std::string CommentValue(const std::string& comments, const std::string& key);

}  // namespace vrml
