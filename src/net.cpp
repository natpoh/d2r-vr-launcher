#include "net.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>

#include "sha256.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#endif

namespace vrml {

// ------------------------------------------------------------------ URLs

UrlParts ParseUrl(const std::string& url) {
    UrlParts u;
    for (char c : url)
        if (static_cast<unsigned char>(c) <= 0x20 || c == '\\' || static_cast<unsigned char>(c) >= 0x7f) return u;   // spaces, controls, backslashes, non-ASCII
    const size_t sep = url.find("://");
    if (sep == std::string::npos) return u;
    u.scheme = Lower(url.substr(0, sep));
    if (u.scheme != "https" && u.scheme != "http") return u;
    size_t i = sep + 3;
    const size_t authEnd = url.find_first_of("/?#", i);
    const std::string auth = url.substr(i, (authEnd == std::string::npos ? url.size() : authEnd) - i);
    if (auth.empty() || auth.find('@') != std::string::npos) return u;      // no user info: "https://good.com@evil.com/" must not read as good.com
    std::string host, portText;
    if (auth[0] == '[') {
        const size_t close = auth.find(']');
        if (close == std::string::npos) return u;
        host = auth.substr(1, close - 1);
        if (close + 1 < auth.size()) {
            if (auth[close + 1] != ':') return u;
            portText = auth.substr(close + 2);
        }
        if (host.empty()) return u;
        for (char c : host)
            if (!(std::isxdigit(static_cast<unsigned char>(c)) || c == ':' || c == '.')) return u;
    } else {
        const size_t colon = auth.find(':');
        host = colon == std::string::npos ? auth : auth.substr(0, colon);
        if (colon != std::string::npos) portText = auth.substr(colon + 1);
        if (host.empty()) return u;
        for (char c : host)
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.')) return u;
        if (host.front() == '.' || host.back() == '.' || host.front() == '-' || host.find("..") != std::string::npos) return u;
    }
    int port = u.scheme == "https" ? 443 : 80;
    if (!portText.empty()) {
        if (portText.size() > 5) return u;
        long v = 0;
        for (char c : portText) {
            if (!std::isdigit(static_cast<unsigned char>(c))) return u;
            v = v * 10 + (c - '0');
        }
        if (v < 1 || v > 65535) return u;
        port = static_cast<int>(v);
    }
    u.host = Lower(host);
    u.port = port;
    u.hostport = Lower(auth);
    std::string rest = authEnd == std::string::npos ? std::string() : url.substr(authEnd);
    const size_t frag = rest.find('#');
    if (frag != std::string::npos) rest.resize(frag);
    if (rest.empty() || rest[0] == '?') rest = "/" + rest;
    u.path = rest;
    u.ok = true;
    return u;
}

bool HostAllowed(const std::string& host, const std::vector<std::string>& allow) {
    const std::string h = Lower(host);
    for (const std::string& a : allow) {
        const std::string e = Lower(a);
        if (e.size() > 2 && e[0] == '*' && e[1] == '.') {
            const std::string tail = e.substr(1);                       // ".domain"
            if (h.size() > tail.size() && EndsWith(h, tail)) return true;
        } else if (h == e) {
            return true;
        }
    }
    return false;
}

std::string TestHttpHost(const std::map<std::string, std::string>& env) {
    std::string v;
    for (const auto& kv : env)
        if (IEquals(kv.first, "VRML_ALLOW_HTTP_HOST")) v = kv.second;
    v = Lower(v);
    // only a loopback address with a port: a stray value of this variable can never open the way to a real server
    size_t portAt = std::string::npos;
    std::string host;
    if (StartsWith(v, "[::1]:")) { host = "[::1]"; portAt = 6; }
    else if (StartsWith(v, "127.0.0.1:")) { host = "127.0.0.1"; portAt = 10; }
    else if (StartsWith(v, "localhost:")) { host = "localhost"; portAt = 10; }
    if (portAt == std::string::npos) return "";
    const std::string port = v.substr(portAt);
    if (port.empty() || port.size() > 5) return "";
    long n = 0;
    for (char c : port) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return "";
        n = n * 10 + (c - '0');
    }
    if (n < 1 || n > 65535) return "";
    return host + ":" + port;
}

