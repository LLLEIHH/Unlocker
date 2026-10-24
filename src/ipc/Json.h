#pragma once
// 手写的最小 JSON（字段少，不引入依赖）。转义必须做全：
//   \ 转成 \\      " 转成 \"     控制字符转成 \u00XX     非 ASCII 直接输出 UTF-8
// 最容易被忽略的是路径里的反斜杠和文件名里的引号 —— 一个没转义就整条消息解析失败。
#include <string>
#include <utility>
#include <vector>

namespace ul {
namespace json {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type        type = Type::Null;
    bool        b    = false;
    double      num  = 0;
    std::string str;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;

    Value() = default;

    static Value MakeString(std::string v);
    static Value MakeNumber(double v);
    static Value MakeBool(bool v);
    static Value MakeArray();
    static Value MakeObject();

    void Set(const std::string& key, Value v);
    const Value* Find(const std::string& key) const;

    std::string GetString(const std::string& key, const std::string& def = {}) const;
    double      GetNumber(const std::string& key, double def = 0) const;
    bool        GetBool(const std::string& key, bool def = false) const;
};

bool Parse(const std::string& text, Value* out, std::string* err);
std::string Dump(const Value& v);

}  // namespace json
}  // namespace ul
