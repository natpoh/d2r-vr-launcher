#include "install.h"

#include <algorithm>
#include <fstream>
#include <set>

#include "game.h"
#include "mods.h"
#include "sha256.h"
#include "zip.h"

namespace vrml {

namespace {

// the top-level names d2r-pcvr's install.py accepts from the D2RLoader archive
bool LoaderArchiveAllows(const std::string& name, bool is_dir) {
    const size_t slash = name.find('/');
    const std::string top = Lower(slash == std::string::npos ? name : name.substr(0, slash));
    if (top == "mods" || top == "d2rloader") return slash != std::string::npos || is_dir;
    if (top == "d2rloader.exe" || top == "d2rcore.dll" || top == "d2rloader.mpq") return slash == std::string::npos && !is_dir;
    return false;
}

// What is the player's, not ours: settings and the mods themselves.
bool PlayersFile(const std::string& rel) {
    const std::string l = Lower(rel);
    return StartsWith(l, "d2rloader/config/") || StartsWith(l, "mods/");
}

struct Cleanup {                     // removes a folder at the end of the scope, whatever happens
    fs::path dir;
    ~Cleanup() { if (!dir.empty()) RemoveQuiet(dir); }
};

[[noreturn]] void IoFail(const std::string& what, const std::error_code& ec, const fs::path& where) {
    Throw("E_IO", what + ": " + DescribeIoError(ec, where));
}

// leftovers of an installation that was killed half way
void RemoveStaleStages(const fs::path& game) {
    std::error_code ec;
    std::vector<fs::path> old;
    for (fs::directory_iterator it(game, ec), end; !ec && it != end; it.increment(ec))
        if (StartsWith(U(it->path().filename()), ".vrml-install-") && it->is_directory(ec)) old.push_back(it->path());
    for (const fs::path& p : old) RemoveQuiet(p);
}

}  // namespace

std::string DescribeIoError(const std::error_code& ec, const fs::path& where) {
#ifdef _WIN32
    const int v = ec.category() == std::system_category() ? ec.value() : 0;
    if (v == 5 || v == 19 || v == 1314)         // ACCESS_DENIED, WRITE_PROTECT, PRIVILEGE_NOT_HELD
        return "administrator rights are needed for this folder (" + U(where) + "), or the file is read-only";
    if (v == 32 || v == 33)                     // SHARING_VIOLATION, LOCK_VIOLATION
        return U(where) + " is in use by another program";
    if (v == 112) return "the disk is full (" + U(where) + ")";
#else
    if (ec == std::errc::permission_denied) return "administrator rights are needed for this folder (" + U(where) + "), or the file is read-only";
#endif
    return ec.message() + " (" + U(where) + ")";
}

bool FolderWritable(const fs::path& dir) {
    const fs::path probe = dir / P(".vrml-write-test-" + NewTxId());
    {
        std::ofstream f(probe, std::ios::binary | std::ios::trunc);
        if (!f) return false;
    }
    RemoveQuiet(probe);
    return true;
}

std::vector<ExpectedFile> LoaderExpectedFiles() {
    std::vector<ExpectedFile> out;
    for (const Manifest& m : BuiltinMods()) {
        for (const Requirement& r : m.requires_) {
            if (r.id != "d2rloader") continue;
            for (const RequiredFile& f : r.files) {
                const std::string rel = GameRel(f.path);
                if (rel.empty() || f.sha256_any_of.empty()) continue;
                out.push_back({rel, f.sha256_any_of});
            }
            return out;
        }
    }
    return out;
}

bool FilesMatch(const fs::path& root, const std::vector<ExpectedFile>& expected, std::vector<std::string>* problems) {
    bool ok = true;
    for (const ExpectedFile& f : expected) {
        const fs::path p = root / P(f.rel);
        std::string h;
        if (!IsFile(p)) {
            ok = false;
            if (problems) problems->push_back(f.rel + " is missing");
        } else if (!Sha256File(p, &h) || !HashAccepted(f.sha256_any_of, h)) {
            ok = false;
            if (problems) problems->push_back(f.rel + " is not the reviewed file (SHA-256 " + h + ")");
        }
    }
    return ok;
}

InstallResult InstallD2RLoader(const fs::path& game, const Pin& pin, DepsCache& cache, ProcessProbe& probe, const InstallProgress& progress,
                               const fs::path& backupDir, const std::vector<ExpectedFile>* expectedIn, Logger* log) {
    InstallResult res;
    auto say = [&](const std::string& m) { if (log) log->Line("install d2rloader: " + m); };
    auto step = [&](const std::string& stage, uint64_t done, uint64_t total) {
        if (progress && !progress(stage, done, total)) Throw("E_CANCELLED", "the installation was cancelled; nothing was changed in the game folder");
    };

    if (!LooksLikeGame(game)) Throw("E_GAME", U(game) + " has no D2R.exe. Pick the folder Diablo II: Resurrected is installed in.");
    const std::vector<std::string> running = probe.Running({"D2R.exe", "D2RLoader.exe"});
    if (!running.empty()) Throw("E_GAME_RUNNING", "Close " + running.front() + " first: the game is running.");
    if (backupDir.empty()) Throw("E_USAGE", "no folder for backups was given");
    const std::vector<ExpectedFile> expected = expectedIn ? *expectedIn : LoaderExpectedFiles();
    if (expected.empty()) Throw("E_PRECONDITION", "the list of reviewed D2RLoader files is empty");

    // 2. already there
    if (FilesMatch(game, expected)) {
        res.already_installed = true;
        res.notes.push_back("D2RLoader " + pin.version + " is already installed (the reviewed files are in the game folder).");
        say("already installed");
        return res;
    }

    // 3. the archive (download + hash check; a cached good file is reused)
    Downloader* dl = cache.downloader();
    if (dl && progress) dl->SetProgress([&](uint64_t d, uint64_t t) { return progress("download", d, t); });
    struct ResetProgress { Downloader* d; ~ResetProgress() { if (d) d->SetProgress({}); } } resetProgress{dl};
    const bool cachedBefore = IsFile(cache.PathOf(pin));
    const fs::path zip = cache.Ensure(pin);
    res.downloaded = !cachedBefore || !IsFile(zip);
    say(std::string("archive ") + U(zip.filename()) + (cachedBefore ? " from the cache" : " downloaded"));
    step("unpack", 0, 0);

    // 4. unpack into a temporary folder inside the game folder
    RemoveStaleStages(game);
    const fs::path stage = game / P(".vrml-install-" + NewTxId());
    {
        std::error_code ec;
        fs::create_directory(stage, ec);
        if (ec) IoFail("cannot work in the game folder", ec, game);
    }
    Cleanup cleanup{stage};
    ExtractResult ex = ExtractAllowed(zip, stage, LoaderArchiveAllows);

    // 5. the reviewed files, as unpacked
    step("verify", 0, expected.size());
    std::vector<std::string> problems;
    if (!FilesMatch(stage, expected, &problems)) {
        std::string msg = "the files in the D2RLoader archive are not the reviewed " + pin.version + " files; nothing was installed.";
        for (const std::string& p : problems) msg += " " + p + ".";
        Throw("E_HASH", msg);
    }

    // 6. what would change
    struct Move { std::string rel; fs::path from, to; bool had_old = false; fs::path backup; };
    std::vector<Move> moves;
    for (const std::string& rel : ex.files) {
        const fs::path from = stage / P(rel), to = game / P(rel);
        if (Exists(to) && (IsReparsePoint(to) || IsDir(to))) Throw("E_IO", U(to) + " is a folder or a link; the installation would have to replace it. Nothing was changed.");
        if (IsFile(to)) {
            if (PlayersFile(rel)) { res.kept.push_back(rel); continue; }
            std::string a, b;
            if (Sha256File(from, &a) && Sha256File(to, &b) && a == b) { res.unchanged.push_back(rel); continue; }
        }
        moves.push_back({rel, from, to, IsFile(to), {}});
    }
    const std::string stamp = NewTxId();
    for (const Move& m : moves)
        if (m.had_old) { res.backup_dir = backupDir / P("d2rloader-" + stamp); break; }
    for (Move& m : moves) {
        if (!m.had_old) continue;
        m.backup = res.backup_dir / P(m.rel);
        try {
            CopyAtomic(m.to, m.backup);
        } catch (const Error& e) {
            Throw("E_IO", std::string("cannot make a backup of ") + m.rel + " first (" + e.what() + "); the game folder was not changed");
        }
        res.backed_up.push_back(m.rel);
    }

    // 7. move in; on a failure put everything back
    std::vector<fs::path> madeDirs;
    size_t done = 0;
    try {
        for (const std::string& d : ex.dirs) {                          // folders the archive lists (also the empty ones)
            const fs::path dir = game / P(d);
            if (IsDir(dir)) continue;
            for (fs::path p = dir; !IsDir(p) && p != p.parent_path(); p = p.parent_path()) madeDirs.push_back(p);
            std::error_code ec;
            fs::create_directories(dir, ec);
            if (ec && !IsDir(dir)) IoFail("cannot create the folder " + d, ec, dir);
        }
        for (Move& m : moves) {
            for (fs::path p = m.to.parent_path(); !IsDir(p) && p != p.parent_path(); p = p.parent_path()) madeDirs.push_back(p);
            std::error_code ec;
            fs::create_directories(m.to.parent_path(), ec);
            if (ec && !IsDir(m.to.parent_path())) IoFail("cannot create the folder for " + m.rel, ec, m.to.parent_path());
            fs::rename(m.from, m.to, ec);
            if (ec) IoFail("cannot place " + m.rel, ec, m.to);
            ++done;
            if (progress) progress("install", done, moves.size());      // (too late to cancel: the files are going in)
        }
        const fs::path modsDir = game / "mods";
        if (!IsDir(modsDir)) {
            std::error_code ec;
            fs::create_directories(modsDir, ec);
            if (ec && !IsDir(modsDir)) IoFail("cannot create the mods folder", ec, modsDir);
            madeDirs.push_back(modsDir);
            res.notes.push_back("created the mods folder");
        }
    } catch (const Error& e) {
        std::vector<std::string> rollbackProblems;
        for (size_t i = done; i-- > 0;) {
            const Move& m = moves[i];
            try {
                if (m.had_old) CopyAtomic(m.backup, m.to);
                else RemoveQuiet(m.to);
            } catch (const Error& re) {
                rollbackProblems.push_back(m.rel + ": " + re.what());
            }
        }
        for (const fs::path& d : madeDirs) {
            std::error_code ec;
            fs::remove(d, ec);                                  // only when empty
        }
        if (!rollbackProblems.empty()) {
            std::string msg = std::string(e.what()) + ". Putting the old files back failed for:";
            for (const std::string& p : rollbackProblems) msg += " " + p + ";";
            msg += " the copies are in " + U(res.backup_dir);
            Throw("E_ROLLBACK", msg);
        }
        Throw(e.code, std::string(e.what()) + ". The game folder was put back as it was.");
    }
    for (const Move& m : moves) res.written.push_back(m.rel);
    if (!res.kept.empty()) res.notes.push_back("kept your own files: " + [&] { std::string s; for (const std::string& k : res.kept) s += (s.empty() ? "" : ", ") + k; return s; }());
    if (!res.backed_up.empty()) res.notes.push_back("the replaced files were copied to " + U(res.backup_dir));
    say(std::to_string(res.written.size()) + " file(s) written, " + std::to_string(res.backed_up.size()) + " backed up, " + std::to_string(res.kept.size()) + " kept");
    return res;
}

}  // namespace vrml
