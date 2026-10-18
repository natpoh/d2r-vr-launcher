// zip.h - a small, self-contained ZIP reader and a safe extractor (no zlib: our own inflate). Reads what the launcher downloads and
// nothing else: ZIP files with the central directory at the end, entries stored (0) or deflated (8); not encrypted, not zip64, not
// split. Everything else is refused with E_ARCHIVE, and so is anything that could write outside the folder it is told to fill.
//
//   ZipArchive::Open(file)        reads the central directory only
//   ZipArchive::Read(entry, sink) inflates one entry, piece by piece; the length and the CRC-32 are checked, a mismatch throws
//   ExtractAllowed(...)           the safe way to unpack: checks EVERY entry first (nothing is written when one is refused), then writes
//                                 each file to "<name>.vrml-tmp" and renames it; any error removes what this call wrote
//
// Names are checked on the raw bytes the archive holds (not on what a particular unzip tool would make of them): no absolute path, no
// drive, no '..' or '.' part, no empty part, no backslash, no ':' (NTFS streams), no control character, no Windows device name (CON, NUL,
// COM1...), no name ending in a dot or space, nothing but ASCII unless the archive says UTF-8, and no two names that Windows would treat
// as the same file (case-insensitive) or a file that is also a folder. A symbolic link entry is refused. Sizes are checked against the
// limits BEFORE anything is inflated, and the inflate itself stops at the size the archive claims.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "util.h"

namespace vrml {

struct ZipLimits {
    uint32_t max_entries = 5000;
    uint64_t max_entry_bytes = 256ull << 20;        // one file, uncompressed
    uint64_t max_total_bytes = 1024ull << 20;       // all files together, uncompressed
    uint64_t max_central_dir_bytes = 8ull << 20;
};

struct ZipEntry {
    std::string name;               // as stored (before any normalising)
    uint16_t method = 0;            // 0 stored, 8 deflate
    uint16_t flags = 0;
    uint16_t made_by = 0;           // high byte: host system (3 = Unix)
    uint32_t crc32 = 0;
    uint64_t comp_size = 0, size = 0;
    uint64_t local_offset = 0;
    uint32_t external_attrs = 0;
    bool is_dir() const { return !name.empty() && name.back() == '/'; }
    bool is_symlink() const;        // a Unix entry whose mode is a symbolic link
};

class ZipArchive {
public:
    // E_IO when the file cannot be read; E_ARCHIVE when it is not a ZIP this reader supports (or its directory is damaged or over the limits).
    static ZipArchive Open(const fs::path& file, const ZipLimits& limits = {});
    const std::vector<ZipEntry>& entries() const { return entries_; }
    const fs::path& path() const { return file_; }
    // Inflates `e` into `sink` (called with pieces of at most 32 KB, in order). Checks the local header against the directory, the
    // length and the CRC-32; throws E_ARCHIVE on a mismatch (the sink may then have got part of the data).
    void Read(const ZipEntry& e, const std::function<void(const char*, size_t)>& sink) const;

private:
    fs::path file_;
    uint64_t file_size_ = 0, cd_offset_ = 0;
    std::vector<ZipEntry> entries_;
};

// The name as the extractor will use it ('/' separators, no trailing '/'), or "" with *why set when the name is not acceptable.
std::string CheckZipName(const std::string& raw, bool utf8_flag, std::string* why);

struct ExtractResult {
    std::vector<std::string> files;     // relative, '/', in archive order
    std::vector<std::string> dirs;
    uint64_t bytes = 0;
};
// `allow` is asked for every entry (folders too) with its normalised name; one "false" refuses the whole archive before anything is written.
// Meant for a FRESH folder: what this call creates is removed again when it fails; a file that was already there is replaced and not restored.
using ZipAllow = std::function<bool(const std::string& name, bool is_dir)>;
ExtractResult ExtractAllowed(const fs::path& zipFile, const fs::path& destDir, const ZipAllow& allow, const ZipLimits& limits = {});

uint32_t Crc32Update(uint32_t crc, const void* data, size_t len);   // start with 0; the result is the CRC-32 of everything fed so far

}  // namespace vrml
