// 迷你 JSON 解析器：只够本程序用（CDP 响应、/json/list），不依赖任何第三方库。
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace json {

enum class Type { Null, Bool, Num, Str, Arr, Obj };

struct Value {
    Type type = Type::Null;
    bool b = false;
    double num = 0.0;
    std::string str;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;

    bool isNull() const { return type == Type::Null; }
    bool isObj() const { return type == Type::Obj; }
    bool isArr() const { return type == Type::Arr; }
    bool isStr() const { return type == Type::Str; }

    const Value* get(const char* key) const;
    const Value* at(size_t i) const;
    size_t size() const;
    std::string s(const char* def = "") const;
    double n(double def = 0.0) const;
    bool flag(bool def = false) const;
};

// 解析整段文本；失败返回 false 并写入 err。
bool parse(const std::string& text, Value& out, std::string* err = nullptr);

// 把字符串转成合法的 JSON 字符串字面量（含引号）。
std::string quote(const std::string& raw);

}  // namespace json
