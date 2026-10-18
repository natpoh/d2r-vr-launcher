// The rules of the network layer that need no network: what a URL is, which hosts are allowed, what the loopback test switch accepts.
// (The wire itself - redirects, size caps, cancel, hashes while streaming - is exercised against a local server by tools/install_smoke.py.)
#include "net.h"
#include "test.h"

using namespace vrml;

TEST(urls_are_taken_apart_the_strict_way) {
    UrlParts u = ParseUrl("https://d2rloader.net/downloads/D2RLoader-1.3.1-beta.zip");
    REQUIRE(u.ok);
    CHECK_EQ(u.scheme, std::string("https"));
    CHECK_EQ(u.host, std::string("d2rloader.net"));
    CHECK_EQ(u.port, 443);
    CHECK_EQ(u.path, std::string("/downloads/D2RLoader-1.3.1-beta.zip"));
    u = ParseUrl("HTTP://Example.COM:8080/a?b=c#frag");
    REQUIRE(u.ok);
    CHECK_EQ(u.host, std::string("example.com"));
    CHECK_EQ(u.port, 8080);
    CHECK_EQ(u.path, std::string("/a?b=c"));
    CHECK_EQ(u.hostport, std::string("example.com:8080"));
    u = ParseUrl("https://api.github.com?per_page=5");
    REQUIRE(u.ok);
    CHECK_EQ(u.path, std::string("/?per_page=5"));
    u = ParseUrl("http://[::1]:8123/x");
    REQUIRE(u.ok);
    CHECK_EQ(u.host, std::string("::1"));
    CHECK_EQ(u.port, 8123);
    CHECK_EQ(u.hostport, std::string("[::1]:8123"));

    for (const char* bad : {"", "ftp://x.com/a", "https:/x.com", "https://", "https:///path", "https://user@x.com/", "https://good.com@evil.com/", "https://x.com:0/",
                            "https://x.com:99999/", "https://x.com:abc/", "https://x .com/", "https://x.com\\evil/", "https://x.com/a b", "https://-x.com/", "https://x..com/ok",
                            "https://.x.com/", "https://x.com./", "https://x_y.com/", "https://x\xC3\xA9.com/", "javascript:alert(1)", "//x.com/a", "file:///c:/x"})
        CHECK(!ParseUrl(bad).ok);
}

TEST(the_allow_list_is_exact_or_a_subdomain_pattern) {
    const std::vector<std::string> allow = {"bodywalkvr.com", "*.githubusercontent.com", "Github.COM"};
    CHECK(HostAllowed("bodywalkvr.com", allow));
    CHECK(HostAllowed("BODYWALKVR.com", allow));
    CHECK(!HostAllowed("www.bodywalkvr.com", allow));              // a plain entry is that host only
    CHECK(!HostAllowed("evilbodywalkvr.com", allow));
    CHECK(!HostAllowed("bodywalkvr.com.evil.net", allow));
    CHECK(HostAllowed("objects.githubusercontent.com", allow));
    CHECK(HostAllowed("a.b.githubusercontent.com", allow));
    CHECK(!HostAllowed("githubusercontent.com", allow));           // "*." is subdomains only
    CHECK(!HostAllowed("evilgithubusercontent.com", allow));
    CHECK(HostAllowed("github.com", allow));
    CHECK(!HostAllowed("api.github.com", allow));
    CHECK(!HostAllowed("", allow));
    CHECK(!HostAllowed("anything.com", {}));
}

TEST(only_https_on_the_list_is_allowed) {
    const std::vector<std::string> allow = {"d2rloader.net"};
    std::string why;
    CHECK(UrlAllowed("https://d2rloader.net/x.zip", allow, "", &why));
    CHECK(!UrlAllowed("http://d2rloader.net/x.zip", allow, "", &why));
    CHECK(why.find("https") != std::string::npos);
    CHECK(!UrlAllowed("https://other.net/x.zip", allow, "", &why));
    CHECK(why.find("other.net") != std::string::npos);
    CHECK(!UrlAllowed("https://d2rloader.net@other.net/x.zip", allow, "", &why));
    CHECK(!UrlAllowed("https://d2rloader.net.other.net/x.zip", allow, "", &why));
    CHECK(!UrlAllowed("not a url", allow, "", &why));
}

TEST(the_loopback_switch_opens_exactly_one_host_and_port) {
    const std::vector<std::string> allow = {"d2rloader.net"};
    CHECK(UrlAllowed("http://127.0.0.1:8123/a.zip", allow, "127.0.0.1:8123"));
    CHECK(!UrlAllowed("http://127.0.0.1:8124/a.zip", allow, "127.0.0.1:8123"));      // another port
    CHECK(!UrlAllowed("http://localhost:8123/a.zip", allow, "127.0.0.1:8123"));      // another name for it
    CHECK(!UrlAllowed("http://127.0.0.1:8123/a.zip", allow, ""));                    // the switch is off
    CHECK(!UrlAllowed("http://127.0.0.1/a.zip", allow, "127.0.0.1:8123"));
    CHECK(UrlAllowed("https://d2rloader.net/a.zip", allow, "127.0.0.1:8123"));      // the real list still works

    auto env = [](const std::string& v) { return std::map<std::string, std::string>{{"VRML_ALLOW_HTTP_HOST", v}}; };
    CHECK_EQ(TestHttpHost(env("127.0.0.1:8123")), std::string("127.0.0.1:8123"));
    CHECK_EQ(TestHttpHost(env("LOCALHOST:80")), std::string("localhost:80"));
    CHECK_EQ(TestHttpHost(env("[::1]:9000")), std::string("[::1]:9000"));
    for (const char* bad : {"", "example.com:80", "0.0.0.0:80", "127.0.0.2:80", "192.168.1.5:80", "127.0.0.1", "127.0.0.1:", "127.0.0.1:0", "127.0.0.1:70000", "127.0.0.1:8a",
                            "127.0.0.1.evil.com:80", "http://127.0.0.1:80", "localhost.evil.com:80"})
        CHECK_EQ(TestHttpHost(env(bad)), std::string());
    CHECK_EQ(TestHttpHost(std::map<std::string, std::string>()), std::string());
    CHECK_EQ(TestHttpHost(std::map<std::string, std::string>{{"vrml_allow_http_host", "127.0.0.1:5"}}), std::string("127.0.0.1:5"));   // names compare like Windows does
}

TEST(a_fetch_of_a_refused_address_fails_before_any_network_use) {
    HttpOptions o;
    o.allow_hosts = {"d2rloader.net"};
    o.allow_http_host = "";
    int sunk = 0;
    HttpOutcome r = HttpFetch("https://evil.example/x.zip", o, [&](const char*, size_t) { ++sunk; return true; }, {});
    CHECK(!r.ok);
    CHECK(r.error.find("evil.example") != std::string::npos);
    r = HttpFetch("http://d2rloader.net/x.zip", o, [&](const char*, size_t) { ++sunk; return true; }, {});
    CHECK(!r.ok);
    CHECK_EQ(sunk, 0);
    std::string body, err;
    CHECK(!HttpGetText("https://evil.example/x.json", {"d2rloader.net"}, 1000, &body, &err));
    HttpDownloader dl(o);
    std::string e2;
    CHECK(!dl.Fetch("http://d2rloader.net/x.zip", fs::temp_directory_path() / "vrml_never_written.zip", &e2));
    CHECK(!IsFile(fs::temp_directory_path() / "vrml_never_written.zip"));
    CHECK(!dl.WasCancelled());
}

TEST_MAIN()
