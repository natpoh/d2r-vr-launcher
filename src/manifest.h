// manifest.h - the mod manifest, schema "vrmod-manifest/1" (schema/vrmod-manifest.1.schema.json is the same contract
// in JSON Schema; tests/fixtures and tools/check_schema.py keep the two in step).
//
// A manifest describes EVERYTHING a mod puts in the game folder - plugin, ini, shaders, textures, ReShade
// configuration, an OpenXR loader beside the exe - so that switching activates the complete setup.
#pragma once

#include <string>
#include <vector>

#include "json.h"
#include "util.h"

namespace vrml {

struct Issue {
    std::string path;      // JSON pointer-ish: "/files/3/dst"
    std::string message;
};

enum class Role { Program, Asset, Config, Shared, Generated };
const char* RoleName(Role r);

struct FileEntry {
    std::string src;       // relative to the manifest's folder; a trailing '/' = a folder (recursive)
    std::string dst;       // "{game}/..." as written; dst_rel is the game-relative, normalised form
    std::string dst_rel;
    Role role = Role::Asset;
    std::string seed;      // config only: the file a first activation starts from
    std::string sha256;    // optional: the payload file must have it (checked at register time)
    bool optional = false; // a missing src is fine (e.g. "vr" files of a flat-only package)
    std::string tag;
    bool is_dir() const { return !src.empty() && src.back() == '/'; }
};

struct RequiredFile {
    std::string path;                       // "{game}/D2RLoader.exe"
    std::vector<std::string> sha256_any_of; // empty: only the presence is checked
};
struct Requirement {
    std::string id, version, provider, note;   // provider: launcher | mod | user
    bool optional = false;
    std::vector<RequiredFile> files;
};

struct Precondition {
    std::string type;                       // process_absent | file_exists | file_sha256_any_of | toml_value | game_build_in
    std::vector<std::string> names;         // process_absent
    std::string file;                       // file_exists, file_sha256_any_of, toml_value
    std::vector<std::string> sha256_any_of;
    std::string key;                        // toml_value ("section.key")
    Json equals;                            // toml_value
    std::vector<std::string> builds;        // game_build_in
    std::string severity = "block";         // block | warn
    std::vector<std::string> applies_to;    // default: activate, start
    std::string message;
};

struct Hook {
    std::string exe;
    std::vector<std::string> args;
    int timeout_s = 30;
};

struct EnvRule {
    std::vector<std::string> stores;        // empty: every store
    std::vector<std::pair<std::string, std::string>> set;
    std::vector<std::string> unset;
};

struct LaunchOption {
    std::string id, label, kind;            // kind: exec | url
    std::string exe, url, cwd, via;         // via: auto | direct | explorer
    std::vector<std::string> args;
    std::vector<std::string> stores;        // steam | battlenet; empty: both
    std::vector<std::string> runtimes;      // system | steamvr | vdxr | file; empty: system
    std::vector<EnvRule> env;
    std::vector<Precondition> needs;
};

struct Identity {
    std::string plugin_id, plugin_file, product_name, about_file, log;
    std::vector<std::string> dll_sha256, markers;
};

struct ImportSpec {
    std::vector<std::string> backup_globs;  // "{game}/d2rloader/plugins/*.before_*"
    std::string record_file;                // "{game}/d2rloader/pcvr-install.json"
    std::string record_paths_at;            // JSON pointer to an array of folders/files to copy
};

struct Manifest {
    std::string schema, id, name, version, edition, publisher, homepage, support, license, description;
    std::string install_help;               // what the player is told about installing the mod (the install view, a card of a mod that is not installed); optional
    std::string group;                      // exclusive_group (swap mods only)
    std::string d2rloader_mod;              // the D2RLoader mod folder name ("D2R-VR"): set = a D2RLoader mod, started with -mod, nothing is swapped
    std::string icon;                       // a picture (PNG/ICO) next to the manifest, for the launcher's window
    std::string game_id;
    std::vector<std::string> builds, stores, processes;
    Identity identity;
    std::vector<Requirement> requires_;
    std::vector<FileEntry> files;
    std::vector<std::string> external;      // informational: files outside the game folder the mod owns
    std::vector<LaunchOption> launch;
    std::map<std::string, std::vector<Hook>> hooks;   // pre_activate, post_activate, pre_deactivate, post_deactivate
    std::vector<Precondition> preconditions;
    ImportSpec import_;
    std::vector<std::string> logs;
    bool has_settings_app = false;
    std::string settings_exe, settings_hint;
    std::string settings_label;             // the name on the window's settings button ("D2R VR Authentic Settings"); empty: "<mod name> Settings"
    fs::path dir;                           // the folder the manifest was loaded from (the package)
    Json raw;
    bool is_d2rloader_mod() const { return !d2rloader_mod.empty(); }
};

// Parses and validates. Returns true when there are no issues; `m` is filled as far as it could be read.
bool ParseManifest(const Json& j, Manifest* m, std::vector<Issue>* issues);
// Reads a file (E_MANIFEST on I/O or JSON errors, issues for schema errors). Sets m->dir.
bool LoadManifest(const fs::path& file, Manifest* m, std::vector<Issue>* issues);
std::string IssuesText(const std::vector<Issue>& issues);

// Does the package on disk match the manifest: every non-optional src exists, listed sha256 match, no src is a link.
// Adds an issue per problem; returns true when none.
bool CheckPackage(const Manifest& m, std::vector<Issue>* issues);

// "1.3.1" vs "0.165.0-rc1": -1 / 0 / +1. Non-numeric parts compare as text; a pre-release is lower than the release.
int CompareVersions(const std::string& a, const std::string& b);
bool IsVersion(const std::string& v);

// {game}/x -> "x"; "" when it does not start with {game}/.
std::string GameRel(const std::string& dst);

}  // namespace vrml
