#include "abp/Json.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace abp::json {
namespace {

class Parser {
public:
    explicit Parser(const std::string& text) : text_(text) {}

    JsonValue parseDocument() {
        skipWhitespace();
        JsonValue value = parseValue();
        skipWhitespace();
        if (pos_ != text_.size()) {
            fail("trailing data after JSON document");
        }
        return value;
    }

private:
    const std::string& text_;
    size_t pos_ = 0;
    int depth_ = 0;

    /// parseValue() recurses for every nested array/object, so an input of
    /// "[[[[..." deep enough would overflow the stack before any error could
    /// be reported. Manifests nest three levels; 200 is far past anything
    /// legitimate and keeps hostile input a clean parse error instead of a
    /// crash.
    static constexpr int kMaxDepth = 200;

    /// Increments the nesting depth for as long as it is in scope.
    class DepthGuard {
    public:
        explicit DepthGuard(Parser& parser) : parser_(parser) {
            if (++parser_.depth_ > kMaxDepth) {
                parser_.fail("maximum nesting depth exceeded");
            }
        }
        ~DepthGuard() { --parser_.depth_; }

        DepthGuard(const DepthGuard&) = delete;
        DepthGuard& operator=(const DepthGuard&) = delete;

    private:
        Parser& parser_;
    };

    [[noreturn]] void fail(const std::string& message) const {
        throw JsonParseError("JSON parse error at offset " + std::to_string(pos_) + ": " + message);
    }

    char peek() const {
        if (pos_ >= text_.size()) fail("unexpected end of input");
        return text_[pos_];
    }

    char next() {
        if (pos_ >= text_.size()) fail("unexpected end of input");
        return text_[pos_++];
    }

    bool consume(char expected) {
        if (pos_ < text_.size() && text_[pos_] == expected) {
            ++pos_;
            return true;
        }
        return false;
    }

    void expect(char expected) {
        if (!consume(expected)) {
            fail(std::string("expected '") + expected + "'");
        }
    }

