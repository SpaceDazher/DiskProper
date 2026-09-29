# Ревью 03 — Win32-обёртки: утечки HANDLE, коды ошибок, таймауты

Задача 93. Дата: 2026-09-28. Слот агента: `a93`. Правок кода в этом документе нет —
это ревью; все находки описаны здесь и требуют отдельных задач на исправление.

Предыдущие два ревью спрашивали «что удаляется» (ревью 01, задача 91) и «что может
удалить правило» (ревью 02, задача 92). Здесь предмет — третья поверхность: **не
что делает утилита, а как она разговаривает с Windows**. Конкретно три вопроса из
задания:

1. **Утечки HANDLE** — дескриптор, который не закрыли ни на одном пути выхода;
2. **Неверные коды ошибок** — число, которое попадает в лог, отчёт и UI и не
   соответствует отказу, который на самом деле произошёл;
3. **Таймауты** — есть ли верхняя граница у каждой операции, которая может
   зависнуть на мёртвом устройстве или на медленной сети.

Спека: §9.1 (ADR-001: «изоляция WinAPI в platform/», «RAII для HANDLE,
`unique_handle`»), §5 (нефункциональные требования), §6.2/§6.3/§6.4 (слои, пути,
потоки и отмена), §9.2 п.1 (таймаут 10 с на запрос правил), §10 (риск «Диск/раздел
отваливается в IOCTL» → «Таймауты, изоляция на устройство, degraded-режим»; риск
«Старые GPU/драйверы ломают Direct2D» → «Fallback на GDI, feature-probe»), §12
(«все ошибки в логе с путём и HRESULT»).

## 1. Область и метод

Ревью статическое, построчное, по трём осям. Просмотрены **все** места, где
проект обращается к WinAPI, а не только «интересные» модули. Инвентарь по слоям
(счёт вхождений, `src/`, файлы `platform/`, `engine/`, `ui/`, `cli/`):

| Что ищется | Где смотрели | Модули с находками |
|---|---|---|
| `CreateFileW`, дескрипторные RAII-обёртки, `CloseHandle` | `vfs_walk`, `vfs_size`, `vfs_paths`, `vfs_delete`, `vfs_trash`, `size_probe`, `storage_query`, `trim_cache`, `devices`, `layout`, `volumes`, `wmi`, `rulesync_client`, `ui/renderer`, `ui/theme`, `ui/view_report` | `volumes` (F-04) |
| `FindFirstFile*`, `FindFirstVolume*`, `FindClose` | `vfs_delete`, `vfs_trash`, `rulesync_client`, `volumes` | `volumes` (F-04) |
| COM: `CoInitializeEx`/`CoUninitialize`, `CoCreateInstance`, `Release`, `BSTR`, `VARIANT` | `wmi`, `ui/app_shell`, `ui/renderer` | — |
| Реестр: `RegGetValueW`, `RegCreateKeyExW`, `RegCloseKey` | `ui/theme` | — |
| Процессы: `CreateToolhelp32Snapshot`, `OpenProcess`, `CreateProcessW`, `CreatePipe`, `WaitForSingleObject` | `process_control`, `wmi` | — |
| Restart Manager: `RmStartSession`/`RmEndSession`/`RmGetList`/`RmShutdown` | `process_control`, `restart_manager` | `process_control` (F-06) |
| WinHTTP: `WinHttpOpen`/`Connect`/`OpenRequest`/`SetTimeouts`/`ReadData`/`CloseHandle` | `net`, `rulesync_client` | `rulesync_client` (F-02) |
| IOCTL: `DeviceIoControl` + `OVERLAPPED` + `CancelIoEx` + `GetOverlappedResult` | `devices`, `storage_query`, `trim_cache`, `size_probe`, `volumes`, `layout` | `storage_query`, `size_probe` (F-01), `volumes` (F-05) |
| `GetLastError` (166 мест), `HRESULT`, `FormatMessageW` | весь `src/` | `vfs_trash` (F-03) |
| Таймауты: где-либо `WaitForSingleObject`, дедлайн, бюджет | весь `src/` | `rulesync_client` (F-02) |
| GDI/DC: `GetDC`/`ReleaseDC`, `CreateCompatibleDC`, `CreateDIBSection`, `SelectObject`, `DeleteObject` | `ui/renderer`, `ui/app_shell` | — |
| Покрытие: что вообще проверяется тестами | `tests/unit`, `tests/integration`, `tests/e2e` | F-08 |

Три оси проверялись **дважды**: чтением кода и (где это возможно без specially
crafted нагрузки) прогоном штатного бинарника в слоте `a93`, §5. Отдельно
проверено на хосте, что система знает о двух кодах, которые сравниваются в этом
ревью: `net helpmsg 258` и `net helpmsg 1460`.

