# MrProper: дымовой прогон окна с проверкой «окно не пустое».
#
# Зачем: окно компилируется, линкуется и запускается, но оболочка могла не
# подключить ни рельс навигации, ни экраны — и всё это выглядит как «приложение
# работает», пока человек не откроет его. Проверка ловит именно это: берёт
# содержимое окна через PrintWindow и считает пиксели, отличные от фона.
# Рамку и заголовок в счёт не берём — их рисует DWM, и пустое окно с заголовком
# проходит наивную проверку «процесс жив» именно на них.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\ui-smoke.ps1 -Exe <путь> [-Shot <png>]
#   ... -Width 1280 -Height 720                    окно 1280x720 логических пикселей
#   ... -Width 1280 -Height 720 -DpiPercent 150    то же при 150 % (окно 1920x1080)
#   ... -ThemeMode light -ExpectBackground light   палитра пришла из темы, а не из константы
#   ... -Page disks -ThemeMode dark                страница выбрана состоянием (БЕЗ фокуса),
#                                                 проверка кода 14 работает и без -AllPages
#
# -Width/-Height заданы в логических пикселях (DIP, 96 DPI = 1.0), а не в
# физических: при -DpiPercent 150 окно 1280x720 DIP становится окном 1920x1080
# физических пикселей — ровно тот случай, который в задании назван «1280x720 в
# логических пикселях при 150 %». Оба ключа задаются вместе: задать один
# значит спросить про раскладку, которой нет.
#
# -DpiPercent не меняет масштаб экрана: это требует прав администратора и выхода
# из сеанса, а в CI такой возможности нет вовсе. Вместо этого скрипт шлёт
# окну WM_DPICHANGED_AFTERPARENT (0x02E3) — то самое сообщение, которым
# Windows в режиме per-monitor v2 сообщает смену DPI дочерним окнам, и которое
# оболочка проводит через тот же handleDpiChanged, что и WM_DPICHANGED.
# Почему не 0x02E0: отправленный из другого процесса WM_DPICHANGED до окна не
# доходит вообще (проверено: WM_GETMINMAXINFO до и после отправки отдаёт одни и
# те же 900x600), а 0x02E3 доходит и переводит приложение на dpi 144
# (900x600 -> 1350x900). Это эмуляция смены монитора, а не самого монитора:
# нетронутными остаются неклиентская рамка (её рисует система в своём DPI) и
# реальный per-monitor контекст — их проверяет только сеанс с масштабом 150 %.
#
# Числа, а не слова: после отправки скрипт спрашивает у окна WM_GETMINMAXINFO и
# требует, чтобы минимум окна вырос ровно в effDpi/96 раз. Молчаливый «масштаб
# не применился» дал бы зелёные ворота на непроверенной раскладке.
#
# -ThemeMode пишет СВОЙ ключ приложения HKCU\Software\MrProper\UI\ThemeMode
# («light»/«dark»/«auto») и возвращает прежнее значение после прогона. Системная
# тема (Personalize\AppsUseLightTheme) не трогается: это настройка машины, а не
# приложения, и «переключить у себя тему Windows» — не то же самое, что проверить
# тёмную палитру продукта.
#
# Чернила считаются в ДВУХ областях, и это не перестраховка, а смысл проверки.
# Область «клиент» — вся клиентская область окна. Область «содержимое» — только
# окно содержимого (дочернее окно справа от рельса навигации). Рельс рисует
# оболочка сама и нарисует его всегда, поэтому одного клиента достаточно, чтобы
# пропустить ровно тот отказ, который ворота и ловят: окно 1136x795, рельс из
# пяти пунктов, а справа — заливка цветом windowBackground, то есть интерфейса
# нет. Проверено числами: при пустом хосте содержимого в нём 0 чернил, а при
# нарисованном экране — от 16530 (экран очистки, самый пустой из пяти) до
# 103745 (экран настроек, светлая панель). Порог по умолчанию 5000 — втрое
# ниже самого пустого нарисованного экрана и в 5000 раз выше пустого хоста;
# замер сделан тем же алгоритмом (шаг 2, самый частый цвет — фон), что и здесь.
# -MinContentInkPixels 0 отключает проверку содержимого — так меряют только
# оболочку, когда экран наполняет ещё другой слой.
#
# Область содержимого берётся у самого окна: скрипт перечисляет дочерние окна
# и берёт самое крупное видимое (сейчас это ровно одно — хост содержимого,
# 896x760 при окне 1136x795, то есть рельс 240 пикселей). Ширина рельса в коде
# не зашита: как только её станет больше 240, зашитая константа тихо съела бы
# часть содержимого и проверка стала бы зелёной на пустом экране.
#
# Второй критерий на ту же область — число разных цветов. Он ловит то, чего не
# ловит счёт пикселей: окно содержимого на 19 пикселей выше клиентской области
# оставляет свою неприпаркованную полосу видимой снизу и справа, это 13152
# «чернильных» пикселя при 2 цветах — и версия «работает и пуста» прошла бы
# счёт. Нарисованный экран даёт 7..9 цветов даже без ClearType.
#
# Третий критерий — чернила в центральных 70 % окна содержимого. Он ловит
# ложнозелёный случай, когда окно содержимого съехало и накрыло рельс: при
# 150 % DPI оно вылезает на 31 пиксель влево, и 7590 «чернил» с 7 цветами из
# подписей рельса дали код 0 при пустом экране. В центре 70 % у того же окна
# 0 чернил и 1 цвет, у пяти нарисованных экранов — от 409 (очистка) до 29264.
# Порог 250 — в 1,6 раза ниже самой пустой нарисованной страницы.
#
# ОБХОД ВСЕХ СТРАНИЦ (-AllPages). Рельс и пять экранов: ворота выше смотрят на
# стартовую страницу, а отказ живёт в остальных четырёх — кнопки «Очистки»,
# обрезанные заголовок и единица измерения в плитке «Дисков» проходят зелёными.
# Обход нужен ещё и потому, что раскладка зависит от высоты окна: при 1280x720
# (клиент 685) нижние кнопки стоят в y=607..677, а при минимальных 900x600
# (клиент 565) последняя кнопка «Отчёта» уезжает на 9 пикселей за правый край
# хоста содержимого и наполовину обрезается краем окна.
#
# Переключение страниц — настоящими клавишами Ctrl+1..5 через keybd_event, а не
# сообщениями WM_KEYDOWN: обработчик рельса читает GetKeyState(VK_CONTROL), а
# синтетическое сообщение состояние клавиатуры не меняет, поэтому PostMessage
# давал тихий ноль — страница не открывалась, а ворота этого не замечали.
# Перед вводом окно приводится на передний план через ALT-нажатие: без него
# SetForegroundWindow отклоняется, и нажатия уходят в чужое окно. И то и другое
# НЕЛЬЗЯ делать на машине человека молча — ключи уходят в системный ввод, окно
# MrProper всплывает и перехватывает фокус. Поэтому обход включается ключом
# -AllPages, а не всегда: в CI его просит шаг, локально — человек.
#
# После переключения страница проверяется не «нарисован ли экран», а двумя
# числами на КАЖДЫЙ видимый потомок окна содержимого:
#   * размер ненулевой — иначе элемент есть, а нажать его нечем;
#   * прямоугольник целиком внутри клиентской области хоста содержимого —
#     именно это ловит кнопку, вылезшую за край. Хост содержимого берётся
#     «самым крупным видимым дочерним окном», как и выше, а сравнение идёт в
#     экранных координатах: так не нужно пересчитывать систему координат и
#     нельзя перепутать клиентскую и оконную.
# Допуск -LayoutTolerance (по умолчанию 1 пиксель) — на округление DIP→пиксель
# вокруг рамок; девять пикселей вылета он не скрывает.
#
# Проверяется только живая ветвь: ShowWindow(SW_HIDE) снимает WS_VISIBLE с окна
# экрана, но не с его потомков, поэтому перечисление всех потомков хоста видело
# бы элементы четырёх скрытых страниц. Сначала берутся прямые дети хоста, и
# обходятся лишь те из них, что видимы.
#
# Обход не состоялся — код 11, а не зелёный: иначе ворота, которые не смогли
# открыть ни одну страницу, рапортовали бы «раскладка в порядке» о пустоте.
#
# СТОЛБЦЫ СПИСКА ПО ПИКСЕЛЯМ (S1). Прежняя проверка брала число столбцов
# сообщением LVM_GETCOLUMNCOUNT. В commctrl.h это ТО ЖЕ САМОЕ число 0x1004, что
# LVM_GETITEMCOUNT, то есть comctl32 отдаёт в нём ЧИСЛО СТРОК; имени с
# отдельным значением не существует. Дальше ворота складывали LVM_GETCOLUMNWIDTH
# по этому индексу, а для индекса за последним столбцом comctl32 отдаёт ширину
# последнего — сумма всегда выходила больше полосы заголовка (428 + 9*96 = 1292
# при полосе 648) и ворота краснели при ЛЮБОЙ вёрстке, требуя, чтобы имя узла
# занимало 1 px. Через чужой процесс число столбцов не берётся: HDM_GETITEMCOUNT
# = 0x0000 = WM_NULL отдаёт 0, а HDM_GETITEMW и LVM_GETCOLUMNW с указателем из
# чужого процесса приложение убивают (проверено на первой версии пробы).
# Теперь границы столбцов определяются по снимку: comctl32 рисует между
# столбцами вертикальную полосу шириной 1..4 px, у которой вся высота полосы
# заголовка отличается от её фона, а подпись столбца такой полосой быть не
# может. Измерено на снимках: доля ненфоновых пикселей 1,000 у каждого
# разделителя и не выше 0,706 у колонок с подписями; порог 0,90. Разделитель,
# от которого до предыдущей границы меньше -MinColumnWidthPx, границей не
# считается: в светлой теме край полосы прокрутки рисуется белой линией в 1 px
# в 6 px от последнего разделителя, и без этого правила «Настройкам» выдавался
# четвёртый столбец шириной 6 px.
# Проверенные числа, живые прогоны ворот при 900x600:
#   «Диски»     3 столбца 424+92+92  полоса 646  хвост 26
#   «Отчёт»     4 из 88+80+128+316   полоса 642  хвост 14
#   «Настройки» 3 столбца 380+118+120 полоса 630  хвост  6
# то есть ровно те, что объявляет src\ui (kColumnCount = 3, 3, 3) — при 1136x795
# «Отчёт» даёт все шесть: 88+80+128+316+116+100 при полосе 880.
# Честная граница метода записана в коде: у «Отчёта» при 900x600 шесть столбцов
# 92+84+132+320+120+104 = 852 не помещаются в полосу 642, последние два обрезаны,
# хвост 14 px, и по пикселям это от пустого места полосы не отличить.
# Метод «изменение цвета колонки на всю её высоту» отвергнут: в области строк
# границу столбцов не рисует никто, а NM_CUSTOMDRAW красит подпункты на плоской
# заливке, поэтому изменения цвета дают глифы подписей (7 «столбцов» вместо 3).
#
# НЕВИДИМЫЕ СТРОКИ СПИСКА (S1). В тёмной теме список «Дисков» рисуется белым по
# белому: сплошной белой блок, в котором нет ни одной подписи. Общие счётчики
# чернил его не видят — они меряют геометрию и «чернила» всего окна, а сплошная
# заливка даёт и то и другое. Поэтому у каждого SysListView32 и SysTreeView32
# меряется область строк: пиксель считается текстом, когда он отличается и от
# фона строки (самый частый цвет области), и от фона страницы.
# Живые числа того же бинарника при 900x600, 10 строк в списке «Дисков»:
#   тёмная тема (дефект): текста 0 px из 114380 (0%), полоса текста 0 px
#   светлая тема:        текста 12108 px из 239020 (5,07%), полоса 574 px (88,85%)
#   «Настройки», 138 строк: текста 18364 px из 127260 (14,43%), полоса 566 px (93,4%)
# Порог задан по ширине полосы в процентах от области строк (-MinListTextSpanPercent,
# 3 %) плюс абсолютный порог пикселей (-MinListTextPixels, 400): у значка строки
# полоса 16 px, у одной короткой подписи около 60 px, и число пикселей у них
# различается вдвое, а ширина полосы — вчетверо.
# Область строк берётся по пикселям, а не по клиентской области окна списка: у
# списка «Дисков» окно 884x584, а нарисованные строки — полоса 177 px, остальное
# клиентской области остаётся фоном страницы (причина — WS_EX_TRANSPARENT у
# SysListView32, см. Measure-RowArea).
# Пустой список — законное состояние («ничего не найдено», «отчёт ещё не
# получен»), и в нём текста нет по определению: проверка применяется только к
# непустому, а «пусто» от «не видно» различает число строк (LVM_GETITEMCOUNT
# 0x1004 и TVM_GETCOUNT 0x1105 — единственные сообщения, которые ворота шлют
# контролу, и оба возвращают число без указателей, то есть чужую память не
# трогают). Проверено: у списка «Отчёта» и у пробного списка «Очистки» строк 0,
# проверка не применяется, обход зелёный.
#
# ПОДПИСЬ ДИСКА НА КАРТЕ РАЗДЕЛОВ (D-81).
#
# Карта разделов рисуется СВОИМИ РУКАМИ (Direct2D, ADR-003):
# ни одно сообщение окна про неё не говорит, и прежние меры
# ворот (прямоугольники потомков и общие чернила) дефект
# D-81 не видели вовсе — при FontScalePercent 200 заголовок
# диска срезался до 3-6 px видимых глифов против 10-11 px
# при шкале 100 %, зазор до полосы сегментов схлопывался,
# а ворота давали код 0: наложение внутри одного окна не
# меняет ни число прямоугольников, ни долю чернил.
#
# Мера та же, что у строк списка (Measure-RowText): пиксель
# считается чернилами, когда он отличается от самого частого
# цвета области (фон карты — render::Renderer::beginFrame
# заливает её палитрой surface перед полосами). Профиль
# чернил по строкам карты делит её на три полосы:
#   * подпись диска («Диск 0» слева, модель и шина справа)
#     — узкая полоса чернил: текст занимает долю ширины,
#     а не всю её;
#   * полоса сегментов — сплошная заливка дорожки: чернила
#     занимают почти всю ширину карты (измерено: 636 px
#     из 648, то есть 98 %);
#   * подпись «Свободно … из …» под полосой.
#
# Видимая высота подписи — число строк от первой строки с
# чернилами до последней строки перед полосой сегментов
# (допуск двух пустых строк: сглаживание шрифта даёт
# редкие строки без чернил внутри глифов). Полоса сегментов
# не найдена — карта пуста (диски не прочитаны или всё
# отфильтровано): у пустой карты подписи нет по определению,
# и это законное состояние, а не дефект.
#
# Отказ — ДВА условия сразу, а не одно. Абсолютное
# (подпись ниже -MinMapLabelHeightPx) — ровно критерий
# приёмки D-81: высота подписи при 200 % не ниже, чем
# при 100 %. Относительное (подпись ниже половины
# высоты полосы сегментов) — страхует от ложного красного
# на машине с большим числом дисков: карта сжимается
# (computeMap, kMinBlockScale 0.55), и при сжатии И
# подпись, И полоса уменьшаются одним множителем, так что
# их отношение не меняется; при срезе же подпись вдвое
# ниже полосы, потому что полоса рисуется на всю высоту
# блока, а подпись — в срезанной константой полосе.
# Измерено на этом же бинарнике (900x600, один диск):
#   шкала 100 %: подпись 10 px, полоса 16 px (10 >= 8)
#   шкала 200 % до правки: подпись 6 px, полоса 16 px
#     (6 < 10 и 12 < 16 — оба условия, код 15)
#   шкала 200 % после правки: подпись 24 px, полоса 31 px
#     (24 >= 10 и 48 >= 31 — код 0)
#
# Снимки страниц: -Shot задаёт имя БАЗОВОГО файла, обход дописывает суффикс
# -p<N>-<имя> перед расширением (D:\Temp\ui.png -> D:\Temp\ui-p3-cleanup.png).
#
# Коды возврата:
#   0 — окно нарисовало содержимое;
#   2 — неверные аргументы;
#   3 — окно не приняло запрошенный размер или DPI;
#   4 — окно пустое: нет ни клиента, ни содержимого (только фон);
#   5 — окно не создано;
#   6 — процесса нет или он упал;
#   7 — среда не подготовлена (реестр недоступен, нужен -ThemeMode);
#   8 — фон не соответствует ожидаемой теме;
#   9 — окно содержимого не найдено, в нём только фон, в нём меньше
#       -MinContentColors разных цветов или в его центральных 70 % меньше
#       -MinCenterInkPixels чернил;
#  10 — сломана раскладка: на обойдённой странице видимый потомок окна
#       содержимого имеет нулевой размер или выходит за его клиентскую
#       область, либо столбцы списка не помещаются в полосу шапки
#       (только при -AllPages);
#  11 — обход не состоялся: окно не удалось привести на передний план или
#       открыть страницу (только при -AllPages);
#  12 — открылась не та страница: -ExpectPage не совпала с видимой (значит
#       состояние интерфейса не сброшено и ворота меряют чужой запуск);
#  13 — прогоны разошлись: -DeterminismRuns 2 и числа двух запусков одной и
#       той же команды не совпали (окно, клиент, чернила, цвета, страница);
#  14 — строки списка или дерева нарисованы невидимыми: список непустой, а
#       текста в области строк меньше -MinListTextPixels или он занимает
#       полосу уже -MinListTextSpanPercent % ширины. Применяется к СТАРТОВОЙ
#       странице всегда (страница выбирается ключом -Page — см. «СТРАНИЦА БЕЗ
#       ФОКУСА»), а к остальным четырём — при -AllPages.
#  15 — подпись диска на карте разделов срезана: видимая высота подписи
#       ниже -MinMapLabelHeightPx и ниже половины высоты полосы сегментов
#       (D-81; страница «Дисков», карта непустая — полоса сегментов
#       на снимке есть)
#
# СТРАНИЦА БЕЗ ФОКУСА (ключ -Page). Проверка невидимых строк меряет область
# строк списка на ОТКРЫТОЙ странице, а открыть страницу по-старому можно было
# только обходом -AllPages: он жмёт Ctrl+1..5 настоящим вводом (keybd_event)
# после ALT-приёма, то есть крадёт фокус, а переднее окно на машине человека
# почти всегда занято чужим приложением (chrome). Локально -AllPages честно
# даёт код 11, и числа проверки кода 14 оказываются недостижимы — а именно
# локально их и надо уметь получать: отсутствие числа не отличить от «дефекта
# нет». Измерено (docs/defects.md §3.18.1): ни в одном локальном прогоне волн
# S и T проверка не выполнилась ни разу, тогда как в CI она выполняется.
#
# Решение — выбрать страницу состоянием, а не вводом. Приложение само
# восстанавливает сохранённую страницу из HKCU\Software\MrProper\UI\nav.current
# (Navigator::importState, src/ui/nav.cpp; ключ формата — «disks», то есть ровно
# pageKey() из src/ui/nav.hpp), поэтому ворота пишут это значение и запускают
# приложение БЕЗ --fresh-ui-state. Ни SetForegroundWindow, ни ALT, ни keybd_event
# при этом не вызываются: переднее окно остаётся прежним, ворота ничего не
# крадут и не печатают. Ключ -Page <overview|disks|cleanup|report|settings>
# делает ровно это, проверяет по -ExpectPage, что страница действительно
# открылась (иначе код 12), и возвращает прежнее значение nav.current после
# прогона — включая состояние «значения не было».
#
# Что -Page НЕ заменяет: обход -AllPages нужен, чтобы проверить все пять
# страниц за один прогон, и он по-прежнему требует переднего плана (код 11).
# -Page даёт одну страницу честно и без фокуса.
#
# СОСТОЯНИЕ ИНТЕРФЕЙСА: почему ворота приводят его к известному (Q3).
#
# Измерено: одна и та же команда в разные прогоны дала окно 1136x795 с
# 127 490 чернилами и окно 1280x720 с 128 149 чернилами. Причина не в окне и не
# в отрисовке: приложение наследует из HKCU\Software\MrProper\UI выбранную
# страницу (nav.current) и положение окна (window.placement), то есть ворота
# меряют не приложение, а то, что человек оставил в прошлый раз. На раннере CI
# это не видно (ветка чистая на каждый запуск), а на машине разработчика —
# ровно то, что и было измерено.
#
# Сброс по умолчанию (ключ -KeepUiState его отключает): приложение запускается
# с документированным флагом запуска --fresh-ui-state (см. src/ui/app_shell.cpp,
# раздел «Флаг запуска»). Флаг, а не удаление ключей реестра скриптом:
#   * скрипт обязан знать ВЕСЬ список сохраняемых ключей, а он пополнялся уже
#     трижды; забытый ключ означал бы тихо вернувшиеся ворота к прежнему;
#   * удаление ключей на машине человека стирает его состояние — ворота не
#     имеют права это делать, а флаг не пишет ничего;
#   * «состояние по умолчанию» считает само приложение, поэтому ворота и
#     приложение не могут разойтись в том, что считать состоянием.
#
# Ключ -DeterminismRuns N превращает это правило в проверку: приложение
# запускается N раз ОДНОЙ И ТОЙ ЖЕ командой, и все числа прогона (размер окна,
# размер клиента, чернила клиента, чернила содержимого, число цветов, страница)
# обязаны совпасть до цифры. Расхождение — код 13, а не зелёный прогон.
#
# Что сброс НЕ делает намеренно: FontScalePercent (масштаб шрифта) лежит в том
# же ключе реестра, но читается не хранилищем состояния, а theme.cpp, и флагом
# не охвачен. Пока он есть, скрипт приводит его к 100 и возвращает прежний —
# тем же приёмом, которым уже готовится -ThemeMode.
#
# Прав администратора не требует и не должен: приложение только рисует окно, а
# состояние готовится в HKCU собственного ключа. Порог чернил подобран по
# худшему состоянию — пустому скану («ничего не найдено», а не пустота); см.
# CONTRIBUTING.md, раздел «Ворота интерфейса».
param(
    [string]$Exe = 'build\main\src\ui\Debug\mrproper.exe',
    [string]$Shot = 'D:\Temp\mrproper-window.png',
    [int]$MinInkPixels = 2000,
    [int]$MinContentInkPixels = 5000,
    [int]$MinContentColors = 4,
    [int]$MinCenterInkPixels = 250,
    # --- строки списка: текст должен быть виден (см. шапку, «СТРОКИ СПИСКА») --
    # Пороги заданы в ПИКСЕЛЯХ КАДРА, а внутри скрипта выборка идёт с шагом 2 по
    # обеим осям, то есть одна посчитанная точка равна четырём пикселям.
    [int]$MinListTextPixels = 400,
    # Ширина полосы, которую занимает текст строк, в процентах от ширины области
    # строк. Значок строки (у «Дисков» это квадрат 16 px слева) текстом не
    # считается: измеренная полоса при «белым по белому» равна 1,8 % ширины,
    # у нарисованного списка — от 9 % (одна короткая подпись) до 97 %.
    [double]$MinListTextSpanPercent = 3.0,
    # Уже столбца: подпись в него не помещается ни при каком шрифте.
    [int]$MinColumnWidthPx = 24,
    # Видимая высота подписи диска на карте разделов (D-81): при
    # масштабе шрифта 100 % измеряется 10-11 px, при 200 % до
    # правки D-81 — 3-6 px, после — 24 px. Порог — ровно высота
    # подписи при шкале 100 %. Второе, относительное условие
    # (подпись ниже половины высоты полосы сегментов) держит
    # проверку честной на машине с большим числом дисков, где
    # карта сжимается (computeMap, kMinBlockScale): и подпись,
    # и полоса сжимаются одним множителем, их отношение при
    # этом не меняется, а при срезе — меняется вдвое.
    [int]$MinMapLabelHeightPx = 10,
    [int]$Width = 0,
    [int]$Height = 0,
    [int]$DpiPercent = 0,
    [string]$ThemeMode = '',
    [string]$ExpectBackground = '',
    [int]$SettleSeconds = 7,
    [int]$RepaintSeconds = 2,
    # Поиск окна и разворачивание окна делаются с повторами: на машине,
    # где параллельно работают другие агенты, окно то появляется позже
    # SettleSeconds, то сворачивается системой. Оба случая давали «красный
    # код из-за среды» вместо честного результата — то есть ворота сами
    # становились недетерминированными.
    [int]$WindowFindTries = 8,
    [int]$WindowRestoreTries = 5,
    # --- состояние интерфейса: сброс по умолчанию (см. шапку выше) -------------
    [switch]$KeepUiState,
    [string]$ExpectPage = '',
    # Страница для замера, выбранная СОСТОЯНИЕМ, а не вводом: см. шапку,
    # «СТРАНИЦА БЕЗ ФОКУСА». Пусто — страница не выбирается, запуск идёт как
    # обычно (с --fresh-ui-state, то есть на «Обзоре»).
    [string]$Page = '',
    [int]$DeterminismRuns = 1,
    [int]$KeepFontScale = 100,
    # --- обход всех страниц (см. шапку) ---------------------------------------
    [switch]$AllPages,
    [int]$PageSwitchTries = 8,
    [int]$LayoutTolerance = 1,
    [int]$PageSettleMilliseconds = 600
)

