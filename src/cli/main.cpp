// mrproper-cli: точка входа процесса.
//
// Спека: §6.2 (`cli` — headless-режим для CI и e2e), §8 Этап 0 («mrproper-cli
// --version печатает версию»), §5 (пути, устойчивость), §12 (0 необработанных
// исключений).
//
// ---------------------------------------------------------------------------
// Почему файл такой короткий
// ---------------------------------------------------------------------------
//
// Вся логика CLI — разбор командной строки, --version, --help, таблица команд
// и коды возврата — лежит в args.hpp/args.cpp. Здесь остаётся ровно то, чему
// не место в переносимом слое: получение argv от операционной системы в UTF-16
// и превращение его в UTF-8, а также последняя граница, где исключение
// превращается в код возврата.
//
// Именно эта граница и делает файл маленьким:
//
//   * argv приходит как wchar_t*, потому что узкий main() отдал бы строки в
//     ANSI-кодировке текущей консоли, а весь CLI (cmd_rules.cpp, cmd_report.cpp,
//     core::log) ждёт UTF-8. На русской Windows «--rules C:\\Мои файлы» в main()
//     превратился бы в мусор, и путь в отчёте разошёлся бы с тем, что на
//     диске (§5 «Пути»). Поэтому здесь WideCharToMultiByte(CP_UTF8), и только
//     здесь: единственный файл слоя, которому позволено знать про WinAPI;
//   * ни одна команда не обязана ловить исключения сама за себя в последний
//     раз: всё, что дошло сюда, — это упавший std::bad_alloc, ошибка
//     std::filesystem или что-то из std::string. Код 70 (EX_SOFTWARE, как у
//     report и rules) и одна строка в stderr — потому что «0 необработанных
//     исключений» из §12 не выполняется, если процесс просто исчезает вместе
//     с сообщением в диспетчере задач.
#include <exception>
#include <iostream>
#include <new>
#include <ostream>
#include <string>
#include <vector>

#include "args.hpp"

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#endif

namespace {

// UTF-16 → UTF-8 для одного аргумента. Пустая строка на ошибке — не разумно:
// аргумент, который не перевёлся, должен быть виден в сообщении, а не
// превратиться в «пустой аргумент» в разборе.
std::string wideToUtf8(const wchar_t* text) {
    if (text == nullptr || *text == L'\0') return std::string{};

    const int size = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return std::string{};

    std::string result(static_cast<std::size_t>(size - 1), '\0');
    const int written = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, result.data(), size, nullptr,
                                              nullptr);
    if (written <= 0) return std::string{};
    return result;
}

// argv целиком в UTF-8, уже без argv[0]: разбор командной строки не должен
// зависеть от того, как файл программы назвали на диске (args.hpp).
std::vector<std::string> argumentsFrom(int argc, wchar_t* const* argv) {
    std::vector<std::string> arguments;
    if (argc <= 1 || argv == nullptr) return arguments;

    arguments.reserve(static_cast<std::size_t>(argc - 1));
    for (int index = 1; index < argc; ++index) {
        arguments.push_back(wideToUtf8(argv[index]));
    }
    return arguments;
}

}  // namespace

// wmain, а не main: см. шапку файла про ANSI и UTF-8. CRT сам выбирает
// wmainCRTStartup, отдельная опция линковки не нужна.
int wmain(int argc, wchar_t* argv[]) {
    using mrproper::cli::CliStreams;
    using mrproper::cli::CliExit;

    // Консольная кодовая страница вывода — UTF-8: русский текст справки и
    // ошибок в cmd.exe иначе печатается как «Ð¼ÑƒÑÐ¾Ð¾Ñ€» (§5 «Локализация»).
    // Команды report и rules делают то же у себя — каркас не может забыть то,
    // что уже сделала команда.
#if defined(_WIN32)
    ::SetConsoleOutputCP(CP_UTF8);
#endif

    const std::vector<std::string> arguments = argumentsFrom(argc, argv);
    const CliStreams streams{std::cout, std::cerr, std::cin};

    try {
        return mrproper::cli::runCli(arguments, streams);
    } catch (const std::bad_alloc&) {
        std::cerr << mrproper::cli::kProgramName << ": ОШИБКА: не хватило памяти\n";
        return static_cast<int>(CliExit::Internal);
    } catch (const std::exception& error) {
        std::cerr << mrproper::cli::kProgramName << ": ОШИБКА: " << error.what() << '\n';
        return static_cast<int>(CliExit::Internal);
    } catch (...) {
        // Хвост ловится по той же причине, что и остальное: необработанное
        // исключение в CI выглядит как «упал и ничего не сказал», и разобрать
        // его потом нечем.
        std::cerr << mrproper::cli::kProgramName << ": ОШИБКА: неизвестный сбой\n";
        return static_cast<int>(CliExit::Internal);
    }
}
