#pragma once

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace nds {

class JsonError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Json {
public:
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json>;
    using Value = std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, Array, Object>;

    Json() : value_(nullptr) {}
    Json(std::nullptr_t) : value_(nullptr) {}
    Json(bool value) : value_(value) {}
    Json(int value) : value_(static_cast<std::int64_t>(value)) {}
    Json(std::int64_t value) : value_(value) {}
    Json(double value) : value_(value) {}
    Json(const char* value) : value_(std::string(value)) {}
    Json(std::string value) : value_(std::move(value)) {}
    Json(Array value) : value_(std::move(value)) {}
    Json(Object value) : value_(std::move(value)) {}

    static Json object();
    static Json array();
    static Json parse(const std::string& text);

    bool is_null() const;
    bool is_bool() const;
    bool is_int() const;
    bool is_number() const;
    bool is_string() const;
    bool is_array() const;
    bool is_object() const;

    bool as_bool() const;
    std::int64_t as_int() const;
    double as_double() const;
    const std::string& as_string() const;
    const Array& as_array() const;
    Array& as_array();
    const Object& as_object() const;
    Object& as_object();

    bool contains(const std::string& key) const;
    const Json& at(const std::string& key) const;
    Json& at(const std::string& key);
    Json& operator[](const std::string& key);
    const Json& operator[](std::size_t index) const;
    Json& operator[](std::size_t index);
    void push_back(Json value);
    std::size_t size() const;

    std::string dump(int indent = 2) const;
    const Value& value() const { return value_; }

    bool operator==(const Json& other) const { return value_ == other.value_; }
    bool operator!=(const Json& other) const { return !(*this == other); }

private:
    Value value_;
};

Json read_json_file(const std::string& path);
void write_json_file(const std::string& path, const Json& value);

}  // namespace nds
