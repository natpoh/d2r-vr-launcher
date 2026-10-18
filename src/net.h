// net.h - the launcher's network access: HTTPS downloads and small JSON/text requests over WinHTTP (Windows' own TLS stack and
// certificate store; nothing else is linked).
//
// Rules, all enforced here and not by the callers:
//   * https only. The ONE exception is a test switch: the environment variable VRML_ALLOW_HTTP_HOST=127.0.0.1:PORT (or localhost:PORT,
//     [::1]:PORT) lets exactly that loopback host:port be reached over plain http. Anything else in it is ignored. Production code never
//     sets it.
//   * every URL - the first and each redirect target - must be on the caller's host allow-list. An entry is an exact host name
//     ("bodywalkvr.com") or "*.domain" (any subdomain of it, not the domain itself). A redirect off the list, to http, or more than
//     5 redirects in a row is refused.
//   * a size cap per call: Content-Length above it is refused before a byte is read; a body that grows past it is cut off.
//   * a download goes to "<dest>.vrml-part" and is renamed to <dest> only when it is complete; every failure and every cancel deletes
//     the partial file. The SHA-256 of the bytes is computed while they stream.
//   * the progress callback may return false: the download stops and its partial file is deleted (cancelled() then says so).
// Certificates are never ignored. User agent: "D2RVRLauncher/<version>".
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "deps.h"
#include "util.h"

namespace vrml {

// ---- URLs and the allow-list
struct UrlParts {
    bool ok = false;
    std::string scheme;     // lower case: "https", "http"
    std::string host;       // lower case, no brackets for IPv6
    int port = 0;           // the scheme's default when the URL has none
    std::string path;       // from the first '/', with the query; "/" when empty. No fragment.
    std::string hostport;   // "host" or "host:port" as written in the URL (lower case); IPv6 keeps its brackets
};
UrlParts ParseUrl(const std::string& url);                  // no user info ("a@b") is accepted
bool HostAllowed(const std::string& host, const std::vector<std::string>& allow);   // exact, or "*.domain"; case-insensitive
// "127.0.0.1:8123" from VRML_ALLOW_HTTP_HOST in `env` when it names a loopback host and a port; "" otherwise (the switch is off).
std::string TestHttpHost(const std::map<std::string, std::string>& env);
std::string TestHttpHostFromProcessEnv();                   // the same, from the real environment (what HttpOptions starts with)
// True when `url` may be fetched: https and the host on the list, or http and exactly `testHttpHost`. *why says what is wrong.
bool UrlAllowed(const std::string& url, const std::vector<std::string>& allow, const std::string& testHttpHost, std::string* why = nullptr);

// ---- one request
struct HttpOptions {
    std::vector<std::string> allow_hosts;
    uint64_t max_bytes = 16ull << 20;           // the cap of one call (the callers set it: JSON a few MB, an installer a few hundred)
    int max_redirects = 5;
    int connect_timeout_ms = 15000;
    int receive_timeout_ms = 30000;             // per read: a server that goes quiet for this long fails the call
    std::string user_agent;                     // "" = "D2RVRLauncher/<version>"
    std::string accept;                         // "" = none; the GitHub API wants "application/vnd.github+json"
    std::string allow_http_host;                // the test switch above; starts as TestHttpHostFromProcessEnv()
    HttpOptions();
};

struct HttpOutcome {
    bool ok = false;
    bool cancelled = false;
    int status = 0;             // of the last response
    std::string error;          // for the player, one sentence; "" when ok
    std::string final_url;
    uint64_t bytes = 0;
};
using HttpSink = std::function<bool(const char* data, size_t len)>;   // false = stop (the call fails)
// GET with redirects. The body goes to `sink`; `progress` is told how much of how much (total 0 = the server did not say).
HttpOutcome HttpFetch(const std::string& url, const HttpOptions& opt, const HttpSink& sink, const ProgressFn& progress);

// ---- what the rest of the launcher uses
class HttpClient {                              // small text answers (JSON); an interface so tests can answer without a network
public:
    virtual ~HttpClient() = default;
    virtual bool GetText(const std::string& url, std::string* body, std::string* error) = 0;
};
std::unique_ptr<HttpClient> MakeHttpClient(const HttpOptions& opt);
bool HttpGetText(const std::string& url, const std::vector<std::string>& allow, uint64_t maxBytes, std::string* body, std::string* error);

class HttpDownloader : public Downloader {
public:
    explicit HttpDownloader(HttpOptions opt, ProgressFn progress = {});
    bool Fetch(const std::string& url, const fs::path& dest, std::string* error) override;
    void SetProgress(ProgressFn p) override { progress_ = std::move(p); }
    bool WasCancelled() const override { return cancelled_; }
    const std::string& last_sha256() const { return sha256_; }   // of the last successful Fetch
    uint64_t last_size() const { return size_; }
    HttpOptions& options() { return opt_; }

private:
    HttpOptions opt_;
    ProgressFn progress_;
    bool cancelled_ = false;
    std::string sha256_;
    uint64_t size_ = 0;
};

}  // namespace vrml
