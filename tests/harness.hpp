// Тест-харнесс без внешних зависимостей.
// Зачем свой, а не Catch2: конфигурация CMake с vcpkg/FetchContent появится в Этапе 0
// (ADR-001), а ядро должно проверяться прямо здесь, на хосте, за секунды.
// Проверки: TEST(name) { CHECK(cond); CHECK_EQ(a, b); CHECK_THROWS(expr); }
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

namespace mrp {

struct TestCase {
    const char* name;
    void (*fn)();
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

struct Failure {
    std::string message;
};

inline void checkTrue(bool ok, const char* expr, const char* file, int line) {
    if (!ok) {
        throw Failure{std::string(file) + ":" + std::to_string(line) + " CHECK(" + expr + ") не выполнено"};
    }
}

template <typename A, typename B>
void checkEq(const A& a, const B& b, const char* ea, const char* eb, const char* file, int line) {
    if (!(a == b)) {
        throw Failure{std::string(file) + ":" + std::to_string(line) + " " + ea + " != " + eb};
    }
}

// Фильтр по префиксу имени проверки через переменную окружения
// MRPROPER_TEST_FILTER. Нужен, чтобы задача команды могла доказать свою часть
// одной автоматической командой: пока соседняя задача чинит свой слой, её
// провалы не должны обнулять наш результат (и наоборот).
// Чтение переменной окружения. На Windows getenv помечен устаревшим (C4996),
// а заглушать предупреждение нельзя: под /WX это тихая замена планки сборки.
// Поэтому ветка Windows читает через _dupenv_s, ветка POSIX — обычный getenv.
inline const char* envValue(const char* name) {
#if defined(_WIN32)
    static std::string storage;
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) return nullptr;
    storage.assign(value);
    std::free(value);
    return storage.empty() ? nullptr : storage.c_str();
#else
    return std::getenv(name);
#endif
}

inline const char* testFilter() {
    static const char* value = [] {
        const char* raw = envValue("MRPROPER_TEST_FILTER");
        return (raw != nullptr && raw[0] != '\0') ? raw : nullptr;
    }();
    return value;
}

inline int runAll(const char* suite) {
    int failed = 0;
    int selected = 0;
    std::printf("== %s ==\n", suite);
    for (const auto& test : registry()) {
        if (const char* filter = testFilter()) {
            if (std::strncmp(test.name, filter, std::strlen(filter)) != 0) continue;
        }
        ++selected;
        try {
            test.fn();
            std::printf("  [ ok ] %s\n", test.name);
        } catch (const Failure& f) {
            std::printf("  [FAIL] %s\n         %s\n", test.name, f.message.c_str());
            ++failed;
        } catch (const std::exception& e) {
            std::printf("  [FAIL] %s\n         непойманное исключение: %s\n", test.name, e.what());
            ++failed;
        } catch (...) {
            std::printf("  [FAIL] %s\n         неизвестное исключение\n", test.name);
            ++failed;
        }
    }
    std::printf("%s: %d проверок, провалов %d\n", failed == 0 ? "ALL PASS" : "FAIL", selected, failed);
    if (selected == 0) {
        std::printf("FAIL: фильтр %s не выбрал ни одной проверки\n", testFilter() ? testFilter() : "");
        return 1;
    }
    return failed == 0 ? 0 : 1;
}

}  // namespace mrp

#define TEST(name)                                                  \
    static void name();                                             \
    static ::mrp::Registrar registrar_##name(#name, &name);        \
    static void name()

#define CHECK(cond) ::mrp::checkTrue(static_cast<bool>(cond), #cond, __FILE__, __LINE__)

#define CHECK_EQ(a, b) ::mrp::checkEq((a), (b), #a, #b, __FILE__, __LINE__)

#define CHECK_THROWS(expr)                        \
    do {                                          \
        bool thrown = false;                      \
        try {                                     \
            (void)(expr);                         \
        } catch (...) {                           \
            thrown = true;                        \
        }                                         \
        ::mrp::checkTrue(thrown, #expr, __FILE__, __LINE__); \
    } while (false)