std::string TestHttpHostFromProcessEnv() {
    std::map<std::string, std::string> env;
#ifdef _WIN32
    wchar_t buf[256];
    const DWORD n = GetEnvironmentVariableW(L"VRML_ALLOW_HTTP_HOST", buf, 256);
    if (n > 0 && n < 256) env["VRML_ALLOW_HTTP_HOST"] = U(fs::path(std::wstring(buf, n)));
#else
    if (const char* e = std::getenv("VRML_ALLOW_HTTP_HOST")) env["VRML_ALLOW_HTTP_HOST"] = e;
#endif
    return TestHttpHost(env);
}

bool UrlAllowed(const std::string& url, const std::vector<std::string>& allow, const std::string& testHttpHost, std::string* why) {
    auto no = [&](const std::string& m) {
        if (why) *why = m;
        return false;
    };
    const UrlParts u = ParseUrl(url);
    if (!u.ok) return no("\"" + url.substr(0, 120) + "\" is not a web address the launcher accepts");
    if (u.scheme == "https") {
        if (!HostAllowed(u.host, allow)) return no(u.host + " is not a server the launcher downloads from");
        return true;
    }
    if (!testHttpHost.empty() && u.hostport == Lower(testHttpHost)) return true;
    return no("only https addresses are allowed (" + u.scheme + "://" + u.hostport + " is not)");
}

HttpOptions::HttpOptions() : allow_http_host(TestHttpHostFromProcessEnv()) {}

#ifdef _WIN32

namespace {

std::wstring Wide(const std::string& s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string Narrow(const std::wstring& w) {
    if (w.empty()) return "";
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string ErrText(DWORD code) {
    wchar_t buf[512] = {};
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, GetModuleHandleW(L"winhttp.dll"), code, 0, buf, 512, nullptr);
    std::string s = Narrow(std::wstring(buf, n));
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '.')) s.pop_back();
    return (s.empty() ? std::string("unknown error") : s) + " (error " + std::to_string(code) + ")";
}

struct Handle {
    HINTERNET h = nullptr;
    Handle() = default;
    explicit Handle(HINTERNET x) : h(x) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { if (h) WinHttpCloseHandle(h); }
};

std::string QueryHeader(HINTERNET req, DWORD level) {
    DWORD len = 0;
    WinHttpQueryHeaders(req, level, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &len, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || len == 0) return "";
    std::wstring w(len / sizeof(wchar_t) + 1, L'\0');
    if (!WinHttpQueryHeaders(req, level, WINHTTP_HEADER_NAME_BY_INDEX, w.data(), &len, WINHTTP_NO_HEADER_INDEX)) return "";
    w.resize(len / sizeof(wchar_t));
    return Narrow(w);
}

std::string ResolveLocation(const UrlParts& base, const std::string& loc) {
    if (StartsWith(loc, "http://") || StartsWith(loc, "https://")) return loc;
    if (StartsWith(loc, "//")) return base.scheme + ":" + loc;
    if (!loc.empty() && loc[0] == '/') return base.scheme + "://" + base.hostport + loc;
    std::string dir = base.path.substr(0, base.path.find('?'));
    dir.resize(dir.find_last_of('/') + 1);
    return base.scheme + "://" + base.hostport + dir + loc;
}

}  // namespace