$ErrorActionPreference = 'Stop'

# --- проверка аргументов: код 2, а не «сработало на значениях по умолчанию» ---
if (-not (Test-Path -LiteralPath $Exe)) { Write-Host "[ui] нет бинарника: $Exe"; exit 6 }
if ($MinInkPixels -lt 0) { Write-Host "[ui] MinInkPixels отрицателен: $MinInkPixels"; exit 2 }
if ($MinContentInkPixels -lt 0) { Write-Host "[ui] MinContentInkPixels отрицателен: $MinContentInkPixels"; exit 2 }
if ($MinContentColors -lt 1) { Write-Host "[ui] MinContentColors меньше 1: $MinContentColors"; exit 2 }
if ($MinCenterInkPixels -lt 0) { Write-Host "[ui] MinCenterInkPixels отрицателен: $MinCenterInkPixels"; exit 2 }
if ($MinListTextPixels -lt 0) { Write-Host "[ui] MinListTextPixels отрицателен: $MinListTextPixels"; exit 2 }
if ($MinListTextSpanPercent -lt 0 -or $MinListTextSpanPercent -gt 100) {
    Write-Host "[ui] MinListTextSpanPercent вне 0..100: $MinListTextSpanPercent"
    exit 2
}
if ($MinColumnWidthPx -lt 0) { Write-Host "[ui] MinColumnWidthPx отрицателен: $MinColumnWidthPx"; exit 2 }
if ($MinMapLabelHeightPx -lt 0) { Write-Host "[ui] MinMapLabelHeightPx отрицателен: $MinMapLabelHeightPx"; exit 2 }
if (($Width -gt 0) -xor ($Height -gt 0)) {
    Write-Host '[ui] -Width и -Height задаются вместе (задано только одно)'
    exit 2
}
if (($Width -le 0) -and ($Height -le 0) -and ($DpiPercent -gt 0)) {
    Write-Host '[ui] -DpiPercent без -Width/-Height: масштабировать нечего'
    exit 2
}
if ($DpiPercent -ne 0 -and ($DpiPercent -lt 50 -or $DpiPercent -gt 500)) {
    Write-Host "[ui] -DpiPercent вне 50..500: $DpiPercent"
    exit 2
}
if ($ThemeMode -ne '' -and @('auto', 'light', 'dark') -notcontains $ThemeMode) {
    Write-Host "[ui] -ThemeMode ожидает auto|light|dark, получено '$ThemeMode'"
    exit 2
}
if ($ExpectBackground -ne '' -and @('dark', 'light') -notcontains $ExpectBackground) {
    Write-Host "[ui] -ExpectBackground ожидает dark|light, получено '$ExpectBackground'"
    exit 2
}
if ($PageSwitchTries -lt 1) { Write-Host "[ui] -PageSwitchTries меньше 1: $PageSwitchTries"; exit 2 }
if ($WindowFindTries -lt 1) { Write-Host "[ui] -WindowFindTries меньше 1: $WindowFindTries"; exit 2 }
if ($WindowRestoreTries -lt 1) { Write-Host "[ui] -WindowRestoreTries меньше 1: $WindowRestoreTries"; exit 2 }
if ($LayoutTolerance -lt 0) { Write-Host "[ui] -LayoutTolerance отрицателен: $LayoutTolerance"; exit 2 }
if ($DeterminismRuns -lt 1 -or $DeterminismRuns -gt 10) {
    Write-Host "[ui] -DeterminismRuns вне 1..10: $DeterminismRuns"
    exit 2
}