    void skipWhitespace() {
        while (pos_ < text_.size()) {
            char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool matchLiteral(const char* literal) {
        size_t len = std::strlen(literal);
        if (pos_ + len > text_.size()) return false;
        if (text_.compare(pos_, len, literal) == 0) {
            pos_ += len;
            return true;
        }
        return false;
    }

    JsonValue parseValue() {
        skipWhitespace();
        char c = peek();
        switch (c) {
            case '{': return parseObject();
            case '[': return parseArray();
            case '"': return JsonValue(parseString());
            case 't':
                if (matchLiteral("true")) return JsonValue(true);
                fail("invalid literal");
            case 'f':
                if (matchLiteral("false")) return JsonValue(false);
                fail("invalid literal");
            case 'n':
                if (matchLiteral("null")) return JsonValue(nullptr);
                fail("invalid literal");
            default:
                return parseNumber();
        }
    }

    JsonValue parseObject() {
        DepthGuard guard(*this);
        expect('{');
        JsonValue obj = JsonValue::makeObject();
        skipWhitespace();
        if (consume('}')) return obj;
        while (true) {
            skipWhitespace();
            if (peek() != '"') fail("expected string key");
            std::string key = parseString();
            skipWhitespace();
            expect(':');
            JsonValue value = parseValue();
            obj.set(key, std::move(value));
            skipWhitespace();
            if (consume(',')) continue;
            expect('}');
            break;
        }
        return obj;
    }

    JsonValue parseArray() {
        DepthGuard guard(*this);
        expect('[');
        JsonValue arr = JsonValue::makeArray();
        skipWhitespace();
        if (consume(']')) return arr;
        while (true) {
            JsonValue value = parseValue();
            arr.push_back(std::move(value));
            skipWhitespace();
            if (consume(',')) continue;
            expect(']');
            break;
        }
        return arr;
    }

    std::string parseString() {
        expect('"');
        std::string result;
        while (true) {
            char c = next();
            if (c == '"') break;
            if (c == '\\') {
                char esc = next();
                switch (esc) {
                    case '"': result.push_back('"'); break;
                    case '\\': result.push_back('\\'); break;
                    case '/': result.push_back('/'); break;
                    case 'b': result.push_back('\b'); break;
                    case 'f': result.push_back('\f'); break;
                    case 'n': result.push_back('\n'); break;
                    case 'r': result.push_back('\r'); break;
                    case 't': result.push_back('\t'); break;
                    case 'u': {
                        unsigned int codepoint = parseHex4();
                        // A code point above the BMP arrives as a UTF-16
                        // surrogate pair. Encoding each half on its own would
                        // emit CESU-8, which is not valid UTF-8, so the pair
                        // is recombined here.
                        if (codepoint >= 0xD800 && codepoint <= 0xDBFF && pos_ + 1 < text_.size() &&
                            text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
                            size_t savedPos = pos_;
                            pos_ += 2;
                            unsigned int low = parseHex4();
                            if (low >= 0xDC00 && low <= 0xDFFF) {
                                codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
                            } else {
                                pos_ = savedPos; // Not a low surrogate; leave it for the next round.
                            }
                        }
                        appendUtf8(result, codepoint);
                        break;
                    }
                    default:
                        fail("invalid escape sequence");
                }
            } else {
                result.push_back(c);
            }
        }
        return result;
    }

    unsigned int parseHex4() {
        unsigned int value = 0;
        for (int i = 0; i < 4; ++i) {
            char c = next();
            value <<= 4;
            if (c >= '0' && c <= '9') value |= static_cast<unsigned int>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<unsigned int>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= static_cast<unsigned int>(c - 'A' + 10);
            else fail("invalid unicode escape");
        }
        return value;
    }

    static void appendUtf8(std::string& out, unsigned int codepoint) {
        // An unpaired surrogate has no UTF-8 encoding; emit U+FFFD so the
        // result is always well-formed text.
        if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
            codepoint = 0xFFFD;
        }

        if (codepoint <= 0x7F) {
            out.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else if (codepoint <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
    }

    JsonValue parseNumber() {
        size_t start = pos_;
        if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
        if (pos_ >= text_.size() || !std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
            fail("invalid number");
        }
        while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
        }
        std::string numberText = text_.substr(start, pos_ - start);
        try {
            return JsonValue(std::stod(numberText));
        } catch (const std::exception&) {
            fail("invalid number literal: " + numberText);
        }
    }
};

void appendEscapedString(std::string& out, const std::string& value) {
    out.push_back('"');
    for (char rawChar : value) {
        unsigned char c = static_cast<unsigned char>(rawChar);
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
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

std::string formatNumber(double value) {
    if (std::isfinite(value) && value == std::floor(value) &&
        std::abs(value) < 1e15) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value));
        return buf;
    }
    std::ostringstream oss;
    oss.precision(17);
    oss << value;
    return oss.str();
}

} // namespace

JsonValue JsonValue::parse(const std::string& text) {
    Parser parser(text);
    return parser.parseDocument();
}

void JsonValue::dumpTo(std::string& out, int indent, int depth) const {
    auto writeIndent = [&](int level) {
        if (indent > 0) {
            out.push_back('\n');
            out.append(static_cast<size_t>(indent * level), ' ');
        }
    };

    switch (type_) {
        case Type::Null:
            out += "null";
            break;
        case Type::Bool:
            out += boolValue_ ? "true" : "false";
            break;
        case Type::Number:
            out += formatNumber(numberValue_);
            break;
        case Type::String:
            appendEscapedString(out, stringValue_);
            break;
        case Type::Array: {
            if (arrayValue_.empty()) {
                out += "[]";
                break;
            }
            out.push_back('[');
            for (size_t i = 0; i < arrayValue_.size(); ++i) {
                if (i != 0) out.push_back(',');
                writeIndent(depth + 1);
                arrayValue_[i].dumpTo(out, indent, depth + 1);
            }
            writeIndent(depth);
            out.push_back(']');
            break;
        }
        case Type::Object: {
            if (objectValue_.empty()) {
                out += "{}";
                break;
            }
            out.push_back('{');
            for (size_t i = 0; i < objectValue_.size(); ++i) {
                if (i != 0) out.push_back(',');
                writeIndent(depth + 1);
                appendEscapedString(out, objectValue_[i].first);
                out += indent > 0 ? ": " : ":";
                objectValue_[i].second.dumpTo(out, indent, depth + 1);
            }
            writeIndent(depth);
            out.push_back('}');
            break;
        }
    }
}

std::string JsonValue::dump(int indent) const {
    std::string out;
    dumpTo(out, indent, 0);
    return out;
}

} // namespace abp::json