HttpOutcome HttpFetch(const std::string& url, const HttpOptions& opt, const HttpSink& sink, const ProgressFn& progress) {
    HttpOutcome out;
    auto fail = [&](const std::string& m) {
        out.ok = false;
        out.error = m;
        return out;
    };
    const std::string ua = opt.user_agent.empty() ? std::string("D2RVRLauncher/") + kLauncherVersion : opt.user_agent;
    std::string cur = url;
    for (int hop = 0; hop <= opt.max_redirects; ++hop) {
        std::string why;
        if (!UrlAllowed(cur, opt.allow_hosts, opt.allow_http_host, &why)) return fail(hop == 0 ? why : "a redirect led off the list of servers: " + why);
        const UrlParts u = ParseUrl(cur);
        const bool secure = u.scheme == "https";
        out.final_url = cur;

        const bool plainTest = !secure;      // the loopback test server: no proxy
        Handle ses(WinHttpOpen(Wide(ua).c_str(), plainTest ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (!ses.h) ses.h = WinHttpOpen(Wide(ua).c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!ses.h) return fail("cannot start the network layer: " + ErrText(GetLastError()));
        WinHttpSetTimeouts(ses.h, 15000, opt.connect_timeout_ms, 30000, opt.receive_timeout_ms);
        if (secure) {
            DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | 0x00002000 /* TLS 1.3 */;
            if (!WinHttpSetOption(ses.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols)) {
                protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
                WinHttpSetOption(ses.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
            }
        }
        Handle con(WinHttpConnect(ses.h, Wide(u.host).c_str(), static_cast<INTERNET_PORT>(u.port), 0));
        if (!con.h) return fail("cannot connect to " + u.host + ": " + ErrText(GetLastError()));
        Handle req(WinHttpOpenRequest(con.h, L"GET", Wide(u.path).c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0));
        if (!req.h) return fail("cannot start the request to " + u.host + ": " + ErrText(GetLastError()));
        // redirects are followed here, one by one, so that each target is checked against the list; no cookies, no stored logins
        DWORD disable = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
        WinHttpSetOption(req.h, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof disable);
        std::wstring headers;
        if (!opt.accept.empty()) headers += L"Accept: " + Wide(opt.accept) + L"\r\n";
        if (!WinHttpSendRequest(req.h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), headers.empty() ? 0 : static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
            return fail("cannot reach " + u.host + ": " + ErrText(GetLastError()));
        if (!WinHttpReceiveResponse(req.h, nullptr)) return fail("no answer from " + u.host + ": " + ErrText(GetLastError()));

        DWORD status = 0, sz = sizeof status;
        if (!WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX))
            return fail("unreadable answer from " + u.host);
        out.status = static_cast<int>(status);

        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            const std::string loc = QueryHeader(req.h, WINHTTP_QUERY_LOCATION);
            if (loc.empty()) return fail(u.host + " sent a redirect without a target");
            if (hop == opt.max_redirects) return fail("too many redirects (more than " + std::to_string(opt.max_redirects) + ")");
            cur = ResolveLocation(u, loc);
            continue;
        }
        if (status != 200) {
            const char* hint = (status == 403 || status == 429) ? " (too many questions, or no access: try again later)" : "";
            return fail(u.host + " answered HTTP " + std::to_string(status) + hint);
        }

        uint64_t total = 0;
        const std::string cl = QueryHeader(req.h, WINHTTP_QUERY_CONTENT_LENGTH);
        bool haveLen = !cl.empty();
        for (char c : cl)
            if (!std::isdigit(static_cast<unsigned char>(c))) haveLen = false;
        if (haveLen && cl.size() <= 18) total = std::strtoull(cl.c_str(), nullptr, 10);
        else haveLen = false;
        if (haveLen && total > opt.max_bytes) return fail("the file is " + std::to_string(total) + " bytes, more than the " + std::to_string(opt.max_bytes) + " the launcher allows here");

        std::string buf(64 * 1024, '\0');
        uint64_t done = 0;
        for (;;) {
            DWORD got = 0;
            if (!WinHttpReadData(req.h, buf.data(), static_cast<DWORD>(buf.size()), &got)) return fail("the download broke off: " + ErrText(GetLastError()));
            if (got == 0) break;
            done += got;
            out.bytes = done;
            if (done > opt.max_bytes) return fail("the download is larger than the " + std::to_string(opt.max_bytes) + " bytes the launcher allows here");
            if (haveLen && done > total) return fail("the server sent more than it announced");
            if (!sink(buf.data(), got)) return fail("the data could not be stored");
            if (progress && !progress(done, haveLen ? total : 0)) {
                out.cancelled = true;
                return fail("cancelled");
            }
        }
        if (haveLen && done != total) return fail("the download is incomplete (" + std::to_string(done) + " of " + std::to_string(total) + " bytes)");
        out.ok = true;
        return out;
    }
    return fail("too many redirects");
}

#else  // not Windows: the launcher is a Windows program; the stub keeps the library building and says so

HttpOutcome HttpFetch(const std::string& url, const HttpOptions& opt, const HttpSink&, const ProgressFn&) {
    HttpOutcome out;
    std::string why;
    out.error = UrlAllowed(url, opt.allow_hosts, opt.allow_http_host, &why) ? "network access is implemented for Windows only" : why;
    return out;
}

#endif

// ------------------------------------------------------------------ what the rest of the launcher uses

namespace {

class RealHttpClient : public HttpClient {
public:
    explicit RealHttpClient(HttpOptions o) : opt_(std::move(o)) {}
    bool GetText(const std::string& url, std::string* body, std::string* error) override {
        std::string text;
        const HttpOutcome r = HttpFetch(url, opt_, [&](const char* d, size_t n) { text.append(d, n); return true; }, {});
        if (!r.ok) {
            if (error) *error = r.error;
            return false;
        }
        if (body) *body = std::move(text);
        return true;
    }

private:
    HttpOptions opt_;
};

}  // namespace

std::unique_ptr<HttpClient> MakeHttpClient(const HttpOptions& opt) { return std::make_unique<RealHttpClient>(opt); }

bool HttpGetText(const std::string& url, const std::vector<std::string>& allow, uint64_t maxBytes, std::string* body, std::string* error) {
    HttpOptions o;
    o.allow_hosts = allow;
    o.max_bytes = maxBytes;
    return RealHttpClient(o).GetText(url, body, error);
}

HttpDownloader::HttpDownloader(HttpOptions opt, ProgressFn progress) : opt_(std::move(opt)), progress_(std::move(progress)) {}

bool HttpDownloader::Fetch(const std::string& url, const fs::path& dest, std::string* error) {
    cancelled_ = false;
    sha256_.clear();
    size_ = 0;
    auto fail = [&](const std::string& m) {
        if (error) *error = m;
        return false;
    };
    std::string why;
    if (!UrlAllowed(url, opt_.allow_hosts, opt_.allow_http_host, &why)) return fail(why);
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    fs::path part = dest;
    part += ".vrml-part";
    RemoveQuiet(part);
    std::ofstream f(part, std::ios::binary | std::ios::trunc);
    if (!f) return fail("cannot write " + U(part));
    Sha256 hash;
    bool writeFailed = false;
    const HttpOutcome r = HttpFetch(url, opt_,
        [&](const char* d, size_t n) {
            hash.update(d, n);
            f.write(d, static_cast<std::streamsize>(n));
            if (!f) writeFailed = true;
            return !writeFailed;
        },
        progress_);
    f.flush();
    const bool closedOk = static_cast<bool>(f);
    f.close();
    if (!r.ok || writeFailed || !closedOk) {
        RemoveQuiet(part);
        cancelled_ = r.cancelled;
        return fail(r.ok ? "cannot write " + U(part) + " (the disk is full or the folder is protected)" : r.error);
    }
    fs::rename(part, dest, ec);
    if (ec) {
        RemoveQuiet(part);
        return fail("cannot place " + U(dest) + ": " + ec.message());
    }
    sha256_ = hash.hex();
    size_ = r.bytes;
    return true;
}

}  // namespace vrml
