// zip_writer.h - a tiny ZIP writer for tests (stored entries only), able to write the odd archives the reader must refuse. The good-archive
// fixtures (deflate levels, nested folders) come from Python's zipfile instead (tests/tools/make_zip_fixtures.py): a writer that is not ours.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "zip.h"

namespace tt {

struct ZEntry {
    std::string name;
    std::string data;
    uint16_t flags = 0;
    bool crc_set = false;
    uint32_t crc = 0;                // used when crc_set (a wrong checksum)
    bool size_set = false;
    uint32_t size_claim = 0;         // the uncompressed size the directory states (stored entries need both sizes equal: this breaks that)
    uint32_t external_attrs = 0;
};

inline void Put16(std::string& s, uint32_t v) { s += static_cast<char>(v & 0xFF); s += static_cast<char>((v >> 8) & 0xFF); }
inline void Put32(std::string& s, uint32_t v) { Put16(s, v & 0xFFFF); Put16(s, v >> 16); }

inline std::string MakeZip(const std::vector<ZEntry>& entries) {
    std::string out, cd;
    for (const ZEntry& e : entries) {
        const uint32_t crc = e.crc_set ? e.crc : vrml::Crc32Update(0, e.data.data(), e.data.size());
        const uint32_t size = static_cast<uint32_t>(e.data.size());
        const uint32_t claim = e.size_set ? e.size_claim : size;
        const uint32_t offset = static_cast<uint32_t>(out.size());
        Put32(out, 0x04034b50); Put16(out, 20); Put16(out, e.flags); Put16(out, 0); Put16(out, 0); Put16(out, 0x5A21);
        Put32(out, crc); Put32(out, size); Put32(out, claim); Put16(out, static_cast<uint32_t>(e.name.size())); Put16(out, 0);
        out += e.name;
        out += e.data;
        Put32(cd, 0x02014b50); Put16(cd, 0x031E); Put16(cd, 20); Put16(cd, e.flags); Put16(cd, 0); Put16(cd, 0); Put16(cd, 0x5A21);
        Put32(cd, crc); Put32(cd, size); Put32(cd, claim); Put16(cd, static_cast<uint32_t>(e.name.size())); Put16(cd, 0); Put16(cd, 0);
        Put16(cd, 0); Put16(cd, 0); Put32(cd, e.external_attrs); Put32(cd, offset);
        cd += e.name;
    }
    const uint32_t cdOffset = static_cast<uint32_t>(out.size());
    out += cd;
    Put32(out, 0x06054b50); Put16(out, 0); Put16(out, 0); Put16(out, static_cast<uint32_t>(entries.size())); Put16(out, static_cast<uint32_t>(entries.size()));
    Put32(out, static_cast<uint32_t>(cd.size())); Put32(out, cdOffset); Put16(out, 0);
    return out;
}

}  // namespace tt
