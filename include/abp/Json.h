#pragma once

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace abp::json {

/// Thrown by JsonValue::parse() on malformed input.
class JsonParseError : public std::runtime_error {
public:
    explicit JsonParseError(const std::string& message) : std::runtime_error(message) {}
};

/// A minimal, dependency-free JSON document model. abp only needs to read
/// and write its own small backup manifest format, so this intentionally
/// skips features a general-purpose JSON library would need (streaming,
/// comments, big-number precision, etc).
///
/// Objects preserve insertion order so that written manifests are stable
/// and diff-friendly.
class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    JsonValue() = default;
    JsonValue(std::nullptr_t) {}
    JsonValue(bool b) : type_(Type::Bool), boolValue_(b) {}
    JsonValue(int i) : type_(Type::Number), numberValue_(static_cast<double>(i)) {}
    JsonValue(long long i) : type_(Type::Number), numberValue_(static_cast<double>(i)) {}
    JsonValue(unsigned long long i) : type_(Type::Number), numberValue_(static_cast<double>(i)) {}
    JsonValue(double d) : type_(Type::Number), numberValue_(d) {}
    JsonValue(const char* s) : type_(Type::String), stringValue_(s) {}
    JsonValue(std::string s) : type_(Type::String), stringValue_(std::move(s)) {}

    static JsonValue makeArray() {
        JsonValue v;
        v.type_ = Type::Array;
        return v;
    }
    static JsonValue makeObject() {
        JsonValue v;
        v.type_ = Type::Object;
        return v;
    }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isObject() const { return type_ == Type::Object; }
    bool isArray() const { return type_ == Type::Array; }
    bool isString() const { return type_ == Type::String; }

    bool asBool(bool def = false) const { return type_ == Type::Bool ? boolValue_ : def; }
    double asDouble(double def = 0.0) const { return type_ == Type::Number ? numberValue_ : def; }
    long long asInt(long long def = 0) const {
        return type_ == Type::Number ? static_cast<long long>(numberValue_) : def;
    }
    std::string asString(const std::string& def = "") const {
        return type_ == Type::String ? stringValue_ : def;
    }

    // --- Array interface ---
    void push_back(JsonValue v) {
        type_ = Type::Array;
        arrayValue_.push_back(std::move(v));
    }
    const std::vector<JsonValue>& items() const { return arrayValue_; }

    // --- Object interface ---
    void set(const std::string& key, JsonValue v) {
        type_ = Type::Object;
        for (auto& member : objectValue_) {
            if (member.first == key) {
                member.second = std::move(v);
                return;
            }
        }
        objectValue_.emplace_back(key, std::move(v));
    }
    bool has(const std::string& key) const {
        for (const auto& member : objectValue_) {
            if (member.first == key) return true;
        }
        return false;
    }
    const JsonValue& at(const std::string& key) const {
        for (const auto& member : objectValue_) {
            if (member.first == key) return member.second;
        }
        throw std::out_of_range("JsonValue: no such key: " + key);
    }
    JsonValue get(const std::string& key) const {
        for (const auto& member : objectValue_) {
            if (member.first == key) return member.second;
        }
        return JsonValue();
    }
    JsonValue get(const std::string& key, const JsonValue& def) const {
        for (const auto& member : objectValue_) {
            if (member.first == key) return member.second;
        }
        return def;
    }
    const std::vector<std::pair<std::string, JsonValue>>& members() const { return objectValue_; }

    /// Serializes this value as pretty-printed JSON (indent spaces per level).
    std::string dump(int indent = 2) const;

    /// Parses `text` into a JsonValue, throwing JsonParseError on failure.
    static JsonValue parse(const std::string& text);

private:
    void dumpTo(std::string& out, int indent, int depth) const;

    Type type_ = Type::Null;
    bool boolValue_ = false;
    double numberValue_ = 0.0;
    std::string stringValue_;
    std::vector<JsonValue> arrayValue_;
    std::vector<std::pair<std::string, JsonValue>> objectValue_;
};

} // namespace abp::json
