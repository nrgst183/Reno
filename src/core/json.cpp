#include "json.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>

namespace reno {

static const Json kNullJson;

struct Json::Data {
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;
};

void Json::detach()
{
    if (!d_) d_ = std::make_shared<Data>();
    else if (d_.use_count() > 1) d_ = std::make_shared<Data>(*d_);
}

size_t Json::size() const
{
    if (!d_) return 0;
    return type_ == Array ? d_->arr.size() : (type_ == Object ? d_->obj.size() : 0);
}

void Json::push(const Json& v)
{
    if (type_ != Array) *this = array();
    detach();
    d_->arr.push_back(v);
}

const std::vector<Json>& Json::items() const
{
    static const std::vector<Json> none;
    return d_ ? d_->arr : none;
}

const std::vector<std::pair<std::string, Json>>& Json::members() const
{
    static const std::vector<std::pair<std::string, Json>> none;
    return d_ ? d_->obj : none;
}

const Json& Json::at(size_t i) const { return (type_ == Array && d_ && i < d_->arr.size()) ? d_->arr[i] : kNullJson; }

bool Json::has(const std::string& key) const
{
    if (type_ != Object || !d_) return false;
    for (const auto& m : d_->obj) if (m.first == key) return true;
    return false;
}

const Json& Json::get(const std::string& key) const
{
    if (type_ == Object && d_)
        for (const auto& m : d_->obj) if (m.first == key) return m.second;
    return kNullJson;
}

Json& Json::operator[](const std::string& key)
{
    if (type_ != Object) *this = object();
    detach();
    for (auto& m : d_->obj) if (m.first == key) return m.second;
    d_->obj.emplace_back(key, Json());
    return d_->obj.back().second;
}

static void writeString(std::string& out, const std::string& s)
{
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
            else out.push_back((char)c);
        }
    }
    out.push_back('"');
}

void Json::write(std::string& out, int indent, int depth) const
{
    auto nl = [&](int d) { if (indent > 0) { out.push_back('\n'); out.append((size_t)(indent * d), ' '); } };
    switch (type_) {
    case Null: out += "null"; break;
    case Bool: out += num_ != 0 ? "true" : "false"; break;
    case Number: {
        char b[40];
        if (!std::isfinite(num_)) { out += "0"; break; }
        if (num_ == std::floor(num_) && std::fabs(num_) < 9.0e15) std::snprintf(b, sizeof b, "%.0f", num_);
        else std::snprintf(b, sizeof b, "%.17g", num_);
        out += b;
        break;
    }
    case String: writeString(out, str_); break;
    case Array: {
        const std::vector<Json>& arr_ = items();
        out.push_back('[');
        for (size_t i = 0; i < arr_.size(); ++i) {
            if (i) out.push_back(',');
            nl(depth + 1);
            arr_[i].write(out, indent, depth + 1);
        }
        if (!arr_.empty()) nl(depth);
        out.push_back(']');
        break;
    }
    case Object: {
        const std::vector<std::pair<std::string, Json>>& obj_ = members();
        out.push_back('{');
        for (size_t i = 0; i < obj_.size(); ++i) {
            if (i) out.push_back(',');
            nl(depth + 1);
            writeString(out, obj_[i].first);
            out += indent > 0 ? ": " : ":";
            obj_[i].second.write(out, indent, depth + 1);
        }
        if (!obj_.empty()) nl(depth);
        out.push_back('}');
        break;
    }
    }
}

std::string Json::dump(int indent) const
{
    std::string out;
    write(out, indent, 0);
    return out;
}

namespace {
struct Parser {
    const char* p;
    const char* end;
    std::string err;
    int depth = 0;

    void ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }
    bool fail(const char* m) { if (err.empty()) err = m; return false; }

