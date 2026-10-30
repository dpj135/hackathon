#include "nds/json.hpp"

#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

namespace nds {
namespace {

class Parser {
public:
    explicit Parser(const std::string& text) : text_(text) {}

    Json parse() {
        skip_ws();
        Json result = parse_value();
        skip_ws();
        if (pos_ != text_.size()) fail("trailing characters");
        return result;
    }

private:
    const std::string& text_;
    std::size_t pos_{0};

    [[noreturn]] void fail(const std::string& message) const {
        throw JsonError("JSON parse error at byte " + std::to_string(pos_) + ": " + message);
    }

    void skip_ws() {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    }

    char peek() const {
        if (pos_ >= text_.size()) fail("unexpected end of input");
        return text_[pos_];
    }

    bool consume(char value) {
        if (pos_ < text_.size() && text_[pos_] == value) {
            ++pos_;
            return true;
        }
        return false;
    }

    void expect(char value) {
        if (!consume(value)) fail(std::string("expected '") + value + "'");
    }

    Json parse_value() {
        skip_ws();
        switch (peek()) {
        case 'n': parse_literal("null"); return Json(nullptr);
        case 't': parse_literal("true"); return Json(true);
        case 'f': parse_literal("false"); return Json(false);
        case '"': return Json(parse_string());
        case '[': return parse_array();
        case '{': return parse_object();
        default:
            if (peek() == '-' || std::isdigit(static_cast<unsigned char>(peek()))) return parse_number();
            fail("unexpected token");
        }
    }

    void parse_literal(const char* literal) {
        for (const char* p = literal; *p; ++p) {
            if (pos_ >= text_.size() || text_[pos_] != *p) fail("invalid literal");
            ++pos_;
        }
    }

    static void append_utf8(std::string& out, unsigned code) {
        if (code <= 0x7f) {
            out.push_back(static_cast<char>(code));
        } else if (code <= 0x7ff) {
            out.push_back(static_cast<char>(0xc0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        } else {
            out.push_back(static_cast<char>(0xe0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        }
    }

    std::string parse_string() {
        expect('"');
        std::string out;
        while (pos_ < text_.size()) {
            char c = text_[pos_++];
            if (c == '"') return out;
            if (static_cast<unsigned char>(c) < 0x20) fail("control character in string");
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= text_.size()) fail("unterminated escape");
            char e = text_[pos_++];
            switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                unsigned code = 0;
                for (int i = 0; i < 4; ++i) {
                    if (pos_ >= text_.size()) fail("truncated unicode escape");
                    char h = text_[pos_++];
                    code <<= 4;
                    if (h >= '0' && h <= '9') code += h - '0';
                    else if (h >= 'a' && h <= 'f') code += h - 'a' + 10;
                    else if (h >= 'A' && h <= 'F') code += h - 'A' + 10;
                    else fail("invalid unicode escape");
                }
                append_utf8(out, code);
                break;
            }
            default: fail("invalid string escape");
            }
        }
        fail("unterminated string");
    }

    Json parse_number() {
        std::size_t start = pos_;
        consume('-');
        if (consume('0')) {
            if (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
                fail("leading zero in number");
            }
        } else {
            if (!std::isdigit(static_cast<unsigned char>(peek()))) fail("invalid number");
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
        }
        bool floating = false;
        if (consume('.')) {
            floating = true;
            if (pos_ >= text_.size() || !std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
                fail("missing fractional digits");
            }
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            floating = true;
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
            if (pos_ >= text_.size() || !std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
                fail("missing exponent digits");
            }
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
        }
        std::string token = text_.substr(start, pos_ - start);
        try {
            if (floating) return Json(std::stod(token));
            return Json(static_cast<std::int64_t>(std::stoll(token)));
        } catch (const std::exception&) {
            fail("number out of range");
        }
    }

    Json parse_array() {
        expect('[');
        Json::Array out;
        skip_ws();
        if (consume(']')) return Json(std::move(out));
        while (true) {
            out.push_back(parse_value());
            skip_ws();
            if (consume(']')) return Json(std::move(out));
            expect(',');
            skip_ws();
        }
    }

    Json parse_object() {
        expect('{');
        Json::Object out;
        skip_ws();
        if (consume('}')) return Json(std::move(out));
        while (true) {
            if (peek() != '"') fail("object key must be a string");
            std::string key = parse_string();
            skip_ws();
            expect(':');
            Json value = parse_value();
            if (!out.emplace(std::move(key), std::move(value)).second) fail("duplicate object key");
            skip_ws();
            if (consume('}')) return Json(std::move(out));
            expect(',');
            skip_ws();
        }
    }
};

std::string escape_string(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : value) {
        switch (c) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (c < 0x20) {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<int>(c) << std::dec;
            } else {
                out << static_cast<char>(c);
            }
        }
    }
    out << '"';
    return out.str();
}

void dump_impl(std::ostringstream& out, const Json& value, int indent, int depth) {
    auto newline = [&](int child_depth) {
        if (indent >= 0) out << '\n' << std::string(child_depth * indent, ' ');
    };
    if (value.is_null()) out << "null";
    else if (value.is_bool()) out << (value.as_bool() ? "true" : "false");
    else if (value.is_int()) out << value.as_int();
    else if (std::holds_alternative<double>(value.value())) {
        double number = value.as_double();
        if (!std::isfinite(number)) throw JsonError("cannot serialize non-finite number");
        std::ostringstream number_text;
        number_text << std::setprecision(15) << number;
        std::string encoded = number_text.str();
        if (encoded.find_first_of(".eE") == std::string::npos) encoded += ".0";
        out << encoded;
    } else if (value.is_string()) out << escape_string(value.as_string());
    else if (value.is_array()) {
        out << '[';
        const auto& array = value.as_array();
        for (std::size_t i = 0; i < array.size(); ++i) {
            if (i) out << ',';
            newline(depth + 1);
            dump_impl(out, array[i], indent, depth + 1);
        }
        if (!array.empty()) newline(depth);
        out << ']';
    } else {
        out << '{';
        const auto& object = value.as_object();
        std::size_t index = 0;
        for (const auto& [key, item] : object) {
            if (index++) out << ',';
            newline(depth + 1);
            out << escape_string(key) << (indent >= 0 ? ": " : ":");
            dump_impl(out, item, indent, depth + 1);
        }
        if (!object.empty()) newline(depth);
        out << '}';
    }
}

}  // namespace

