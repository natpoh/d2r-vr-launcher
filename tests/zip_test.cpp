// The ZIP reader and the safe extractor: archives made by Python's zipfile (stored, deflate 0/1/6/9, folders, empty files, UTF-8 names),
// the same archives with one fault patched in (bad CRC, damaged data, a lying size, truncation...), unsafe names, and limits.
#include <set>

#include "json.h"
#include "sandbox.h"
#include "sha256.h"
#include "test.h"
#include "zip.h"
#include "zip_writer.h"

using namespace vrml;

namespace {

fs::path Fix(const std::string& name) { return fs::path(VRML_SOURCE_DIR) / "tests" / "fixtures" / "zip" / name; }

Json Listing() {
    std::string t;
    ReadFile(Fix("fixtures.json"), &t);
    return Json::Parse(t);
}

// every file under dir -> SHA-256, every folder
void Tree(const fs::path& dir, std::map<std::string, std::string>* files, std::set<std::string>* dirs = nullptr) {
    std::error_code ec;
    if (!IsDir(dir)) return;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const std::string rel = U(it->path().lexically_relative(dir));
        if (it->is_directory()) {
            if (dirs) dirs->insert(rel);
        } else {
            std::string h;
            Sha256File(it->path(), &h);
            (*files)[rel] = h;
        }
    }
}

bool AllowAll(const std::string&, bool) { return true; }

bool NoFolders(const fs::path& dir) {
    std::set<std::string> d;
    std::map<std::string, std::string> f;
    Tree(dir, &f, &d);
    return d.empty();
}

size_t CountFiles(const fs::path& dir) {
    std::map<std::string, std::string> f;
    Tree(dir, &f);
    return f.size();
}

}  // namespace

TEST(crc32_of_the_standard_check_string) {
    CHECK_EQ(Crc32Update(0, "123456789", 9), 0xCBF43926u);
    uint32_t c = Crc32Update(0, "1234", 4);
    c = Crc32Update(c, "56789", 5);
    CHECK_EQ(c, 0xCBF43926u);          // in pieces gives the same
    CHECK_EQ(Crc32Update(0, "", 0), 0u);
}

TEST(every_good_archive_unpacks_to_exactly_what_python_put_in) {
    const Json list = Listing();
    const Json* files = list.find("files");
    REQUIRE(files != nullptr);
    std::set<std::string> wantDirs;
    for (const Json& d : list.find("dirs")->items()) wantDirs.insert(d.as_string());
    int n = 0;
    for (const Json& g : list.find("good")->items()) {
        tt::Sandbox sb;
        const fs::path dest = sb.root / "out";
        ExtractResult r = ExtractAllowed(Fix(g.as_string()), dest, AllowAll);
        std::map<std::string, std::string> got;
        std::set<std::string> gotDirs;
        Tree(dest, &got, &gotDirs);
        CHECK_EQ(got.size(), files->members().size());
        for (const Json::Member& m : files->members()) {
            auto it = got.find(m.first);
            if (it == got.end()) { CHECK(!"a file is missing"); std::cerr << "    missing " << m.first << " in " << g.as_string() << "\n"; continue; }
            if (it->second != m.second.as_string()) { CHECK(!"a file differs"); std::cerr << "    differs " << m.first << " in " << g.as_string() << "\n"; }
        }
        for (const std::string& d : wantDirs) CHECK(gotDirs.count(d) > 0);
        CHECK_EQ(r.files.size(), files->members().size());
        CHECK_EQ(r.dirs.size(), wantDirs.size());
        for (const auto& f : got) CHECK(!EndsWith(f.first, kTmpSuffix));        // no temporary file is left
        ++n;
    }
    CHECK_EQ(n, 5);                                                              // stored, deflate 0, 1, 6, 9
}

