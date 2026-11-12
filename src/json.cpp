#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace json {

const Value* Value::get(const char* key) const {
    if (type != Type::Obj || !key) return nullptr;
    for (const auto& kv : obj) {
        if (kv.first == key) return &kv.second;
    }
    return nullptr;
}

const Value* Value::at(size_t i) const {
    if (type != Type::Arr || i >= arr.size()) return nullptr;
    return &arr[i];
}

size_t Value::size() const {
    if (type == Type::Arr) return arr.size();
    if (type == Type::Obj) return obj.size();
    return 0;
}

std::string Value::s(const char* def) const {
    if (type == Type::Str) return str;
    return def ? def : "";
}

double Value::n(double def) const {
    if (type == Type::Num) return num;
    if (type == Type::Bool) return b ? 1.0 : 0.0;
    return def;
}

bool Value::flag(bool def) const {
    if (type == Type::Bool) return b;
    if (type == Type::Num) return num != 0.0;
    return def;
}

namespace {

struct Parser {
    const std::string& t;
    size_t i = 0;
    std::string err;

    explicit Parser(const std::string& text) : t(text) {}

    void ws() {
        while (i < t.size()) {
            char c = t[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++i;
            } else {
                break;
            }
        }
    }

    bool fail(const char* msg) {
        if (err.empty()) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%s (offset %zu)", msg, i);
            err = buf;
        }
        return false;
    }

    static void putUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool hex4(unsigned& out) {
        if (i + 4 > t.size()) return fail("\\u 截断");
        unsigned v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = t[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') {
                v |= static_cast<unsigned>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                v |= static_cast<unsigned>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                v |= static_cast<unsigned>(c - 'A' + 10);
            } else {
                return fail("\\u 非十六进制");
            }
        }
        out = v;
        return true;
    }

    bool str(std::string& out) {
        if (i >= t.size() || t[i] != '"') return fail("缺少字符串引号");
        ++i;
        out.clear();
        while (i < t.size()) {
            unsigned char c = static_cast<unsigned char>(t[i++]);
            if (c == '"') return true;
            if (c != '\\') {
                out.push_back(static_cast<char>(c));
                continue;
            }
            if (i >= t.size()) return fail("转义截断");
            char e = t[i++];
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
                    unsigned cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < t.size() && t[i] == '\\' &&
                        t[i + 1] == 'u') {
                        i += 2;
                        unsigned lo = 0;
                        if (!hex4(lo)) return false;
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                    }
                    putUtf8(out, cp);
                    break;
                }
                default: out.push_back(e); break;
            }
        }
        return fail("字符串未闭合");
    }

    bool number(Value& v) {
        size_t start = i;
        if (i < t.size() && (t[i] == '-' || t[i] == '+')) ++i;
        while (i < t.size() && ((t[i] >= '0' && t[i] <= '9') || t[i] == '.' || t[i] == 'e' ||
                                t[i] == 'E' || t[i] == '-' || t[i] == '+')) {
            ++i;
        }
        if (i == start) return fail("数字为空");
        std::string tmp = t.substr(start, i - start);
        v.type = Type::Num;
        v.num = std::strtod(tmp.c_str(), nullptr);
        return true;
    }

    bool literal(const char* word) {
        size_t n = std::strlen(word);
        if (t.compare(i, n, word) != 0) return fail("非法字面量");
        i += n;
        return true;
    }

    bool value(Value& v, int depth) {
        if (depth > 64) return fail("嵌套过深");
        ws();
        if (i >= t.size()) return fail("提前结束");
        char c = t[i];
        if (c == '{') {
            ++i;
            v.type = Type::Obj;
            ws();
            if (i < t.size() && t[i] == '}') {
                ++i;
                return true;
            }
            while (true) {
                ws();
                std::string key;
                if (!str(key)) return false;
                ws();
                if (i >= t.size() || t[i] != ':') return fail("缺少冒号");
                ++i;
                Value child;
                if (!value(child, depth + 1)) return false;
                v.obj.emplace_back(std::move(key), std::move(child));
                ws();
                if (i < t.size() && t[i] == ',') {
                    ++i;
                    continue;
                }
                if (i < t.size() && t[i] == '}') {
                    ++i;
                    return true;
                }
                return fail("对象未闭合");
            }
        }
        if (c == '[') {
            ++i;
            v.type = Type::Arr;
            ws();
            if (i < t.size() && t[i] == ']') {
                ++i;
                return true;
            }
            while (true) {
                Value child;
                if (!value(child, depth + 1)) return false;
                v.arr.push_back(std::move(child));
                ws();
                if (i < t.size() && t[i] == ',') {
                    ++i;
                    continue;
                }
                if (i < t.size() && t[i] == ']') {
                    ++i;
                    return true;
                }
                return fail("数组未闭合");
            }
        }
        if (c == '"') {
            v.type = Type::Str;
            return str(v.str);
        }
        if (c == 't') {
            if (!literal("true")) return false;
            v.type = Type::Bool;
            v.b = true;
            return true;
        }
        if (c == 'f') {
            if (!literal("false")) return false;
            v.type = Type::Bool;
            v.b = false;
            return true;
        }
        if (c == 'n') {
            if (!literal("null")) return false;
            v.type = Type::Null;
            return true;
        }
        return number(v);
    }
};

}  // namespace

bool parse(const std::string& text, Value& out, std::string* err) {
    Parser p(text);
    out = Value();
    if (!p.value(out, 0)) {
        if (err) *err = p.err;
        return false;
    }
    return true;
}

std::string quote(const std::string& raw) {
    std::string out;
    out.reserve(raw.size() + 8);
    out.push_back('"');
    for (unsigned char c : raw) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
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
    return out;
}

}  // namespace json
