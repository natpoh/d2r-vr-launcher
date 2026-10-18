#include "zip.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <set>

namespace vrml {

// ------------------------------------------------------------------ CRC-32

uint32_t Crc32Update(uint32_t crc, const void* data, size_t len) {
    static const std::vector<uint32_t> table = [] {
        std::vector<uint32_t> t(256);
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t c = ~crc;
    for (size_t i = 0; i < len; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

namespace {

[[noreturn]] void Bad(const std::string& msg) { Throw("E_ARCHIVE", msg); }

// ------------------------------------------------------------------ inflate (RFC 1951)

const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

constexpr int kFastBits = 9;

struct Huff {
    uint16_t fast[1 << kFastBits];      // (length << 9) | symbol for codes of up to kFastBits bits; 0 = not in the table
    uint16_t count[16];                 // codes of each length
    uint16_t symbol[288];               // symbols in canonical order

    // false when the lengths describe an over-subscribed code (not a valid prefix code)
    bool Build(const uint8_t* lengths, int n) {
        std::memset(fast, 0, sizeof fast);
        std::memset(count, 0, sizeof count);
        for (int i = 0; i < n; ++i) ++count[lengths[i]];
        count[0] = 0;
        int left = 1;
        for (int len = 1; len < 16; ++len) {
            left <<= 1;
            left -= count[len];
            if (left < 0) return false;
        }
        uint16_t offs[16];
        offs[1] = 0;
        for (int len = 1; len < 15; ++len) offs[len + 1] = static_cast<uint16_t>(offs[len] + count[len]);
        for (int s = 0; s < n; ++s)
            if (lengths[s]) symbol[offs[lengths[s]]++] = static_cast<uint16_t>(s);
        // the fast table: canonical codes, bit-reversed because Deflate packs a code from its first bit
        int code = 0, idx = 0, prev = 0;
        for (int len = 1; len <= 15; ++len) {
            code = (code + prev) << 1;
            prev = count[len];
            if (len <= kFastBits) {
                for (int k = 0; k < count[len]; ++k) {
                    int c = code + k, rev = 0;
                    for (int b = 0; b < len; ++b) rev |= ((c >> b) & 1) << (len - 1 - b);
                    const uint16_t entry = static_cast<uint16_t>((len << 9) | symbol[idx + k]);
                    for (int j = rev; j < (1 << kFastBits); j += 1 << len) fast[j] = entry;
                }
            }
            idx += count[len];
        }
        return true;
    }
};

// Reads the compressed bytes of one entry and writes the inflated data to a sink, never more than `limit` bytes.
class Inflater {
public:
    Inflater(std::ifstream& in, uint64_t compressed, uint64_t limit, const std::function<void(const char*, size_t)>& sink)
        : in_(in), remaining_(compressed), limit_(limit), sink_(sink), inbuf_(64 * 1024) {}

    uint64_t Run() {
        for (;;) {
            const unsigned last = Bits(1);
            const unsigned type = Bits(2);
            if (type == 0) Stored();
            else if (type == 1) Fixed();
            else if (type == 2) Dynamic();
            else Bad("damaged data in the archive (unknown block type)");
            if (Overrun()) Bad("the archive is cut short (compressed data ends early)");
            if (last) break;
        }
        Flush();
        return total_;
    }

private:
    std::ifstream& in_;
    uint64_t remaining_;
    uint64_t limit_;
    const std::function<void(const char*, size_t)>& sink_;
    std::vector<uint8_t> inbuf_;
    size_t ipos_ = 0, ilen_ = 0;
    uint64_t bitbuf_ = 0;
    int bitcnt_ = 0;
    int padbytes_ = 0;
    uint8_t win_[32768];
    uint64_t total_ = 0, flushed_ = 0;

    bool Overrun() const { return bitcnt_ < padbytes_ * 8; }

    int NextByte() {
        if (ipos_ == ilen_) {
            if (remaining_ == 0) return -1;
            const size_t want = static_cast<size_t>(std::min<uint64_t>(remaining_, inbuf_.size()));
            in_.read(reinterpret_cast<char*>(inbuf_.data()), static_cast<std::streamsize>(want));
            const size_t got = static_cast<size_t>(in_.gcount());
            if (got == 0) {
                remaining_ = 0;
                return -1;
            }
            remaining_ -= got;
            ipos_ = 0;
            ilen_ = got;
        }
        return inbuf_[ipos_++];
    }
    void Fill() {
        while (bitcnt_ <= 56) {
            int b = NextByte();
            if (b < 0) {
                b = 0;
                ++padbytes_;
            }
            bitbuf_ |= static_cast<uint64_t>(b) << bitcnt_;
            bitcnt_ += 8;
        }
    }
    unsigned Bits(int n) {
        if (n == 0) return 0;
        if (bitcnt_ < n) Fill();
        const unsigned v = static_cast<unsigned>(bitbuf_ & ((1ull << n) - 1));
        bitbuf_ >>= n;
        bitcnt_ -= n;
        return v;
    }

    void Flush() {
        const uint64_t pending = total_ - flushed_;
        if (pending == 0) return;
        // pending bytes sit at the ring positions of flushed_..total_; a flush happens at every 32 KB wrap and at the end,
        // so they are contiguous from the ring's start
        sink_(reinterpret_cast<const char*>(win_), static_cast<size_t>(pending));
        flushed_ = total_;
    }
    void Put(uint8_t b) {
        if (total_ >= limit_) Bad("the archive holds more data than it says");
        win_[total_ & 32767] = b;
        ++total_;
        if ((total_ & 32767) == 0) Flush();
    }

    void Stored() {
        // drop the bits up to the next byte boundary, then LEN and NLEN
        const int drop = bitcnt_ & 7;
        bitbuf_ >>= drop;
        bitcnt_ -= drop;
        const unsigned len = Bits(16);
        const unsigned nlen = Bits(16);
        if ((len ^ 0xFFFF) != nlen) Bad("damaged data in the archive (stored block length)");
        for (unsigned i = 0; i < len; ++i) {
            Put(static_cast<uint8_t>(Bits(8)));
            if (Overrun()) Bad("the archive is cut short (compressed data ends early)");
        }
    }

    int Decode(const Huff& h) {
        if (bitcnt_ < 15) Fill();
        const uint16_t f = h.fast[bitbuf_ & ((1 << kFastBits) - 1)];
        if (f) {
            const int len = f >> 9;
            bitbuf_ >>= len;
            bitcnt_ -= len;
            return f & 511;
        }
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= 15; ++len) {
            code |= static_cast<int>(Bits(1));
            const int cnt = h.count[len];
            if (code - cnt < first) return h.symbol[index + (code - first)];
            index += cnt;
            first += cnt;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }

    void Codes(const Huff& lit, const Huff& dist) {
        for (;;) {
            const int sym = Decode(lit);
            if (sym < 0) Bad("damaged data in the archive (bad code)");
            if (Overrun()) Bad("the archive is cut short (compressed data ends early)");
            if (sym < 256) {
                Put(static_cast<uint8_t>(sym));
            } else if (sym == 256) {
                return;
            } else {
                const int li = sym - 257;
                if (li >= 29) Bad("damaged data in the archive (bad length)");
                const unsigned len = kLenBase[li] + Bits(kLenExtra[li]);
                const int ds = Decode(dist);
                if (ds < 0 || ds >= 30) Bad("damaged data in the archive (bad distance)");
                const unsigned d = kDistBase[ds] + Bits(kDistExtra[ds]);
                if (d > total_) Bad("damaged data in the archive (distance too far back)");
                for (unsigned i = 0; i < len; ++i) Put(win_[(total_ - d) & 32767]);
            }
        }
    }

    void Fixed() {
        static const struct Tables {
            Huff lit, dist;
            Tables() {
                uint8_t l[288];
                int i = 0;
                for (; i < 144; ++i) l[i] = 8;
                for (; i < 256; ++i) l[i] = 9;
                for (; i < 280; ++i) l[i] = 7;
                for (; i < 288; ++i) l[i] = 8;
                lit.Build(l, 288);
                uint8_t d[30];
                for (int k = 0; k < 30; ++k) d[k] = 5;
                dist.Build(d, 30);
            }
        } t;
        Codes(t.lit, t.dist);
    }

    void Dynamic() {
        static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        const int nlen = static_cast<int>(Bits(5)) + 257;
        const int ndist = static_cast<int>(Bits(5)) + 1;
        const int ncode = static_cast<int>(Bits(4)) + 4;
        if (nlen > 286 || ndist > 30) Bad("damaged data in the archive (too many codes)");
        uint8_t lengths[320] = {};
        for (int i = 0; i < ncode; ++i) lengths[order[i]] = static_cast<uint8_t>(Bits(3));
        Huff lencode;
        if (!lencode.Build(lengths, 19)) Bad("damaged data in the archive (code lengths)");
        uint8_t all[320] = {};
        int idx = 0;
        while (idx < nlen + ndist) {
            const int sym = Decode(lencode);
            if (sym < 0 || Overrun()) Bad("damaged data in the archive (code lengths)");
            if (sym < 16) {
                all[idx++] = static_cast<uint8_t>(sym);
            } else {
                int rep, val = 0;
                if (sym == 16) {
                    if (idx == 0) Bad("damaged data in the archive (repeat with nothing before it)");
                    val = all[idx - 1];
                    rep = 3 + static_cast<int>(Bits(2));
                } else if (sym == 17) {
                    rep = 3 + static_cast<int>(Bits(3));
                } else {
                    rep = 11 + static_cast<int>(Bits(7));
                }
                if (idx + rep > nlen + ndist) Bad("damaged data in the archive (code lengths overrun)");
                while (rep--) all[idx++] = static_cast<uint8_t>(val);
            }
        }
        if (all[256] == 0) Bad("damaged data in the archive (no end-of-block code)");
        Huff lit, dist;
        if (!lit.Build(all, nlen)) Bad("damaged data in the archive (literal codes)");
        if (!dist.Build(all + nlen, ndist)) Bad("damaged data in the archive (distance codes)");
        Codes(lit, dist);
    }
};

// ------------------------------------------------------------------ little-endian fields

uint16_t Le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t Le32(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24); }

bool ValidUtf8(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t n = 0;
        uint32_t cp = 0;
        if (c < 0x80) { ++i; continue; }
        else if ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; }
        else return false;
        for (size_t k = 1; k <= n; ++k) {
            if (i + k >= s.size()) return false;
            const unsigned char d = static_cast<unsigned char>(s[i + k]);
            if ((d & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (d & 0x3F);
        }
        if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += n + 1;
    }
    return true;
}

bool DeviceName(const std::string& part) {
    // CON, PRN, AUX, NUL, COM1-9, LPT1-9 - with or without an extension ("nul.txt" is the NUL device on Windows)
    std::string base = Lower(part.substr(0, part.find('.')));
    while (!base.empty() && base.back() == ' ') base.pop_back();
    if (base == "con" || base == "prn" || base == "aux" || base == "nul" || base == "conin$" || base == "conout$") return true;
    if (base.size() == 4 && (StartsWith(base, "com") || StartsWith(base, "lpt")) && base[3] >= '1' && base[3] <= '9') return true;
    return false;
}

}  // namespace

bool ZipEntry::is_symlink() const {
    if ((made_by >> 8) != 3) return false;                         // only Unix entries carry a mode
    const uint32_t mode = external_attrs >> 16;
    return (mode & 0xF000) == 0xA000;
}

// ------------------------------------------------------------------ names

std::string CheckZipName(const std::string& raw, bool utf8_flag, std::string* why) {
    auto no = [&](const char* m) {
        if (why) *why = m;
        return std::string();
    };
    if (raw.empty()) return no("an empty name");
    if (raw.size() > 240) return no("a name longer than 240 bytes");
    for (char ch : raw) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == 0x7f) return no("a control character in the name");
        if (c == '\\') return no("a backslash in the name");
        if (c == ':') return no("a ':' in the name (a drive or a stream)");
        if (c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return no("a character Windows does not allow in a name");
        if (c >= 0x80 && !utf8_flag) return no("a non-ASCII name that is not marked as UTF-8");
    }
    if (!ValidUtf8(raw)) return no("a name that is not valid UTF-8");
    std::string n = raw;
    if (n.back() == '/') n.pop_back();
    if (n.empty()) return no("a name that is only a '/'");
    if (n[0] == '/') return no("an absolute path");
    for (const std::string& part : Split(n, '/')) {
        if (part.empty()) return no("an empty part in the path");
        if (part == "." || part == "..") return no("a '.' or '..' part in the path");
        if (part.back() == '.' || part.back() == ' ') return no("a name ending in a dot or a space");
        if (DeviceName(part)) return no("a Windows device name");
    }
    return n;
}

// ------------------------------------------------------------------ opening

ZipArchive ZipArchive::Open(const fs::path& file, const ZipLimits& lim) {
    ZipArchive z;
    z.file_ = file;
    std::ifstream in(file, std::ios::binary);
    if (!in) Throw("E_IO", "cannot read " + U(file));
    in.seekg(0, std::ios::end);
    const std::streamoff sz = in.tellg();
    if (sz < 22) Bad(U(file.filename()) + " is not a ZIP file (too small)");
    z.file_size_ = static_cast<uint64_t>(sz);

    // the end-of-central-directory record: in the last 22 + 65535 bytes
    const size_t tailLen = static_cast<size_t>(std::min<uint64_t>(z.file_size_, 22 + 65535));
    std::vector<uint8_t> tail(tailLen);
    in.seekg(static_cast<std::streamoff>(z.file_size_ - tailLen), std::ios::beg);
    in.read(reinterpret_cast<char*>(tail.data()), static_cast<std::streamsize>(tailLen));
    if (static_cast<size_t>(in.gcount()) != tailLen) Throw("E_IO", "cannot read " + U(file));
    size_t eocd = std::string::npos;
    for (size_t i = tailLen - 22 + 1; i-- > 0;) {
        if (Le32(&tail[i]) == 0x06054b50) {
            const uint16_t commentLen = Le16(&tail[i + 20]);
            if (i + 22 + commentLen <= tailLen) {
                eocd = i;
                break;
            }
        }
    }
    if (eocd == std::string::npos) Bad(U(file.filename()) + " is not a ZIP file (no end record)");
    const uint8_t* e = &tail[eocd];
    const uint16_t diskNo = Le16(e + 4), cdDisk = Le16(e + 6), entriesHere = Le16(e + 8), entriesAll = Le16(e + 10);
    const uint32_t cdSize = Le32(e + 12), cdOffset = Le32(e + 16);
    if (diskNo != 0 || cdDisk != 0 || entriesHere != entriesAll) Bad("split ZIP archives are not supported");
    if (entriesAll == 0xFFFF || cdSize == 0xFFFFFFFFu || cdOffset == 0xFFFFFFFFu) Bad("zip64 archives are not supported");
    if (eocd >= 20 && Le32(&tail[eocd - 20]) == 0x07064b50) Bad("zip64 archives are not supported");
    if (entriesAll > lim.max_entries) Bad("the archive has " + std::to_string(entriesAll) + " entries, more than the " + std::to_string(lim.max_entries) + " allowed");
    if (cdSize > lim.max_central_dir_bytes) Bad("the archive's directory is too large");
    if (static_cast<uint64_t>(cdOffset) + cdSize > z.file_size_ - tailLen + eocd) Bad("the archive's directory lies outside the file");
    z.cd_offset_ = cdOffset;

    std::vector<uint8_t> cd(cdSize);
    in.seekg(static_cast<std::streamoff>(cdOffset), std::ios::beg);
    in.read(reinterpret_cast<char*>(cd.data()), static_cast<std::streamsize>(cdSize));
    if (static_cast<uint64_t>(in.gcount()) != cdSize) Bad("the archive's directory is cut short");
    size_t pos = 0;
    for (uint32_t n = 0; n < entriesAll; ++n) {
        if (pos + 46 > cd.size() || Le32(&cd[pos]) != 0x02014b50) Bad("the archive's directory is damaged");
        const uint8_t* h = &cd[pos];
        ZipEntry en;
        en.made_by = Le16(h + 4);
        en.flags = Le16(h + 8);
        en.method = Le16(h + 10);
        en.crc32 = Le32(h + 16);
        const uint32_t comp = Le32(h + 20), size = Le32(h + 24);
        const uint16_t nameLen = Le16(h + 28), extraLen = Le16(h + 30), commentLen = Le16(h + 32);
        en.external_attrs = Le32(h + 38);
        const uint32_t off = Le32(h + 42);
        if (comp == 0xFFFFFFFFu || size == 0xFFFFFFFFu || off == 0xFFFFFFFFu) Bad("zip64 archives are not supported");
        en.comp_size = comp;
        en.size = size;
        en.local_offset = off;
        if (pos + 46 + nameLen + extraLen + commentLen > cd.size()) Bad("the archive's directory is damaged");
        en.name.assign(reinterpret_cast<const char*>(h + 46), nameLen);
        pos += 46u + nameLen + extraLen + commentLen;
        z.entries_.push_back(std::move(en));
    }
    return z;
}

// ------------------------------------------------------------------ reading one entry

void ZipArchive::Read(const ZipEntry& e, const std::function<void(const char*, size_t)>& sink) const {
    if (e.flags & 1) Bad("\"" + e.name + "\" is encrypted; encrypted archives are not supported");
    if (e.method != 0 && e.method != 8) Bad("\"" + e.name + "\" uses compression method " + std::to_string(e.method) + ", which is not supported");
    std::ifstream in(file_, std::ios::binary);
    if (!in) Throw("E_IO", "cannot read " + U(file_));
    uint8_t lh[30];
    in.seekg(static_cast<std::streamoff>(e.local_offset), std::ios::beg);
    in.read(reinterpret_cast<char*>(lh), 30);
    if (in.gcount() != 30 || Le32(lh) != 0x04034b50) Bad("\"" + e.name + "\": the entry's header is damaged");
    const uint16_t nameLen = Le16(lh + 26), extraLen = Le16(lh + 28);
    if (nameLen != e.name.size()) Bad("\"" + e.name + "\": the entry's header names a different file");
    std::string localName(nameLen, '\0');
    in.read(localName.data(), nameLen);
    if (static_cast<size_t>(in.gcount()) != nameLen || localName != e.name) Bad("\"" + e.name + "\": the entry's header names a different file");
    const uint64_t dataAt = e.local_offset + 30 + nameLen + extraLen;
    if (dataAt + e.comp_size > cd_offset_) Bad("\"" + e.name + "\": its data lies outside the archive");
    in.seekg(static_cast<std::streamoff>(dataAt), std::ios::beg);

    uint32_t crc = 0;
    uint64_t total = 0;
    const std::function<void(const char*, size_t)> feed = [&](const char* d, size_t n) {
        crc = Crc32Update(crc, d, n);
        total += n;
        sink(d, n);
    };
    if (e.method == 0) {
        if (e.comp_size != e.size) Bad("\"" + e.name + "\": a stored entry whose two sizes differ");
        std::vector<char> buf(32 * 1024);
        uint64_t left = e.size;
        while (left) {
            const size_t want = static_cast<size_t>(std::min<uint64_t>(left, buf.size()));
            in.read(buf.data(), static_cast<std::streamsize>(want));
            if (static_cast<size_t>(in.gcount()) != want) Bad("\"" + e.name + "\": the archive is cut short");
            feed(buf.data(), want);
            left -= want;
        }
    } else {
        Inflater inf(in, e.comp_size, e.size, feed);
        try {
            inf.Run();
        } catch (const Error& err) {
            throw Error(err.code, err.exit_code, "\"" + e.name + "\": " + err.what());
        }
    }
    if (total != e.size) Bad("\"" + e.name + "\": the data is " + std::to_string(total) + " bytes, the archive says " + std::to_string(e.size));
    if (crc != e.crc32) Bad("\"" + e.name + "\": the data does not match its checksum (CRC-32); the archive is damaged");
}

// ------------------------------------------------------------------ extracting

ExtractResult ExtractAllowed(const fs::path& zipFile, const fs::path& destDir, const ZipAllow& allow, const ZipLimits& lim) {
    const ZipArchive z = ZipArchive::Open(zipFile, lim);

    // 1. every entry is checked before anything is written
    struct Item { const ZipEntry* e; std::string name; };
    std::vector<Item> items;
    std::set<std::string> seen, files, dirs;
    uint64_t total = 0;
    for (const ZipEntry& e : z.entries()) {
        std::string why;
        const std::string name = CheckZipName(e.name, (e.flags & 0x800) != 0, &why);
        if (name.empty()) Bad("the archive holds an entry with " + why + " (\"" + e.name.substr(0, 80) + "\"); nothing was unpacked");
        if (e.flags & 1) Bad("\"" + name + "\" is encrypted; nothing was unpacked");
        if (e.method != 0 && e.method != 8) Bad("\"" + name + "\" uses compression method " + std::to_string(e.method) + "; nothing was unpacked");
        if (e.is_symlink()) Bad("\"" + name + "\" is a symbolic link; nothing was unpacked");
        if (!seen.insert(Lower(name)).second) Bad("the archive holds \"" + name + "\" twice; nothing was unpacked");
        if (allow && !allow(name, e.is_dir())) Bad("the archive holds \"" + name + "\", which is not on the list of files this install may write; nothing was unpacked");
        if (e.is_dir()) {
            if (e.size != 0) Bad("\"" + name + "\" is a folder with data; nothing was unpacked");
            dirs.insert(Lower(name));
        } else {
            if (e.size > lim.max_entry_bytes) Bad("\"" + name + "\" claims " + std::to_string(e.size) + " bytes, more than the " + std::to_string(lim.max_entry_bytes) + " allowed; nothing was unpacked");
            total += e.size;
            if (total > lim.max_total_bytes) Bad("the archive unpacks to more than " + std::to_string(lim.max_total_bytes) + " bytes; nothing was unpacked");
            files.insert(Lower(name));
        }
        items.push_back({&e, name});
    }
    // a name that is a file and also the parent of another entry
    for (const Item& it : items) {
        std::string parent = Lower(it.name);
        for (size_t slash; (slash = parent.rfind('/')) != std::string::npos;) {
            parent.resize(slash);
            if (files.count(parent)) Bad("\"" + it.name + "\" lies inside \"" + parent + "\", which is a file; nothing was unpacked");
        }
    }

    // 2. write: files to <name>.vrml-tmp, then renamed; anything this call created is removed again on an error
    ExtractResult res;
    std::vector<fs::path> madeFiles, madeDirs;
    auto undo = [&]() {
        for (const fs::path& f : madeFiles) RemoveQuiet(f);
        for (auto it = madeDirs.rbegin(); it != madeDirs.rend(); ++it) {
            std::error_code ec;
            fs::remove(*it, ec);                       // only when empty
        }
    };
    auto mkdir = [&](const fs::path& d) {
        if (IsDir(d)) {
            if (IsReparsePoint(d)) Bad("a link in the way at " + U(d) + "; nothing was unpacked");
            return;
        }
        if (Exists(d)) Bad(U(d) + " exists and is not a folder; nothing was unpacked");
        std::error_code ec;
        fs::create_directory(d, ec);
        if (ec && !IsDir(d)) Throw("E_IO", "cannot create the folder " + U(d) + ": " + ec.message());
        madeDirs.push_back(d);
        if (IsReparsePoint(d)) Bad("a link in the way at " + U(d) + "; nothing was unpacked");
    };
    try {
        // the destination itself, created level by level so each new folder is recorded
        std::vector<fs::path> chain;
        for (fs::path p = fs::absolute(destDir).lexically_normal(); !p.empty() && !IsDir(p) && p != p.parent_path(); p = p.parent_path()) chain.push_back(p);
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) mkdir(*it);
        const fs::path root = fs::absolute(destDir).lexically_normal();
        if (IsReparsePoint(root)) Bad("the destination folder is a link; nothing was unpacked");
        for (const Item& it : items) {
            const fs::path target = root / P(it.name);
            // every parent below the root must be a real folder
            std::vector<fs::path> parents;
            for (fs::path p = it.e->is_dir() ? target : target.parent_path(); p != root && p.has_relative_path() && p != p.parent_path(); p = p.parent_path()) parents.push_back(p);
            for (auto pi = parents.rbegin(); pi != parents.rend(); ++pi) mkdir(*pi);
            if (it.e->is_dir()) {
                res.dirs.push_back(it.name);
                continue;
            }
            if (Exists(target) && (IsReparsePoint(target) || IsDir(target))) Bad(U(target) + " is in the way (a folder or a link); nothing was unpacked");
            fs::path tmp = target;
            tmp += kTmpSuffix;
            RemoveQuiet(tmp);
            {
                std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
                if (!f) Throw("E_IO", "cannot write " + U(tmp));
                madeFiles.push_back(tmp);
                try {
                    z.Read(*it.e, [&](const char* d, size_t n) {
                        f.write(d, static_cast<std::streamsize>(n));
                        if (!f) Throw("E_IO", "cannot write " + U(tmp) + " (the disk is full or the folder is protected)");
                    });
                } catch (...) {
                    f.close();
                    throw;
                }
                f.flush();
                if (!f) Throw("E_IO", "cannot write " + U(tmp));
            }
            std::error_code ec;
            const bool existed = Exists(target);
            fs::rename(tmp, target, ec);
            if (ec) Throw("E_IO", "cannot place " + U(target) + ": " + ec.message());
            madeFiles.pop_back();                      // the temporary name is gone ...
            if (!existed) madeFiles.push_back(target); // ... and the file is ours to remove again only when it is new
            res.files.push_back(it.name);
            res.bytes += it.e->size;
        }
    } catch (...) {
        undo();
        throw;
    }
    return res;
}

}  // namespace vrml