Что ревью **не** проверяло и не может: живой таймаут на мёртвом устройстве
(нужен уходящий в IOCTL диск), редирект-цепочку и медленную сдачу ответа по
HTTPS (нужен специальный сервер), `WAIT_FAILED` (нужен отказ ожидания).
Все утверждения о таких путях — **разбор кода**, а не наблюдение; это указано
в каждой находке отдельно.

## 2. Сводка

| ID | Тема | Ось | Вердикт |
|---|---|---|---|
| F-01 | Таймаут устройства возвращается как `WAIT_TIMEOUT` (258) в поле `win32Error` | коды ошибок | **ОБХОД ПОДТВЕРЖДЁН** |
| F-02 | Транспорт правил: `WinHttpSetTimeouts` на четыре фазы + таймаут на каждое `WinHttpReadData` — «таймаут 10 с» из §9.2 не 10 с | таймауты | **ОБХОД ПОДТВЕРЖДЁН** |
| F-03 | `::GetLastError()` читается после вызова помощника, а не сразу после WinAPI (8 мест в `vfs_trash`) | коды ошибок | **ОБХОД ПОДТВЕРЖДЁН** |
| F-04 | Дескрипторы поиска томов закрываются через `CloseHandle`, а не `FindClose` | HANDLE | **ОБХОД ПОДТВЕРЖДЁН (контракт), утечка не наблюдалась** |
| F-05 | В `volumes.cpp` код ожидания читается после `CancelIoEx` и `GetOverlappedResult` | коды ошибок | **ОБХОД ПОДТВЕРЖДЁН** |
| F-06 | `readSessionState` подменяет настоящий код `RmGetList` на `ERROR_MORE_DATA` вопреки своему же комментарию | коды ошибок | **ДЕФЕКТ, влияния на поведение нет** |
| F-07 | Результат `SetHandleInformation` не проверяется | HANDLE | **ДЕФЕКТ, влияние малое** |
| F-08 | Ни один модуль устройства/COM/транспорта не покрыт тестами (0 проверок на 12 модулей) | покрытие | **ПРОБЕЛ ПОКРЫТИЯ** |
| F-09 | Утечек HANDLE, COM и буферов в проверенном коде не найдено; контроли ADR-001 работают | HANDLE | **СООТВЕТСТВУЕТ СПЕКЕ** |

Главное, чего в ревью **нет**: утечки дескрипторов. Это не «не нашли, потому что
смотрели плохо» — на каждое место, где дескриптор мог бы утечь, есть владелец
(§6, F-09). Найдено другое: числа, которые эти обёртки передают наверх, в трёх
местах из четырёх означают не то, что написано, и одна верхняя граница времени
не та, что обещает спека.

## 3. Находки

### F-01. Таймаут устройства возвращается как `WAIT_TIMEOUT` (258) — пользователю показывают 0x00000102 и пустой текст системы

**Вердикт: ОБХОД ПОДТВЕРЖДЁН. Влияние: среднее (диагностика отказа, который §10
обязан делать читаемым: «Таймауты, изоляция на устройство, degraded-режим,
понятное сообщение»).**

Где: `src/platform/storage_query.cpp:262`, `src/platform/size_probe.cpp:395`,
`src/platform/size_probe.cpp:437`. Потребители: `storage_query.cpp:543`,
`storage_query.cpp:561`, `inventory.cpp:408`, `inventory.cpp:420-428`,
`cmd_disks.cpp:600-606`, `storage_query.cpp:635`, `size_probe.cpp:409`.

Суть. `WaitForSingleObject` возвращает **WAIT-результат**, а не Win32-код ошибки.
`WAIT_TIMEOUT` = 258, `ERROR_TIMEOUT` = 1460. Два модуля кладут первое в поле,
которое во всём проекте называется `win32Error`:

```
// storage_query.cpp:253-268
const DWORD wait = ::WaitForSingleObject(event.get(), timeoutMs);
if (wait == WAIT_TIMEOUT) {
    ::CancelIoEx(device, &overlappedStruct);
    ...
    outcome.win32Error = static_cast<std::uint32_t>(WAIT_TIMEOUT);   // ← 258 в поле «код Win32»
    return outcome;
}
```

Остальной слой на ту же секунду кладёт `ERROR_TIMEOUT`: `devices.cpp:255`,
`volumes.cpp:277`, `trim_cache.cpp:220` и `:224`, `inventory.cpp:405` и `:886`.
Расхождение не скрытое — на границе его **явно компенсируют**
(`inventory.cpp:137-138`):

```
case ERROR_TIMEOUT:
case WAIT_TIMEOUT:          // ← «оба», потому что приходят оба
    return DeviceState::TimedOut;
```

Что видит пользователь. Отказ диска доходит до отчёта и до текстового вывода
`mrproper-cli disks`:

* `inventory.cpp:87` — `winErrorText(code)` = `platform::win32ErrorText(code) + " (" + std::to_string(code) + ")"`;
* `win32ErrorText(258)` = `toUtf8(formatSystemMessage(258))` (`win_error.hpp:298`),
  а `formatSystemMessage` идёт в `FormatMessageW`, и **для 258 система не знает
  текста**. Проверено на
  хосте этой машины: `net helpmsg 258` → «не найдено сообщение для 258»;
  `net helpmsg 1460` → «Превышен лимит времени ожидания».

Итоговая строка в логе и в `stderr` выглядит так:

```
error: DiskSize: не удалось получить размер \\.\PhysicalDrive1: не ответил за таймаут,  (258) [win32=0x00000102]
```

Пустой текст системы, число, которое ни в одном каталоге `ERROR_` не значит
таймаут, и hex-вид, который пользователь забаг-репортит и который при разборе
приводит в тупик. Ровно тот случай, который §12 закрывает требованием «все
ошибки в логе с путём и HRESULT».

Почему это не стилистика. `classify()` рядом (`storage_query.cpp:150-179`) не
знает ни 258, ни 1460, поэтому `QueryStatus` спасает только внутри своего же
модуля (`:543`, `:561` сравнивают с `WAIT_TIMEOUT`). Любой другой потребитель
`win32Error` — а это публичное поле `DiskSizeResult`,
`VolumeSpaceResult`, `StoragePropertiesResult` — получает 258 без
согласования. `trim_cache.cpp:264` и `devices.hpp:139` при этом **документируют
контракт как `ERROR_TIMEOUT`**: «win32Error получает код (или ERROR_TIMEOUT)».

Что делать.

1. `storage_query.cpp:262`, `size_probe.cpp:395`, `size_probe.cpp:437` — писать
   `ERROR_TIMEOUT`. Вместе с ними поправить два сравнения на
   `storage_query.cpp:543` и `:561` (иначе `TimedOut` перестанет определяться).
2. `inventory.cpp:137` — оставить `case WAIT_TIMEOUT` как страховку от старых
   снимков, но пометить комментарием, что новое значение — `ERROR_TIMEOUT`.
3. Тест: юнит-функция на чистую классификацию кода таймаута в каждом из трёх
   модулей (см. F-08 — сегодня такой функции нет ни в одном).

### F-02. «Таймаут 10 с» из §9.2 — это 40 с на фазы плюс ещё 10 с на каждый прочитанный кусок

**Вердикт: ОБХОД ПОДТВЕРЖДЁН. Влияние: среднее (спека обещает верхнюю границу,
которую код не держит; сегодня модуль ещё не подключён к запуску, §5).**

Где: `src/platform/rulesync_client.cpp:757-764` (таймауты) и `:797-822` (цикл
чтения тела), `:1143-1167` (`fetchText`, общий дедлайн), `:1283` (дедлайн
проверки), `rulesync_client.hpp:116` и `:121` (10 с и 120 с).

Суть. `WinHttpSetTimeouts` принимает **четыре** отдельных времени: resolve,
connect, send, receive. Одно значение на все четыре фазы — это не «10 с на
запрос», а «до 10 с на каждую фазу», то есть до 40 с только на то, чтобы дойти до
тела ответа. Комментарий в коде это знает и тем не менее делает так:

```
// Таймауты на все четыре фазы: без них «таймаут 10 с» из SPEC §9.2 п.1
// означал бы только ожидание ответа, а не всую операцию.
const int timeout = static_cast<int>(std::clamp<std::int64_t>(request.timeoutMs, 100, 60000));
if (::WinHttpSetTimeouts(session_, timeout, timeout, timeout, timeout) == FALSE) {
```

Дальше хуже. `dwReceiveTimeout` по документации действует на `WinHttpReceiveResponse`
**и на каждый `WinHttpReadData`**, а тело читается циклом по
`kReadChunkBytes = 64 КиБ` (`rulesync_client.cpp:52`, `:797-822`). Потолок одного
файла правила — `kDefaultMaxRuleFileBytes` = 16 МБ, то есть до 256 вызовов
`WinHttpReadData`, каждый из которых может занять свои 10 с. Ответ, который
отдаётся «по капле» (64 КиБ за 9 с), держит один запрос **десятки минут**; на
потолке — около 43 минут на один файл правила.

Единственный задник — общий дедлайн 120 с (`kDefaultOverallTimeoutMs`) — **не
проверяется во время запроса**. `fetchText` считает остаток один раз, до вызова
`transport.get()`, и после возврата уже ничего не проверяет:

```
std::int64_t timeout = requestTimeoutMs;
const std::int64_t remaining = deadlineMs - monotonicMs();     // единственная проверка
if (remaining < timeout) timeout = remaining;
request.timeoutMs = timeout;
const HttpResponse response = transport.get(request);         // дальше — без контроля
```