TEST(entries_are_read_piece_by_piece_with_length_and_crc_checked) {
    const ZipArchive z = ZipArchive::Open(Fix("deflate9.zip"));
    bool found = false;
    for (const ZipEntry& e : z.entries()) {
        if (e.name != "big/noise.bin") continue;
        found = true;
        std::string data;
        size_t biggestPiece = 0;
        z.Read(e, [&](const char* d, size_t n) { data.append(d, n); biggestPiece = std::max(biggestPiece, n); });
        CHECK_EQ(data.size(), e.size);
        CHECK(biggestPiece <= 32768);
        CHECK_EQ(Sha256Hex(data), Listing().find("files")->get_string("big/noise.bin"));
    }
    CHECK(found);
}

TEST(an_archive_with_no_entries_unpacks_to_nothing) {
    tt::Sandbox sb;
    ExtractResult r = ExtractAllowed(Fix("empty.zip"), sb.root / "out", AllowAll);
    CHECK_EQ(r.files.size(), size_t(0));
    CHECK_EQ(CountFiles(sb.root / "out"), size_t(0));
}

TEST(a_utf8_name_is_kept) {
    tt::Sandbox sb;
    const fs::path dest = sb.root / "out";
    ExtractAllowed(Fix("utf8_name.zip"), dest, AllowAll);
    CHECK(IsFile(dest / P("caf\xC3\xA9/\xC3\xBC.txt")));
}

TEST(names_are_judged_on_the_raw_bytes) {
    std::string why;
    CHECK_EQ(CheckZipName("a/b/c.txt", false, &why), std::string("a/b/c.txt"));
    CHECK_EQ(CheckZipName("folder/", false, &why), std::string("folder"));
    CHECK_EQ(CheckZipName("D2RLoader.exe", false, &why), std::string("D2RLoader.exe"));
    for (const char* bad : {"", "/", "/abs", "a/../b", "../b", "..", "a/./b", "./a", "a//b", "a\\b", "C:/x", "C:x", "a:b", "x.txt:stream", "con", "nul.txt", "COM1", "lpt9.dat",
                            "trail.", "trail ", "a/b./c", "q?.txt", "star*.txt", "a<b", "pipe|x", "ctl\x01x", "tab\tx", "x\x7f"}) {
        why.clear();
        CHECK(CheckZipName(bad, false, &why).empty());
        CHECK(!why.empty());
    }
    CHECK(CheckZipName("caf\xC3\xA9.txt", false, &why).empty());            // non-ASCII without the UTF-8 flag
    CHECK(!CheckZipName("caf\xC3\xA9.txt", true, &why).empty());            // ... with it
    CHECK(CheckZipName("bad\xC3(.txt", true, &why).empty());                // not valid UTF-8
    CHECK(CheckZipName("overlong\xC0\x80.txt", true, &why).empty());
    CHECK(CheckZipName(std::string(300, 'a'), false, &why).empty());        // too long
    CHECK(!CheckZipName("COM10.txt", false, &why).empty());                  // COM10 is an ordinary name; only COM1-9 are devices
}

TEST(unsafe_or_unsupported_archives_are_refused_before_anything_is_written) {
    for (const char* f : {"zipslip.zip", "zipslip_deep.zip", "absolute.zip", "drive.zip", "backslash.zip", "stream.zip", "device.zip", "dup.zip", "dup_case.zip",
                          "file_and_folder.zip", "symlink.zip", "bzip2.zip", "encrypted.zip", "huge_claim.zip"}) {
        tt::Sandbox sb;
        const fs::path dest = sb.root / "out";
        CHECK_ERROR("E_ARCHIVE", ExtractAllowed(Fix(f), dest, AllowAll));
        if (CountFiles(dest) != 0) std::cerr << "    files were written for " << f << "\n";
        CHECK_EQ(CountFiles(dest), size_t(0));
        CHECK(!Exists(sb.root / "evil.txt"));                                       // nothing escaped
        CHECK(!Exists(sb.root / "out" / ".." / "evil.txt"));
    }
}

