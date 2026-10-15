#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace vrml {

const std::string Json::empty_;

bool Json::is_integer() const {
    if (t_ != Type::Number) return false;
    if (!std::isfinite(n_) || std::floor(n_) != n_) return false;
    return n_ >= -9.2e18 && n_ <= 9.2e18;
}

const Json* Json::find(const std::string& key) const {
    if (t_ != Type::Object) return nullptr;
    for (const Member& m : obj_)
        if (m.first == key) return &m.second;
    return nullptr;
}

Json& Json::set(const std::string& key, Json v) {
    if (t_ == Type::Null) t_ = Type::Object;
    for (Member& m : obj_)
        if (m.first == key) { m.second = std::move(v); return m.second; }
    obj_.emplace_back(key, std::move(v));
    return obj_.back().second;
}

Json& Json::operator[](const std::string& key) {
    if (t_ == Type::Null) t_ = Type::Object;
    for (Member& m : obj_)
        if (m.first == key) return m.second;
    obj_.emplace_back(key, Json());
    return obj_.back().second;
}

Json& Json::push(Json v) {
    if (t_ == Type::Null) t_ = Type::Array;
    arr_.push_back(std::move(v));
    return arr_.back();
}

std::string Json::get_string(const std::string& key, const std::string& def) const {
    const Json* j = find(key);
    return j && j->is_string() ? j->as_string() : def;
}
bool Json::get_bool(const std::string& key, bool def) const {
    const Json* j = find(key);
    return j && j->is_bool() ? j->as_bool() : def;
}
int64_t Json::get_int(const std::string& key, int64_t def) const {
    const Json* j = find(key);
    return j && j->is_integer() ? j->as_int() : def;
}

bool Json::operator==(const Json& o) const {
    if (t_ != o.t_) return false;
    switch (t_) {
        case Type::Null: return true;
        case Type::Bool: return b_ == o.b_;
        case Type::Number: return n_ == o.n_;
        case Type::String: return s_ == o.s_;
        case Type::Array: return arr_ == o.arr_;
        case Type::Object: return obj_ == o.obj_;
    }
    return false;
}

// ------------------------------------------------------------------ writer

static void WriteString(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);   // UTF-8 passes through
                }
        }
    }
    out += '"';
}

void Json::write(std::string& out, int indent, int depth) const {
    auto nl = [&](int d) {
        if (indent < 0) return;
        out += '\n';
        out.append(static_cast<size_t>(indent) * static_cast<size_t>(d), ' ');
    };
    switch (t_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += b_ ? "true" : "false"; break;
        case Type::Number: {
            char buf[40];
            if (is_integer()) std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(n_));
            else std::snprintf(buf, sizeof buf, "%.17g", n_);
            out += buf;
            break;
        }
        case Type::String: WriteString(out, s_); break;
        case Type::Array:
            if (arr_.empty()) { out += "[]"; break; }
            out += '[';
            for (size_t i = 0; i < arr_.size(); ++i) {
                if (i) out += ',';
                nl(depth + 1);
                arr_[i].write(out, indent, depth + 1);
            }
            nl(depth);
            out += ']';
            break;
        case Type::Object:
            if (obj_.empty()) { out += "{}"; break; }
            out += '{';
            for (size_t i = 0; i < obj_.size(); ++i) {
                if (i) out += ',';
                nl(depth + 1);
                WriteString(out, obj_[i].first);
                out += indent < 0 ? ":" : ": ";
                obj_[i].second.write(out, indent, depth + 1);
            }
            nl(depth);
            out += '}';
            break;
    }
}

std::string Json::dump(int indent) const {
    std::string out;
    write(out, indent, 0);
    if (indent >= 0) out += '\n';
    return out;
}

// ------------------------------------------------------------------ parser

namespace {

class Parser {
public:
    explicit Parser(const std::string& t) : s_(t) {
        if (s_.size() >= 3 && static_cast<unsigned char>(s_[0]) == 0xEF && static_cast<unsigned char>(s_[1]) == 0xBB &&
            static_cast<unsigned char>(s_[2]) == 0xBF)
            i_ = 3;   // a UTF-8 BOM
    }

