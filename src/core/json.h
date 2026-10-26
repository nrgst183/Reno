// A small JSON value with a parser and a writer. Object keys keep the order they were added in.
#pragma once
#include <string>
#include <vector>
#include <utility>
#include <memory>
#include <cstdint>

namespace reno {

class Json {
public:
    enum Type { Null, Bool, Number, String, Array, Object };

    Json() {}
    Json(bool b) : type_(Bool), num_(b ? 1 : 0) {}
    Json(int v) : type_(Number), num_(v) {}
    Json(int64_t v) : type_(Number), num_((double)v) {}
    Json(uint64_t v) : type_(Number), num_((double)v) {}
    Json(double v) : type_(Number), num_(v) {}
    Json(const char* s) : type_(String), str_(s ? s : "") {}
    Json(const std::string& s) : type_(String), str_(s) {}

    static Json array() { Json j; j.type_ = Array; return j; }
    static Json object() { Json j; j.type_ = Object; return j; }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Null; }
    bool isObject() const { return type_ == Object; }
    bool isArray() const { return type_ == Array; }
    bool isString() const { return type_ == String; }
    bool isNumber() const { return type_ == Number; }

    bool asBool(bool def = false) const { return (type_ == Bool || type_ == Number) ? num_ != 0 : def; }
    double asDouble(double def = 0) const { return (type_ == Number || type_ == Bool) ? num_ : def; }
    int asInt(int def = 0) const { return (type_ == Number || type_ == Bool) ? (int)num_ : def; }
    int64_t asInt64(int64_t def = 0) const { return (type_ == Number || type_ == Bool) ? (int64_t)num_ : def; }
    uint64_t asUInt64(uint64_t def = 0) const { return (type_ == Number || type_ == Bool) ? (uint64_t)num_ : def; }
    const std::string& asString() const { return str_; }
    std::string asString(const std::string& def) const { return type_ == String ? str_ : def; }

    // arrays
    size_t size() const;
    const Json& at(size_t i) const;
    void push(const Json& v);
    const std::vector<Json>& items() const;

    // objects
    bool has(const std::string& key) const;
    const Json& get(const std::string& key) const;         // a null value if absent
    Json& operator[](const std::string& key);              // adds the key if absent
    const std::vector<std::pair<std::string, Json>>& members() const;

    std::string dump(int indent = 0) const;
    static bool parse(const std::string& text, Json& out, std::string* error = nullptr);

private:
    void write(std::string& out, int indent, int depth) const;
    struct Data;
    void detach();      // the contents become this value's own before it changes them
    Type type_ = Null;
    double num_ = 0;
    std::string str_;
    std::shared_ptr<Data> d_;       // arrays and objects
};

}  // namespace reno
