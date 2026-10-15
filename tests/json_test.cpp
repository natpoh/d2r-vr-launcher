#include "json.h"
#include "test.h"

using vrml::Json;
using vrml::JsonError;

static bool Fails(const std::string& text) {
    try { Json::Parse(text); } catch (const JsonError&) { return true; }
    return false;
}

TEST(parses_values) {
    Json j = Json::Parse(R"({"a": 1, "b": [true, false, null, "x"], "c": {"d": -2.5e1}, "s": "h\u00e9llo\n"})");
    CHECK(j.is_object());
    CHECK_EQ(j.get_int("a"), 1);
    CHECK_EQ(j.find("b")->items().size(), size_t(4));
    CHECK(j.find("b")->items()[0].as_bool());
    CHECK(j.find("b")->items()[2].is_null());
    CHECK_EQ(j.find("c")->find("d")->as_number(), -25.0);
    CHECK_EQ(j.get_string("s"), std::string("h\xC3\xA9llo\n"));
}

TEST(keeps_member_order) {
    Json j = Json::Parse(R"({"z": 1, "a": 2, "m": 3})");
    CHECK_EQ(j.members()[0].first, std::string("z"));
    CHECK_EQ(j.members()[1].first, std::string("a"));
    CHECK_EQ(j.members()[2].first, std::string("m"));
}

TEST(round_trips) {
    const std::string src = R"({"id":"x","n":[1,2,{"k":"v\"q\\"}],"f":1.5,"t":true,"u":null})";
    Json a = Json::Parse(src);
    Json b = Json::Parse(a.dump());
    CHECK(a == b);
    CHECK_EQ(a.dump(-1), src);
}

TEST(surrogate_pairs) {
    Json j = Json::Parse(R"("\ud83d\ude00")");
    CHECK_EQ(j.as_string(), std::string("\xF0\x9F\x98\x80"));
    CHECK(Fails(R"("\ud83d")"));      // a lone high surrogate
    CHECK(Fails(R"("\ude00")"));      // a lone low surrogate
}

TEST(rejects_bad_json) {
    CHECK(Fails(""));
    CHECK(Fails("{"));
    CHECK(Fails(R"({"a": 1,})"));          // trailing comma
    CHECK(Fails(R"([1, 2,])"));
    CHECK(Fails(R"({"a": 1, "a": 2})"));   // duplicate key
    CHECK(Fails(R"({'a': 1})"));
    CHECK(Fails(R"({"a": 01})"));
    CHECK(Fails(R"({"a": 1} x)"));
    CHECK(Fails("\"a\nb\""));              // a raw newline in a string
    CHECK(Fails(R"("\q")"));
    CHECK(Fails(R"({"a": tru})"));
}

TEST(depth_limit) {
    std::string deep(100, '[');
    deep += std::string(100, ']');
    CHECK(Fails(deep));
}

TEST(error_has_position) {
    try {
        Json::Parse("{\n  \"a\": ?\n}");
        CHECK(false);
    } catch (const JsonError& e) {
        CHECK_EQ(e.line, size_t(2));
        CHECK(e.col >= 8);
    }
}

TEST(bom_is_skipped) {
    CHECK_EQ(Json::Parse("\xEF\xBB\xBF{\"a\":1}").get_int("a"), 1);
}

TEST(setters) {
    Json j;
    j["a"] = Json::Number(1);
    j.set("b", Json::String("x"));
    j.set("a", Json::Number(2));
    CHECK_EQ(j.get_int("a"), 2);
    CHECK_EQ(j.size(), size_t(2));
    Json arr;
    arr.push(Json::Bool(true));
    CHECK(arr.is_array());
}

TEST_MAIN()