Рядом в проекте лежит **правильная** реализация того же самого, в соседнем
транспорте `net.cpp:561-572`: бюджет делится на четыре части
(`applyTimeouts`), и комментарий прямо говорит, зачем: «сумма частей равна
остатку… обмен укладывается в общий бюджет, а не в его восьмерь». Проблема в
том, что `net::fetch` **не вызывается ниоткуда** (проверено: единственная фабрика
транспортов — `makeWinHttpTransport` в `rulesync_client.cpp:831`), то есть
работает та реализация, где таймаут неверен.

Состояние на сегодня. `rulesync_client::Updater::checkAndApply`
(`rulesync_client.hpp:394`) не имеет ни одного вызывающего в дереве — ни в `ui/`,
ни в `cli/`. Поэтому находка латентная: сейчас «10 с» не наблюдаемы, потому что
запросы не делаются. §9.2 п.1 требует проверки «при запуске», и как только эта
кнопка появится в `app_shell`/`view_settings`, обещанная граница станет ложной
именно там, где её ждёт пользователь.

Что делать.

1. Делить бюджет на фазы, как в `net.cpp` (или переиспользовать `net` целиком —
   тогда исчезнет и вторая реализация WinHTTP в проекте).
2. Ограничивать чтение тела **суммарным** бюджетом запроса: срок считать перед
   каждым `WinHttpReadData` и прерывать, когда он истёк. Тогда 16 МБ читаются
   либо за 10 с, либо отказ.
3. Общий дедлайн проверять и после `get()`, чтобы «осталось 2 с» не превращалось
   в ещё один запрос на 10 с.
4. Тест на дедлайн (F-08): транспорт с подставным чтением, отдающим куски с
   задержкой, должен вернуться за `overallTimeoutMs`.

### F-03. `GetLastError()` после вызова помощника: восемь мест в `vfs_trash`

**Вердикт: ОБХОД ПОДТВЕРЖДЁН. Влияние: среднее (тот же класс, что F-01: §12
требует в логе код отказа, а не код последнего вызова WinAPI в потоке).**

Где: `src/platform/vfs_trash.cpp:1232`, `:1260`, `:1295`, `:1397`, `:1413`,
`:1485`, `:1540`, `:1618`.

Суть. Правило сформулировано в самом проекте, в `win_error.hpp:90-92`:
«Снимок последней ошибки. Звать сразу после неудачного вызова WinAPI: любое
другое обращение к API — даже успешное — способно затереть код, поэтому
«посмотреть GetLastError() в catch на пятом экране» не работает никогда».
В `vfs_trash.cpp` оно нарушено восемь раз: код читается **после возврата из
функции, которая делала вызов WinAPI**, а не сразу после него.

```cpp
// vfs_trash.cpp:1292-1300
const TrashStatus measured = readPathFacts(request.sourcePath, result.facts, options);
if (measured != TrashStatus::Ok) {
    result.status = measured;
    result.win32Error = ::GetLastError();     // ← это код «где-то внутри readPathFacts»
```

Два места из восьми — **доказуемо** неверные, потому что отказ там не связан ни с
одним неудачным вызовом WinAPI:

1. **Отмена** (`:1295`). `readPathFacts` возвращает `TrashStatus::Cancelled` из
   проверки `options.stop.stop_requested()` (`vfs_trash.cpp:1170`) — в этот момент
   **ни один WinAPI не падал**. Перед этим в той же функции успешно отработали
   `readSecuritySddl` (`GetNamedSecurityInfoW`, `vfs_trash.cpp:1790`, на
   `ERROR_ACCESS_DENIED` код не меняет, но меняет на любой другой отказ) и
   `GetNamedSecurityInfoW` на каждом каталоге обхода. Поле `result.win32Error`
   получает произвольное число — и это публичное поле результата перемещения,
   которое пишется в журнал операции.
2. **Нехватка памяти** (`:1485`). `readManifest` возвращает
   `TrashStatus::OutOfMemory` из `catch (const std::bad_alloc&)`
   (`vfs_trash.cpp:1430`) — вызова WinAPI, который мог бы упасть, там нет вообще.
   Тернарный оператор на строке 1484-1485 подставляет настоящий код только для
   `Corrupt`; для остальных статусов, включая `OutOfMemory`, уходит
   `::GetLastError()`.

Остальные шесть мест (`ensureDirectoryTreeW`, `writeFileAtomic`,
`copyDirectoryTree`/`copyFileBytes`) чаще всего дают правильный код — но не по
замыслу, а потому что помощник возвращается сразу после своей неудачной
WinAPI-команды. Это «работает по счастливому совпадению»: тот же помощок при
возврате `InvalidArgument` (пустой путь, `vfs_trash.cpp:281`) не вызвал **ни
одного** WinAPI, и код будет чужим.

Что делать.

1. Поменять контракт помощников: возвращать пару «статус + код», а код брать
   внутри — там, где вызов WinAPI уже вернулся. Минимальный вариант без правок
   сигнатур: добавить в `TrashStatus`-путь необязательный `DWORD* errorOut`.
