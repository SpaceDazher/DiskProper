// Сопоставление путей с шаблонами правил очистки.
// Поддерживаются: * (внутри сегмента), ** (через сегменты), ? (один символ),
// [abc] / [a-z] (класс символов), сравнение без учёта регистра.
// Переносимый модуль: без Windows API.
#pragma once

#include <string>
#include <string_view>

namespace mrproper::core {

enum class CaseMode { Sensitive, AsciiInsensitive };

// Приводит разделители к одному виду ('/'), чтобы правила одинаково работали
// при отладке на Linux и при обходе диска на Windows.
std::string normalizeSeparators(std::string_view path);

// true, если path соответствует шаблону pattern.
bool matchPath(std::string_view pattern, std::string_view path, CaseMode mode = CaseMode::AsciiInsensitive);

// true, если шаблон синтаксически корректен (парные скобки, непустой).
bool isValidPattern(std::string_view pattern);

// Раскрывает %ПЕРЕМЕННАЯ% в шаблоне. env задаёт значения известных переменных
// (платформа наполняет его из GetEnvironmentVariableW). Неизвестные переменные
// оставляются как есть и помечаются как «неразрешённые» через ok.
std::string expandEnvironment(std::string_view pattern, const std::string& env, bool* ok = nullptr);

}  // namespace mrproper::core
