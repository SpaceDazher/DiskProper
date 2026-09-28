// MrProper — минимальный JSON для правил очистки и отчётов.
// Переносимый модуль: без Windows API, собирается и тестируется на любом хосте.
// Объекты хранят порядок вставки — вывод детерминирован, это нужно golden-тестам.
#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mrproper::json {

class ParseError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class Type { Null, Bool, Number, String, Array, Object };

class Value {
public:
    Value() = default;
    explicit Value(bool b) : type_(Type::Bool), bool_(b) {}
    explicit Value(double n) : type_(Type::Number), num_(n) {}
    explicit Value(int n) : type_(Type::Number), num_(static_cast<double>(n)) {}
    explicit Value(std::string s) : type_(Type::String), str_(std::move(s)) {}
    explicit Value(const char* s) : type_(Type::String), str_(s) {}

    static Value array(std::vector<Value> items);
    static Value object(std::vector<std::pair<std::string, Value>> members);

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    bool asBool() const;
    double asNumber() const;
    const std::string& asString() const;

    const std::vector<Value>& items() const;
    const std::vector<std::pair<std::string, Value>>& members() const;

    // Возвращает nullptr, если ключа нет или значение не объект.
    const Value* find(std::string_view key) const;

    // Достаёт поле объекта с проверкой типа; бросает с понятным сообщением.
    const Value& require(std::string_view key) const;

    std::string dump(int indent = -1) const;

private:
    void dumpTo(std::string& out, int indent, int depth) const;

    Type type_{Type::Null};
    bool bool_{false};
    double num_{0.0};
    std::string str_;
    std::vector<Value> arr_;
    std::vector<std::pair<std::string, Value>> obj_;
};

Value parse(std::string_view text);

}  // namespace mrproper::json