Json Json::object() { return Json(Object{}); }
Json Json::array() { return Json(Array{}); }
Json Json::parse(const std::string& text) { return Parser(text).parse(); }
bool Json::is_null() const { return std::holds_alternative<std::nullptr_t>(value_); }
bool Json::is_bool() const { return std::holds_alternative<bool>(value_); }
bool Json::is_int() const { return std::holds_alternative<std::int64_t>(value_); }
bool Json::is_number() const { return is_int() || std::holds_alternative<double>(value_); }
bool Json::is_string() const { return std::holds_alternative<std::string>(value_); }
bool Json::is_array() const { return std::holds_alternative<Array>(value_); }
bool Json::is_object() const { return std::holds_alternative<Object>(value_); }

bool Json::as_bool() const {
    if (!is_bool()) throw JsonError("expected boolean");
    return std::get<bool>(value_);
}
std::int64_t Json::as_int() const {
    if (!is_int()) throw JsonError("expected integer");
    return std::get<std::int64_t>(value_);
}
double Json::as_double() const {
    if (is_int()) return static_cast<double>(as_int());
    if (!std::holds_alternative<double>(value_)) throw JsonError("expected number");
    return std::get<double>(value_);
}
const std::string& Json::as_string() const {
    if (!is_string()) throw JsonError("expected string");
    return std::get<std::string>(value_);
}
const Json::Array& Json::as_array() const {
    if (!is_array()) throw JsonError("expected array");
    return std::get<Array>(value_);
}
Json::Array& Json::as_array() {
    if (!is_array()) throw JsonError("expected array");
    return std::get<Array>(value_);
}
const Json::Object& Json::as_object() const {
    if (!is_object()) throw JsonError("expected object");
    return std::get<Object>(value_);
}
Json::Object& Json::as_object() {
    if (!is_object()) throw JsonError("expected object");
    return std::get<Object>(value_);
}
bool Json::contains(const std::string& key) const {
    return is_object() && as_object().find(key) != as_object().end();
}
const Json& Json::at(const std::string& key) const {
    const auto& object = as_object();
    auto it = object.find(key);
    if (it == object.end()) throw JsonError("missing key: " + key);
    return it->second;
}
Json& Json::at(const std::string& key) {
    auto& object = as_object();
    auto it = object.find(key);
    if (it == object.end()) throw JsonError("missing key: " + key);
    return it->second;
}
Json& Json::operator[](const std::string& key) {
    if (is_null()) value_ = Object{};
    return as_object()[key];
}
const Json& Json::operator[](std::size_t index) const { return as_array().at(index); }
Json& Json::operator[](std::size_t index) { return as_array().at(index); }
void Json::push_back(Json value) { as_array().push_back(std::move(value)); }
std::size_t Json::size() const {
    if (is_array()) return as_array().size();
    if (is_object()) return as_object().size();
    throw JsonError("size() requires array or object");
}
std::string Json::dump(int indent) const {
    std::ostringstream out;
    dump_impl(out, *this, indent, 0);
    return out.str();
}

Json read_json_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw JsonError("cannot open JSON file: " + path);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return Json::parse(buffer.str());
}

void write_json_file(const std::string& path, const Json& value) {
    std::ofstream output(path, std::ios::binary);
    if (!output) throw JsonError("cannot create JSON file: " + path);
    output << value.dump(2) << '\n';
    if (!output) throw JsonError("failed writing JSON file: " + path);
}

}  // namespace nds
