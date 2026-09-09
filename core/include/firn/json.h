#pragma once
#include <string>
#include <utility>
#include <vector>

// A small JSON value for the scripting interface (no dependencies).
namespace firn::json {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool b = false;
    double num = 0.0;
    std::string str;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;

    static Value null() { return Value{}; }
    static Value boolean(bool v) { Value x; x.type = Type::Bool; x.b = v; return x; }
    static Value number(double v) { Value x; x.type = Type::Number; x.num = v; return x; }
    static Value string(std::string s) { Value x; x.type = Type::String; x.str = std::move(s); return x; }
    static Value array() { Value x; x.type = Type::Array; return x; }
    static Value object() { Value x; x.type = Type::Object; return x; }

    bool is_null() const { return type == Type::Null; }
    bool is_number() const { return type == Type::Number; }
    bool is_string() const { return type == Type::String; }
    bool is_array() const { return type == Type::Array; }
    bool is_object() const { return type == Type::Object; }

    // Object member by key (or nested "a.b.c" path); null when absent.
    const Value* find(const std::string& key) const;
    const Value& get(const std::string& path) const;   // a shared null when absent
    Value& set(const std::string& key, Value v);        // objects only; replaces an existing key
    void push(Value v) { arr.push_back(std::move(v)); }

    double as_number(double def = 0.0) const;   // numbers, numeric strings, bools
    bool as_bool(bool def = false) const;       // bools, numbers, "True"/"False"
    std::string as_string(const std::string& def = "") const;
    size_t size() const { return type == Type::Array ? arr.size() : type == Type::Object ? obj.size() : 0; }
    const Value& operator[](size_t i) const;    // arrays; null when out of range
};

// Parses text; returns false (with a message) on malformed input.
bool parse(const std::string& text, Value& out, std::string* err = nullptr);
std::string dump(const Value& v);

}  // namespace firn::json