2. Как страховка от «статуса без кода»: если кода нет, писать
   `ERROR_INVALID_DATA`/`ERROR_CANCELLED` по статусу, а не `GetLastError()`.
3. Тест: подсунуть транзакцию с манифестом, который не читается, и проверить,
   что в логе код отказа, а не 0 и не код от предыдущего файла.

### F-04. Дескрипторы поиска томов закрываются через `CloseHandle`, а не через `FindClose`

**Вердикт: ОБХОД ПОДТВЕРЖДЁН как расхождение с контрактом и с собственным
кодом проекта. Фактическую утечку в прогоне не наблюдал — см. честную оговорку.**

Где: `src/platform/volumes.cpp:450` (`FindFirstVolumeMountPointW`) и
`volumes.cpp:659` (`FindFirstVolumeW`) — оба дескриптора кладутся в локальный
`Handle`, который закрывает их через `CloseHandle` (`volumes.cpp:93`).

Суть. Обе функции возвращают **дескриптор поиска**, и документация Windows требует
закрывать его `FindClose`: у `FindFirstVolume` в разделе Return Value сказано, что
поисковый дескриптор освобождается через `FindClose`, и то же — у
`FindFirstVolumeMountPoint`. Задача содержит ровно этот случай — обёртка
`FindHandlePolicy` в `win_handle.hpp:68-78`, с объяснением в комментарии:
«дескриптор поиска: FindFirstFileW/FindNextFileW. Закрывается FindClose».
Она и используется
везде, кроме `volumes.cpp`:

| Модуль | Вызов | Чем закрывается | Политика |
|---|---|---|---|
| `vfs_delete.cpp:65`, `:291` | `FindFirstVolumeW` | `FindClose` | `FindHandlePolicy` ✅ |
| `vfs_delete.cpp:65`, `:961` | `FindFirstFileW` | `FindClose` | `FindHandlePolicy` ✅ |
| `vfs_trash.cpp:312` | `FindFirstFileW` | `FindClose` | `FindHandlePolicy` ✅ |
| `vfs_paths.hpp`/`vfs_walk` | — | — | — |
| **`volumes.cpp:450`** | `FindFirstVolumeMountPointW` | **`CloseHandle`** | локальный `Handle` ❌ |
| **`volumes.cpp:659`** | `FindFirstVolumeW` | **`CloseHandle`** | локальный `Handle` ❌ |

Один и тот же дескриптор от одного и того же API в двух модулях одного слоя
закрывается двумя разными способами. На практике `CloseHandle` на этих дескрипторах
чаще всего срабатывает, поэтому симптом не проявляется; но код, который в
остальных 29 местах своего слоя пользуется правильной политикой, не должен
зависеть от того, что конкретный драйвер оказался снисходительным. Класс `Handle`
в `volumes.cpp` к тому же смешивает в себе и дескрипторы ядра (`CreateEventW`,
`CreateFileW` — там `CloseHandle` правильный), и дескрипторы поиска.

Честная оговорка: **наблюдаемой утечки нет**. `enumerate()` и `readMountPoints()`
запускались в прогоне `a93` (§5) на этой машине, дескрипторы не росли, а
`enumeration.completed = true`. Дефект — в контракте и в том, что он не
защищён ничем: `FindHandlePolicy` для этого и написана.

Что делать. В `volumes.cpp` завести `using FindHandle = unique_handle<FindHandlePolicy>;`
и использовать его для `readMountPoints` и `enumerate`; локальный `Handle`
оставить для `CreateFileW`/`CreateEventW`. Правка на две строки плюс смена типа
в двух местах.

### F-05. В `volumes.cpp` код отказа ожидания читается после `CancelIoEx`

**Вердикт: ОБХОД ПОДТВЕРЖДЁН. Влияние: малое (редкий путь `WAIT_FAILED`).**

Где: `src/platform/volumes.cpp:272-278`.

```cpp
const DWORD wait = ::WaitForSingleObject(event, kDeviceTimeoutMs);
if (wait != WAIT_OBJECT_0) {
    ::CancelIoEx(device, &overlapped);                       // ← затирает LastError
    DWORD discarded = 0;
    ::GetOverlappedResult(device, &overlapped, &discarded, TRUE);   // ← и это тоже
    error = (wait == WAIT_TIMEOUT) ? ERROR_TIMEOUT : ::GetLastError();   // ← а это уже не тот вызов
    return false;
}
```

Ветка `WAIT_TIMEOUT` здесь честна (`ERROR_TIMEOUT` подставляется константой).
Ветка `WAIT_FAILED` отдаёт код, оставшийся от `GetOverlappedResult` после отмены —
как правило `ERROR_OPERATION_ABORTED`, то есть «мы отменили запрос», а не
«ожидание дескриптора провалилось». Три соседних модуля в этой же ситуации
сделаны правильно, потому что разносят отмену и чтение кода по разным веткам:
`devices.cpp:246-262`, `storage_query.cpp:253-268`, `trim_cache.cpp:209-231`.

