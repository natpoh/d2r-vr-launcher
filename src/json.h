// json.h - a small JSON value + parser + writer (RFC 8259), for manifests, state and journals.
//
// Why our own: the repository has no JSON library, and the launcher is meant to live in its own
// repo with no dependencies. Objects keep their insertion order (stable files, readable diffs);
// a duplicate key is a parse error (a manifest with two "dst" is a typo, not a choice).
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace vrml {

struct JsonError : std::runtime_error {
    size_t line = 0, col = 0;
    JsonError(const std::string& m, size_t l, size_t c)
        : std::runtime_error(m + " (line " + std::to_string(l) + ", column " + std::to_string(c) + ")"), line(l), col(c) {}
};

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    using Member = std::pair<std::string, Json>;

    Json() = default;
    static Json Bool(bool v) { Json j; j.t_ = Type::Bool; j.b_ = v; return j; }
    static Json Number(double v) { Json j; j.t_ = Type::Number; j.n_ = v; return j; }
    static Json String(std::string v) { Json j; j.t_ = Type::String; j.s_ = std::move(v); return j; }
    static Json Array() { Json j; j.t_ = Type::Array; return j; }
    static Json Object() { Json j; j.t_ = Type::Object; return j; }

    Type type() const { return t_; }
    bool is_null() const { return t_ == Type::Null; }
    bool is_bool() const { return t_ == Type::Bool; }
    bool is_number() const { return t_ == Type::Number; }
    bool is_string() const { return t_ == Type::String; }
    bool is_array() const { return t_ == Type::Array; }
    bool is_object() const { return t_ == Type::Object; }
    bool is_integer() const;   // a number with no fraction, within int64

    bool as_bool(bool def = false) const { return t_ == Type::Bool ? b_ : def; }
    double as_number(double def = 0) const { return t_ == Type::Number ? n_ : def; }
    int64_t as_int(int64_t def = 0) const { return is_integer() ? static_cast<int64_t>(n_) : def; }
    const std::string& as_string() const { return t_ == Type::String ? s_ : empty_; }

    const std::vector<Json>& items() const { return arr_; }
    std::vector<Json>& items() { return arr_; }
    const std::vector<Member>& members() const { return obj_; }

    // object access
    const Json* find(const std::string& key) const;
    bool has(const std::string& key) const { return find(key) != nullptr; }
    Json& set(const std::string& key, Json v);   // replaces or appends (turns a Null into an Object)
    Json& operator[](const std::string& key);    // finds or appends a Null member
    // array access
    Json& push(Json v);                          // (turns a Null into an Array)
    size_t size() const { return t_ == Type::Array ? arr_.size() : t_ == Type::Object ? obj_.size() : 0; }

    // convenience getters with defaults
    std::string get_string(const std::string& key, const std::string& def = "") const;
    bool get_bool(const std::string& key, bool def = false) const;
    int64_t get_int(const std::string& key, int64_t def = 0) const;

    std::string dump(int indent = 2) const;      // indent < 0: one line
    static Json Parse(const std::string& text);  // throws JsonError

    bool operator==(const Json& o) const;
    bool operator!=(const Json& o) const { return !(*this == o); }

private:
    void write(std::string& out, int indent, int depth) const;

    Type t_ = Type::Null;
    bool b_ = false;
    double n_ = 0;
    std::string s_;
    std::vector<Json> arr_;
    std::vector<Member> obj_;
    static const std::string empty_;
};

}  // namespace vrml
