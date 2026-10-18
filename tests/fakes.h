// fakes.h - stand-ins for the network and for running a Setup, shared by the install tests. Nothing here touches a network or starts a process.
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "deps.h"
#include "json.h"
#include "net.h"
#include "sandbox.h"
#include "setup_run.h"

namespace tt {

using namespace vrml;

// A downloader that "downloads" `content`; reports half way and at the end like a real one, and honours a cancel.
class FakeDownloader : public Downloader {
public:
    std::string content = "the archive";
    bool fail = false;
    int calls = 0;
    std::string last_url;
    std::vector<std::string> urls;
    ProgressFn progress;
    bool cancelled = false;
    bool Fetch(const std::string& url, const fs::path& dest, std::string* error) override {
        ++calls;
        last_url = url;
        urls.push_back(url);
        cancelled = false;
        if (fail) { *error = "no network (pretend)"; return false; }
        if (progress && !progress(content.size() / 2, content.size())) { cancelled = true; *error = "cancelled"; return false; }
        Write(dest, content);
        if (progress) progress(content.size(), content.size());
        return true;
    }
    void SetProgress(ProgressFn p) override { progress = std::move(p); }
    bool WasCancelled() const override { return cancelled; }
};

// An HTTP client that answers from a table (url -> body) and remembers what was asked.
class FakeHttp : public HttpClient {
public:
    std::map<std::string, std::string> answers;
    std::vector<std::string> asked;
    bool GetText(const std::string& url, std::string* body, std::string* error) override {
        asked.push_back(url);
        auto it = answers.find(url);
        if (it == answers.end()) {
            *error = "no network (pretend): " + url;
            return false;
        }
        *body = it->second;
        return true;
    }
};

// A Setup runner that records what it was told to run, optionally does what a Setup would (on_run), and answers with a status and exit code.
class FakeSetup : public SetupBackend {
public:
    std::vector<SetupCommand> runs;
    SetupStatus status = SetupStatus::Finished;
    int exit_code = 0;
    std::function<void(const SetupCommand&)> on_run;
    SetupStatus Run(const SetupCommand& cmd, int* exitCode, std::string* error) override {
        runs.push_back(cmd);
        if (status == SetupStatus::FailedToStart) { *error = "pretend failure"; return status; }
        if (status == SetupStatus::UacDeclined) return status;
        if (on_run) on_run(cmd);
        *exitCode = exit_code;
        return status;
    }
};

// What a mod's Setup leaves in the game folder: modinfo.json and the mod's own manifest (the launcher's example, at `version`).
inline void LeaveMod(const fs::path& game, const std::string& id, const std::string& version) {
    const std::string folder = id == "d2r-vr" ? "D2R-VR" : "D2R-PCVR";
    Write(game / "mods" / folder / (folder + ".mpq") / "modinfo.json", "{}");
    std::string manifest;
    ReadFile(fs::path(VRML_SOURCE_DIR) / "examples" / "d2rloader-mods" / (id == "d2r-vr" ? "d2r-vr" : "d2r-pcvr") / "vrmod-manifest.json", &manifest);
    Json j = Json::Parse(manifest);
    j.set("version", Json::String(version));
    Write(game / "mods" / folder / "vrmod-manifest.json", j.dump());
}

}  // namespace tt