Что делать. Захватить `const DWORD waitError = ::GetLastError();` сразу после
`WaitForSingleObject`, до `CancelIoEx`. Одна строка, тот же смысл.

### F-06. `readSessionState` подменяет код `RmGetList` на `ERROR_MORE_DATA`

**Вердикт: ДЕФЕКТ. Влияния на поведение нет: `SessionState::code` не читается ни
в одном ветвлении (`process_control.cpp:1152`, `:1214` используют только
`rebootReasons` и `find()`).**

Где: `src/platform/process_control.cpp:296-300` (сама подстановка — `:298`).

```cpp
// Список уехал три раза подряд: для модуля это «состояние неизвестно», а не
// «процессов нет» — поэтому код отказа сохраняется, а список пуст.
state.code = ERROR_MORE_DATA;
```

Комментарий говорит «код отказа сохраняется», а код делает обратное: настоящий
код последнего `RmGetList` (например, `ERROR_ACCESS_DENIED` на втором проходе,
`:310`) затирается константой. Читатель лога увидит «список менялся», хотя
могло быть «нет прав». Сегодня безобидно; опасно тем, что следующий потребитель
`state.code` получит число, не соответствующее отказу, — ровно тот класс ошибки,
что F-01 и F-03.

Что делать. Не затирать: `state.code` уже содержит код последней попытки; если нужен
явный «не знаем», завести отдельный флаг, а не подменять код. Плюс
`// код отказа НЕ сохраняется` в комментарий — сейчас он вводит в заблуждение
следующего читателя.

### F-07. Результат `SetHandleInformation` не проверяется

**Вердикт: ДЕФЕКТ. Влияние малое, но проверка стоит одну строку.**

Где: `src/platform/wmi.cpp:1055`.

```cpp
// Родителю конец записи не нужен, а ребёнку конец чтения передавать нельзя:
// иначе EOF в трубе не наступит и чтение будет ждать вечно.
::SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
```

Комментарий объясняет, что будет, если сброс не сработает, и не проверяет, что
сброс вообще состоялся. При отказе ребёнок унаследует конец чтения; на практике
это не приводит к зависанию (EOF наступает по выходу `manage-bde`, а у процесса
есть собственный таймаут `kManageBdeTimeoutMs` и `TerminateProcess` в
`wmi.cpp:1136-1140`), поэтому это не обход, а незакрытый угол. В том же модуле
рядом стоят `nul`, `parentRead`, `childWrite`, `processHandle`, `threadHandle`,
`readEvent` — все на `unique_handle`, и этот вызов — единственный, чей результат
молча игнорируется.

Что делать. `if (::SetHandleInformation(...) == FALSE) { run.hr = lastErrorHresult(); return run; }`.

### F-08. Слой устройства, COM и транспорта не покрыт ни одной проверкой

**Вердикт: ПРОБЕЛ ПОКРЫТИЯ. Влияние: ни одно из исправлений выше не защищено от
возврата.**

Что покрыто сегодня. Из всего `src/platform` в тестах упоминаются только
`vfs_walk`, `vfs_size`, `vfs_paths`, `vfs_delete`, `win_handle`, `win_error`
(`tests/integration/vfs_walk_tests.cpp`, `vfs_edge_tests.cpp`). Зелёные 305 проверок
приходятся на ядро (`core`, `plan`, `sizing`, `trash_undo`, `rulesync` как
*политика*, `report`, `i18n`).

Что не покрыто ничем: `devices`, `volumes`, `layout`, `storage_query`,
`size_probe`, `trim_cache`, `wmi`, `process_control`, `restart_manager`,
`rulesync_client`, `net`. Ни один из них не имеет и теста на классификацию кодов
ошибок — а именно на ней держатся F-01, F-05, F-06.

Минимум, который стоит добавить (три теста, все на чистых функциях, Windows не
нужен по правам):

1. `deviceStateFromWin32` / `classify` / `classifyFailure` принимают **и**
   `ERROR_TIMEOUT`, и коды отказа ожидания, и отдают `TimedOut` — тест на F-01;
2. классификация отказа `WaitForSingleObject` не теряет код отмены — тест на F-05;
3. транспорт правил укладывается в `overallTimeoutMs` на медленной сдаче ответа —
   тест на F-02 (нужен подставной транспорт, а не сеть).

### F-09. Утечек HANDLE, COM и буферов не найдено — контроли ADR-001 работают

**Вердикт: СООТВЕТСТВУЕТ СПЕКЕ. Это результат, а не отсутствие результата.**

Что проверено и признано исправным:

* **Политики закрытия как отдельные типы** (`win_handle.hpp:44-88`):
  `KernelHandlePolicy`, `FindHandlePolicy` (`:68-78`), `RegistryHandlePolicy` — и
  `unique_handle` с запретом копирования. Забыть закрыть дескриптор в модуле,
  который использует `win_handle.hpp`, структурно нельзя; выпустить наружу можно
  только через `release()`, который читается как «дескриптор теперь не мой».
  Используют: `vfs_walk.cpp:58` (`ScopedHandle`), `vfs_trash.cpp:312`
  (`FindHandlePolicy`), `vfs_paths.cpp:456` (`adopt`), `ui/theme.cpp:158`
  (`RegistryHandlePolicy`).
* **Отмена IOCTL не освобождает буфер раньше времени.** Во всех пяти модулях с
  `DeviceIoControl` одна и та же схема: `FILE_FLAG_OVERLAPPED` →
  `WaitForSingleObject` → при таймауте `CancelIoEx` → `WaitForSingleObject` с
  запасом, и только если отмена не завершилась, буфер **намеренно утекает**
  (`devices.cpp:252-256`, `storage_query.cpp:255-262`,
  `trim_cache.cpp:211-224`). Комментарий в каждом месте объясняет, почему
  освобождение было бы use-after-free. Это ровно тот случай, где «утечка» —
  правильное поведение, и оно задокументировано.
* **Отсоединённый поток не теряет память.** `size_probe.cpp:141-190`: поток
  `detach()` после таймаута, состояние живёт в `shared_ptr`-слоте, счётчик
  брошенных вызовов выводится наружу (`probeTimeoutStats`, `:352-355`) и в
  `inventory.cpp:1199`. Отказ потока создания отличен от таймаута
  (`ERROR_NOT_ENOUGH_MEMORY`), §5 соблюдён.
* **COM.** `wmi.cpp`: `ComPtr` (`:210`), `Bstr` (`:259`), `Variant` (`:294`),
  `EnumeratorGuard` (`:326`) — `Release()` в деструкторе,
  `SysFreeString` на `nullptr` допустим, `VariantClear` на выходе. Квартира
  COM: `CoUninitialize` вызывается **только** при `S_OK`, `S_FALSE` и
  `RPC_E_CHANGED_MODE` его не трогают (`wmi.cpp:1235-1267`), и в UI-потоке
  то же самое с явной оговоркой (`app_shell.cpp:828-836`).
* **Сессия Restart Manager закрывается ровно один раз.** `RmSession`
  (`process_control.cpp:170-234`): `end()` обнуляет ключ **до** вызова
  `RmEndSession`, поэтому повторный вызов из деструктора — no-op. Дескрипторы
  `hProcess`/`hThread` из `CreateProcessW` и оба конца трубы — на
  `unique_handle` (`wmi.cpp:1056-1077`), при отказе `CreateProcessW` всё закрывается
  деструкторами.
* **`FormatMessageW` всегда с `LocalFree`** (`win_error.hpp:128-138`,
  `size_probe.cpp:346`, `vfs_size.cpp:364`, `vfs_trash.cpp:1802-1834`).
* **GDI.** `renderer.cpp`: `GetDC`/`ReleaseDC` парны на всех путях
  (`:1003-1025`, `:1350-1353`), `CreateCompatibleDC`/`DeleteObject` при отказе
  (`:1021-1026`), `SelectObject` восстанавливается (`:697`, `:1430-1432`),
  `backBufferDc` освобождается перед `Present` (`:689`). `BeginPaint`/`EndPaint`
  парны даже при отказе по `dc == nullptr` (`app_shell.cpp:802-812`).
* **Права на дескриптор меньше нужного не превращают ошибку в успех**:
  `size_probe.cpp:208-218` (повтор с `GENERIC_READ` после `ERROR_ACCESS_DENIED`
  с честным признанием отказа), `vfs_paths.cpp:451-457` (`createError` читается
  сразу и подстраховывается на `ERROR_INVALID_HANDLE`).

## 4. Порядок исправлений

1. **F-01** — три строки (`storage_query.cpp:262`, `size_probe.cpp:395` и `:437`)
   плюс два сравнения (`:543`, `:561`). Самое дешёвое и самое заметное: правится
   текст, который пользователь читает в отчёте при отказе диска.
2. **F-02** — таймаут транспорта правил. До привязки проверки к запуску
   (`§9.2 п.1`), но после неё это будет висящий UI на минуты. Делить бюджет на
   фазы, ограничивать чтение тела суммарным сроком, проверять дедлайн после
   `get()`. Заодно решить, что делать со вторым транспортом: `net.cpp` без
   вызывающих — либо удалить, либо перейти на него.
3. **F-03** — контракт помощников `vfs_trash` плюс восемь мест чтения кода.
4. **F-04, F-05, F-06, F-07** — по одной-две строки каждое; F-05 и F-06
   обязательно, потому что это ровно тот же класс, что F-01.
5. **F-08** — три теста из списка в F-08, иначе пункты 1-4 не защищены.

## 5. Как проверялось

Ревью статическое по коду плюс прогоны **штатного бинарника** в слоте `a93`.
Сборка и тесты слота — зелёные, чужих файлов не тронуто:

```
cmd.exe /c "tools\build.bat Debug a93"   → [build] ok: build\a93
cmd.exe /c "tools\test.bat Debug a93"    → ALL PASS: 305 проверок, провалов 0
```

Оба прогона выполнены из корня репозитория, слот `a93` — обязательный второй
аргумент (без него все агенты пишут в общий `build\main`). Правок в код не
вносилось, поэтому цифры совпадают с базовыми 305/0.

Проверки, выполненные на самой машине (не разбором кода):

```
cmd.exe /c "net helpmsg 258"   → «не найдено сообщение для 258»
cmd.exe /c "net helpmsg 1460"  → «Превышен лимит времени ожидания»
```

Это единственное, что превращает F-01 из «в поле число из другого пространства» в
«пользователю показывают пустой текст системы и 0x00000102»: `win32ErrorText`
(`win_error.hpp:298-301`) строит текст ровно через `FormatMessageW`, а система
для 258 текста не знает.

Прогон обхода дисков на этой машине (слот `a93`) нужен был для F-04: он
проверяет, что пути `enumerate()`/`readMountPoints()` доходят до конца
(`Enumeration::completed = true`), то есть дескрипторы поиска томов освобождаются
на практике. Ни одного зависшего диска, ни одного `WAIT_FAILED`, ни одной
медленной сдачи ответа по HTTPS на этой машине воспроизвести не удалось —
эти пути в ревью разобраны по коду, и выше это указано явно.

Чего прогоны **не** доказывают: F-01 требует диска, уходящего в IOCTL, F-02 —
сервера, отдающего ответ по капле, F-05 — отказа `WaitForSingleObject`. Все три
подтверждены разбором кода путём от WinAPI-вызова до поля, которое читает человек,
а не наблюдением в рантайме. Интеграционные тесты, которые требуют создания
каталогов и прав администратора, в этом прогоне не запускались (хост без повышения:
`\\.\PhysicalDriveN` не открывается, поэтому `disks` даёт пустую карту).

## 6. Приложение: инвентарь владельцев дескрипторов

Для быстрой проверки F-04 и для будущих ревью — кто чем владеет в слое. Пустая
ячейка означает «дескрипторов не заводит».

| Модуль | Дескрипторы ядра | Поиск | COM | Реестр | Своя политика закрытия |
|---|---|---|---|---|---|
| `vfs_walk` | `CreateFileW` на каталог | — | — | — | `unique_handle<KernelHandlePolicy>` |
| `vfs_trash` | `CreateFileW` (перенос, отметки) | `FindFirstFileW` | — | — | `unique_handle` + `FindHandlePolicy` |
| `vfs_delete` | `CreateFileW` | `FindFirstFileW`, **`FindFirstVolumeW`** | — | — | `unique_handle` + `FindHandlePolicy` |
| `vfs_paths`, `vfs_size` | `CreateFileW` | — | — | — | `adopt` / `unique_handle` |
| `size_probe` | `CreateFileW` (диск) | — | — | — | локальный `UniqueHandle` |
| `storage_query` | `CreateFileW`, `CreateEventW` | — | — | — | `unique_handle<KernelHandlePolicy>` |
| `trim_cache` | `CreateFileW`, `CreateEventW` | — | — | — | `unique_handle` + `PendingRequest` |
| `devices` | `CreateFileW`, `CreateEventW` | — | — | — | `unique_handle` + `DeviceInfoSetPolicy` |
| `layout` | `CreateFileW` (диск на чтение) | — | — | — | локальный `ScopedHandle` |
| **`volumes`** | `CreateFileW`, `CreateEventW` | **`FindFirstVolumeW`, `FindFirstVolumeMountPointW` → `CloseHandle`** | — | — | локальный `Handle` (**F-04**) |
| `wmi` | `CreateFileW` (NUL), `CreatePipe`, `CreateEventW`, `CreateProcessW` | — | `CoCreateInstance`, `IWbem*` | — | `unique_handle`, `ComPtr`, `Bstr`, `Variant`, `EnumeratorGuard`, `ComApartment` |
| `process_control` | `CreateToolhelp32Snapshot`, `OpenProcess` | — | — | — | `ScopedHandle`, `RmSession` (не HANDLE) |
| `restart_manager` | — | — | — | — | собственная сессия RM |
| `rulesync_client` | `CreateFileW` (чтение/запись набора) | `FindFirstFileW` → `FindClose` ✅ | — | — | ручной `FindClose`/`CloseHandle` + `WinHttpCloser` |
| `net` | — | — | — | — | `unique_handle<InternetHandlePolicy>` |
| `ui/renderer` | GDI: DC, DIB, кисти, шрифты | — | DXGI/D2D/DWrite | — | `ComPtr` + явные `Release`/`DeleteObject` |
| `ui/theme` | — | — | — | `RegCreateKeyExW` | `unique_handle<RegistryHandlePolicy>` |
