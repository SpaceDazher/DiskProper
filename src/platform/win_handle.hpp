// MrProper — RAII-обёртки над дескрипторами Win32.
// Спека: §9.1 ADR-001 («изоляция WinAPI в platform/», «Rust-подобные обёртки
// (RAII для HANDLE, unique_handle)»), §6.2 (слой platform), §5 (утилита не имеет
// права молча терять ресурсы).
//
// Зачем именно так. В слое Win32 забытый CloseHandle не падает: процесс
// продолжает работать, а дескрипторы утекают. Утилита, которая обходит
// полмиллиона файлов и держит по дескриптору на каталог, умирает от
// ERROR_NO_SYSTEM_RESOURCES через несколько тысяч каталогов — и умирает не
// там, где ошибка, а в случайном месте другого модуля. unique_handle делает
// невозможным закрыть дескриптор иначе, чем через деструктор, и невозможным
// забыть про закрыть: единственный способ выпустить дескриптор наружу —
// release(), и он читается как «дескриптор теперь не мой».
//
// Модуль header-only (всё inline), .cpp ему не нужен: объектный файл появился
// бы только ради символов, которых компилятор и так не порождает. Возьмите
// дескриптор одним из двух способов:
//
//   const auto file = platform::adoptChecked(CreateFileW(path, …), "CreateFileW", path);
//   const auto file = platform::adopt(CreateFileW(path, …));   // без проверки
//
// Политика закрытия вынесена в отдельные структуры (KernelHandlePolicy,
// FindHandlePolicy, RegistryHandlePolicy): что считать «пустым» дескриптором у
// FindFirstFile и у CreateFile различается, а забыть об этом — источник
// утечки, потому что INVALID_HANDLE_VALUE для FindClose означает CloseHandle.
// Своя политика — четыре строки, поэтому новый вид дескриптора добавляется
// здесь, а не в голове у вызывающего.
#pragma once

#include <windows.h> // NOLINT(bugprone-suspicious-include) — слой Win32, единственное законное место

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "win_error.hpp"

namespace mrproper::platform {

// ---------------------------------------------------------------------------
// Политики закрытия
// ---------------------------------------------------------------------------

// Дескриптор ядра: CreateFileW, CreateEventW, CreateMutexW, CreateNamedPipeW,
// RegOpenKeyExW (обёрнутый в HKEY отдельно, см. RegistryHandlePolicy), сокеты
// и всё прочее из kernel32. Действительный дескриптор не бывает ни nullptr, ни
// INVALID_HANDLE_VALUE — считаем невалидными оба, чтобы политика работала и
// там, где вызывающий уже проверил результат.
struct KernelHandlePolicy {
    using HandleType = HANDLE;

    [[nodiscard]] static bool isValid(HandleType handle) noexcept {
        return handle != nullptr && handle != INVALID_HANDLE_VALUE;
    }

    // Проверка внутри close, а не в вызывающем: деструктор обязан закрывать
    // безусловно, иначе пустой дескриптор в unique_handle станет источником
    // CloseHandle(INVALID_HANDLE_VALUE).
    static void close(HandleType handle) noexcept {
        if (isValid(handle)) ::CloseHandle(handle);
    }
};

// Дескриптор поиска: FindFirstFileW/FindNextFileW. Закрывается FindClose, а
// единственный невалидный дескриптор — INVALID_HANDLE_VALUE; nullptr там не
// встречается, но isValid считает и его пустым — так безопаснее при отладке.
struct FindHandlePolicy {
    using HandleType = HANDLE;

    [[nodiscard]] static bool isValid(HandleType handle) noexcept {
        return handle != nullptr && handle != INVALID_HANDLE_VALUE;
    }

    static void close(HandleType handle) noexcept {
        if (isValid(handle)) ::FindClose(handle);
    }
};

// Ключ реестра: HKEY — отдельный тип (SPEC §5, настройки приложения), и
// закрывается он RegCloseKey, а не CloseHandle.
struct RegistryHandlePolicy {
    using HandleType = HKEY;

    [[nodiscard]] static bool isValid(HandleType handle) noexcept {
        return handle != nullptr;
    }

    static void close(HandleType handle) noexcept {
        if (isValid(handle)) ::RegCloseKey(handle);
    }
};

// ---------------------------------------------------------------------------
// unique_handle
// ---------------------------------------------------------------------------

// Владение дескриптором. Копирования запрещены — два владельца одного
// дескриптора закрыли бы его дважды; перемещение разрешено, потому что
// перемещение Ownership и есть его цель. Деструктор noexcept: во время
// раскрутки стека исключение из деструктора превращает утечку дескриптора в
// std::terminate.
template <typename Policy = KernelHandlePolicy>
class unique_handle {
public:
    using HandleType = typename Policy::HandleType;
    using PolicyType = Policy;