Add-Type -TypeDefinition @'
using System; using System.Text; using System.Runtime.InteropServices;
public class MrWin {
 [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
 [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr p);
 public delegate bool EnumProc(IntPtr h, IntPtr p);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
 // CharSet.Unicode обязателен: без него среда объявляет вызов как ANSI,
 // маршализер отдаёт буфер как байтовый, а GetWindowTextW пишет туда
 // UTF-16 — заголовок «MrProper» читался как «M» (обрезание по первому
 // нулю), и строка ворота печатала не то название окна.
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
 [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
 [DllImport("user32.dll", SetLastError=true)] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int ht, bool repaint);
 [DllImport("user32.dll")] public static extern bool UpdateWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern bool RedrawWindow(IntPtr h, IntPtr rect, IntPtr rgn, uint flags);
 [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
 [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
 // Стиль окна и метрика полосы прокрутки: нужны, чтобы область строк списка не
 // включала горизонтальную полосу прокрутки (WS_HSCROLL есть у «Настроек»).
 // Обе функции — обычные вызовы без сообщений, поэтому чужой процесс они не
 // трогают и указателей из чужой памяти не требуют.
 [DllImport("user32.dll", EntryPoint="GetWindowLongPtrW")] public static extern IntPtr GetWindowLongPtr(IntPtr h, int i);
 [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
 // Обход страниц: передний план, клавиатура и обход дерева потомков.
 [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
 [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
 [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageTimeout(IntPtr h, uint msg, IntPtr wp, IntPtr lp, uint flags, uint timeout, out IntPtr result);
 // SendMessage без таймаута — только для СВОИХ окон и только синхронных
 // сообщений списка (LVM_GETCOLUMNWIDTH, LVM_GETHEADER): висящее окно в воротах
 // не зависает, потому что приложение нашёлось и отвечает на WM_GETMINMAXINFO.
 [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
 [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
 [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
 [StructLayout(LayoutKind.Sequential)] public struct MINMAXINFO {
   public POINT ptReserved; public POINT ptMaxSize; public POINT ptMaxPosition;
   public POINT ptMinTrackSize; public POINT ptMaxTrackSize;
 }
}
'@
Add-Type -AssemblyName System.Drawing

# Константы Win32, которые иначе пришлось бы считать в голову:
#   WM_DPICHANGED_AFTERPARENT = 0x02E3 (winuser.h), WM_GETMINMAXINFO = 0x0024,
#   SMTO_ABORTIFHUNG = 0x0002, RDW_INVALIDATE|RDW_UPDATENOW|RDW_ALLCHILDREN = 0x0181.
$script:wmDpiAfterParent = 0x02E3
$script:wmGetMinMaxInfo = 0x0024
$script:smtoAbortIfHung = 0x0002
$script:redrawAll = 0x0181
$script:baseDpi = 96
#   GW_CHILD = 5, GW_HWNDNEXT = 2 (winuser.h) — обход прямых детей без
#   перечисления всех потомков: EnumChildWindows рекурсивный и смотрел бы в
#   ветви скрытых экранов.
$script:gwChild = 5
$script:gwHwndNext = 2
$script:keyEventKeyUp = 0x0002
$script:vkControl = 0x11
$script:vkMenu = 0x12
# Сообщения списка и дерева (commctrl.h Windows SDK 10.0.19041).
#
# В commctrl.h LVM_GETCOLUMNCOUNT и LVM_GETITEMCOUNT — ОДНО И ТО ЖЕ число
# 0x1004 (LVM_FIRST+4), то есть имя есть, а числа за ним нет: comctl32 отдаёт
# в нём ЧИСЛО СТРОК. Прежняя проверка брала это значение как число столбцов и
# складывала LVM_GETCOLUMNWIDTH (0x101D) по такому индексу; для индекса за
# последним столбцом comctl32 отдаёт ширину последнего, поэтому сумма всегда
# выходила больше полосы заголовка (428 + 9*96 = 1292 при полосе 648) и ворота
# краснели при ЛЮБОЙ вёрстке, требуя, чтобы имя узла занимало 1 px.
# Через чужой процесс число столбцов не берётся: HDM_GETITEMCOUNT = 0x0000 =
# WM_NULL отдаёт 0, а HDM_GETITEMW и LVM_GETCOLUMNW с указателем из чужого
# процесса приложение убивают (проверено на первой версии пробы — процесс падал).
# Поэтому и число столбцов, и их ширины берутся из пикселей снимка, а это
# сообщение осталось только для числа СТРОК: указателей оно не принимает, то
# есть чужую память не трогает.
$script:lvmGetItemCount = 0x1004
# TVM_GETCOUNT = TV_FIRST+5 = 0x1105 (commctrl.h) — число строк дерева.
$script:tvmGetCount = 0x1105
# GWL_STYLE = -16 (winuser.h), WS_HSCROLL = 0x00100000, WS_VSCROLL = 0x00200000,
# SM_CYHSCROLL = 7, SM_CXVSCROLL = 8 (winuser.h).
$script:gwlStyle = -16
$script:wsHScroll = 0x00100000
$script:wsVScroll = 0x00200000
$script:smCyHScroll = 7
$script:smCxVScroll = 8
# Классы контролов, у которых есть строки.
$script:classListView = 'SysListView32'
$script:classTreeView = 'SysTreeView32'
$script:classHeader = 'SysHeader32'
# Карта разделов рисуется Direct2D в СВОЁМ дочернем окне
# (detail::kDisksMapClass — «MrProper.DisksMap»,
# src\ui\view_disks.cpp), прямом потомке окна экрана
# «Диски» (detail::kDisksViewClass). Ворота находят её
# по классу: ни одно сообщение перечисления не знает
# про содержимое окна собственной отрисовки.
$script:classDisksView = 'MrProper.DisksView'
$script:classDisksMap = 'MrProper.DisksMap'
# Пороги пиксельной меры полосы заголовка (см. Measure-HeaderColumns).
$script:minHeaderStripPx = 24
$script:minSeparatorShare = 0.90
$script:maxSeparatorWidthPx = 12
# Пять страниц рельса в порядке kPages (src/ui/nav.hpp): Overview, Disks,
# Cleanup, Report, Settings; Ctrl+1..5 и класс окна экрана.
$script:pages = @(
    @{ Name = 'overview'; Digit = 0x31; View = 'MrProper.OverviewView' },
    @{ Name = 'disks';    Digit = 0x32; View = 'MrProper.DisksView' },
    @{ Name = 'cleanup';  Digit = 0x33; View = 'MrProper.CleanupView' },
    @{ Name = 'report';   Digit = 0x34; View = 'MrProper.ReportView' },
    @{ Name = 'settings'; Digit = 0x35; View = 'MrProper.SettingsView' }
)


if ($ExpectPage -ne '' -and @($script:pages | Where-Object { $_.Name -eq $ExpectPage }).Count -eq 0) {
    # Имя страницы проверяется по тому же списку, что и обход: страница без
    # цифры в рельсе обходиться не будет, и опечатка в -ExpectPage молча
    # сделала бы проверку зелёной.
    Write-Host "[ui] -ExpectPage ожищает overview|disks|cleanup|report|settings, получено '$ExpectPage'"
    exit 2
}

if ($Page -ne '') {
    # -Page пишется в nav.current буквально, поэтому опечатка здесь означала бы
    # «страница не найдена» в журнале приложения, а тихий возврат к «Обзору» —
    # то есть замер чужой страницы. Имя проверяется по тому же списку рельса.
    if (@($script:pages | Where-Object { $_.Name -eq $Page }).Count -eq 0) {
        Write-Host "[ui] -Page ожидает overview|disks|cleanup|report|settings, получено '$Page'"
        exit 2
    }
    if ($ExpectPage -ne '' -and $ExpectPage -ne $Page) {
        # Два ключа о разном противоречат друг другу: -Page выбирает страницу,
        # -ExpectPage проверяет, какая открылась. Молча брать одну из них —
        # это замер не того, что человек попросил.
        Write-Host "[ui] -Page '$Page' и -ExpectPage '$ExpectPage' называют разные страницы"
        exit 2
    }
    # Страница выбрана — проверять надо именно её, иначе ворота напечатают
    # «стартовая страница: overview» и промолчат о невидимых строках.
    $ExpectPage = $Page
}

# --- состояние темы в реестре: запомнить, поставить, вернуть как было ---------
$script:themePath = 'HKCU:\Software\MrProper\UI'
$script:themeName = 'ThemeMode'
$script:themeSavedExisted = $false
$script:themeSavedValue = ''
$script:themeKeyExisted = $false

function Save-ThemeState {
    $script:themeKeyExisted = Test-Path -LiteralPath $script:themePath
    try {
        $key = Get-Item -LiteralPath $script:themePath -ErrorAction Stop
        $value = $key.GetValue($script:themeName, $null)
        if ($null -ne $value) {
            $script:themeSavedExisted = $true
            $script:themeSavedValue = [string]$value
        }
    } catch {
        # Ключа нет — это не ошибка: состояние «тема не задана» тоже состояние.
    }
}

function Set-AppThemeMode([string]$mode) {
    if (-not (Test-Path -LiteralPath $script:themePath)) {
        New-Item -Path $script:themePath -Force | Out-Null
    }
    New-ItemProperty -Path $script:themePath -Name $script:themeName -Value $mode `
        -PropertyType String -Force | Out-Null
}

function Restore-AppThemeMode {
    if ($ThemeMode -eq '') { return 0 }
    try {
        if ($script:themeSavedExisted) {
            New-ItemProperty -Path $script:themePath -Name $script:themeName -Value $script:themeSavedValue `
                -PropertyType String -Force | Out-Null
        } elseif (Test-Path -LiteralPath $script:themePath) {
            Remove-ItemProperty -LiteralPath $script:themePath -Name $script:themeName `
                -ErrorAction SilentlyContinue
        }
        # Ключа не было — не оставляем после себя пустой ветки.
        if (-not $script:themeKeyExisted -and (Test-Path -LiteralPath $script:themePath)) {
            $key = Get-Item -LiteralPath $script:themePath
            if ($key.GetValueNames().Length -eq 0 -and $key.GetSubKeyNames().Length -eq 0) {
                Remove-Item -LiteralPath $script:themePath -Force
            }
        }
    } catch {
        Write-Host "[ui] ВНИМАНИЕ: прежний ThemeMode не восстановлен ($($_.Exception.Message))"
        return 7
    }
    return 0
}

# --- состояние страницы в реестре: nav.current (см. шапку, «СТРАНИЦА БЕЗ ФОКУСА») ---
#
# Почему скрипт, а не ключ запуска приложения: приложение пишет и читает
# nav.current само (Navigator::importState), то есть согласованный ключ запуска
# уже есть по построению, и добавлять второй ради ворот означало бы менять
# приложение под инструмент. Флаг --fresh-ui-state при этом не подходит: он
# именно СБРАСЫВАЕТ состояние, то есть выбранную страницу вместе с ним.
$script:navCurrentName = 'nav.current'
$script:navCurrentSaved = $false
$script:navCurrentSavedValue = ''

# Запомнить прежнее значение nav.current, включая состояние «значения не было».
function Save-NavCurrentState {
    $script:navCurrentSaved = $false
    $script:navCurrentSavedValue = ''
    try {
        if (-not (Test-Path -LiteralPath $script:themePath)) { return }
        $key = Get-Item -LiteralPath $script:themePath
        $value = $key.GetValue($script:navCurrentName, $null)
        if ($null -ne $value) {
            $script:navCurrentSaved = $true
            $script:navCurrentSavedValue = [string]$value
        }
    } catch {
        # Ключа нет — это не ошибка: «страница не задана» тоже состояние.
    }
}

# Поставить страницу на время прогона. Значение — ровно pageKey() из
# src/ui/nav.hpp: «overview», «disks», «cleanup», «report», «settings».
function Set-AppNavCurrent([string]$page) {
    if (-not (Test-Path -LiteralPath $script:themePath)) {
        New-Item -Path $script:themePath -Force | Out-Null
    }
    New-ItemProperty -Path $script:themePath -Name $script:navCurrentName -Value $page `
        -PropertyType String -Force | Out-Null
}

# Вернуть прежнее значение. Вызывается ПОСЛЕ закрытия приложения: само
# приложение пишет nav.current при выходе, и возврат раньше закрытия был бы
# зачёркнут следующей записью.
function Restore-AppNavCurrent {
    if ($Page -eq '') { return 0 }
    try {
        if ($script:navCurrentSaved) {
            New-ItemProperty -Path $script:themePath -Name $script:navCurrentName `
                -Value $script:navCurrentSavedValue -PropertyType String -Force | Out-Null
        } elseif (Test-Path -LiteralPath $script:themePath) {
            Remove-ItemProperty -LiteralPath $script:themePath -Name $script:navCurrentName `
                -ErrorAction SilentlyContinue
        }
        # Возврат ПРОВЕРЯЕТСЯ чтением, а не считается выполненным по факту записи.
        # На машине, где параллельно работают другие агенты, чужой запуск того же
        # приложения дописывает nav.current при своём закрытии — то есть настройка
        # общая, и «ворота вернули своё» может оказаться неправдой через секунду.
        # Тихая неудача здесь хуже явной: она выглядела бы как «состояние цело».
        $current = $null
        try { $current = (Get-Item -LiteralPath $script:themePath).GetValue($script:navCurrentName, $null) } catch { }
        $expected = if ($script:navCurrentSaved) { $script:navCurrentSavedValue } else { $null }
        $same = if ($null -eq $expected) { $null -eq $current } else { [string]$current -eq [string]$expected }
        if ($same) {
            Write-Host ("[ui] nav.current возвращён: {0}" -f `
                $(if ($script:navCurrentSaved) { "'$($script:navCurrentSavedValue)'" } else { 'значения не было' }))
        } else {
            Write-Host ("[ui] ВНИМАНИЕ: nav.current после возврата = '{0}', а ожидалось '{1}' — состояние переписал чужой запуск приложения" -f `
                $(if ($null -eq $current) { '<нет>' } else { $current }), `
                $(if ($script:navCurrentSaved) { $script:navCurrentSavedValue } else { '<нет>' }))
            return 7
        }
    } catch {
        Write-Host "[ui] ВНИМАНИЕ: прежний nav.current не восстановлен ($($_.Exception.Message))"
        return 7
    }
    return 0
}

# --- состояние, готовимое скриптом: FontScalePercent ------------------------
# Масштаб шрифта лежит в той же ветке, но читает его theme.cpp напрямую, а не
# хранилище состояния, поэтому --fresh-ui-state его не касается. Пока значение
# есть на машине человека, ворота меряют другой кегль — и числа двух запусков
# расходятся. Поэтому на время прогона ставится -KeepFontScale (по умолчанию
# 100, то есть значение по умолчанию приложения), а прежнее возвращается.
$script:fontScaleName = 'FontScalePercent'
$script:fontScaleSaved = $false
$script:fontScaleSavedValue = 100

function Save-FontScaleState {
    $script:fontScaleSaved = $false
    try {
        if (-not (Test-Path -LiteralPath $script:themePath)) { return }
        $key = Get-Item -LiteralPath $script:themePath
        $value = $key.GetValue($script:fontScaleName, $null)
        if ($null -ne $value) {
            $script:fontScaleSaved = $true
            $script:fontScaleSavedValue = [int]$value
        }
    } catch {
        # Ключа нет — это не ошибка: «шрифт по умолчанию» тоже состояние.
    }
}

function Set-AppFontScale([int]$percent) {
    if (-not (Test-Path -LiteralPath $script:themePath)) {
        New-Item -Path $script:themePath -Force | Out-Null
    }
    New-ItemProperty -Path $script:themePath -Name $script:fontScaleName -Value $percent `
        -PropertyType DWord -Force | Out-Null
}

function Restore-AppFontScale {
    if ($KeepUiState) { return 0 }
    try {
        if ($script:fontScaleSaved) {
            New-ItemProperty -Path $script:themePath -Name $script:fontScaleName `
                -Value $script:fontScaleSavedValue -PropertyType DWord -Force | Out-Null
        } elseif (Test-Path -LiteralPath $script:themePath) {
            Remove-ItemProperty -LiteralPath $script:themePath -Name $script:fontScaleName `
                -ErrorAction SilentlyContinue
        }
        # Ветку, которой не было, не оставляем после себя пустой.
        if (-not $script:themeKeyExisted -and (Test-Path -LiteralPath $script:themePath)) {
            $key = Get-Item -LiteralPath $script:themePath
            if ($key.GetValueNames().Length -eq 0 -and $key.GetSubKeyNames().Length -eq 0) {
                Remove-Item -LiteralPath $script:themePath -Force
            }
        }
    } catch {
        Write-Host "[ui] ВНИМАНИЕ: прежний FontScalePercent не восстановлен ($($_.Exception.Message))"
        return 7
    }
    return 0
}

# Относительная яркость по WCAG: решением «тёмный фон или светлый» занимается
# не цветовой канал, а формула, иначе тёмно-синий прошёл бы за серый.
function Get-RelativeLuminance([int]$r, [int]$g, [int]$b) {
    $channel = {
        param($value)
        $c = $value / 255.0
        if ($c -le 0.03928) { return $c / 12.92 }
        return [Math]::Pow(($c + 0.055) / 1.055, 2.4)
    }
    return 0.2126 * (& $channel $r) + 0.7152 * (& $channel $g) + 0.0722 * (& $channel $b)
}

function Find-AppWindow([int]$processId) {
    $script:hwnd = [IntPtr]::Zero
    $script:hwndClass = ''
    $cb = [MrWin+EnumProc] {
        param($h, $l)
        $q = 0
        [void][MrWin]::GetWindowThreadProcessId($h, [ref]$q)
        if ($q -eq $processId) {
            $t = New-Object Text.StringBuilder 512
            [void][MrWin]::GetWindowTextW($h, $t, 512)
            $r = New-Object MrWin+RECT
            [void][MrWin]::GetWindowRect($h, [ref]$r)
            if ($script:hwnd -eq [IntPtr]::Zero -and [MrWin]::IsWindowVisible($h) -and
                ($r.R - $r.L) -gt 200 -and ($r.B - $r.T) -gt 200 -and $t.Length -gt 0) {
                $script:hwnd = $h
                $c = New-Object Text.StringBuilder 512
                [void][MrWin]::GetClassNameW($h, $c, 512)
                $script:hwndClass = $c.ToString()
                Write-Host ("[ui] окно '{0}' класс '{1}' {2}x{3}" -f $t.ToString(), $script:hwndClass, ($r.R - $r.L), ($r.B - $r.T))
            }
        }
        return $true
    }
    [void][MrWin]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:hwnd
}

# Что у процесса есть на самом деле. Сообщение «окно не найдено» само по себе
# бесполезно: из него не видно, было ли окно свёрнутым (тогда GetWindowRect
# отдаёт 160x24 в (-32000,-32000)), пустым по размеру или без заголовка, — а
# это три разные причины и три разные починки.
$script:windowRows = $null

function Describe-AppWindows([int]$processId) {
    $script:windowRows = New-Object System.Collections.ArrayList
    $cb = [MrWin+EnumProc] {
        param($h, $l)
        $q = 0
        [void][MrWin]::GetWindowThreadProcessId($h, [ref]$q)
        if ($q -eq $processId) {
            $r = New-Object MrWin+RECT
            [void][MrWin]::GetWindowRect($h, [ref]$r)
            $c = New-Object Text.StringBuilder 256
            [void][MrWin]::GetClassNameW($h, $c, 256)
            $t = New-Object Text.StringBuilder 256
            [void][MrWin]::GetWindowTextW($h, $t, 256)
            [void]$script:windowRows.Add(("{0} класс='{1}' заголовок='{2}' видимо={3} свёрнуто={4} размер={5}x{6}" -f `
                $h, $c.ToString(), $t.ToString(), [MrWin]::IsWindowVisible($h), [MrWin]::IsIconic($h),
                ($r.R - $r.L), ($r.B - $r.T)))
        }
        return $true
    }
    [void][MrWin]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:windowRows
}

# Снимок окна в файл. Отдельная функция нужна обходу страниц: он снимает ту же
# иерархию пять раз и обязан писать в разные файлы, не трогая код стартовой
# страницы. Вместе с путём возвращает и пиксели того же снимка: меры столбцов и
# строк обязаны мерить ровно тот кадр, который лежит в артефакте, — перечитывать
# файл значило бы мерить второй раз, который может отличаться от сохранённого.
function Save-WindowShot([IntPtr]$hwnd, $windowRect, [string]$path) {
    $shotDir = Split-Path -Parent $path
    if ($shotDir -ne '' -and -not (Test-Path -LiteralPath $shotDir)) {
        New-Item -ItemType Directory -Path $shotDir -Force | Out-Null
    }
    $shotWidth = $windowRect.R - $windowRect.L
    $shotHeight = $windowRect.B - $windowRect.T
    $bmp = New-Object System.Drawing.Bitmap($shotWidth, $shotHeight)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $dc = $g.GetHdc()
    [void][MrWin]::PrintWindow($hwnd, $dc, 2)
    $g.ReleaseHdc($dc)
    $g.Dispose()
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $data = $bmp.LockBits((New-Object System.Drawing.Rectangle(0, 0, $shotWidth, $shotHeight)),
                          [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                          [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $stride = $data.Stride
    $bytes = New-Object byte[] ($stride * $shotHeight)
    [Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
    $bmp.UnlockBits($data)
    $bmp.Dispose()
    return @{ Path = $path; Bytes = $bytes; Stride = $stride; Width = $shotWidth; Height = $shotHeight }
}

# Окно содержимого — самое крупное видимое дочернее окно главного. Рельс
# навигации рисует оболочка на самом окне, дочерних окон у него одно: хост
# содержимого, в который views_* и рисуют. Возвращает @{ Found; X; Y; W; H } в
# координатах СНИМКА окна (то есть уже с вычетом неклиентской рамки).
function Find-ContentHost([IntPtr]$hwnd, $windowRect) {
    $script:bestArea = 0
    $script:best = $null
    $cb = [MrWin+EnumProc] {
        param($h, $l)
        if ([MrWin]::IsWindowVisible($h)) {
            $r = New-Object MrWin+RECT
            [void][MrWin]::GetWindowRect($h, [ref]$r)
            $area = ($r.R - $r.L) * ($r.B - $r.T)
            if ($area -gt $script:bestArea) {
                $script:bestArea = $area
                $script:best = New-Object MrWin+RECT
                $script:best.L = $r.L
                $script:best.T = $r.T
                $script:best.R = $r.R
                $script:best.B = $r.B
                $script:bestHwnd = $h
            }
        }
        return $true
    }
    [void][MrWin]::EnumChildWindows($hwnd, $cb, [IntPtr]::Zero)
    if ($null -eq $script:best) {
        return @{ Found = $false; X = 0; Y = 0; W = 0; H = 0; Hwnd = [IntPtr]::Zero }
    }
    $x = $script:best.L - $windowRect.L
    $y = $script:best.T - $windowRect.T
    return @{ Found = $true; X = $x; Y = $y; W = $script:best.R - $script:best.L; H = $script:best.B - $script:best.T; Hwnd = $script:bestHwnd }
}

# Считает чернила в прямоугольнике: пиксели, отличные от самого частого цвета
# внутри области. Шаг 2 по обеим осям — четверть выборки (на 1920x1080 это
# 494 тысячи точек, полный обход в PowerShell 5.1 занял бы минуты, а решение
# «нарисован интерфейс или нет» от числа точек не зависит).
function Measure-Ink($bytes, $stride, $x0, $y0, $x1, $y1) {
    $counts = @{}
    $samples = 0
    for ($y = $y0; $y -lt $y1; $y += 2) {
        $row = $y * $stride
        for ($x = $x0; $x -lt $x1; $x += 2) {
            $i = $row + $x * 4
            $key = "$($bytes[$i]),$($bytes[$i + 1]),$($bytes[$i + 2])"
            if ($counts.ContainsKey($key)) { $counts[$key]++ } else { $counts[$key] = 1 }
            $samples++
        }
    }
    if ($samples -eq 0) { return @{ Background = ''; Ink = 0; Samples = 0; Ratio = 0.0; Colors = 0 } }
    $background = ($counts.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 1).Name
    $ink = 0
    foreach ($kv in $counts.GetEnumerator()) {
        if ($kv.Name -ne $background) { $ink += $kv.Value }
    }
    return @{ Background = $background; Ink = $ink; Samples = $samples;
              Ratio = [Math]::Round(100.0 * $ink / $samples, 2); Colors = $counts.Count }
}

# Минимум окна, объявленный самим приложением (AppShell::handleGetMinMaxInfo
# отдаёт ptMinTrackSize = логический минимум, умноженный на dpi_/96). Число
# служит измерителем внутреннего DPI: при 96 dpi это 900x600, при 144 — 1350x900.
function Get-AppMinTrack([IntPtr]$hwnd) {
    $info = New-Object MrWin+MINMAXINFO
    $size = [Runtime.InteropServices.Marshal]::SizeOf($info)
    $ptr = [Runtime.InteropServices.Marshal]::AllocHGlobal($size)
    try {
        $info.ptMaxSize.X = -1; $info.ptMaxSize.Y = -1
        $info.ptMaxPosition.X = -1; $info.ptMaxPosition.Y = -1
        $info.ptMinTrackSize.X = -1; $info.ptMinTrackSize.Y = -1
        $info.ptMaxTrackSize.X = -1; $info.ptMaxTrackSize.Y = -1
        [Runtime.InteropServices.Marshal]::StructureToPtr($info, $ptr, $false)
        $result = [IntPtr]::Zero
        $sent = [MrWin]::SendMessageTimeout($hwnd, $script:wmGetMinMaxInfo, [IntPtr]::Zero, $ptr,
                                            $script:smtoAbortIfHung, 5000, [ref]$result)
        if ($sent -eq [IntPtr]::Zero) { return $null }
        $back = [Runtime.InteropServices.Marshal]::PtrToStructure($ptr, [type][MrWin+MINMAXINFO])
        if ($back.ptMinTrackSize.X -le 0 -or $back.ptMinTrackSize.Y -le 0) { return $null }
        return @([int]$back.ptMinTrackSize.X, [int]$back.ptMinTrackSize.Y)
    } finally {
        [Runtime.InteropServices.Marshal]::FreeHGlobal($ptr)
    }
}

# Прямые дети окна: EnumChildWindows рекурсивный, а нужно только верхний
# уровень, чтобы отличить окно экрана от его собственных элементов.
function Get-DirectChild([IntPtr]$parent) {
    $result = New-Object System.Collections.ArrayList
    $child = [MrWin]::GetWindow($parent, $script:gwChild)
    while ($child -ne [IntPtr]::Zero) {
        [void]$result.Add($child)
        $child = [MrWin]::GetWindow($child, $script:gwHwndNext)
    }
    return $result
}

# Видимые окна экранов (класс MrProper.*View), прямые дети хоста содержимого.
# Возвращает массив @{ Class; Hwnd } — по нему видно, какая страница открыта.
function Get-VisibleScreen([IntPtr]$hostWindow) {
    $found = New-Object System.Collections.ArrayList
    foreach ($child in Get-DirectChild $hostWindow) {
        if (-not [MrWin]::IsWindowVisible($child)) { continue }
        $c = New-Object Text.StringBuilder 256
        [void][MrWin]::GetClassNameW($child, $c, 256)
        $name = $c.ToString()
        if ($name -like 'MrProper.*View') {
            [void]$found.Add(@{ Class = $name; Hwnd = $child })
        }
    }
    return $found
}

# Окно на передний план. SetForegroundWindow без ALT система его отклоняет:
# право есть только у процесса, который последним щёлкнул мышью. ALT-нажатие
# снимает ограничение — приём из штатной автоматизации, он же объясняет, почему
# синтетический WM_SETFOCUS не помогает: он меняет фокус внутри потока, а
# передний план определяет система.
function Set-AppForeground([IntPtr]$hwnd) {
    [void][MrWin]::SetForegroundWindow($hwnd)
    if ([MrWin]::GetForegroundWindow() -eq $hwnd) { return $true }
    [MrWin]::keybd_event($script:vkMenu, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 80
    [void][MrWin]::SetForegroundWindow($hwnd)
    [MrWin]::keybd_event($script:vkMenu, 0, $script:keyEventKeyUp, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 250
    return ([MrWin]::GetForegroundWindow() -eq $hwnd)
}

# Кто держит передний план. Измерено: на машине, где параллельно работают чужие
# окна, обход давал код 11 «страница не открылась», и по этому коду нельзя было
# понять, приложение это или среда. Имя и заголовок чужого окна превращают
# отказ среды в конкретное указание.
function Get-ForegroundOwner {
    $fg = [MrWin]::GetForegroundWindow()
    if ($fg -eq [IntPtr]::Zero) { return 'переднего окна нет' }
    $pid2 = 0
    [void][MrWin]::GetWindowThreadProcessId($fg, [ref]$pid2)
    $c = New-Object Text.StringBuilder 256
    [void][MrWin]::GetClassNameW($fg, $c, 256)
    $t = New-Object Text.StringBuilder 256
    [void][MrWin]::GetWindowTextW($fg, $t, 256)
    $who = ''
    if ($pid2 -ne 0) {
        try {
            $p = Get-Process -Id $pid2 -ErrorAction Stop
            $who = $p.ProcessName + ' '
        } catch {
            $who = ''
        }
    }
    return ('{0}pid {1} класс {2} заголовок «{3}»' -f $who, $pid2, $c.ToString(), $t.ToString())
}

# Ctrl+<цифра> настоящим вводом. Возвращает @{ Ok; Tries; Visible; Foreign }.
function Switch-AppPage([IntPtr]$hwnd, [IntPtr]$hostWindow, [hashtable]$page) {
    $result = @{ Ok = $false; Tries = 0; Visible = ''; Foreign = '' }
    $tries = 0
    while ($tries -lt $PageSwitchTries) {
        $tries++
        $result.Tries = $tries
        if (-not (Set-AppForeground $hwnd)) { continue }
        [MrWin]::keybd_event($script:vkControl, 0, 0, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 80
        [MrWin]::keybd_event($page.Digit, 0, 0, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 80
        [MrWin]::keybd_event($page.Digit, 0, $script:keyEventKeyUp, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 80
        [MrWin]::keybd_event($script:vkControl, 0, $script:keyEventKeyUp, [IntPtr]::Zero)
        Start-Sleep -Milliseconds $PageSettleMilliseconds
        $visible = @(Get-VisibleScreen $hostWindow)
        if ($visible.Count -eq 1 -and $visible[0].Class -eq $page.View) {
            $result.Ok = $true
            $result.Visible = $visible[0].Class
            return $result
        }
        if ($visible.Count -gt 0) { $result.Visible = $visible[0].Class }
        # Почему не открылось: чужое окно успело перехватить фокус, пока
        # жали клавиши. Без этой строки код 11 читается как отказ приложения.
        if ($result.Foreign -eq '') { $result.Foreign = Get-ForegroundOwner }
        # Пауза перед следующей попыткой: на занятом рабочем столе чужое окно
        # возвращается на передний план через доли секунды, и восемь попыток
        # подряд уходили все в одно и то же чужое окно.
        Start-Sleep -Milliseconds 400
    }
    return $result
}

# Прямой потомок заданного класса (SysHeader32 у SysListView32). Именно прямой:
# EnumChildWindows рекурсивный и нашёл бы шапки списков внутри чужих вложенных
# окон. Класс читается GetClassNameW, то есть без сообщений чужому процессу.
function Find-DirectChildByClass([IntPtr]$parent, [string]$className) {
    foreach ($child in Get-DirectChild $parent) {
        $c = New-Object Text.StringBuilder 256
        [void][MrWin]::GetClassNameW($child, $c, 256)
        if ($c.ToString() -eq $className -and [MrWin]::IsWindowVisible($child)) { return $child }
    }
    return [IntPtr]::Zero
}

# Число строк списка или дерева. Единственное сообщение, которое ворота шлют
# контролу: LVM_GETITEMCOUNT (0x1004) у списка и TVM_GETCOUNT (0x1105) у дерева.
# Оба возвращают число и НЕ ПРИНИМАЮТ УКАЗАТЕЛЕЙ, поэтому чужую память не трогают
# (в отличие от HDM_GETITEMW и LVM_GETCOLUMNW, которые приложение убивают).
# lParam здесь NULL, а ответ читается из pdwResult: значение, которое сообщение
# возвращает, SendMessageTimeout кладёт именно туда, а в lParam пишут только
# сообщения с выходным буфером (как WM_GETMINMAXINFO).
# Проверено пробой на живом окне: LVM_GETITEMCOUNT у списка «Дисков» = 10 —
# это строки, а не столбцы (столбцов три, см. kColumnCount в src\ui), и именно
# поэтому сумма ширин в прежней проверке выходила 428 + 9*96.
# -1 означает «не ответил», и тогда проверка строк просто не применяется.
function Get-ListItemCount([IntPtr]$hwnd, [string]$class) {
    $message = if ($class -eq $script:classTreeView) { $script:tvmGetCount } else { $script:lvmGetItemCount }
    $result = [IntPtr]::Zero
    $sent = [MrWin]::SendMessageTimeout($hwnd, $message, [IntPtr]::Zero, [IntPtr]::Zero,
                                        $script:smtoAbortIfHung, 5000, [ref]$result)
    if ($sent -eq [IntPtr]::Zero) { return -1 }
    return [int]$result.ToInt64()
}

# Самый частый цвет прямоугольника. Им ищется фон страницы: пиксель строки
# считается текстом только когда он отличается и от фона строки, и от фона
# страницы, иначе рамка окна содержимого, протекающая в область строк, сошла бы
# за текст.
function Measure-DominantColor($bytes, $stride, $x0, $y0, $x1, $y1, [int]$step) {
    $counts = @{}
    for ($y = $y0; $y -le $y1; $y += $step) {
        $row = $y * $stride
        for ($x = $x0; $x -le $x1; $x += $step) {
            $i = $row + $x * 4
            $key = "$($bytes[$i]),$($bytes[$i + 1]),$($bytes[$i + 2])"
            if ($counts.ContainsKey($key)) { $counts[$key]++ } else { $counts[$key] = 1 }
        }
    }
    if ($counts.Count -eq 0) { return '' }
    return ($counts.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 1).Name
}

# ГРАНИЦЫ СТОЛБЦОВ ПО ПИКСЕЛЯМ (вместо LVM_GETCOLUMNCOUNT, S1).
#
# Разделитель между столбцами рисует сам comctl32: это вертикальная полоса
# шириной 2..4 px, у которой ВСЯ высота полосы заголовка отличается от фона
# полосы. Подпись столбца такой полосой быть не может — буквы занимают часть
# высоты, а не всю. Измерено на снимках из D:\Temp (шаг 1 по x, вся высота
# полосы): доля ненфоновых пикселей 1,000 у каждого разделителя и не выше
# 0,706 («Диски», подпись «Диски») и 0,667 («Настройки», три подписи) у
# колонок с текстом. Порог 0,90 разведён от обоих более чем в 1,2 раза.
#
# Ширина полосы берётся как прогон столбцов, у которых фон полосы держится хотя
# бы на половине высоты: рамка окна шапки фоном не является и в прогон не
# попадает (проверено: рамка слева 2 px цвета 255,255,255 и справа 14 px цвета
# 23,23,23 обрезали бы первый и последний столбцы).
#
# Отвергнут метод «изменение цвета колонки на всю её высоту». В области строк
# comctl32 границу столбцов не рисует, а NM_CUSTOMDRAW красит подпункты на
# плоской заливке, поэтому изменения цвета там дают не границы столбцов, а
# глифы подписей: на q3-allpages3-p2-disks.png цвет в области строк меняется в
# 7 столбцах x из 440, и число «столбцов» вышло бы 7 вместо трёх.
#
# Возвращает $null, когда полосы в снимке нет (список без заголовка).
function Measure-HeaderColumns($bytes, $stride, $x0, $y0, $x1, $y1) {
    $rows = $y1 - $y0 + 1
    $cols = $x1 - $x0 + 1
    if ($rows -lt 4 -or $cols -lt $script:minHeaderStripPx) { return $null }
    $counts = @{}
    for ($y = $y0; $y -le $y1; $y++) {
        $row = $y * $stride
        for ($x = $x0; $x -le $x1; $x++) {
            $i = $row + $x * 4
            $key = "$($bytes[$i]),$($bytes[$i + 1]),$($bytes[$i + 2])"
            if ($counts.ContainsKey($key)) { $counts[$key]++ } else { $counts[$key] = 1 }
        }
    }
    $background = ($counts.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 1).Name
    $profile = New-Object int[] $cols
    $left = -1
    $right = -1
    for ($i = 0; $i -lt $cols; $i++) {
        $x = $x0 + $i
        $same = 0
        for ($y = $y0; $y -le $y1; $y++) {
            $k = $y * $stride + $x * 4
            if ("$($bytes[$k]),$($bytes[$k + 1]),$($bytes[$k + 2])" -eq $background) { $same++ }
        }
        $profile[$i] = $rows - $same
        if (($same / [double]$rows) -ge 0.5) {
            if ($left -lt 0) { $left = $i }
            $right = $i
        }
    }
    if ($left -lt 0) { return $null }
    $bands = New-Object System.Collections.ArrayList
    $start = -1
    for ($i = $left; $i -le $right; $i++) {
        $isSeparator = ($profile[$i] -ge ($rows * $script:minSeparatorShare))
        if ($isSeparator -and $start -lt 0) { $start = $i }
        if (-not $isSeparator -and $start -ge 0) {
            # Полоса шире 12 px — не разделитель столбцов, а рамка или заливка
            # темы: такие пропускаются, иначе один такой прогон склеил бы все
            # столбцы в один. На проверенных снимках разделители 2..4 px.
            if (($i - $start) -le $script:maxSeparatorWidthPx) {
                [void]$bands.Add(@{ From = $x0 + $start; To = $x0 + $i - 1 })
            }
            $start = -1
        }
    }
    $widths = New-Object System.Collections.ArrayList
    $stripLeft = $x0 + $left
    $stripRight = $x0 + $right
    $cursor = $stripLeft
    $skipped = 0
    foreach ($band in $bands) {
        $width = $band.From - $cursor
        if ($width -ge $MinColumnWidthPx) {
            [void]$widths.Add($width)
            $cursor = $band.To + 1
        } else {
            # Разделитель, от которого до предыдущей границы меньше
            # -MinColumnWidthPx, границей столбцов НЕ считается, и его пиксели
            # уходят в хвост. Так отсекается не разделитель, а край полосы
            # прокрутки: в светлой теме он рисуется белой линией в 1 px на
            # расстоянии 6 px от последнего разделителя, и без этого правила
            # ворота объявляли «Настройкам» четвёртый столбец шириной 6 px.
            $skipped++
        }
    }
    $tail = $stripRight - $cursor + 1
    $sum = if ($bands.Count -eq 0) { 0 } else { $cursor - $stripLeft }
    return @{ Background = $background; Bands = $bands; Widths = $widths; Sum = $sum
              Tail = $tail; Skipped = $skipped; Left = $stripLeft; Right = $stripRight
              Strip = $stripRight - $stripLeft + 1 }
}

# ОБЛАСТЬ СТРОК ПО ПИКСЕЛЯМ.
#
# Возвращает самый большой прямоугольник внутри $area, залитый ЦЕЛИКОМ цветом,
# отличным от фона страницы, и начинающийся не выше $area.T + 1: такими блоками
# приложение рисует строки списка и дерева (см. Measure-ListItem — почему не
# берётся клиентская область окна). Прямоугольник ищется построчно сверху вниз:
# для каждой верхней строки нижележащие строки добавляются, пока они целиком
# лежат в одном прогоне цвета, а площадь не перестала расти, — это даёт самый
# большой однородный блок без перебора всех пар границ.
# Поле Found = $false означает, что такого блока нет: список пуст и строки не
# нарисованы вовсе.
function Measure-RowArea($bytes, $stride, $area, [string]$pageBackground) {
    $none = @{ Found = $false; Left = $area.L; Top = $area.T; Right = $area.R; Bottom = $area.B }
    $height = $area.B - $area.T
    $width = $area.R - $area.L
    if ($height -lt 8 -or $width -lt 8) { return $none }
    # Фон строк ищется по верхней четверти: строки начинаются сразу под шапкой,
    # а низ клиентской области (у списка «Дисков» это 395 из 584 пикселей)
    # может быть фоном страницы, и тогда фон строк искался бы не там.
    $top = $area.T
    $probeBottom = [Math]::Min($area.B, $top + [Math]::Max(16, ($height / 4)))
    $rowBackground = Measure-DominantColor $bytes $stride $area.L $top $area.R $probeBottom 2
    if ($rowBackground -eq '' -or $rowBackground -eq $pageBackground) { return $none }
    $best = 0
    $bestRect = $none
    for ($y = $top; $y -lt $area.B; $y++) {
        $runStart = -1
        $bestForRow = 0
        $bestRectForRow = $none
        for ($x = $area.L; $x -le $area.R + 1; $x++) {
            $inside = $false
            if ($x -le $area.R) {
                $i = $y * $stride + $x * 4
                $key = "$($bytes[$i]),$($bytes[$i + 1]),$($bytes[$i + 2])"
                $inside = ($key -eq $rowBackground)
            }
            if ($inside -and $runStart -lt 0) { $runStart = $x }
            if (-not $inside -and $runStart -ge 0) {
                $runEnd = $x - 1
                $heightNow = $y - $top + 1
                $areaSize = ($runEnd - $runStart + 1) * $heightNow
                if ($areaSize -gt $bestForRow) {
                    $bestForRow = $areaSize
                    $bestRectForRow = @{ Found = $true; Left = $runStart; Top = $top
                                        Right = $runEnd; Bottom = $y }
                }
                $runStart = -1
            }
        }
        if ($bestForRow -gt $best) {
            $best = $bestForRow
            $bestRect = $bestRectForRow
        }
        if (($width * ($y - $top + 1)) -le $best) { break }
    }
    if ($best -le 0) { return $none }
    return $bestRect
}

# ТЕКСТ СТРОК СПИСКА (второй отказ ворот, S1).
#
# Пиксель строки считается текстом, когда его цвет отличается и от фона строки
# (самый частый цвет области строк), и от фона страницы. Такое требование ловит
# отказ, который геометрия и общие счётчики пропускают: в тёмной теме список
# «Дисков» рисуется белым по белому — сплошной белый блок, в котором есть
# пиксели цвета, отличного от фона строки (значки строк слева), но нет ни одной
# подписи. Измерено на q3-allpages3-p2-disks.png: 94,32 % площади цвет
# 255,255,255, весь «текст» — 623 точки в 7 столбцах x, то есть полоса шириной
# 16 px из 880. У нарисованного списка «Настроек» на том же снимке 13,16 %
# точек и полоса 858 px из 858.
#
# Порог задан ПО ПОЛОСЕ, а не по числу точек: у значка строки 16 px, у одной
# короткой подписи на 900x600 — около 60 px, и число точек у них отличается
# вдвое, а ширина полосы — вчетверо. Порог в процентах от ширины области строк
# не плывёт ни от DPI, ни от размера окна.
#
# Шаг 2 по обеим осям: одна посчитанная точка равна четырём пикселям кадра, и
# все пороги приведены к пикселям кадра умножением на 4.
function Measure-RowText($bytes, $stride, $x0, $y0, $x1, $y1, [string]$pageBackground) {
    $step = 2
    $counts = @{}
    $samples = 0
    for ($y = $y0; $y -le $y1; $y += $step) {
        $row = $y * $stride
        for ($x = $x0; $x -le $x1; $x += $step) {
            $i = $row + $x * 4
            $key = "$($bytes[$i]),$($bytes[$i + 1]),$($bytes[$i + 2])"
            if ($counts.ContainsKey($key)) { $counts[$key]++ } else { $counts[$key] = 1 }
            $samples++
        }
    }
    if ($samples -eq 0) { return $null }
    $rowBackground = ($counts.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 1).Name
    $text = 0
    $maxRun = 0
    $first = -1
    $last = -1
    for ($y = $y0; $y -le $y1; $y += $step) {
        $row = $y * $stride
        $run = 0
        for ($x = $x0; $x -le $x1; $x += $step) {
            $i = $row + $x * 4
            $key = "$($bytes[$i]),$($bytes[$i + 1]),$($bytes[$i + 2])"
            if ($key -ne $rowBackground -and $key -ne $pageBackground) {
                $text++
                # Полоса текста — это МИНИМУМ и МАКСИМУМ x, а не последний
                # встреченный: обход идёт построчно, и последняя строка, где есть
                # текст, — это обычно значок слева (у «Дисков» это 8 значков в
                # колонке x=240..254 под последней строкой с подписями). При
                # записи «последнего встреченного» полоса нарисованного списка
                # выходила 16 px — ровно как у невидимых строк, и проверка не
                # отличала их.
                if ($text -eq 1) { $first = $x }
                if ($x -gt $last) { $last = $x }
                $run++
                if ($run -gt $maxRun) { $maxRun = $run }
            } else {
                $run = 0
            }
        }
    }
    $span = if ($first -lt 0) { 0 } else { $last - $first + $step }
    return @{ RowBackground = $rowBackground; Text = $text; Samples = $samples; Span = $span
              MaxRun = $maxRun * $step; Width = ($x1 - $x0 + 1); Height = ($y1 - $y0 + 1)
              Ratio = [Math]::Round(100.0 * $text / $samples, 2) }
}

# Список или дерево на снимке: строка отчёта и до двух нарушений — «столбцы» и
# «строки». Обе меры в одной функции, потому что читают один и тот же
# прямоугольник окна и обе молчат, когда окно меньше 8 px.
function Measure-ListItem($shot, $windowRect, [IntPtr]$hwnd, [string]$class,
                          [string]$pageBackground, [int]$tolerance) {
    $result = @{ Log = ''; ColumnWhy = ''; RowsWhy = '' }
    $rect = New-Object MrWin+RECT
    [void][MrWin]::GetWindowRect($hwnd, [ref]$rect)
    $top = $rect.T - $windowRect.T
    $left = $rect.L - $windowRect.L
    $right = $rect.R - $windowRect.L
    if (($right - $left) -lt 8 -or ($rect.B - $top) -lt 8) { return $result }
    $rows = Get-ListItemCount $hwnd $class
    $style = [int64][MrWin]::GetWindowLongPtr($hwnd, $script:gwlStyle)

    # Область строк: по клиентской области окна минус полосы прокрутки. Левая и
    # правая границы берутся из полосы заголовка, когда он есть: полоса
    # заголовка у SysListView32 в режиме отчёта уже без вертикальной полосы
    # прокрутки, то есть это ровно та ширина, на которой список рисует столбцы
    # (то же рассуждение в src\ui\view_disks.cpp, columnRoomPx).
    $rowsLeft = $left + 1
    $rowsRight = $right - 1
    $rowsTop = $top + 1
    $strip = $null
    $header = Find-DirectChildByClass $hwnd $script:classHeader
    if ($header -ne [IntPtr]::Zero) {
        $headerRect = New-Object MrWin+RECT
        [void][MrWin]::GetWindowRect($header, [ref]$headerRect)
        $hx0 = $headerRect.L - $windowRect.L
        $hy0 = $headerRect.T - $windowRect.T
        $hx1 = $headerRect.R - $windowRect.L - 1
        $hy1 = $headerRect.B - $windowRect.T - 1
        $strip = Measure-HeaderColumns $shot.Bytes $shot.Stride $hx0 $hy0 $hx1 $hy1
        if ($null -ne $strip) {
            $rowsLeft = $strip.Left
            $rowsRight = $strip.Right
            $rowsTop = $headerRect.B - $windowRect.T
        }
    }
    $clientOrigin = New-Object MrWin+POINT
    $clientOrigin.X = 0
    $clientOrigin.Y = 0
    [void][MrWin]::ClientToScreen($hwnd, [ref]$clientOrigin)
    $clientRect = New-Object MrWin+RECT
    [void][MrWin]::GetClientRect($hwnd, [ref]$clientRect)
    if ($null -eq $strip) {
        $rowsLeft = $clientOrigin.X - $windowRect.L
        $rowsRight = $rowsLeft + ($clientRect.R - $clientRect.L) - 1
        if (($style -band $script:wsVScroll) -ne 0) {
            $rowsRight -= [MrWin]::GetSystemMetrics($script:smCxVScroll)
        }
    }
    $rowsBottom = ($clientOrigin.Y + $clientRect.B) - $windowRect.T - 1
    if (($style -band $script:wsHScroll) -ne 0) {
        $rowsBottom -= [MrWin]::GetSystemMetrics($script:smCyHScroll)
    }

    # ГРАНИЦЫ ОБЛАСТИ СТРОК ПО ПИКСЕЛЯМ.
    #
    # Клиентская область окна списка — это НЕ то же самое, что нарисованные
    # строки. Измерено на списке «Дисков» при окне 1136x795: окно списка
    # 884x584, шапка 884x20, а нарисованные строки — полоса высотой 189 px, и
    # всё остальное клиентской области остаётся фоном страницы. Причина в
    # WS_EX_TRANSPARENT (extstyle 0x00000200 у SysListView32): ком-контроль
    # не красит свой фон, и красит его NM_CUSTOMDRAW только под строками, а
    # «строка» без подпунктов (список пуст) не рисуется вовсе. Ни одно
    # сообщение списка про это не говорит, а LVM_GETNEXTITEM идёт по элементам,
    # а не по нарисованным строкам.
    #
    # Поэтому область строк ищется по пикселям: это самый большой прямоугольник
    # внутри клиентской области списка, залитый ЦЕЛИКОМ цветом строки (фоном
    # строк), — то есть самый большой однородный блок, начинающийся под шапкой.
    # На пустом списке такого блока нет, и проверка строк не применяется.
    $area = New-Object MrWin+RECT
    $area.L = $rowsLeft
    $area.T = $rowsTop
    $area.R = $rowsRight
    $area.B = $rowsBottom
    $painted = Measure-RowArea $shot.Bytes $shot.Stride $area $pageBackground
    if ($painted.Found) {
        $rowsLeft = $painted.Left
        $rowsTop = $painted.Top
        $rowsRight = $painted.Right
        $rowsBottom = $painted.Bottom
    }

    $line = ''
    if ($null -ne $strip) {
        $narrowest = 0
        foreach ($w in $strip.Widths) { if ($narrowest -eq 0 -or $w -lt $narrowest) { $narrowest = $w } }
        $line = "  [шапка] столбцов={0} ширины={1} полоса={2} хвост={3} фон={4}" -f `
            $strip.Widths.Count, ($strip.Widths -join '+'), $strip.Strip, $strip.Tail, $strip.Background
        if ($strip.Skipped -gt 0) {
            $line += " (полос не-столбцов отброшено: {0})" -f $strip.Skipped
        }
        if ($strip.Widths.Count -eq 0) {
            # Ни одного разделителя на полосе шириной Strip: первый столбец
            # либо обрезан краем окна, либо шапка не нарисована вовсе. Раньше тот
            # же случай давался как «столбцы шире списка: 1292 > 648».
            $result.ColumnWhy = "столбцы не различимы: на полосе заголовка $($strip.Strip) px нет ни одного разделителя столбцов"
        } else {
            if ($result.ColumnWhy -eq '' -and $strip.Tail -gt $tolerance -and $strip.Tail -ge $narrowest) {
                # Хвост полосы шире самого узкого столбца — это не «свободное
                # место», а отрезанная часть последнего столбца: comctl32 красит
                # её тем же фоном шапки, и отличить от пустого места по пикселям
                # нельзя. Порог выбран по измеренным хвостам: 26 px при самом
                # узком столбце 92 («Диски» при 900x600), 6 при 120
                # («Настройки»), 21 у «Настроек» в светлой теме — запас в 3,5
                # раза. Обратная сторона честно записана в шапке: у «Отчёта» при
                # 900x600 шесть столбцов по 92+84+132+320+120+104 = 852 не
                # помещаются в полосу 642, последние два обрезаны, хвост 14 px,
                # и это НЕ ловится — отрезанная часть меньше самого узкого
                # видимого столбца.
                $result.ColumnWhy = "хвост полосы $($strip.Tail) px не уже самого узкого столбца $narrowest px — часть столбца уехала за край"
            }
        }
    }
    $text = Measure-RowText $shot.Bytes $shot.Stride $rowsLeft $rowsTop $rowsRight $rowsBottom $pageBackground
    if ($null -ne $text) {
        $pixels = $text.Text * 4
        $spanPercent = 0.0
        if ($text.Width -gt 0) { $spanPercent = [Math]::Round(100.0 * $text.Span / $text.Width, 2) }
        $line += ("`n  [строки] строк={0} фон строки={1} текст={2} px из {3} px ({4}%), полоса текста={5} px ({6}% ширины), макс. серия={7} px" -f `
            $rows, $text.RowBackground, $pixels, ($text.Samples * 4), $text.Ratio,
            $text.Span, $spanPercent, $text.MaxRun)
        if ($rows -gt 0) {
            # Пустой список — законное состояние («ничего не найдено», «отчёт ещё
            # не получен»), и текста в нём нет по определению: проверка
            # применяется только к непустому. Это единственное место, где
            # «пусто» отличается от «не видно», и различает их по числу строк,
            # а не по пикселям: у невидимых строк пикселей ровно столько же,
            # сколько у пустого списка.
            if ($pixels -lt $MinListTextPixels) {
                $result.RowsWhy = "строк $rows, а текста всего $pixels px при пороге $MinListTextPixels — строки нарисованы цветом фона"
            } elseif ($spanPercent -lt $MinListTextSpanPercent) {
                $result.RowsWhy = "строк $rows, текст занимает полосу $spanPercent % при пороге $MinListTextSpanPercent % — это значки строк, а не подписи"
            }
        }
    }
    $result.Log = $line
    return $result
}

# Окно карты разделов: прямой потомок окна экрана «Диски»,
# прямого потомка хоста содержимого. Классы — те же, что
# объявлены в src\ui\view_disks.cpp (detail::kDisksViewClass
# и detail::kDisksMapClass); ворота зависят от них,
# как зависят от классов списков, и об этом написано
# в шапке («ПОДПИСЬ ДИСКА НА КАРТЕ РАЗДЕЛОВ»).
# Возвращает [IntPtr]::Zero, когда страница не «Диски»
# или её окно не открыто: проверять нечего.
function Find-DisksMapWindow([IntPtr]$hostWindow) {
    $view = [IntPtr]::Zero
    foreach ($child in Get-DirectChild $hostWindow) {
        $c = New-Object Text.StringBuilder 256
        [void][MrWin]::GetClassNameW($child, $c, 256)
        if ($c.ToString() -eq $script:classDisksView -and [MrWin]::IsWindowVisible($child)) {
            $view = $child
            break
        }
    }
    if ($view -eq [IntPtr]::Zero) { return [IntPtr]::Zero }
    foreach ($child in Get-DirectChild $view) {
        $c = New-Object Text.StringBuilder 256
        [void][MrWin]::GetClassNameW($child, $c, 256)
        if ($c.ToString() -eq $script:classDisksMap -and [MrWin]::IsWindowVisible($child)) {
            return $child
        }
    }
    return [IntPtr]::Zero
}

# Чернила в области карты: видимая высота подписи диска,
# высота полосы сегментов и зазор между ними (D-81).
# Полный разбор меры — в шапке, «ПОДПИСЬ ДИСКА НА КАРТЕ
# РАЗДЕЛОВ». $mapWindow может быть нулевым (страница не
# «Диски»): тогда Checked = $false и проверка не
# применяется. Checked = $false также у ПУСТОЙ карты
# (полоса сегментов на снимке не найдена): у карты
# без дисков подписи нет по определению.
function Measure-MapLabel($bytes, $stride, [IntPtr]$mapWindow, $windowRect) {
    $none = @{ Checked = $false; LabelHeight = 0; LabelTop = -1
               LabelBottom = -1; BandTop = -1; BandBottom = -1
               Gap = -1; Width = 0 }
    if ($mapWindow -eq [IntPtr]::Zero) { return $none }
    $mapRect = New-Object MrWin+RECT
    [void][MrWin]::GetWindowRect($mapWindow, [ref]$mapRect)
    $x0 = $mapRect.L - $windowRect.L
    $y0 = $mapRect.T - $windowRect.T
    $x1 = $mapRect.R - $windowRect.L - 1
    $y1 = $mapRect.B - $windowRect.T - 1
    $width = $x1 - $x0 + 1
    $height = $y1 - $y0 + 1
    if ($width -lt 8 -or $height -lt 8) { return $none }
    # Фон карты — самый частый цвет её области: рендерер
    # заливает всю клиентскую область палитрой surface
    # (Renderer::beginFrame) перед полосами.
    $counts = @{}
    for ($y = $y0; $y -le $y1; $y++) {
        $row = $y * $stride
        for ($x = $x0; $x -le $x1; $x++) {
            $i = $row + $x * 4
            $key = "$($bytes[$i]),$($bytes[$i + 1]),$($bytes[$i + 2])"
            if ($counts.ContainsKey($key)) { $counts[$key]++ } else { $counts[$key] = 1 }
        }
    }
    if ($counts.Count -eq 0) { return $none }
    $background = ($counts.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 1).Name
    # Профиль чернил по строкам: сколько пикселей строки
    # отличается от фона карты.
    $profile = New-Object int[] $height
    for ($y = $y0; $y -le $y1; $y++) {
        $row = $y * $stride
        $ink = 0
        for ($x = $x0; $x -le $x1; $x++) {
            $i = $row + $x * 4
            $key = "$($bytes[$i]),$($bytes[$i + 1]),$($bytes[$i + 2])"
            if ($key -ne $background) { $ink++ }
        }
        $profile[$y - $y0] = $ink
    }
    $half = [int](($width + 1) / 2)
    # Верхняя текстовая полоса: от первой строки с чернилами
    # до первой сплошной строки (полоса сегментов).
    $top = -1
    for ($i = 0; $i -lt $profile.Length; $i++) {
        if ($profile[$i] -gt 0) { $top = $i; break }
    }
    if ($top -lt 0) { return $none }
    $bottom = $top
    $missed = 0
    for ($i = $top; $i -lt $profile.Length; $i++) {
        if ($profile[$i] -ge $half) { break }
        if ($profile[$i] -gt 0) { $bottom = $i; $missed = 0 }
        else {
            $missed++
            if ($missed -gt 2) { break }
        }
    }
    # Полоса сегментов: первая строка не уже половины
    # ширины карты, и её низ — последняя такая строка
    # подряд (заливка дорожки сплошная, без пустых
    # строк внутри).
    $bandTop = -1
    for ($i = $bottom + 1; $i -lt $profile.Length; $i++) {
        if ($profile[$i] -ge $half) { $bandTop = $i; break }
    }
    if ($bandTop -lt 0) { return $none }
    $bandBottom = $bandTop
    for ($i = $bandTop; $i -lt $profile.Length; $i++) {
        if ($profile[$i] -lt $half) { break }
        $bandBottom = $i
    }
    return @{ Checked = $true; LabelHeight = ($bottom - $top + 1)
              LabelTop = $top; LabelBottom = $bottom
              BandTop = $bandTop; BandBottom = $bandBottom
              BandHeight = ($bandBottom - $bandTop + 1)
              Gap = ($bandTop - $bottom - 1); Width = $width
              Background = $background }
}

# Раскладка одной страницы. Возвращает @{ Checked; Violations; Hidden } где
# Violations — массив строк с классом, подписью, прямоугольником в координатах
# хоста и величиной вылета по каждой стороне, а Hidden — строки списка и
# дерева, нарисованные невидимыми (отдельный отказ, отдельный код 14).
#
# $listsOnly — режим «только невидимые строки» (D-77). Обход раскладки ждёт
# переднего плана и потому недостижим локально, а мера строк — нет: она читает
# снимок уже открытой страницы. Флаг отбрасывает вылет элементов (код 10),
# оставляя ровно то, что нужно стартовой странице: обход всех страниц по-прежнему
# получает обе меры целиком. Измерение при этом ОДНО и то же — второй код
# измерения разошёлся бы с первым через месяц.
function Measure-PageLayout([IntPtr]$hostWindow, [int]$tolerance, $shot, $windowRect,
                            [bool]$listsOnly = $false) {
    $client = New-Object MrWin+RECT
    [void][MrWin]::GetClientRect($hostWindow, [ref]$client)
    $origin = New-Object MrWin+POINT
    $origin.X = $client.L
    $origin.Y = $client.T
    [void][MrWin]::ClientToScreen($hostWindow, [ref]$origin)
    $limitLeft = $origin.X
    $limitTop = $origin.Y
    $limitRight = $origin.X + ($client.R - $client.L)
    $limitBottom = $origin.Y + ($client.B - $client.T)

    # Фон страницы: самый частый цвет окна содержимого. Шаг 4 — фон страницы это
    # большая площадь, а не подпись, и точность ему не нужна.
    $pageBackground = ''
    if ($null -ne $shot) {
        $pageX = $origin.X - $windowRect.L
        $pageY = $origin.Y - $windowRect.T
        $pageBackground = Measure-DominantColor $shot.Bytes $shot.Stride $pageX $pageY `
            ($pageX + ($client.R - $client.L) - 1) ($pageY + ($client.B - $client.T) - 1) 4
    }

    $violations = New-Object System.Collections.ArrayList
    $hidden = New-Object System.Collections.ArrayList
    $checked = 0
    foreach ($root in Get-DirectChild $hostWindow) {
        if (-not [MrWin]::IsWindowVisible($root)) { continue }
        $queue = New-Object System.Collections.ArrayList
        [void]$queue.Add($root)
        $cb = [MrWin+EnumProc] {
            param($h, $l)
            [void]$queue.Add($h)
            return $true
        }
        [void][MrWin]::EnumChildWindows($root, $cb, [IntPtr]::Zero)
        foreach ($item in $queue) {
            if (-not [MrWin]::IsWindowVisible($item)) { continue }
            $rect = New-Object MrWin+RECT
            [void][MrWin]::GetWindowRect($item, [ref]$rect)
            $checked++
            $cls = New-Object Text.StringBuilder 256
            [void][MrWin]::GetClassNameW($item, $cls, 256)
            $cap = New-Object Text.StringBuilder 256
            [void][MrWin]::GetWindowTextW($item, $cap, 256)
            $label = $cap.ToString()
            if ($label.Length -gt 40) { $label = $label.Substring(0, 40) + '...' }
            $w = $rect.R - $rect.L
            $h = $rect.B - $rect.T
            $why = ''
            if ($w -le 0 -or $h -le 0) {
                $why = 'нулевой размер'
            } else {
                # Допуск ТОЛЬКО расширяет границу: элемент должен умещаться в
                # прямоугольник [граница - T, граница + T]. Считать «вылет» как
                # «граница + T минус элемент» нельзя — тогда элемент ровно по
                # границе давал бы T и красный на пустом месте.
                $over = ''
                if ((($limitLeft - $tolerance) - $rect.L) -gt 0) {
                    $over = 'слева ' + (($limitLeft - $tolerance) - $rect.L)
                }
                if (($rect.R - ($limitRight + $tolerance)) -gt 0) {
                    $over = $over + '; справа ' + ($rect.R - ($limitRight + $tolerance))
                }
                if ((($limitTop - $tolerance) - $rect.T) -gt 0) {
                    $over = $over + '; сверху ' + (($limitTop - $tolerance) - $rect.T)
                }
                if (($rect.B - ($limitBottom + $tolerance)) -gt 0) {
                    $over = $over + '; снизу ' + ($rect.B - ($limitBottom + $tolerance))
                }
                if ($over -ne '') { $why = $over.TrimStart(';').Trim() }
            }
            $className = $cls.ToString()
            $relL = $rect.L - $origin.X
            $relT = $rect.T - $origin.Y
            $head = '{0,-22} {1,-16} ({2},{3}) {4}x{5}  ' -f $className, ('"' + $label + '"'), $relL, $relT, $w, $h
            $hiddenWhy = ''
            if ($null -ne $shot -and ($className -eq $script:classListView -or $className -eq $script:classTreeView)) {
                $list = Measure-ListItem $shot $windowRect $item $className $pageBackground $tolerance
                if ($list.Log -ne '') { Write-Host $list.Log }
                if ($why -eq '') { $why = $list.ColumnWhy }
                $hiddenWhy = $list.RowsWhy
            }
            if ($why -ne '' -and -not $listsOnly) { [void]$violations.Add($head + $why.TrimStart(';').Trim()) }
            if ($hiddenWhy -ne '') { [void]$hidden.Add($head + $hiddenWhy) }
        }
    }
    return @{ Checked = $checked; Violations = $violations; Hidden = $hidden }
}

function Stop-AppProcess($proc) {
    if ($null -eq $proc) { return }
    if ($proc.HasExited) { return }
    [void]$proc.CloseMainWindow()
    Start-Sleep -Seconds 3
    if (-not $proc.HasExited) {
        Stop-Process -Id $proc.Id -Force
        Write-Host '[ui] закрыт принудительно'
    } else {
        Write-Host "[ui] закрылся сам, код $($proc.ExitCode)"
    }
}

# Запуск приложения. Флаг --fresh-ui-state — это и есть сброс состояния
# интерфейса (см. шапку): приложение стартует как при первой установке и
# ничего не пишет обратно в HKCU. -KeepUiState оставляет привычное поведение
# для того, кто СПЕЦИАЛЬНО хочет померить своё сохранённое состояние.
#
# -Page — третий случай, и он важнее обоих: страницу выбрать иначе нечем (см.
# шапку, «СТРАНИЦА БЕЗ ФОКУСА»). Выбранная страница лежит в nav.current, а
# --fresh-ui-state это значение как раз стирает, поэтому с -Page флаг не
# передаётся. Побочный эффект тот же, что у -KeepUiState: приложение
# восстанавливает и window.placement, и настроенные ранее страницы, — но размер
# окна ворота всё равно задают сами (-Width/-Height плюс MoveWindow).
function Start-AppProcess {
    if ($KeepUiState -or $Page -ne '') {
        if ($Page -ne '') {
            Write-Host ("[ui] страница '{0}' выбрана состоянием (nav.current), состояние интерфейса НЕ сбрасывается — фокус не трогаем" -f $Page)
        } else {
            Write-Host '[ui] состояние интерфейса НЕ сбрасывается (-KeepUiState): окно может прийти из HKCU'
        }
        return Start-Process $Exe -PassThru
    }
    Write-Host '[ui] состояние интерфейса сброшено флагом запуска --fresh-ui-state'
    return Start-Process $Exe -ArgumentList '--fresh-ui-state' -PassThru
}

# Числа одного прогона. Их сравнивает -DeterminismRuns, поэтому сюда попадает
# ровно то, что обязано совпасть: размеры, оба счётчика чернил, число цветов и
# открытая страница.
$script:lastRun = @{}

function Invoke-Smoke {
    $proc = Start-AppProcess
    try {
        # pid прогоняемого процесса печатается рядом с владельцем переднего плана
        # (Get-ForegroundOwner тоже печатает pid): без него в логе не видно, чей
        # это процесс, когда на машине параллельно запущено ещё одно окно того же
        # приложения, и чужой прогон ворота можно спутать со своим.
        Write-Host ("[ui] процесс приложения: pid {0}" -f $proc.Id)
        Start-Sleep -Seconds $SettleSeconds
        if ($proc.HasExited) { Write-Host "[ui] процесс упал, код $($proc.ExitCode)"; return 6 }

        # Поиск окна с повторами: SettleSeconds — это ожидание, а не
        # гарантия. На загруженной машине окно появляется позже (два
        # прогона подряд из десяти заканчивались кодом 5 при живом
        # процессе), и такой «красный» код говорил о среде, а не о
        # приложении.
        $hwnd = [IntPtr]::Zero
        for ($attempt = 1; $attempt -le $WindowFindTries -and $hwnd -eq [IntPtr]::Zero; $attempt++) {
            if ($attempt -gt 1) { Start-Sleep -Seconds 1 }
            $hwnd = Find-AppWindow $proc.Id
            if ($hwnd -eq [IntPtr]::Zero -and $proc.HasExited) {
                Write-Host "[ui] процесс упал, код $($proc.ExitCode)"
                return 6
            }
        }
        if ($hwnd -eq [IntPtr]::Zero) {
            Write-Host "[ui] окно не найдено за $WindowFindTries попытками"
            $seen = Describe-AppWindows $proc.Id
            if ($null -eq $seen -or $seen.Count -eq 0) {
                Write-Host '  [окно] у процесса нет ни одного верхнего окна'
            } else {
                foreach ($line in $seen) { Write-Host "  [окно] $line" }
            }
            return 5
        }

        # Какая страница открылась — проверка состояния, а не рисунка. Ворота
        # обязаны знать, ЧТО они измеряют: до сброса одна и та же команда
        # открывала то «Настройки» (128 149 чернил), то «Обзор» (38 315) в
        # зависимости от nav.current в реестре. Спрашивается у ВИДИМОГО окна
        # экрана, а не у реестра: реестр показывает, что было записано, а
        # вопрос в том, что приложение открыло на самом деле.
        $startRect = New-Object MrWin+RECT
        [void][MrWin]::GetWindowRect($hwnd, [ref]$startRect)
        $startHost = Find-ContentHost $hwnd $startRect
        $visibleScreens = @()
        if ($startHost.Found) { $visibleScreens = @(Get-VisibleScreen $startHost.Hwnd) }
        $startPage = ''
        if ($visibleScreens.Count -eq 1) {
            $startPage = ($visibleScreens[0].Class -replace '^MrProper\.', '' -replace 'View$', '').ToLower()
        }
        Write-Host ("[ui] стартовая страница: {0} (видимых экранов: {1})" -f `
            $(if ($startPage -eq '') { 'не определена' } else { $startPage }), $visibleScreens.Count)
        if ($ExpectPage -ne '' -and $startPage -ne $ExpectPage) {
            Write-Host ("[ui] ПРОВАЛ: открыта страница '{0}', а -ExpectPage='{1}' — состояние интерфейса НЕ сброшено, ворота меряют чужой запуск" -f $startPage, $ExpectPage)
            return 12
        }

        $realDpi = [MrWin]::GetDpiForWindow($hwnd)
        if ($realDpi -eq 0) { $realDpi = $script:baseDpi }
        $effDpi = $realDpi
        $minTrackBefore = Get-AppMinTrack $hwnd
        if ($null -eq $minTrackBefore) {
            Write-Host '[ui] ПРОВАЛ: окно не ответило на WM_GETMINMAXINFO — нечем подтвердить масштаб'
            return 3
        }
        Write-Host ("[ui] минимум окна до эмуляции: {0}x{1} (системный dpi окна: {2})" -f `
            $minTrackBefore[0], $minTrackBefore[1], $realDpi)

        if ($DpiPercent -gt 0) {
            $effDpi = [int][Math]::Round($DpiPercent * $script:baseDpi / 100.0)
            $wp = [IntPtr](($effDpi -band 0xFFFF) -bor ($effDpi -shl 16))
            $result = [IntPtr]::Zero
            $sent = [MrWin]::SendMessageTimeout($hwnd, $script:wmDpiAfterParent, $wp, [IntPtr]::Zero,
                                                 $script:smtoAbortIfHung, 5000, [ref]$result)
            Start-Sleep -Milliseconds 300
            $minTrackAfter = Get-AppMinTrack $hwnd
            if ($null -eq $minTrackAfter) {
                Write-Host '[ui] ПРОВАЛ: после WM_DPICHANGED_AFTERPARENT окно перестало отвечать'
                return 3
            }
            $wantX = [int][Math]::Round($minTrackBefore[0] * $effDpi / [double]$realDpi)
            $wantY = [int][Math]::Round($minTrackBefore[1] * $effDpi / [double]$realDpi)
            Write-Host ("[ui] DPI {0} (запрошено {1}%), минимум окна {2}x{3}, ждали {4}x{5}" -f `
                $effDpi, $DpiPercent, $minTrackAfter[0], $minTrackAfter[1], $wantX, $wantY)
            if ($sent -eq [IntPtr]::Zero -and ([Math]::Abs($minTrackAfter[0] - $wantX) -gt 4 -or
                                               [Math]::Abs($minTrackAfter[1] - $wantY) -gt 4)) {
                Write-Host '[ui] ПРОВАЛ: приложение не приняло эмуляцию DPI — раскладка не проверена'
                return 3
            }
            if ([Math]::Abs($minTrackAfter[0] - $wantX) -gt 4 -or
                [Math]::Abs($minTrackAfter[1] - $wantY) -gt 4) {
                Write-Host '[ui] ПРОВАЛ: минимум окна не вырос в effDpi/96 раз'
                return 3
            }
        }

        if ($Width -gt 0 -and $Height -gt 0) {
            $targetW = [int][Math]::Round($Width * $effDpi / [double]$script:baseDpi)
            $targetH = [int][Math]::Round($Height * $effDpi / [double]$script:baseDpi)
            [void][MrWin]::MoveWindow($hwnd, 40, 40, $targetW, $targetH, $true)
            Start-Sleep -Milliseconds 400
            $after = New-Object MrWin+RECT
            [void][MrWin]::GetWindowRect($hwnd, [ref]$after)
            $gotW = $after.R - $after.L
            $gotH = $after.B - $after.T
            Write-Host ("[ui] запросили окно {0}x{1} ({2}x{3} DIP при dpi {4}), получили {5}x{6}" -f `
                $targetW, $targetH, $Width, $Height, $effDpi, $gotW, $gotH)
            if ([Math]::Abs($gotW - $targetW) -gt 2 -or [Math]::Abs($gotH - $targetH) -gt 2) {
                Write-Host '[ui] ПРОВАЛ: окно не приняло запрошенный размер'
                return 3
            }
            if ($proc.HasExited) { Write-Host '[ui] процесс упал после смены размера'; return 6 }
        }

        # Снимок берём после принудительной перерисовки всей иерархии: иначе
        # PrintWindow успевает до WM_PAINT и ловит предыдущий кадр.
        #
        # Окно к этому моменту может оказаться свёрнутым: на машине, где
        # параллельно работают другие сессии, фокус уходит, и приложение (или
        # система) сворачивает окно. GetWindowRect у свёрнутого окна отдаёт
        # иконку 160x24 в точке (-32000,-32000) — снимок такого окна дал бы
        # «11% чернил в окне 160x24» и код 8 вместо честного отказа. Поэтому
        # перед съёмкой окно разворачивается, а размер проверяется повторно.
        for ($attempt = 1; $attempt -le $WindowRestoreTries; $attempt++) {
            if ([MrWin]::IsIconic($hwnd)) {
                Write-Host ("[ui] окно свёрнуто (попытка {0} из {1}) — разворачиваем (SW_RESTORE)" -f $attempt, $WindowRestoreTries)
                [void][MrWin]::ShowWindow($hwnd, 9)
                Start-Sleep -Milliseconds 400
            }
            $probe = New-Object MrWin+RECT
            [void][MrWin]::GetWindowRect($hwnd, [ref]$probe)
            if ((($probe.R - $probe.L) -ge 200) -and (($probe.B - $probe.T) -ge 200)) { break }
            # Свёрнутое окно отдаёт 160x24 в (-32000,-32000): пока размер
            # такой, снимок брать рано — это был бы отказ из-за среды.
            Start-Sleep -Milliseconds 500
        }
        Start-Sleep -Seconds $RepaintSeconds
        [void][MrWin]::RedrawWindow($hwnd, [IntPtr]::Zero, [IntPtr]::Zero, $script:redrawAll)
        [void][MrWin]::UpdateWindow($hwnd)
        Start-Sleep -Milliseconds 300

        $rect = New-Object MrWin+RECT
        [void][MrWin]::GetWindowRect($hwnd, [ref]$rect)
        $w = $rect.R - $rect.L
        $h = $rect.B - $rect.T
        if ($w -lt 200 -or $h -lt 200) {
            Write-Host ("[ui] ПРОВАЛ: к моменту съёмки окно имеет размер {0}x{1} — это не окно приложения" -f $w, $h)
            return 3
        }
        $shotDir = Split-Path -Parent $Shot
        if ($shotDir -ne '' -and -not (Test-Path -LiteralPath $shotDir)) {
            New-Item -ItemType Directory -Path $shotDir -Force | Out-Null
        }
        $bmp = New-Object System.Drawing.Bitmap($w, $h)
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $dc = $g.GetHdc()
        $ok = [MrWin]::PrintWindow($hwnd, $dc, 2)
        $g.ReleaseHdc($dc)
        $g.Dispose()
        $bmp.Save($Shot, [System.Drawing.Imaging.ImageFormat]::Png)

        # Клиентская область в координатах снимка окна: рамку и заголовок рисует DWM,
        # и без отсечения пустое окно проходит проверку (самый частый цвет тогда —
        # белый заголовок плюс тёмная рамка).
        $origin = New-Object MrWin+POINT
        $origin.X = 0; $origin.Y = 0
        [void][MrWin]::ClientToScreen($hwnd, [ref]$origin)
        $cx = $origin.X - $rect.L
        $cy = $origin.Y - $rect.T
        # D-65: размеры клиентской области берём у GetClientRect, а НЕ выводим из размеров
        # окна вычитанием симметричной рамки. Рамка у Windows несимметрична: при окне 1136x795
        # сверху 27, снизу 8, то есть прежняя арифметика давала клиент 1120x741 вместо
        # настоящих 1120x760 — на 19 px меньше. Именно эта ошибка породила ложный вывод про
        # срезанные кнопки «Очистки» (D-62).
        $clientRect = New-Object MrWin+RECT
        [void][MrWin]::GetClientRect($hwnd, [ref]$clientRect)
        $cw = $clientRect.R - $clientRect.L
        $ch = $clientRect.B - $clientRect.T
        if ($cw -le 0 -or $ch -le 0) { $cx = 0; $cy = 0; $cw = $w; $ch = $h }

        $data = $bmp.LockBits((New-Object System.Drawing.Rectangle(0, 0, $w, $h)),
                              [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                              [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $stride = $data.Stride
        $bytes = New-Object byte[] ($stride * $h)
        [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
        $bmp.UnlockBits($data)
        $bmp.Dispose()
        Write-Host "[ui] клиентская область ${cw}x${ch} (окно ${w}x${h})"

        # Считаем «чернила»: пиксели, отличные от самого частого цвета (фона).
        $client = Measure-Ink $bytes $stride $cx $cy ($cx + $cw) ($cy + $ch)
        $background = $client.Background
        $ink = $client.Ink
        $samples = $client.Samples
        $ratio = $client.Ratio
        Write-Host ("[ui] printwindow=$ok размер=${w}x${h} фон=$background чернила=$ink из $samples точек (${ratio}%, порог $MinInkPixels)")

        if ($ExpectBackground -ne '') {
            $parts = $background.Split(',')
            $lum = Get-RelativeLuminance ([int]$parts[0]) ([int]$parts[1]) ([int]$parts[2])
            Write-Host ("[ui] яркость фона {0} (ожидалась тема {1})" -f [Math]::Round($lum, 4), $ExpectBackground)
            # Границы 0.03 и 0.50 разведены в 16 раз, а крайние палитры дают
            # 0.013 (тёмная 0x20) и 0.886 (светлая 0xF3).
            $bad = if ($ExpectBackground -eq 'dark') { $lum -ge 0.03 } else { $lum -lt 0.50 }
            if ($bad) {
                Write-Host "[ui] ПРОВАЛ: фон не соответствует теме $ExpectBackground — палитра пришла не из темы"
                return 8
            }
        }

        if ($ink -lt $MinInkPixels) {
            Write-Host "[ui] ПРОВАЛ: окно показывает только фон — интерфейс не нарисован. Снимок: $Shot"
            return 4
        }

        # --- содержимое: окно содержимого справа от рельса ----------------------
        # Клиентских чернил мало, чтобы отличить «интерфейс есть» от «нарисован
        # только рельс»: рельс — это всегда ~17% клиента при 1280x720. Содержимое
        # проверяется отдельно, иначе версия «работает и пуста» снова пройдёт.
        # Счётчики содержимого объявлены здесь, а не внутри проверки ниже: они
        # попадают в ИТОГ, а ИТОГ печатается и при -MinContentInkPixels 0.
        $contentInk = 0
        $contentColors = 0
        $centerInk = 0
        if ($MinContentInkPixels -gt 0) {
            $contentWindow = Find-ContentHost $hwnd $rect
            if (-not $contentWindow.Found) {
                Write-Host ("[ui] ПРОВАЛ: у окна нет дочернего окна содержимого — нечего проверять. Снимок: {0}" -f $Shot)
                return 9
            }
            # Область содержимого обрезается по клиенту: дочернее окно не может
            # вылезать за него, а лишние пиксели рамки в подсчёте только шумят.
            $hx = [Math]::Max($cx, $contentWindow.X)
            $hy = [Math]::Max($cy, $contentWindow.Y)
            $hx2 = [Math]::Min($cx + $cw, $contentWindow.X + $contentWindow.W)
            $hy2 = [Math]::Min($cy + $ch, $contentWindow.Y + $contentWindow.H)
            Write-Host ("[ui] окно содержимого {0}x{1} в ({2},{3}), в снимке {4}x{5}" -f `
                $contentWindow.W, $contentWindow.H, $contentWindow.X, $contentWindow.Y, ($hx2 - $hx), ($hy2 - $hy))
            if (($hx2 - $hx) -le 0 -or ($hy2 - $hy) -le 0) {
                Write-Host '[ui] ПРОВАЛ: окно содержимого не попало в клиентскую область'
                return 9
            }
            $content = Measure-Ink $bytes $stride $hx $hy $hx2 $hy2
            $contentInk = $content.Ink
            $contentColors = $content.Colors
            Write-Host ("[ui] содержимое: фон={0} чернила={1} из {2} точек ({3}%, порог {4}) цветов={5}" -f `
                $content.Background, $content.Ink, $content.Samples, $content.Ratio, $MinContentInkPixels, $content.Colors)
            if ($content.Ink -lt $MinContentInkPixels) {
                Write-Host ("[ui] ПРОВАЛ: рельс нарисован ({0} чернил), но окно содержимого пустое — экран не отрисован. Снимок: {1}" -f $ink, $Shot)
                return 9
            }
            if ($content.Colors -lt $MinContentColors) {
                # Плоская заливка плюс рамка — это не экран. Проверено числами: у
                # пустого хоста 1 цвет, у артефакта «дочернее окно выше клиентской
                # на 19 пикселей, его неприпаркованный фон виден белой полосой
                # снизу и справа» — 2 цвета, у пяти нарисованных экранов — 7..9
                # даже без ClearType (с ним больше). Порог 4 отделён от обоих.
                Write-Host ("[ui] ПРОВАЛ: в окне содержимого {0} цветов (порог {1}) — это плоская заливка, а не экран. Снимок: {2}" -f `
                    $content.Colors, $MinContentColors, $Shot)
                return 9
            }

            # Центральные 70 % окна содержимого. Ловят класс «счёт прошёл, а
            # экрана нет»: при 150 % DPI окно содержимого съезжает влево на
            # 31 пиксель и накрывает правый край рельса — 7590 «чернил» и 7
            # цветов из подписей рельса давали зелёный код при пустом экране.
            # В центре 70 % у того же окна 0 чернил и 1 цвет, а у пяти
            # нарисованных экранов — от 409 (очистка, самая пустая) до 29264.
            # Порог 250 — в 1,6 раза ниже самой пустой нарисованной страницы.
            $centerShare = 0.70
            $mx0 = $hx + [int](($hx2 - $hx) * (1.0 - $centerShare) / 2.0)
            $mx1 = $hx2 - [int](($hx2 - $hx) * (1.0 - $centerShare) / 2.0)
            $my0 = $hy + [int](($hy2 - $hy) * (1.0 - $centerShare) / 2.0)
            $my1 = $hy2 - [int](($hy2 - $hy) * (1.0 - $centerShare) / 2.0)
            $center = Measure-Ink $bytes $stride $mx0 $my0 $mx1 $my1
            $centerInk = $center.Ink
            Write-Host ("[ui] центр 70%: {0}x{1} чернила={2} из {3} точек ({4}%, порог {5})" -f `
                ($mx1 - $mx0), ($my1 - $my0), $center.Ink, $center.Samples, $center.Ratio, $MinCenterInkPixels)
            if ($center.Ink -lt $MinCenterInkPixels) {
                Write-Host ("[ui] ПРОВАЛ: в центре окна содержимого {0} чернил (порог {1}) — экран не отрисован, края закрашены. Снимок: {2}" -f `
                    $center.Ink, $MinCenterInkPixels, $Shot)
                return 9
            }
        }

        # --- невидимые строки списка на СТАРТОВОЙ странице (код 14, D-77) -------
        # Живёт здесь, а не только в обходе -AllPages: обход требует переднего
        # плана, то есть локально (переднее окно занято чужим приложением) он
        # даёт код 11 и числа этой проверки недостижимы — а это ровно тот режим,
        # в котором их и надо получать. Страница при этом выбирается состоянием
        # (-Page -> nav.current), а не вводом, поэтому ворота не крадут фокус.
        #
        # Под -AllPages проверка НЕ повторяется: обход начинается с той же
        # стартовой страницы и меряет её целиком (вместе с раскладкой).
        #
        # Отказ здесь — самый важный из кодов содержимого: элемент, нарисованный
        # белым по белому, пользователь не видит, и никакие счётчики чернил его
        # не показывают. Пустой список (строк 0) проверку не вызывает — «ничего
        # не найдено» законно, см. Measure-ListItem.
        $script:mapLabelLast = 0
        if (-not $AllPages) {
            $startListHost = Find-ContentHost $hwnd $rect
            if (-not $startListHost.Found) {
                Write-Host '[ui] ПРОВАЛ: окно содержимого не найдено — негде искать списки'
                return 9
            }
            $startShot = @{ Bytes = $bytes; Stride = $stride }
            $startLayout = Measure-PageLayout $startListHost.Hwnd $LayoutTolerance $startShot $rect $true
            Write-Host ("[ui] страница '{0}': проверено видимых элементов {1}, невидимых списков {2} (без -AllPages)" -f `
                $startPage, $startLayout.Checked, $startLayout.Hidden.Count)
            if ($startLayout.Hidden.Count -gt 0) {
                Write-Host '[ui] ПРОВАЛ: строки списка нарисованы невидимыми на стартовой странице'
                foreach ($line in $startLayout.Hidden) { Write-Host $line }
                Write-Host ("[ui] Хост содержимого: клиентская область {0}x{1}, окно содержимого {2}x{3}" -f `
                    $startListHost.W, $startListHost.H, $cw, $ch)
                Write-Host ("[ui] страница выбрана ключом -Page '{0}' (nav.current), фокус не перехватывался" -f $Page)
                return 14
            }
            # Подпись диска на карте разделов (D-81): карта рисуется
            # Direct2D, и ни мера прямоугольников, ни общие чернила
            # её среза не видят — проверка отдельная, по чернилам
            # области окна карты (разбор меры — в шапке).
            $mapWindow = Find-DisksMapWindow $startListHost.Hwnd
            $mapMeasure = Measure-MapLabel $bytes $stride $mapWindow $rect
            if ($mapMeasure.Checked) {
                $script:mapLabelLast = $mapMeasure.LabelHeight
                Write-Host ("[ui] карта разделов: подпись диска {0} px (строки {1}..{2}), полоса сегментов {3} px (строки {4}..{5}), зазор {6} px, порог {7} px" -f `
                    $mapMeasure.LabelHeight, $mapMeasure.LabelTop, $mapMeasure.LabelBottom, `
                    $mapMeasure.BandHeight, $mapMeasure.BandTop, $mapMeasure.BandBottom, `
                    $mapMeasure.Gap, $MinMapLabelHeightPx)
                if ($mapMeasure.LabelHeight -lt $MinMapLabelHeightPx -and `
                        $mapMeasure.LabelHeight * 2 -lt $mapMeasure.BandHeight) {
                    Write-Host "[ui] ПРОВАЛ: подпись диска на карте разделов срезана (D-81)"
                    Write-Host ("[ui] видимая высота подписи {0} px ниже порога {1} px И ниже половины полосы сегментов {2} px (половина — {3} px)" -f `
                        $mapMeasure.LabelHeight, $MinMapLabelHeightPx, $mapMeasure.BandHeight, `
                        [int]($mapMeasure.BandHeight / 2))
                    Write-Host ("[ui] карта в снимке: {0}, строки подписи {1}..{2}, полоса сегментов {3}..{4}" -f `
                        $Shot, $mapMeasure.LabelTop, $mapMeasure.LabelBottom, `
                        $mapMeasure.BandTop, $mapMeasure.BandBottom)
                    return 15
                }
            }
        }

        # --- обход всех страниц: раскладка каждой (ключ -AllPages) --------------
        # Стоит ПОСЛЕ проверок чернил: они смотрят на стартовую страницу, и
        # их отказ не должен мешать обходу поставить свой, более точный.
        if ($AllPages) {
            $walkHost = Find-ContentHost $hwnd $rect
            if (-not $walkHost.Found) {
                Write-Host '[ui] ПРОВАЛ: обход страниц — окно содержимого не найдено'
                return 9
            }
            $walk = New-Object System.Collections.ArrayList
            $walkHidden = New-Object System.Collections.ArrayList
            $walkMap = New-Object System.Collections.ArrayList
            $script:contentHwnd = $walkHost.Hwnd
            $script:brokenPages = New-Object System.Collections.ArrayList
            $script:hiddenPages = New-Object System.Collections.ArrayList
            $script:mapPages = New-Object System.Collections.ArrayList
            for ($index = 0; $index -lt $script:pages.Count; $index++) {
                $page = $script:pages[$index]
                $state = Switch-AppPage $hwnd $script:contentHwnd $page
                if (-not $state.Ok) {
                    Write-Host ("[ui] ПРОВАЛ: не удалось открыть страницу {0} за {1} попыток (последняя видимая: {2}) — обход не состоялся, раскладка не проверена" -f $page.Name, $state.Tries, $state.Visible)
                    Write-Host ("[ui] ПЕРЕДНИЙ ПЛАН В МОМЕНТ ОТКАЗА: {0}. Чужое окно перехватило фокус, пока шёл ввод; это отказ среды, а не дефект приложения." -f $state.Foreign)
                    return 11
                }
                [void][MrWin]::RedrawWindow($hwnd, [IntPtr]::Zero, [IntPtr]::Zero, $script:redrawAll)
                [void][MrWin]::UpdateWindow($hwnd)
                Start-Sleep -Milliseconds $PageSettleMilliseconds
                $pageShot = "$($Shot -replace '\.[^.]+$', '')-p$($index + 1)-$($page.Name)$([IO.Path]::GetExtension($Shot))"
                # Снимок снимается ДО измерения: меры столбцов и строк читают его
                # пиксели, а не окна.
                $pageShotPixels = Save-WindowShot $hwnd $rect $pageShot
                $layout = Measure-PageLayout $script:contentHwnd $LayoutTolerance $pageShotPixels $rect
                Write-Host ("[ui] страница {0} (Ctrl+{1}, попыток {2}): проверено видимых элементов {3}, нарушений раскладки {4}, невидимых списков {5}; снимок {6}" -f `
                    $page.Name, ($index + 1), $state.Tries, $layout.Checked, $layout.Violations.Count, $layout.Hidden.Count, $pageShot)
                if ($layout.Violations.Count -gt 0) {
                    [void]$script:brokenPages.Add($page.Name)
                    foreach ($line in $layout.Violations) {
                        [void]$walk.Add("  [$($page.Name)] $line")
                    }
                }
                if ($layout.Hidden.Count -gt 0) {
                    [void]$script:hiddenPages.Add($page.Name)
                    foreach ($line in $layout.Hidden) {
                        [void]$walkHidden.Add("  [$($page.Name)] $line")
                    }
                }
                # Карта разделов на «Дисках»: та же мера чернил,
                # что и на стартовой странице (D-81, разбор в
                # шапке). Окно карты есть только у экрана
                # «Диски», на остальных четырёх страницах
                # Find-DisksMapWindow возвращает ноль и
                # проверка не применяется.
                $mapWindow = Find-DisksMapWindow $script:contentHwnd
                $mapMeasure = Measure-MapLabel $pageShotPixels.Bytes $pageShotPixels.Stride $mapWindow $rect
                if ($mapMeasure.Checked) {
                    $script:mapLabelLast = $mapMeasure.LabelHeight
                    Write-Host ("[ui] карта разделов: подпись диска {0} px, полоса сегментов {1} px, зазор {2} px, порог {3} px" -f `
                        $mapMeasure.LabelHeight, $mapMeasure.BandHeight, $mapMeasure.Gap, $MinMapLabelHeightPx)
                    if ($mapMeasure.LabelHeight -lt $MinMapLabelHeightPx -and `
                            $mapMeasure.LabelHeight * 2 -lt $mapMeasure.BandHeight) {
                        [void]$script:mapPages.Add($page.Name)
                        [void]$walkMap.Add("  [$($page.Name)] подпись диска $($mapMeasure.LabelHeight) px ниже порога $MinMapLabelHeightPx px и ниже половины полосы сегментов $($mapMeasure.BandHeight) px (строки подписи $($mapMeasure.LabelTop)..$($mapMeasure.LabelBottom), полоса $($mapMeasure.BandTop)..$($mapMeasure.BandBottom), зазор $($mapMeasure.Gap) px)")
                    }
                }
            }
            # Оба отказа печатаются целиком, даже когда вернётся только один
            # код: невидимые строки и вылет элемента лежат на разных страницах,
            # и молчаливый второй список прятал бы ровно тот дефект, который
            # чинят.
            if ($script:hiddenPages.Count -gt 0) {
                Write-Host "[ui] ПРОВАЛ: строки списка нарисованы невидимыми на страницах: $($script:hiddenPages -join ', ')"
                foreach ($line in $walkHidden) { Write-Host $line }
            }
            if ($script:brokenPages.Count -gt 0) {
                Write-Host "[ui] ПРОВАЛ: сломана раскладка на страницах: $($script:brokenPages -join ', ')"
                foreach ($line in $walk) { Write-Host $line }
            }
            if ($script:mapPages.Count -gt 0) {
                Write-Host "[ui] ПРОВАЛ: подпись диска на карте разделов срезана (D-81) на страницах: $($script:mapPages -join ', ')"
                foreach ($line in $walkMap) { Write-Host $line }
            }
            if ($script:hiddenPages.Count -gt 0 -or $script:brokenPages.Count -gt 0 -or $script:mapPages.Count -gt 0) {
                Write-Host ("[ui] Хост содержимого: клиентская область {0}x{1}, окно содержимого {2}x{3}" -f $walkHost.W, $walkHost.H, $cw, $ch)
                # Невидимые строки важнее вылета: элемент за краем пользователь
                # видит и может нажать, а пустой список не отличить от «ничего
                # не найдено» вообще. Срез подписи карты — тот же класс
                # невидимого дефекта (D-81), что и невидимые строки.
                if ($script:hiddenPages.Count -gt 0) { return 14 }
                if ($script:mapPages.Count -gt 0) { return 15 }
                return 10
            }
            Write-Host '[ui] раскладка всех пяти страниц в порядке'
            Write-Host '[ui] столбцы помещаются в полосу шапки, строки списков и деревьев нарисованы видимыми'
        }

        # Числа прогона. Это и есть результат ворота: по ним два запуска одной и
        # той же команды обязаны совпасть до цифры (-DeterminismRuns), и по ним
        # видно, что измерялось.
        $script:lastRun = [ordered]@{
            Window      = "$($w)x$($h)"
            Client      = "$($cw)x$($ch)"
            InkClient   = $ink
            InkContent  = $contentInk
            Colors      = $contentColors
            InkCenter   = $centerInk
            StartPage   = $startPage
            MapLabel    = $script:mapLabelLast
        }
        Write-Host ("[ui] ИТОГ: окно={0} клиент={1} чернила-клиент={2} чернила-содержимое={3} цветов={4} центр={5} страница={6} подпись-карты={7}" -f `
            $script:lastRun.Window, $script:lastRun.Client, $script:lastRun.InkClient, `
            $script:lastRun.InkContent, $script:lastRun.Colors, $script:lastRun.InkCenter, `
            $script:lastRun.StartPage, $script:lastRun.MapLabel)

        Write-Host "[ui] окно нарисовало содержимое. Снимок: $Shot"
        return 0
    } finally {
        Stop-AppProcess $proc
    }
}

# Прогоны одной и той же команды, которые обязаны дать один и тот же
# результат: разница размеров или чернил — код 13, а не «ну почти».
function Invoke-DeterminismRuns {
    $runs = New-Object System.Collections.ArrayList
    $originalShot = $Shot
    try {
        for ($index = 1; $index -le $DeterminismRuns; $index++) {
            if ($DeterminismRuns -gt 1) {
                # Снимок второго и следующих прогонов получает свой суффикс:
                # один и тот же файл перезаписал бы доказательство, что кадров
                # было два, а не один.
                $Shot = "$($originalShot -replace '\.[^.]+$', '')-run$index$([IO.Path]::GetExtension($originalShot))"
            }
            $script:lastRun = @{}
            Write-Host ("[ui] --- прогон {0} из {1} ---" -f $index, $DeterminismRuns)
            $result = Invoke-Smoke
            if ($result -ne 0) { return $result }
            [void]$runs.Add([pscustomobject]@{ Index = $index; Data = $script:lastRun })
        }
    } finally {
        $Shot = $originalShot
    }

    if ($runs.Count -lt 2) { return 0 }

    $first = $runs[0].Data
    $differences = New-Object System.Collections.ArrayList
    foreach ($run in $runs) {
        foreach ($field in $first.Keys) {
            $reference = $first[$field]
            $value = $run.Data[$field]
            if ([string]$reference -ne [string]$value) {
                [void]$differences.Add(("{0}: прогон 1 = '{1}', прогон {2} = '{3}'" -f `
                    $field, $reference, $run.Index, $value))
            }
        }
    }
    if ($differences.Count -gt 0) {
        Write-Host '[ui] ПРОВАЛ: одна и та же команда дала разные числа в прогонах:'
        foreach ($line in $differences) { Write-Host "  $line" }
        Write-Host ("[ui] сравнено полей: {0}, прогонов: {1}. Состояние интерфейса не сброшено?" -f `
            $first.Keys.Count, $runs.Count)
        return 13
    }
    $summary = @()
    foreach ($field in $first.Keys) { $summary += ("{0}={1}" -f $field, $first[$field]) }
    Write-Host ("[ui] детерминизм подтверждён: {0} прогонов одной команды совпали по всем полям ({1})" -f `
        $runs.Count, ($summary -join ', '))
    return 0
}

$themeReady = 0
Save-ThemeState
Save-FontScaleState
if ($Page -ne '') {
    # Страница ставится ДО запуска приложения и снимается ПОСЛЕ его закрытия
    # (Invoke-DeterminismRuns закрывает процесс сам). Причина порядка названа
    # в Restore-AppNavCurrent: приложение пишет nav.current при выходе.
    try {
        Save-NavCurrentState
        Set-AppNavCurrent $Page
        Write-Host ("[ui] страница '{0}': nav.current поставлен, прежнее значение '{1}' (было: {2})" -f `
            $Page, $script:navCurrentSavedValue, $(if ($script:navCurrentSaved) { 'да' } else { 'нет' }))
    } catch {
        Write-Host "[ui] ПРОВАЛ: не удалось выбрать страницу через nav.current: $($_.Exception.Message)"
        $themeReady = 7
    }
}
if ($ThemeMode -ne '') {
    try {
        Set-AppThemeMode $ThemeMode
        Write-Host "[ui] режим темы приложения: $ThemeMode (системную тему не трогаем)"
    } catch {
        Write-Host "[ui] ПРОВАЛ: не удалось подготовить режим темы: $($_.Exception.Message)"
        $themeReady = 7
    }
}
if ($themeReady -eq 0 -and -not $KeepUiState) {
    try {
        Set-AppFontScale $KeepFontScale
        Write-Host "[ui] масштаб шрифта на время прогона: $KeepFontScale% (прежнее значение вернётся)"
    } catch {
        Write-Host "[ui] ПРОВАЛ: не удалось привести масштаб шрифта к известному: $($_.Exception.Message)"
        $themeReady = 7
    }
}

$code = if ($themeReady -eq 0) { Invoke-DeterminismRuns } else { $themeReady }
$restored = Restore-AppThemeMode
$fontRestored = Restore-AppFontScale
$navRestored = Restore-AppNavCurrent
if ($code -eq 0 -and $restored -ne 0) { $code = $restored }
if ($code -eq 0 -and $fontRestored -ne 0) { $code = $fontRestored }
if ($code -eq 0 -and $navRestored -ne 0) { $code = $navRestored }
exit $code