TEST(a_damaged_archive_leaves_nothing_behind) {
    // these only show their fault while a file is being inflated: everything written so far is removed again
    for (const char* f : {"bad_crc.zip", "damaged_data.zip", "small_claim.zip", "name_mismatch.zip"}) {
        tt::Sandbox sb;
        const fs::path dest = sb.root / "out";
        CHECK_ERROR("E_ARCHIVE", ExtractAllowed(Fix(f), dest, AllowAll));
        if (CountFiles(dest) != 0) std::cerr << "    files are left after " << f << "\n";
        CHECK_EQ(CountFiles(dest), size_t(0));
        CHECK(NoFolders(dest));                                                     // the folders it made are gone too
    }
    for (const char* f : {"truncated.zip", "not_a_zip.zip"}) {
        tt::Sandbox sb;
        CHECK_ERROR("E_ARCHIVE", ExtractAllowed(Fix(f), sb.root / "out", AllowAll));
    }
    tt::Sandbox sb;
    CHECK_ERROR("E_IO", ExtractAllowed(Fix("no_such_file.zip"), sb.root / "out", AllowAll));
}

TEST(the_error_names_the_file_that_is_wrong) {
    tt::Sandbox sb;
    try {
        ExtractAllowed(Fix("bad_crc.zip"), sb.root / "out", AllowAll);
        CHECK(!"should have thrown");
    } catch (const Error& e) {
        CHECK(std::string(e.what()).find("readme.txt") != std::string::npos);
        CHECK(std::string(e.what()).find("CRC") != std::string::npos);
    }
    try {
        ExtractAllowed(Fix("zipslip.zip"), sb.root / "out", AllowAll);
        CHECK(!"should have thrown");
    } catch (const Error& e) {
        CHECK(std::string(e.what()).find("evil.txt") != std::string::npos);
        CHECK(std::string(e.what()).find("nothing was unpacked") != std::string::npos);
    }
}

TEST(one_name_the_predicate_refuses_refuses_the_whole_archive) {
    tt::Sandbox sb;
    const fs::path dest = sb.root / "out";
    int asked = 0;
    ZipAllow no = [&](const std::string& name, bool) {
        ++asked;
        return name != "x/y/z/one-byte";
    };
    CHECK_ERROR("E_ARCHIVE", ExtractAllowed(Fix("deflate6.zip"), dest, no));
    CHECK_EQ(asked, 12);                                       // every entry was asked (6 files + 6 folders), none was written before the answer
    CHECK_EQ(CountFiles(dest), size_t(0));
    // the predicate also sees whether an entry is a folder
    std::set<std::string> dirsSeen;
    ExtractAllowed(Fix("deflate6.zip"), sb.root / "out2", [&](const std::string& name, bool isDir) { if (isDir) dirsSeen.insert(name); return true; });
    CHECK(dirsSeen.count("x/y/z") > 0 && dirsSeen.count("empty-folder") > 0);
}

TEST(limits_are_checked_before_anything_is_inflated) {
    {
        tt::Sandbox sb;
        ZipLimits l;
        l.max_entry_bytes = 100 * 1024;                        // big/text.txt is bigger
        CHECK_ERROR("E_ARCHIVE", ExtractAllowed(Fix("deflate6.zip"), sb.root / "out", AllowAll, l));
        CHECK_EQ(CountFiles(sb.root / "out"), size_t(0));
    }
    {
        tt::Sandbox sb;
        ZipLimits l;
        l.max_total_bytes = 50 * 1024;
        CHECK_ERROR("E_ARCHIVE", ExtractAllowed(Fix("deflate6.zip"), sb.root / "out", AllowAll, l));
    }
    {
        tt::Sandbox sb;
        ZipLimits l;
        l.max_entries = 5;
        CHECK_ERROR("E_ARCHIVE", ExtractAllowed(Fix("deflate6.zip"), sb.root / "out", AllowAll, l));
    }
    {
        tt::Sandbox sb;                                         // the claimed 2 GB is refused by the default limits, before a byte is read
        try {
            ExtractAllowed(Fix("huge_claim.zip"), sb.root / "out", AllowAll);
            CHECK(!"should have thrown");
        } catch (const Error& e) {
            CHECK(std::string(e.what()).find("more than") != std::string::npos);
        }
    }
}