    static void utf8(std::string& s, unsigned cp)
    {
        if (cp < 0x80) s.push_back((char)cp);
        else if (cp < 0x800) { s.push_back((char)(0xC0 | (cp >> 6))); s.push_back((char)(0x80 | (cp & 0x3F))); }
        else if (cp < 0x10000) { s.push_back((char)(0xE0 | (cp >> 12))); s.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); s.push_back((char)(0x80 | (cp & 0x3F))); }
        else { s.push_back((char)(0xF0 | (cp >> 18))); s.push_back((char)(0x80 | ((cp >> 12) & 0x3F))); s.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); s.push_back((char)(0x80 | (cp & 0x3F))); }
    }

    bool hex4(unsigned& v)
    {
        if (end - p < 4) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool str(std::string& s)
    {
        if (p >= end || *p != '"') return fail("expected a string");
        ++p;
        while (p < end && *p != '"') {
            if (*p == '\\') {
                ++p;
                if (p >= end) return fail("bad escape");
                char c = *p++;
                switch (c) {
                case 'n': s.push_back('\n'); break;
                case 'r': s.push_back('\r'); break;
                case 't': s.push_back('\t'); break;
                case 'b': s.push_back('\b'); break;
                case 'f': s.push_back('\f'); break;
                case 'u': {
                    unsigned cp;
                    if (!hex4(cp)) return fail("bad \\u escape");
                    if (cp >= 0xD800 && cp < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                        p += 2;
                        unsigned lo;
                        if (!hex4(lo)) return fail("bad \\u escape");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    utf8(s, cp);
                    break;
                }
                default: s.push_back(c);
                }
            } else s.push_back(*p++);
        }
        if (p >= end) return fail("unterminated string");
        ++p;
        return true;
    }

    bool value(Json& out)
    {
        if (++depth > 200) return fail("nested too deeply");
        ws();
        if (p >= end) return fail("unexpected end");
        bool ok = true;
        char c = *p;
        if (c == '{') {
            ++p;
            out = Json::object();
            ws();
            if (p < end && *p == '}') ++p;
            else for (;;) {
                ws();
                std::string k;
                if (!str(k)) { ok = false; break; }
                ws();
                if (p >= end || *p != ':') { ok = fail("expected ':'"); break; }
                ++p;
                Json v;
                if (!value(v)) { ok = false; break; }
                out[k] = v;
                ws();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == '}') { ++p; break; }
                ok = fail("expected ',' or '}'");
                break;
            }
        } else if (c == '[') {
            ++p;
            out = Json::array();
            ws();
            if (p < end && *p == ']') ++p;
            else for (;;) {
                Json v;
                if (!value(v)) { ok = false; break; }
                out.push(v);
                ws();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == ']') { ++p; break; }
                ok = fail("expected ',' or ']'");
                break;
            }
        } else if (c == '"') {
            std::string s;
            ok = str(s);
            if (ok) out = Json(s);
        } else if (end - p >= 4 && !std::strncmp(p, "true", 4)) { p += 4; out = Json(true); }
        else if (end - p >= 5 && !std::strncmp(p, "false", 5)) { p += 5; out = Json(false); }
        else if (end - p >= 4 && !std::strncmp(p, "null", 4)) { p += 4; out = Json(); }
        else {
            const char* s = p;
            while (p < end && (std::strchr("+-.eE", *p) || (*p >= '0' && *p <= '9'))) ++p;
            if (p == s) ok = fail("unexpected character");
            else out = Json(std::strtod(std::string(s, p).c_str(), nullptr));
        }
        --depth;
        return ok;
    }
};
}  // namespace

bool Json::parse(const std::string& text, Json& out, std::string* error)
{
    Parser ps;
    ps.p = text.data();
    ps.end = text.data() + text.size();
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) ps.p += 3;
    Json v;
    bool ok = ps.value(v);
    if (ok) { ps.ws(); if (ps.p != ps.end) ok = ps.fail("text after the end"); }
    if (!ok) { if (error) *error = ps.err; return false; }
    out = v;
    return true;
}

}  // namespace reno