    // Пустой дескриптор: ничего не закрывает при разрушении.
    unique_handle() noexcept = default;

    // Взять дескриптор в владение. Проверки здесь нет намеренно: у APIs,
    // где «неудача» — это INVALID_HANDLE_VALUE, а не nullptr, проверка без
    // имени вызова всё равно даст бесполезное сообщение. Для проверки есть
    // adoptChecked() и makeHandle().
    explicit unique_handle(HandleType handle) noexcept
        : handle_(handle) {
    }

    unique_handle(const unique_handle&) = delete;
    unique_handle& operator=(const unique_handle&) = delete;

    unique_handle(unique_handle&& other) noexcept
        : handle_(other.release()) {
    }

    // Сначала закрываем своё, потом забираем чужое: если дескрипторы совпали
    // (move самого в себя), ничего не закрываем.
    unique_handle& operator=(unique_handle&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }

    ~unique_handle() noexcept {
        Policy::close(handle_);
    }

    // Наблюдение без передачи владения: для вызовов вида GetFileSize(handle,
    // …). Ошибиться тут нельзя — такие вызовы ничего не закрывают.
    [[nodiscard]] HandleType get() const noexcept {
        return handle_;
    }

    [[nodiscard]] bool valid() const noexcept {
        return Policy::isValid(handle_);
    }

    explicit operator bool() const noexcept {
        return valid();
    }

    // Отдать дескриптор наружу: с этого момента закрывает вызывающий.
    // Единственный способ выпустить дескриптор из владения — и именно поэтому
    // он назван release, а не get.
    [[nodiscard]] HandleType release() noexcept {
        const HandleType handle = handle_;
        handle_ = HandleType{};
        return handle;
    }

    // Закрыть текущий дескриптор и взять новый. reset() без аргументов —
    // просто закрыть. Тот же дескриптор, переданный в reset(), не закрывается:
    // он переходит к нам, а не выбрасывается.
    void reset(HandleType handle = HandleType{}) noexcept {
        if (handle != handle_) Policy::close(handle_);
        handle_ = handle;
    }

    void swap(unique_handle& other) noexcept {
        const HandleType current = handle_;
        handle_ = other.handle_;
        other.handle_ = current;
    }

private:
    HandleType handle_{};
};

// Своп для алгоритмов std::swap: обмен без исключений и без копирования.
template <typename Policy>
void swap(unique_handle<Policy>& left, unique_handle<Policy>& right) noexcept {
    left.swap(right);
}

// ---------------------------------------------------------------------------
// Захват дескриптора
// ---------------------------------------------------------------------------

// Взять дескриптор в владение без проверки: nullptr и INVALID_HANDLE_VALUE
// допустимы (вызывающий сам решил, что пустой дескриптор — не беда).
template <typename Policy = KernelHandlePolicy>
[[nodiscard]] inline unique_handle<Policy> adopt(typename Policy::HandleType handle) noexcept {
    return unique_handle<Policy>(handle);
}

// Взять дескриптор в владение с проверкой: невалидный дескриптор — WinError с
// текстом системы и путём. where — имя вызова («CreateFileW»), без скобок.
template <typename Policy = KernelHandlePolicy, typename Path = std::string>
[[nodiscard]] inline unique_handle<Policy> adoptChecked(typename Policy::HandleType handle, std::string_view where,
                                                        const Path& path = Path{}) {
    if (Policy::isValid(handle)) return unique_handle<Policy>(handle);
    throwLastError(where, path);
}

// Создать дескриптор и сразу взять его в владение — короткая форма для
// CreateFileW и подобных: аргументы передаются в create как есть, проверка и
// WinError — как в adoptChecked.
template <typename Policy = KernelHandlePolicy, typename Create, typename... Args>
[[nodiscard]] inline unique_handle<Policy> makeHandle(Create create, std::string_view where, Args&&... args) {
    return adoptChecked<Policy>(create(std::forward<Args>(args)...), where);
}

// Закрыть дескриптор, полученный мимо RAII. Нужен там, где владение уже
// выпущено через release() или дескриптор пришёл из чужой функции, — и
// только там: в остальных случаях правильный инструмент unique_handle.
template <typename Policy = KernelHandlePolicy>
inline void closeSilently(typename Policy::HandleType handle) noexcept {
    Policy::close(handle);
}

}  // namespace mrproper::platform
