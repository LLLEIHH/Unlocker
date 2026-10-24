#include "ipc/Json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ul {
namespace json {
namespace {

struct Parser {
    const std::string& s;
    size_t i = 0;
    std::string err;

    explicit Parser(const std::string& text) : s(text) {}

    void SkipWs() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
    }

    bool Fail(const char* msg) {
        if (err.empty()) err = std::string(msg) + " @" + std::to_string(i);
        return false;
    }

    bool ParseValue(Value* out);

    bool ParseString(std::string* out) {
        if (i >= s.size() || s[i] != '"') return Fail("expect string");
        ++i;
        out->clear();
        while (i < s.size()) {
            const char c = s[i++];
            if (c == '"') return true;
            if (c != '\\') { out->push_back(c); continue; }
            if (i >= s.size()) return Fail("bad escape");
            const char e = s[i++];
            switch (e) {
                case '"':  out->push_back('"'); break;
                case '\\': out->push_back('\\'); break;
                case '/':  out->push_back('/'); break;
                case 'b':  out->push_back('\b'); break;
                case 'f':  out->push_back('\f'); break;
                case 'n':  out->push_back('\n'); break;
                case 'r':  out->push_back('\r'); break;
                case 't':  out->push_back('\t'); break;
                case 'u': {
                    if (i + 4 > s.size()) return Fail("bad \\u");
                    unsigned cp = 0;
                    for (int k = 0; k < 4; ++k) {
                        const char h = s[i + k];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                        else return Fail("bad \\u digit");
                    }
                    i += 4;
                    // UTF-8 编码（含代理对的最小处理：单独代理项直接按码位输出会坏，这里退化为 '?'）
                    if (cp >= 0xD800 && cp <= 0xDFFF) {
                        out->push_back('?');
                    } else if (cp < 0x80) {
                        out->push_back(static_cast<char>(cp));
                    } else if (cp < 0x800) {
                        out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
                        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    } else {
                        out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
                        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    }
                    break;
                }
                default: return Fail("unknown escape");
            }
        }
        return Fail("unterminated string");
    }

    bool ParseNumber(double* out) {
        const char* start = s.c_str() + i;
        char* end = nullptr;
        const double v = std::strtod(start, &end);
        if (end == start) return Fail("bad number");
        i += static_cast<size_t>(end - start);
        *out = v;
        return true;
    }
};

bool Parser::ParseValue(Value* out) {
    SkipWs();
    if (i >= s.size()) return Fail("unexpected end");
    const char c = s[i];

    if (c == '{') {
        ++i;
        out->type = Value::Type::Object;
        SkipWs();
        if (i < s.size() && s[i] == '}') { ++i; return true; }
        for (;;) {
            SkipWs();
            std::string key;
            if (!ParseString(&key)) return false;
            SkipWs();
            if (i >= s.size() || s[i] != ':') return Fail("expect ':'");
            ++i;
            Value v;
            if (!ParseValue(&v)) return false;
            out->obj.emplace_back(key, std::move(v));
            SkipWs();
            if (i < s.size() && s[i] == ',') { ++i; continue; }
            if (i < s.size() && s[i] == '}') { ++i; return true; }
            return Fail("expect ',' or '}'");
        }
    }
    if (c == '[') {
        ++i;
        out->type = Value::Type::Array;
        SkipWs();
        if (i < s.size() && s[i] == ']') { ++i; return true; }
        for (;;) {
            Value v;
            if (!ParseValue(&v)) return false;
            out->arr.push_back(std::move(v));
            SkipWs();
            if (i < s.size() && s[i] == ',') { ++i; continue; }
            if (i < s.size() && s[i] == ']') { ++i; return true; }
            return Fail("expect ',' or ']'");
        }
    }
    if (c == '"') {
        out->type = Value::Type::String;
        return ParseString(&out->str);
    }
    if (s.compare(i, 4, "true") == 0)  { i += 4; out->type = Value::Type::Bool; out->b = true;  return true; }
    if (s.compare(i, 5, "false") == 0) { i += 5; out->type = Value::Type::Bool; out->b = false; return true; }
    if (s.compare(i, 4, "null") == 0)  { i += 4; out->type = Value::Type::Null; return true; }

    out->type = Value::Type::Number;
    return ParseNumber(&out->num);
}

void Escape(std::string& out, const std::string& s) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04X", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));   // 非 ASCII 直接输出 UTF-8
                }
                break;
        }
    }
    out.push_back('"');
}

}  // namespace

Value Value::MakeString(std::string v) { Value o; o.type = Type::String; o.str = std::move(v); return o; }
Value Value::MakeNumber(double v)      { Value o; o.type = Type::Number; o.num = v; return o; }
Value Value::MakeBool(bool v)          { Value o; o.type = Type::Bool; o.b = v; return o; }
Value Value::MakeArray()               { Value o; o.type = Type::Array; return o; }
Value Value::MakeObject()              { Value o; o.type = Type::Object; return o; }

void Value::Set(const std::string& key, Value v) {
    if (type != Type::Object) type = Type::Object;
    for (auto& kv : obj) {
        if (kv.first == key) { kv.second = std::move(v); return; }
    }
    obj.emplace_back(key, std::move(v));
}

const Value* Value::Find(const std::string& key) const {
    for (const auto& kv : obj) if (kv.first == key) return &kv.second;
    return nullptr;
}

std::string Value::GetString(const std::string& key, const std::string& def) const {
    const Value* v = Find(key);
    return (v && v->type == Type::String) ? v->str : def;
}

double Value::GetNumber(const std::string& key, double def) const {
    const Value* v = Find(key);
    return (v && v->type == Type::Number) ? v->num : def;
}

bool Value::GetBool(const std::string& key, bool def) const {
    const Value* v = Find(key);
    return (v && v->type == Type::Bool) ? v->b : def;
}

bool Parse(const std::string& text, Value* out, std::string* err) {
    Parser p(text);
    Value v;
    if (!p.ParseValue(&v)) {
        if (err) *err = p.err;
        return false;
    }
    if (out) *out = std::move(v);
    return true;
}

std::string Dump(const Value& v) {
    std::string out;
    switch (v.type) {
        case Value::Type::Null:   return "null";
        case Value::Type::Bool:   return v.b ? "true" : "false";
        case Value::Type::Number: {
            char buf[64];
            if (v.num == std::floor(v.num) && std::fabs(v.num) < 1e15) {
                std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v.num));
            } else {
                std::snprintf(buf, sizeof(buf), "%.17g", v.num);
            }
            return buf;
        }
        case Value::Type::String: Escape(out, v.str); return out;
        case Value::Type::Array: {
            out.push_back('[');
            for (size_t i = 0; i < v.arr.size(); ++i) {
                if (i) out.push_back(',');
                out += Dump(v.arr[i]);
            }
            out.push_back(']');
            return out;
        }
        case Value::Type::Object: {
            out.push_back('{');
            for (size_t i = 0; i < v.obj.size(); ++i) {
                if (i) out.push_back(',');
                Escape(out, v.obj[i].first);
                out.push_back(':');
                out += Dump(v.obj[i].second);
            }
            out.push_back('}');
            return out;
        }
    }
    return "null";
}

}  // namespace json
}  // namespace ul
