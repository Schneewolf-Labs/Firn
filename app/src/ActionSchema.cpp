#include "Actions.h"
#include <cmath>
#include <regex>
#include <stdexcept>

using firn::json::Value;

Value action_schema(const Action& a) {
    Value schema = Value::object(), props = Value::object(), required = Value::array();
    schema.set("type", Value::string("object"));
    for (const auto& p : a.params) {
        Value spec = Value::object();
        const std::string type = std::string(p.type) == "bool" ? "boolean" : p.type;
        spec.set("type", Value::string(type));
        spec.set("description", Value::string(p.summary));
        if (p.choices) {
            Value choices = Value::array();
            std::string s(p.choices);
            size_t begin = 0;
            do {
                const auto end = s.find(',', begin);
                choices.push(Value::string(s.substr(begin, end - begin)));
                if (end == std::string::npos) break;
                begin = end + 1;
            } while (true);
            spec.set("enum", std::move(choices));
        }
        if (p.fallback) {
            Value def;
            if (type == "string") spec.set("default", Value::string(p.fallback));
            else if (firn::json::parse(p.fallback, def) &&
                     ((type == "number" && def.is_number()) || (type == "boolean" && def.type == Value::Type::Bool)))
                spec.set("default", def);
            else spec.set("x-default-description", Value::string(p.fallback));
        }
        if (spec.find("enum") && spec.find("default")) {
            bool valid = false;
            for (const auto& choice : spec.get("enum").arr)
                if (firn::json::dump(choice) == firn::json::dump(spec.get("default"))) valid = true;
            if (!valid) {
                spec.set("x-default-description", spec.get("default"));
                for (auto it = spec.obj.begin(); it != spec.obj.end(); ++it)
                    if (it->first == "default") { spec.obj.erase(it); break; }
            }
        }
        if (p.schema_json) {
            Value extra;
            if (!firn::json::parse(p.schema_json, extra) || !extra.is_object())
                throw std::logic_error(std::string("invalid schema for ") + a.name + "." + p.name);
            for (const auto& entry : extra.obj) spec.set(entry.first, entry.second);
        }
        props.set(p.name, std::move(spec));
        if (p.required) required.push(Value::string(p.name));
    }
    schema.set("properties", std::move(props));
    schema.set("required", std::move(required));
    schema.set("additionalProperties", Value::boolean(false));
    return schema;
}

namespace {
bool validate(const Value& v, const Value& spec, const std::string& path, std::string& error) {
    auto fail = [&](const std::string& why) { error = path + ": " + why; return false; };
    const std::string type = spec.get("type").as_string();
    if ((type == "object" && !v.is_object()) || (type == "array" && !v.is_array()) ||
        (type == "string" && !v.is_string()) || (type == "boolean" && v.type != Value::Type::Bool) ||
        ((type == "number" || type == "integer") && (!v.is_number() || !std::isfinite(v.num))) ||
        (type == "integer" && std::floor(v.num) != v.num)) return fail("expected " + type);
    if (const Value* choices = spec.find("enum")) {
        bool found = false;
        for (const auto& choice : choices->arr) if (firn::json::dump(choice) == firn::json::dump(v)) found = true;
        if (!found) return fail("value is not in the allowed enum");
    }
    if (v.is_number()) {
        if (const Value* n = spec.find("minimum"); n && v.num < n->num) return fail("below minimum " + firn::json::dump(*n));
        if (const Value* n = spec.find("maximum"); n && v.num > n->num) return fail("above maximum " + firn::json::dump(*n));
        if (const Value* n = spec.find("exclusiveMinimum"); n && v.num <= n->num) return fail("must exceed " + firn::json::dump(*n));
    }
    if (v.is_string()) {
        if (const Value* n = spec.find("minLength"); n && v.str.size() < n->num) return fail("string is too short");
        if (const Value* n = spec.find("maxLength"); n && v.str.size() > n->num) return fail("string is too long");
        if (const Value* pattern = spec.find("pattern"); pattern && !std::regex_match(v.str, std::regex(pattern->str))) return fail("invalid string format");
    }
    if (v.is_array()) {
        if (const Value* n = spec.find("minItems"); n && v.size() < n->num) return fail("too few items");
        if (const Value* n = spec.find("maxItems"); n && v.size() > n->num) return fail("too many items");
        if (const Value* items = spec.find("items"))
            for (size_t i = 0; i < v.size(); ++i)
                if (!validate(v[i], *items, path + "[" + std::to_string(i) + "]", error)) return false;
    }
    if (v.is_object()) {
        for (const auto& key : spec.get("required").arr)
            if (!v.find(key.str)) return fail("missing required parameter " + key.str);
        const Value& props = spec.get("properties");
        for (const auto& entry : v.obj) {
            const Value* sub = props.find(entry.first);
            if (sub) { if (!validate(entry.second, *sub, path + "." + entry.first, error)) return false; }
            else if (spec.find("additionalProperties") && !spec.get("additionalProperties").as_bool())
                return fail("unknown parameter " + entry.first);
        }
    }
    return true;
}
}
bool validate_action(const Action& a, const Value& params, std::string& error) {
    return validate(params, action_schema(a), a.name, error);
}