    Json Run() {
        Json v = Value(0);
        Ws();
        if (i_ != s_.size()) Fail("unexpected data after the value");
        return v;
    }

private:
    [[noreturn]] void Fail(const std::string& m) const {
        size_t line = 1, col = 1;
        for (size_t k = 0; k < i_ && k < s_.size(); ++k) {
            if (s_[k] == '\n') { ++line; col = 1; }
            else ++col;
        }
        throw JsonError(m, line, col);
    }
    void Ws() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_;
    }
    bool Eat(char c) {
        if (i_ < s_.size() && s_[i_] == c) { ++i_; return true; }
        return false;
    }
    void Expect(const char* word) {
        for (const char* p = word; *p; ++p)
            if (!Eat(*p)) Fail(std::string("expected '") + word + "'");
    }

    Json Value(int depth) {
        if (depth > 64) Fail("nested too deeply");
        Ws();
        if (i_ >= s_.size()) Fail("unexpected end");
        char c = s_[i_];
        if (c == '{') return Object(depth);
        if (c == '[') return Array(depth);
        if (c == '"') return Json::String(Str());
        if (c == 't') { Expect("true"); return Json::Bool(true); }
        if (c == 'f') { Expect("false"); return Json::Bool(false); }
        if (c == 'n') { Expect("null"); return Json(); }
        if (c == '-' || (c >= '0' && c <= '9')) return Num();
        Fail(std::string("unexpected character '") + c + "'");
    }

    Json Object(int depth) {
        ++i_;
        Json o = Json::Object();
        Ws();
        if (Eat('}')) return o;
        for (;;) {
            Ws();
            if (i_ >= s_.size() || s_[i_] != '"') Fail("expected a string key");
            std::string k = Str();
            if (o.has(k)) Fail("duplicate key \"" + k + "\"");
            Ws();
            if (!Eat(':')) Fail("expected ':'");
            o.set(k, Value(depth + 1));
            Ws();
            if (Eat(',')) continue;
            if (Eat('}')) return o;
            Fail("expected ',' or '}'");
        }
    }

    Json Array(int depth) {
        ++i_;
        Json a = Json::Array();
        Ws();
        if (Eat(']')) return a;
        for (;;) {
            a.push(Value(depth + 1));
            Ws();
            if (Eat(',')) continue;
            if (Eat(']')) return a;
            Fail("expected ',' or ']'");
        }
    }

    unsigned Hex4() {
        unsigned v = 0;
        for (int k = 0; k < 4; ++k) {
            if (i_ >= s_.size()) Fail("bad \\u escape");
            char c = s_[i_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
            else Fail("bad \\u escape");
        }
        return v;
    }

    static void Utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    std::string Str() {
        ++i_;   // opening quote
        std::string out;
        for (;;) {
            if (i_ >= s_.size()) Fail("unterminated string");
            unsigned char c = static_cast<unsigned char>(s_[i_++]);
            if (c == '"') return out;
            if (c < 0x20) { --i_; Fail("control character in a string"); }
            if (c != '\\') { out += static_cast<char>(c); continue; }
            if (i_ >= s_.size()) Fail("unterminated escape");
            char e = s_[i_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp = Hex4();
                    if (cp >= 0xD800 && cp < 0xDC00) {   // a high surrogate: the low one must follow
                        if (!(Eat('\\') && Eat('u'))) Fail("lone surrogate");
                        unsigned lo = Hex4();
                        if (lo < 0xDC00 || lo > 0xDFFF) Fail("bad surrogate pair");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        Fail("lone surrogate");
                    }
                    Utf8(out, cp);
                    break;
                }
                default: --i_; Fail("bad escape");
            }
        }
    }

    Json Num() {
        size_t b = i_;
        Eat('-');
        if (i_ >= s_.size()) Fail("bad number");
        if (s_[i_] == '0') ++i_;
        else if (s_[i_] >= '1' && s_[i_] <= '9') { while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_; }
        else Fail("bad number");
        if (Eat('.')) {
            size_t d = i_;
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
            if (i_ == d) Fail("bad number");
        }
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            ++i_;
            if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
            size_t d = i_;
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
            if (i_ == d) Fail("bad number");
        }
        return Json::Number(std::strtod(s_.substr(b, i_ - b).c_str(), nullptr));
    }

    const std::string& s_;
    size_t i_ = 0;
};

}  // namespace

Json Json::Parse(const std::string& text) { return Parser(text).Run(); }

}  // namespace vrml
