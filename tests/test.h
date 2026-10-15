// test.h - a very small test harness (no framework: the launcher has no dependencies).
//   TEST(name) { CHECK(x == y); CHECK_EQ(a, b); CHECK_ERROR(code, expr); REQUIRE(ptr); }
// CHECK records a failure and goes on; REQUIRE stops the test. main() runs every TEST (or those whose name contains argv[1]).
#pragma once

#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "util.h"

namespace t {

struct Case {
    std::string name;
    std::function<void()> fn;
};
inline std::vector<Case>& Cases() {
    static std::vector<Case> c;
    return c;
}
inline int& Failures() {
    static int n = 0;
    return n;
}
struct Reg {
    Reg(const char* n, std::function<void()> f) { Cases().push_back({n, std::move(f)}); }
};
struct Stop : std::exception {};

inline void Fail(const char* file, int line, const std::string& msg) {
    ++Failures();
    std::cerr << file << ":" << line << ": FAILED " << msg << "\n";
}

template <class A, class B>
inline void CheckEq(const char* file, int line, const char* ea, const char* eb, const A& a, const B& b) {
    if (a == b) return;
    std::ostringstream ss;
    ss << ea << " == " << eb << "   (" << a << " vs " << b << ")";
    Fail(file, line, ss.str());
}

inline int RunAll(int argc, char** argv) {
    int ran = 0;
    for (const Case& c : Cases()) {
        if (argc > 1 && c.name.find(argv[1]) == std::string::npos) continue;
        ++ran;
        const int before = Failures();
        try {
            c.fn();
        } catch (const Stop&) {
        } catch (const vrml::Error& e) {
            Fail(c.name.c_str(), 0, std::string("unexpected error [") + e.code + "]: " + e.what());
        } catch (const std::exception& e) {
            Fail(c.name.c_str(), 0, std::string("unexpected exception: ") + e.what());
        }
        std::cerr << (Failures() == before ? "  ok    " : "  FAIL  ") << c.name << "\n";
    }
    std::cerr << ran << " test(s), " << Failures() << " failure(s)\n";
    return Failures() ? 1 : 0;
}

}  // namespace t

#define TEST(name)                                  \
    static void name();                             \
    static t::Reg reg_##name(#name, name);          \
    static void name()

#define CHECK(cond) \
    do { if (!(cond)) t::Fail(__FILE__, __LINE__, #cond); } while (0)
#define CHECK_EQ(a, b) t::CheckEq(__FILE__, __LINE__, #a, #b, (a), (b))
#define REQUIRE(cond) \
    do { if (!(cond)) { t::Fail(__FILE__, __LINE__, "REQUIRE " #cond); throw t::Stop(); } } while (0)
// the expression must throw vrml::Error with this code
#define CHECK_ERROR(codeStr, expr)                                                                                    \
    do {                                                                                                              \
        bool thrown_ = false;                                                                                         \
        try { expr; }                                                                                                 \
        catch (const vrml::Error& e_) {                                                                               \
            thrown_ = true;                                                                                           \
            if (e_.code != (codeStr)) t::Fail(__FILE__, __LINE__, std::string(#expr " threw ") + e_.code + " (" + e_.what() + "), wanted " codeStr); \
        }                                                                                                             \
        if (!thrown_) t::Fail(__FILE__, __LINE__, #expr " did not throw " codeStr);                                   \
    } while (0)

#define TEST_MAIN() \
    int main(int argc, char** argv) { return t::RunAll(argc, argv); }
