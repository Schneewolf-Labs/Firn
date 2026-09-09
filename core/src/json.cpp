#include "firn/json.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace firn::json {

namespace {
const Value kNull;

struct Parser {
    const std::string& s;
    size_t p = 0;
    std::string err;
    void ws() { while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) ++p; }
    bool fail(const char* m) { if (err.empty()) err = std::string(m) + " at " + std::to_string(p); return false; }
    bool value(Value& v) {
        ws();
        if (p >= s.size()) return fail("unexpected end");
        const char c = s[p];
        if (c == '{') return object(v);
        if (c == '[') return array(v);
        if (c == '"') { v = Value::string(""); return string(v.str); }
        if (s.compare(p, 4, "true") == 0) { p += 4; v = Value::boolean(true); return true; }
        if (s.compare(p, 5, "false") == 0) { p += 5; v = Value::boolean(false); return true; }
        if (s.compare(p, 4, "null") == 0) { p += 4; v = Value::null(); return true; }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
            char* end = nullptr;
            const double d = std::strtod(s.c_str() + p, &end);
            if (end == s.c_str() + p) return fail("bad number");
            p = static_cast<size_t>(end - s.c_str());
            v = Value::number(d);
            return true;
        }
        return fail("unexpected character");
    }
    bool string(std::string& out) {
        if (s[p] != '"') return fail("expected string");
        ++p;
        while (p < s.size()) {
            const char c = s[p++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (p >= s.size()) break;
            const char e = s[p++];
            switch (e) {
                case 'n': out += '\n'; break; case 't': out += '\t'; break; case 'r': out += '\r'; break;
                case 'b': out += '\b'; break; case 'f': out += '\f'; break;
                case 'u': {
                    if (p + 4 > s.size()) return fail("bad escape");
                    const unsigned cp = static_cast<unsigned>(std::strtoul(s.substr(p, 4).c_str(), nullptr, 16));
                    p += 4;
                    if (cp < 0x80) out += static_cast<char>(cp);
                    else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
                    else { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
                    break;
                }
                default: out += e; break;
            }
        }
        return fail("unterminated string");
    }
    bool array(Value& v) {
        v = Value::array();
        ++p; ws();
        if (p < s.size() && s[p] == ']') { ++p; return true; }
        while (true) {
            Value item;
            if (!value(item)) return false;
            v.arr.push_back(std::move(item));
            ws();
            if (p < s.size() && s[p] == ',') { ++p; continue; }
            if (p < s.size() && s[p] == ']') { ++p; return true; }
            return fail("expected , or ]");
        }
    }
    bool object(Value& v) {
        v = Value::object();
        ++p; ws();
        if (p < s.size() && s[p] == '}') { ++p; return true; }
        while (true) {
            ws();
            std::string key;
            if (p >= s.size() || s[p] != '"' || !string(key)) return fail("expected key");
            ws();
            if (p >= s.size() || s[p] != ':') return fail("expected :");
            ++p;
            Value item;
            if (!value(item)) return false;
            v.obj.emplace_back(std::move(key), std::move(item));
            ws();
            if (p < s.size() && s[p] == ',') { ++p; continue; }
            if (p < s.size() && s[p] == '}') { ++p; return true; }
            return fail("expected , or }");
        }
    }
};

void dump_string(const std::string& s, std::string& out) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break; case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break; case '\t': out += "\\t"; break; case '\r': out += "\\r"; break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
                else out += static_cast<char>(c);
        }
    }
    out += '"';
}
}  // namespace

const Value* Value::find(const std::string& key) const {
    if (type != Type::Object) return nullptr;
    for (const auto& kv : obj) if (kv.first == key) return &kv.second;
    return nullptr;
}

const Value& Value::get(const std::string& path) const {
    const Value* cur = this;
    size_t start = 0;
    while (cur) {
        const size_t dot = path.find('.', start);
        const std::string key = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        cur = cur->find(key);
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return cur ? *cur : kNull;
}

Value& Value::set(const std::string& key, Value v) {
    if (type != Type::Object) { *this = Value::object(); }
    for (auto& kv : obj) if (kv.first == key) { kv.second = std::move(v); return kv.second; }
    obj.emplace_back(key, std::move(v));
    return obj.back().second;
}

double Value::as_number(double def) const {
    switch (type) {
        case Type::Number: return num;
        case Type::Bool: return b ? 1.0 : 0.0;
        case Type::String: { char* end = nullptr; const double d = std::strtod(str.c_str(), &end); return end != str.c_str() ? d : def; }
        default: return def;
    }
}

bool Value::as_bool(bool def) const {
    switch (type) {
        case Type::Bool: return b;
        case Type::Number: return num != 0.0;
        case Type::String: return str == "True" || str == "true" || str == "1" || (str != "False" && str != "false" && str != "0" && str != "" && def);
        default: return def;
    }
}

std::string Value::as_string(const std::string& def) const {
    switch (type) {
        case Type::String: return str;
        case Type::Number: { char b[32]; if (std::floor(num) == num && std::abs(num) < 1e15) std::snprintf(b, sizeof(b), "%.0f", num); else std::snprintf(b, sizeof(b), "%g", num); return b; }
        case Type::Bool: return b ? "True" : "False";
        default: return def;
    }
}

const Value& Value::operator[](size_t i) const { return type == Type::Array && i < arr.size() ? arr[i] : kNull; }

bool parse(const std::string& text, Value& out, std::string* err) {
    Parser p{text, 0, {}};
    if (!p.value(out)) { if (err) *err = p.err; return false; }
    p.ws();
    if (p.p != text.size()) { if (err) *err = "trailing characters"; return false; }
    return true;
}

std::string dump(const Value& v) {
    std::string out;
    switch (v.type) {
        case Value::Type::Null: return "null";
        case Value::Type::Bool: return v.b ? "true" : "false";
        case Value::Type::Number: { char b[32]; if (std::floor(v.num) == v.num && std::abs(v.num) < 1e15) std::snprintf(b, sizeof(b), "%.0f", v.num); else std::snprintf(b, sizeof(b), "%.10g", v.num); return b; }
        case Value::Type::String: dump_string(v.str, out); return out;
        case Value::Type::Array:
            out += '[';
            for (size_t i = 0; i < v.arr.size(); ++i) { if (i) out += ','; out += dump(v.arr[i]); }
            return out + ']';
        case Value::Type::Object:
            out += '{';
            for (size_t i = 0; i < v.obj.size(); ++i) { if (i) out += ','; dump_string(v.obj[i].first, out); out += ':'; out += dump(v.obj[i].second); }
            return out + '}';
    }
    return out;
}

}  // namespace firn::json