TEST(stored_archives_made_by_the_test_writer_and_the_odd_ones) {
    tt::Sandbox sb;
    const fs::path zip = sb.root / "w.zip";
    tt::Write(zip, tt::MakeZip({{"d2rloader.exe", "EXE"}, {"d2rloader/config/d2rloader.toml", "toml"}, {"mods/", ""}}));
    ExtractResult r = ExtractAllowed(zip, sb.root / "out", AllowAll);
    CHECK_EQ(r.files.size(), size_t(2));
    CHECK_EQ(tt::Read(sb.root / "out" / "d2rloader.exe"), std::string("EXE"));
    CHECK(IsDir(sb.root / "out" / "mods"));

    // a wrong checksum in a stored entry
    tt::ZEntry bad{"x.txt", "data"};
    bad.crc_set = true;
    bad.crc = 1234;
    tt::Write(sb.root / "crc.zip", tt::MakeZip({bad}));
    CHECK_ERROR("E_ARCHIVE", ExtractAllowed(sb.root / "crc.zip", sb.root / "out2", AllowAll));
    // a stored entry whose two sizes differ
    tt::ZEntry lie{"x.txt", "data"};
    lie.size_set = true;
    lie.size_claim = 3;
    tt::Write(sb.root / "lie.zip", tt::MakeZip({lie}));
    CHECK_ERROR("E_ARCHIVE", ExtractAllowed(sb.root / "lie.zip", sb.root / "out3", AllowAll));
    // a folder entry that has data
    tt::Write(sb.root / "dirdata.zip", tt::MakeZip({{"f/", "x"}}));
    CHECK_ERROR("E_ARCHIVE", ExtractAllowed(sb.root / "dirdata.zip", sb.root / "out4", AllowAll));
    // a Unix symlink entry from the test writer
    tt::ZEntry link{"l", "../x"};
    link.external_attrs = 0xA1FF0000u;
    tt::Write(sb.root / "link.zip", tt::MakeZip({link}));
    CHECK_ERROR("E_ARCHIVE", ExtractAllowed(sb.root / "link.zip", sb.root / "out5", AllowAll));
}

TEST(a_file_already_in_the_destination_is_replaced_but_a_folder_or_link_in_the_way_is_not) {
    tt::Sandbox sb;
    const fs::path dest = sb.root / "out";
    tt::Write(dest / "readme.txt", "old");
    ExtractAllowed(Fix("stored.zip"), dest, AllowAll);
    CHECK(tt::Read(dest / "readme.txt") != std::string("old"));
    // a folder where a file should go
    tt::Sandbox sb2;
    fs::create_directories(sb2.root / "out" / "readme.txt");
    CHECK_ERROR("E_ARCHIVE", ExtractAllowed(Fix("stored.zip"), sb2.root / "out", AllowAll));
}

TEST(a_junction_in_the_destination_is_refused) {
#ifdef _WIN32
    tt::Sandbox sb;
    const fs::path elsewhere = sb.root / "elsewhere";
    fs::create_directories(elsewhere);
    const fs::path dest = sb.root / "out";
    fs::create_directories(dest);
    const std::string cmd = "mklink /J \"" + (dest / "big").string() + "\" \"" + elsewhere.string() + "\" >nul 2>&1";
    if (std::system(cmd.c_str()) != 0 || !IsReparsePoint(dest / "big")) {
        std::cerr << "    (junction could not be made here: skipped)\n";
        return;
    }
    CHECK_ERROR("E_ARCHIVE", ExtractAllowed(Fix("stored.zip"), dest, AllowAll));     // big/text.txt would land in "elsewhere"
    CHECK_EQ(CountFiles(elsewhere), size_t(0));
#endif
}

TEST_MAIN()
