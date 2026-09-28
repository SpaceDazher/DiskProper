// Тест-харнесс без внешних зависимостей.
// Зачем свой, а не Catch2: конфигурация CMake с vcpkg/FetchContent появится в Этапе 0
// (ADR-001), а ядро должно проверяться прямо здесь, на хосте, за секунды.
// Проверки: TEST(name) { CHECK(cond); CHECK_EQ(a, b); CHECK_THROWS(expr); }
#pragma once

#include <cstdio>
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

inline int runAll(const char* suite) {
    int failed = 0;
    std::printf("== %s ==\n", suite);
    for (const auto& test : registry()) {
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
    std::printf("%s: %d проверок, провалов %d\n", failed == 0 ? "ALL PASS" : "FAIL", static_cast<int>(registry().size()),
                failed);
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
