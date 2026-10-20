# Журнал изменений (Soft_Decoder_V3)

Хронология проекта. Текущая версия — в `firmware/version.txt`;
устройство и архитектура — в `firmware/ARCHITECTURE.md`.

## Сборка

Проект собирается через **ESP-IDF 6.0** (`idf.py`; см. `README.md` и
`firmware/ARCHITECTURE.md` §4). Исторические записи ниже могут упоминать прежнюю
систему сборки.

## Релизы

### 2026-10-20 — усиление защиты звуковых пакетов

- Новый формат `AURAPW02`: scrypt (N=2^13, r=8, p=1) + ChaCha20 +
  HMAC-SHA256 (encrypt-then-MAC), случайные salt 16 Б и nonce 12 Б.
- MAC покрывает заголовок, параметры KDF, salt, nonce и шифротекст; подделка
  и обрезание пакета отклоняются, сравнение MAC constant-time.
- Минимальная длина пароля при экспорте — 8 символов; старый `AURAPW01`
  поддерживается только для импорта с предупреждением.
- Обновлены браузерный экспорт/импорт, `pack_sounds.py`, документация и тесты;
  cross-language тест JS↔Python и проверки tamper/wrong-password/legacy.
- Проверка: 800 C + 75 JS/Python PASS.

### 2026-10-20 — проверка sound editor

- Редактор остаётся полностью встроенным в прошивку (не optional): ассеты
  `blocks.html` и `THIRD_PARTY_NOTICES.txt` отдаются с gzip и корректным MIME.
- Добавлен интеграционный тест реального `sound_editor/dist`: проверяет свежесть
  `build-inputs.json`, наличие всех маршрутов `/sound-editor/*`, бюджет 512 КиБ
  и точное совпадение распакованного gzip с исходным `blocks.html`.
- Редактор проверен в браузере через `web_preview/mock_server.py`: загрузка
  `/sound-editor/blocks.html`, переключение «Миксы/Звуковые схемы», запросы
  `/api/sound/graph/*`, `/api/sound-files`, `/api/func-map`, `/api/aux/cfg` —
  без console errors.
- Итог: 800 C-тестов и 75 JS/Python-тестов PASS.

### 2026-10-20 — оптимизация аудиотракта

- Микшер переведён с `double` на `float`: на ESP32-S3 `double` эмулируется
  программно, теперь расчёт позиции/шага использует аппаратный FPU.
- PCM-голоса читают WAV блоками по 256 кадров в per-voice кэш вместо `fread`
  на каждый сэмпл; стерео читается чанками по 128 кадров.
- ADPCM-кэш (505 сэмплов) стал динамическим: выделяется только у играющего
  ADPCM-голоса и освобождается при закрытии, вместо статических ~20 КБ.
- `VOICE_IO_BUF` уменьшен с 2048 до 1024 байт.
- Горячий компонент `audio` собирается с `-O2` (+1 032 Б flash), остальной
  образ остаётся на `-Os`.
- Итог: DIRAM 145 454 → 125 054 Б (−20 400 Б, −14 %), `libaudio.a` DIRAM
  25 796 → 5 396 Б; flash-код 668 084 → 668 992 Б (+908 Б брутто, +1 032 Б
  к варианту без `-O2`). Тесты: 800 C + 75 JS/Python PASS; сборка ESP-IDF
  успешна.

### 2026-10-20 — очистка мёртвого кода, зависимостей и документации

- Удалены неиспользуемые API: `settings_active_scheme_*`, `sound_engine_is_on`,
  `sound_graph_file_used`, `web_get_function_state`, `web_fs_busy`,
  `audio_voice_alloc/release`, `audio_voice_set_rate/is_active/position`,
  `audio_play/audio_stop`, `auxio_set_output`, `motor_bemf_adc_dump`,
  `motor_set_bemf_max_fraction`, `storage_ext_available`, `settings_func_bind_find`.
- Удалены мёртвые поля Wi-Fi-конфига (`sta_ssid`, `sta_password`, `port`, `hold`)
  и неиспользуемые макросы; AP остаётся единственным режимом.
- Удалены исторические `CODE_REVIEW_REPORT.md`, `FIXES_LOG.md`,
  `SOUND_ENGINE_IMPLEMENTATION.md`, `SOUND_ENGINE_ROADMAP.md`. Актуальные
  ограничения и открытые дефекты перенесены в `firmware/docs/known_issues.md`;
  `BEMF_DIAGNOSTICS.md` переписан под текущую прошивку.
- Удалены все упоминания прежней системы сборки и дублирующие bat-обёртки
  `build_idf`, `flash_idf`, `flash_firmware`, `build_ota_bin`,
  `build_flash_tool_files`. Канонические команды — `idf_build.ps1` и
  `build_firmware.bat`.
- Убраны неиспользуемые REQUIRES: `audio`/`sound`/`storage` — `esp_timer`,
  `storage` — `nvs_flash`, `web` — `esp_driver_gpio`/`pinmap`; добавлены
  недостающие прямые зависимости `track` и `provision`.
- Обновлены `esp_littlefs` 1.22.3 → 1.23.0 и upstream `littlefs` 2.11.3.
- Усилены host-тесты: добавлены проверки ADPCM-воспроизведения, live volume,
  emergency endpoint, graph GET, Wi-Fi fallback; fuzz-тесты проверяют инварианты.
- Проверка: 800 C-тестов и 75 JS/Python-тестов PASS, чистая сборка ESP-IDF
  успешна, покрытие host-строк 92,9 % (8266/8893), ветвей — 76,2 % (6449/8468).
- Добавлены branch-тесты парсеров `ima_adpcm`, `sound_graph`, `settings` и
  error-path `web.c`; CI-workflow запускает host-тесты и ASan+UBSan.

### 2026-10-17 — редактор: два раздела, простой двигатель и вход Старта

- В главном меню два пункта — **Миксы** и **Звуковые схемы**; оба открывают
  один редактор с `?mode=mix` / `?mode=scheme`. В режиме схем палитра содержит
  только `Старт/Простой/Цикл/Конец/Пауза` и общие `Логика/Переменная/Задержка`;
  legacy-пресеты скрыты.
- `Двигатель` теперь всегда активен: без F-клавиши и входного порта, без
  `Margin` и внутренних Start/Idle/Stop/WAV. Остались громкость и количество
  ступеней; каждая ступень — отдельный выход для блоков `Звук`.
- У `Старта` появился входной порт; триггер из Mixes применяется к корневому
  блоку схемы, а не всегда к `Старту`. Цепочка `Пауза → Старт → Цикл` работает;
  проверка недостижимых состояний скорректирована.
- Панель «Переменные», подпись NOT и выравнивание кнопки загрузки — см. запись
  ниже.
- Симулятор доступен в **Миксах**: кнопка «Симулятор» в шапке и ▶ на блоках
  `Звук`. Он компилирует весь микс, показывает F-кнопки, скорость, двигатель,
  каналы, выходы/логику и журнал; проверено на типовом проекте (F2 → Пауза →
  Старт → Цикл).
- Типовой проект: выходы ступеней двигателя подключены к четырём блокам
  `Звук` со схемами `Двигатель 0–25/25–50/50–75/75–100%` (notch-loop,
  удержание по диапазону скорости). Имена WAV приведены к виду, который
  создаёт `pack_sounds.py`; валидатор графа допускает точки в базовом имени
  и UTF-8 (по-прежнему без `/`, `\` и управляющих символов).
- Типовой проект: в звуковых схемах блоки расставлены в ряд с промежутком
  80 px, в Mixes вход и его цель стоят на одной строке, стрелки связей видны;
  цвета блоков теперь различаются по функции (вход/выход/звук/логика/
  переменная/задержка/Старт/Цикл/Конец/двигатель/громкость).
- Пересобраны `sound_editor/dist` и `web_html.h`; 790 C-тестов и 27 JS PASS,
  editor-assets OK, ESP-IDF build SUCCESS (≈1,109 МБ, 44 % OTA свободно).

### 2026-10-17 — редактор: переменные, NOT и выравнивание

- Возвращена панель «Переменные» в правой колонке: `renderVars()` больше не
  мёртвый код, пользовательские переменные можно создавать/переименовывать/
  удалять и задавать `init` без блока «Переменная».
- Флажок NOT в «Логике» вынесен на отдельную строку и подписан; раньше
  чекбокс растягивался CSS и текст уезжал за край инспектора.
- Текст кнопки «Загрузить звуковой пакет» выровнен по центру, как у соседних
  кнопок.
- Пересобраны `sound_editor/dist` и `web_html.h`; JS-тесты 27 PASS,
  editor-assets OK, ESP-IDF build SUCCESS.

### 2026-10-17 — ревизия репозитория и документации

- Удалены пустые scratch-каталоги (`prov_tmp/`, `tm_tmp/`, `web_tmp/`, `TMP/`),
  кэши Python, `firmware/sdkconfig.old` и неиспользуемые артефакты сборки
  редактора (`sound_editor/dist/asset-sizes.json`, `blocks.html.gz`).
- Корневой `.gitignore` игнорирует `*_tmp/` и `/TMP/`: тесты, запущенные из
  корня, больше не оставляют мусор в `git status` (в `firmware/.gitignore`
  такое правило уже было).
- README и `firmware/ARCHITECTURE.md`: актуализирована структура репозитория
  (`docs/`, `sound_editor/`, `web_preview/`, `SOUND/`, полный список `tools/`),
  описан шаг проверки/встраивания офлайн-редактора блоков.
- Повторное код-ревью 2026-10-17: 0 CRITICAL, 1 HIGH (REV-W36 — рассогласование
  политики имён WAV), 4 MEDIUM (W37, S29, W38, W39), 4 LOW; исправления не
  применялись; исторический отчёт доступен в истории git.
- Проверка: host-наборы `ALL TEST SUITES PASSED` (790 C-тестов, 18 наборов),
  `node --test test/test_web_ui.js` — 27 PASS, покрытие host-строк —
  92,8 % (8237/8875), Python-проверки `test_release_artifacts.py` (7) и
  `test_sound_editor_assets.py` — OK, `web_preview/test_mock_pack.py` — PASS.
  Сборка ESP-IDF 6.0 — SUCCESS: `soft_decoder_v3.bin` 1 108 848 Б, свободно
  44 % OTA-слота.

### 2026-10-17 — звуковой редактор: повтор цепочек, циклы и валидация

- Завершённая звуковая цепочка возвращает канал эффекта во вход её корня,
  поэтому повторное нажатие F снова проигрывает `Старт → ... → Конец`
  (раньше канал оставался в последнем состоянии).
- `Цикл «По времени»` больше не сбрасывает таймер при повторе сэмпла: время
  считается от входа в цикл, а не от начала последнего WAV.
- Для `toggle`-входа `Цикл`, `Звонок` и `Компрессор` завершаются повторным
  нажатием F; для `hold` — отпусканием. `Короткое/длинное нажатие` по-прежнему
  различается по отпусканию.
- `Тормоз` с входящей Fx/engine/dir-связью стал управляемым: связь взводит
  блок, звук играет по замедлению; без связи сохранён ambient-режим.
- `trigger=Speed` без входа теперь создаёт эффект; `Random` получил
  завершающее состояние и может продолжать цепочку; `LoopHeld`/`Latched`
  отдают условие завершения следующему блоку.
- Первая ступень двигателя не срабатывает на скорости 0 (idle принадлежит
  холостому ходу).
- Локальная проверка блоков теперь ловит пустые обязательные WAV,
  пустой блок «Звук», недостижимые состояния и предупреждает, если эффектов
  больше 19 (реальных динамических голосов; двигатель использует голос 19).
- При нехватке голоса канал не отключается навсегда, а повторяет захват
  голоса на следующем тике.
- Условия `Логики` теперь переносятся в привязки `Выход`/`Логический выход`:
  до двух сравнений с числом, `AND`/`OR`, операторы `=|!=|<|<=|>|>=` и
  инверсия ветки False. Сравнение с другой переменной (`rhsVar`) и более
  двух условий отклоняются с предупреждением (fail closed).
  `func_eval_ex()` получает значения переменных из активного графа; старый
  `func_eval()` сохранён для совместимости. Условия также восстанавливаются
  при «Импорте выходов» через блоки `Логика`.
- Звуковой двигатель использует голос 19, эффектам доступны голоса 0..18:
  одновременно могут звучать 19 эффектов, а не 18. Каналов графа остаётся 24.
- Симулятор показывает активные выходы/AUX и логические действия, подтягивает
  длительности WAV перед запуском (раньше использовал fallback 1500 мс) и
  добавляет кнопки F для чисто выходных привязок.
- При первом открытии редактор загружает готовый проект `locomotive_default`:
  двигатель с 4 ступенями, **7 именованных звуковых схем** (гудок, звонок,
  компрессор, сцепка, песочница, двери открыть/закрыть), логика Mixes через
  cue-блоки, ambient-тормоз, фары/AUX1..4 и notch up/down. F14 — пример
  условного AUX4 «Вентилятор» (`speed > 20`). Команда
  **Файл → Типовой локомотив** загружает шаблон повторно; **Новый** остаётся
  пустым. WAV-имена соответствуют типовому набору `SOUND`.

### 2026-10-17 — логика, ревизии графов и безопасность загрузки

- `FUNC_TARGET_LOGIC` теперь исполняется: mute stop/move/light, drive hold,
  coast, dynamic brake и notch up/down применяются в `web_apply_function()` и
  `sound.c` (mute, заморозка скорости, idle, сдвиг ±24).
- Графы хранят последние `SG_STORE_KEEP_REVISIONS` ревизий; выбранная ревизия
  сохраняется. Добавлен `POST /api/sound/graph/delete`. Проверка ссылок на WAV
  смотрит latest и выбранную ревизию, поэтому история больше не блокирует
  замену ассетов.
- DCC hot-path больше не читает NVS: треки кэшируются в RAM и обновляются
  после init/upload/delete/pack.
- Громкость effects применяется к уже играющим голосам, а не только к новым.
- `/api/audio/upload?asset=1` загружает файл без привязки к 20 legacy-слотам.
- Pack upload стал транзакционным: staging, backup, откат при ошибке, проверка
  graph-ссылок и сверка метаданных.
- Wi-Fi/DNS вынесены из `web.c` в `components/web/src/web_wifi.c`.
- E-stop больше не зависит от `s_control_mutex`; добавлен неблокирующий
  `web_control_source_is_rails()`.
- WAV upload и combined OTA валидируют staging через `audio_inspect_wav()` и
  снова работают под maintenance.

### Конструктор звуков — подписи на холсте

- `sound_editor/blocks.html`: связи на холсте стало легче выделять — у каждой
  линии невидимая утолщённая область попадания (16 px, `vector-effect`
  сохраняет её на любом зуме); выделенная связь удаляется **Del/Backspace**
  (раньше клик по связи удалял её сразу, теперь — выделяет).

- Фавиконка — большая синяя буква **A** (`#2563eb`) на всех страницах:
  в редакторе (`sound_editor/blocks.html`) и в основном веб-интерфейсе
  (`firmware/web_ui.html`) — встроенный SVG в `data:`-URI; во флешере
  (`web_flasher/favicon.svg`) — тот же SVG файлом.
- `sound_editor/blocks.html`: кнопка **T** на верхней панели (рядом с
  **↶**/**↷**) включает режим подписей: клик по пустому месту холста добавляет
  произвольный текст для подписывания участков схемы. Подпись перетаскивается
  мышью, правится двойным кликом прямо на холсте (`Enter` — применить,
  `Esc` — отменить); в инспекторе задаются текст, размер и цвет; удаление —
  `Del`/`Backspace` или «Удалить». Подписи хранятся в проекте (`texts[]`,
  черновик `localStorage`, экспорт/импорт JSON), участвуют в undo/redo и
  выгружаются в текстовый экспорт (`== Подписи ==`). Старые проекты без
  `texts` загружаются без изменений. Сборка `dist/blocks.html` обновлена.
- `sound_editor/blocks.html`: верхняя панель — меню «Правка» удалено, кнопки
  «Копировать»/«Вставить» убраны (остались только горячие клавиши `Ctrl+C`/
  `Ctrl+V`), а «Отменить» (**↶**) и «Повторить» (**↷**) вынесены отдельными
  кнопками рядом с **T**.
- `sound_editor/blocks.html`: меню «Проверка» («▶ Тест» / «Проверить») удалено —
  проверка выполняется внутри блока (**«▶ Тест блока»** в инспекторе), полная
  валидация запускается при «Сохранить». `refreshButtons` больше не зависит от
  отсутствующей кнопки.
- `sound_editor/blocks.html`: из правой колонки убраны панели **«Переменные»** и
  **«Проверка»** (сводка), а из инспектора — кнопки **«Готово»**/**«Удалить»**
  (у блока остаётся **«▶ Тест блока»** и, для звуковых, **«Проиграть по
  цепочке»**). Проверка по-прежнему в пределах блока; подпись удаляется клавишей
  `Del`, блок — крестиком в его заголовке. `renderSummary`/`renderVars`
  защищены от отсутствующих элементов.
- `sound_editor/blocks.html`: копирование/вставка с клавиатуры
  (`Ctrl+C`/`Ctrl+V`) теперь работает и для **подписей**, и для **блоков** (в т.ч.
  нескольких выделенных). Каждая вставка смещается каскадом; вставленный блок
  получает новую F-клавишу и `id`, подпись — новый `id`. Дублирование
  выделенного — `Ctrl+D` (в том числе для подписи).
- `sound_editor/blocks.html`: горячие клавиши определяются по `event.code`
  (физическая клавиша), а не по `event.key`, поэтому `Ctrl+C/V/Z/Y/D` работают
  на любой раскладке (раньше на русской раскладке `ev.key` давал «с»/«м»/…, и
  шорткаты не срабатывали). Добавлены также `Ctrl+Insert` (копировать) и
  `Shift+Insert` (вставить).

### 0.10 — конструктор блоков Aura X (Миксы / Звуковые схемы)

- `sound_graph`: глобальные переменные (`variables`, до 16; встроенные статусы
  `speed`/`forward`/`moving`/`engine` + пользовательские), действие состояния
  `setVar`/`setValue`, условие `var_cmp` (операторы `=`, `!=`, `<`, `<=`, `>`,
  `>=`, сравнение со значением или второй переменной). Парсер/валидатор/рантайм
  (`sound_graph.c`, `sound_graph_runner.c`) обновлены; схема v1 без переменных
  парсится как раньше. `web`: список условий/лимитов в `/api/sound/graph/state`.
- `sound_editor/blocks.html`: палитра разделена на разделы **Миксы** и
  **Звуковые схемы**; новые блоки `Вход (Fx)` (один выход + выбор режима
  Вкл/выкл, Удержание, Строб),
  `Выход (AUX/Свет)` (объединённый, выбор канала), `Звук`, `Логика`
  (переменные, AND/OR, флаг NOT, отдельные выходы True/False),
  `Установить переменную` (только пользовательские переменные), `Задержка`,
  `Пауза` (чекбокс остановки); порты выходов у блоков, выбор порта на связи;
  панель глобальных переменных; предупреждения об уникальности (F/канал);
  пер-блочная задержка убрана в пользу `Задержка`; компиляция в
  `var_cmp`/`setVar`. Legacy-пресеты скрыты из палитры, но совместимы.
- **Ветвление (fan-out)**: `Вход (Fx)` — источник триггера, каждая его связь
  назначает условие запуска целевому блоку, поэтому один выход Fx запускает
  несколько блоков параллельно (примеры 2–3 макета). Ветвление из выходов
  других блоков не поддержано (предупреждение в проверке).
- Тесты: `test_sound_graph` (переменные, `var_cmp`, `setVar`, встроенные
  статусы, операторы, JSON-отказы).

### 0.9 — звуковой движок (ADDITIPUS AURA-X)

Sound scheme engine, roadmap R0–R6.

- `audio`: playback-rate control (`audio_voice_set_rate`), voice state queries,
  voice allocator, anti-click start/stop fade.
- `settings`: shared `sound_types.h` model, canonical `func_bind` bindings with
  legacy migration, `active_scheme` pointer, manifest v2 (`B;` records),
  CV30/63/114/115/116.
- `sound` (new component): binary `.mds` scheme store (atomic write), engine
  state machine, Init/Loop/End sequencer, chuff/cylinder handling, F-key modes
  (one-shot/trigger/loop-held/short-long/latched), logic mutes, `sync_motion`,
  random/state extras, brake cue, scheme lint.
- `web`: `/api/sound/{state,scheme,table,extra,lint}`, binding API
  (`/api/func-map?view=bind`, `POST ?bind=1`), sound UI panels, `max_uri_handlers`
  raised to 64.
- `motor`: `motor_get_applied_speed()` (ramped output) feeding the engine.
- `main`: `sound_init()` after `audio_init`; sound reset on DCC reset/CV11
  timeout.
- `provision`: HIL commands `HIL-ENGINE`, `HIL-SCHEME-SPEED`.
- Version bumped to 0.9 (CV7 = 9). Existing devices migrate CV7 on first boot.

Host tests: `test/run_tests.ps1` green, `test/coverage.ps1` first-party 100 %.

## Автожурнал (по коммитам)

Записи ниже добавляются автоматически скриптом `.githooks/post-commit`
(`.githooks/changelog.ps1`) при каждом коммите: дата, версия и тема коммита.
Новые — сверху.

<!-- AUTO-LOG -->
- 2026-10-20 (v0.10) — chore(release): rebuild v0.10 artifacts and remove obsolete drawio
- 2026-10-20 (v0.10) — security(pack): replace weak XOR with scrypt+ChaCha20+HMAC
- 2026-10-20 (v0.10) — test(editor): verify embedded sound editor assets and browser flow
- 2026-10-20 (v0.10) — perf(audio): compile mixer hot path with -O2
- 2026-10-20 (v0.10) — perf(audio): block PCM reads, dynamic ADPCM cache and float resampler
- 2026-10-20 (v0.10) — chore(cleanup): remove dead code, update deps/docs and add CI test gates
- 2026-10-20 (v0.10) — feat(sound,web): sound pack import/export, ADPCM compression and scheme editor sync
- 2026-10-19 (v0.10) — feat(sound,sound-editor): brake scheme port, simulator UI, block legends, F-key lock, scheme creation and raised graph limits
- 2026-10-19 (v0.10) — feat(web,sound-editor): flowchart export, UI cleanup, inspector and CV/sound filters
- 2026-10-18 (v0.10) — feat(sound): engine idle output, idle scheme and editor updates
- 2026-10-16 (v0.10) — feat(sound-editor): wider link hit area and Del to delete selected link
- 2026-10-16 (v0.10) — feat(sound-editor): text labels, toolbar cleanup, cross-layout clipboard, favicon
- 2026-10-16 (v0.10) — feat(sound): global variables and var_cmp/setVar in sound graph
- 2026-10-09 (v0.10) — feat(sound): IMA ADPCM sound packs, on-PC preview, editor chain playback
- 2026-10-07 (v0.10) — fix(build): pin LF for hashed sound editor sources
- 2026-10-07 (v0.10) — feat(web): RU/EN localization, header layout, volume block in editor
- 2026-10-05 (v0.10) — feat(web): header page tabs, sidebar uptime, drop F-map/AUX panels
- 2026-10-05 (v0.10) — feat(web): responsive settings master-detail UI and theme polish
- 2026-10-05 (v0.10) — build(release): publish v0.10 firmware with sound block editor and engine updates
- 2026-10-05 (v0.10) — chore: track SOUND test samples and document block editor preview
- 2026-10-05 (v0.10) — feat(sound): move connections scheme/table into toolbar modal, remove from inspector
- 2026-10-05 (v0.10) — feat(sound): detailed scheme PNG export and text logic export
- 2026-10-05 (v0.10) — feat(sound): link-aware auto layout, connections diagram and links table
- 2026-10-05 (v0.10) — style(sound): relabel block output options as Napryamuyu / Posle sobytiya
- 2026-10-05 (v0.10) — feat(sound): full-canvas grid and per-block output event/delay settings
- 2026-10-05 (v0.10) — feat(sound): speed steps recalc on add, default protective interval 5%, drop tone field
- 2026-10-05 (v0.10) — feat(sound): chain links with delay, ready checkmark, engine speed %, output import, mobile layout
- 2026-10-05 (v0.10) — feat(sound): block editor undo/redo, duplicate/copy/paste and F-key conflict warnings
- 2026-10-05 (v0.10) — feat(sound): engine direction/random conditions and effect ducking; editor dir/random blocks
- 2026-10-05 (v0.10) — feat(sound): speed/state triggers, speed bands with protective interval, decel brake
- 2026-10-05 (v0.10) — feat(sound): shared light/dark theme, AUX/headlight defaults 100%, remove duplicate blocks
- 2026-10-05 (v0.10) — style(sound): centered ports, 240x80 blocks, group colors, single light-effect block
- 2026-10-05 (v0.10) — style(sound): grid-aligned 240x120 blocks with ports on grid nodes
- 2026-10-05 (v0.10) — feat(sound): block sound picker, play/test buttons, SOUND folder serving, arrow polish
- 2026-10-05 (v0.10) — style(sound): single block title, trash icon, inline port labels, arrow offset
- 2026-10-05 (v0.10) — feat(sound): block editor wiring, output/light blocks, grid and auto-layout
- 2026-10-05 (v0.10) — feat(sound): add ready-block constructor editor and settings entry
- 2026-10-05 (v0.10) — build(sound-editor): track prebuilt bundle so a fresh clone serves /sound-editor/
- 2026-10-04 (v0.10) — refactor(sound): split sound editor into effect/patch panels and remove legacy sound store
- 2026-10-03 (v0.10) — feat(sound): offline sound graph engine and redesigned decoder UI
- 2026-10-02 (v0.10) — feat(release): verified BIN publishing and web flasher compatibility
- 2026-10-02 (v0.9) — fix: resolve firmware review findings and add safe git sync
- 2026-10-01 (v0.9) — feat(web_flasher): add SVG favicon
- 2026-10-01 (v0.9) — feat(web_flasher): colored log, persistent statistics, auto-stop, adaptive UI
- 2026-09-30 (v0.9) — feat(flasher): reliable esptool flashing, web control panel, cleanup
- 2026-09-30 (v0.9) — fix: resolve review CRITICAL/HIGH (REV-A2/S1/W1/M2/M3)
- 2026-09-30 (v0.9) — docs: verify and fix partition/dependency references
- 2026-09-30 (v0.9) — chore(build): migrate to ESP-IDF 6.0, add VS Code setup
- 2026-09-28 (v0.8) — feat(web): check device online every 5 s; freeze uptime when offline
- 2026-09-27 (v0.8) — docs: refresh stale data; release v0.8
- 2026-09-27 (v0.8) — feat(web): show Additipus as the CV8 manufacturer name
- 2026-09-27 (v0.8) — style(web): place device uptime after the Journal panel
- 2026-09-27 (v0.8) — test(version): guard CV7 against version.txt; harden bump_version.ps1
- 2026-09-27 (v0.8) — chore: bump to 0.8; changelog hook reads firmware/version.txt
- 2026-09-27 (v0.8) — fix(web): valid uptime in /api/device under newlib nano
- 2026-09-27 (v0.7) — feat(web): show device uptime in a fixed bottom bar
- 2026-09-27 (v0.7) — docs: merge TESTS_HANDOFF into ARCHITECTURE; ignore worktrees/generated files; drop VERSION mirror
- 2026-09-27 (v0.7) — chore: prune repo (worktrees, old releases, generated files), merge docs, add README
- 2026-09-27 (v0.7) — fix: code-review remediation across firmware + review agents
- 2026-09-25 (v0.7) — feat: review fixes (ACK/PROV/OTA) + SELFTEST/HIL hardware tests
- 2026-09-25 (v0.7) — docs: exact host test count (427) in ARCHITECTURE/TESTS_HANDOFF
- 2026-09-25 (v0.7) — test: add test_web with host shims; 100% first-party coverage
- 2026-09-25 (v0.7) — docs: add TESTS_HANDOFF.md (coverage status, remaining step 4, gotchas)
- 2026-09-25 (v0.7) — test: add test_provision (BEMF commands, FW upload, PUT flow, listener)
- 2026-09-25 (v0.7) — chore: refresh release artifacts after track_recover refactor
- 2026-09-25 (v0.7) — refactor: extract track recovery into track_recover.c (100% covered)
- 2026-09-25 (v0.7) — test: fuzz/robustness tests (web_util, dcc); optional -Sanitize flags in runners
- 2026-09-25 (v0.7) — test: add test_storage (init/mount/format/free/benchmark); flash+littlefs stubs
- 2026-09-25 (v0.7) — test: dcc.c to ~100% (ISR/task/init/parser edges); bounded task hook; queue stubs
- 2026-09-25 (v0.7) — test: motor.c to ~100% (tick/pid/kickstart/cal-task/info); drop dead guards; bounded task hook
- 2026-09-25 (v0.7) — test: audio.c to ~100% (init/parse/mixer paths); stubs for i2s/task failure
- 2026-09-25 (v0.7) — test: auxio and settings to ~100% coverage
- 2026-09-25 (v0.7) — test: union-per-line coverage; track/pinmap/manifest to 100%
- 2026-09-25 (v0.7) — test: add gcov coverage tooling and a track_manifest suite
- 2026-09-25 (v0.7) — bump_version: also mirror the version into the root VERSION file
- 2026-09-25 (v0.7) — docs: point changelog refs to repo root; note VERSION and git hooks
- 2026-09-25 (v0.7) — Add root VERSION and auto-recorded CHANGELOG via git hook

---

## 2026-09-25 — Ревью архитектуры, safety-фиксы, самотест/HIL

Версия прошивки: **0.7** (`version.txt`). Исторический журнал доступен в истории git.

### Исправления (по итогам ревью)
- **FIX-1**: service-mode ACK вынесен в отдельную задачу `dcc_ack` — задача
  разбора DCC больше не блокируется на 6 мс; пин ACK настраивается один раз.
- **FIX-2**: перед стиранием внешней NOR провижининг требует подтверждение
  (`PROV-CONFIRM?` → `PROV-CONFIRM`), иначе `PROV-ABORT`; скрипты обновлены.
- **FIX-3**: включён OTA rollback (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`),
  образ подтверждается в конце `app_main` (`esp_ota_mark_app_valid_cancel_rollback`).
- **FIX-4**: `functions_get()` читает состояние функций под мьютексом;
  `log_get()` не обрезает JSON внутри записи.
- **FIX-5**: клэмп 14-step скорости до 126.
- Правки по ревью: подтверждение PROV принимает CRLF; очередь ACK не
  освобождается при работающей DCC-задаче; fallback-ACK конфигурирует пин.

### Самотест и HIL
- Новый компонент `selftest`: команда `SELFTEST` (неразрушающие проверки
  heap/pinmap/CV/NVS/LittleFS/ADC/audio/AUX/DCC) и активирующие команды
  `HIL-AUX/HIL-SOUND/HIL-MOTOR/HIL-FN/-SWEEP` (ограничены по времени,
  авто-возврат состояния AUX / стоп звука и мотора).
- `test/hil/run_hil.ps1` (USB: `SELFTEST`, `-Actuate`, `-Sweep`, `-MotorSpeed`)
  и `test/hil/run_hil_web.ps1` (SoftAP: все safe REST-эндпоинты, все F0..F28,
  все AUX, CV и настройки; изменённое возвращает обратно).
- `auxio_get_enabled()` — read-only геттер для самотеста/диагностики.

### Проверено на железе (ESP32-S3)
- USB: `SELFTEST 9/9`, `BEMF-OK`, прогон **всех F0..F28** и **всех 9 AUX**,
  AUX/звук/мотор по командам.
- Web/REST по SoftAP: 45 проверок PASS — все эндпоинты, F0..F28 и AUX0..8
  через веб, AUX cfg, CV write, func-map, аудио, mode, BEMF, control source.
- OTA rollback: после сброса `OTA image confirmed`, образ не откатывается.

### Тесты
- 14 наборов, **453 теста**, покрытие first-party **100 %** (4271/4271 строк).
- Flash 820 629 Б (41.7 %), RAM 50 384 Б (15.4 %).

---

## 2026-09-24 — Разделы, PSRAM, надёжность, манифест метаданных

Версия прошивки: **0.7** (`version.txt`).

### Разметка и память
- OTA-слоты увеличены **896 → 1920 КБ** (`0x1E0000`); `nvs` **24 → 64 КБ**;
  `coredump` 64 → 128 КБ; внутренний раздел `userdata` **удалён**.
- Внутренний fallback-накопитель убран: звуки хранятся только на внешней NOR.
  Устройство работает без внешней памяти (звук отключён): `storage_mount()` не
  фатален, аплоад звука отдаёт `storage unavailable`, провижининг — `PROV-ERR storage`.
- Включена **PSRAM 2 МБ** (quad, 80 МГц, как heap): `CONFIG_SPIRAM=y`.
- Отключён **IPv6** в lwIP (softAP — только IPv4): экономия ~10–15 КБ flash.
- Убран `powf()` (гамма AUX — предвычисленная таблица), снята зависимость от libm.

### Сеть и журнал
- **Канал AP выбирается до старта** точки: скан в режиме STA → `esp_wifi_stop` →
  AP поднимается один раз на выбранном канале 1/6/11. Раньше канал менялся уже
  после старта, сбрасывая подключающихся клиентов (задержка выдачи IP).
- Журнал: буфер тега 12 → 24 Б и **обрезка по границе UTF-8** (устранена
  «кракозябра» в тегах вроде «Источник»); убраны отладочные записи на старте.
- «(мотор остановлен)» показывается только если мотор был в движении.

### Метаданные звуков
- Добавлен **манифест** `/userdata/audio/tracks.txt`: список слотов (файл, метка,
  категория, вкл) + карта F↔AUX. Пишется при любом изменении метаданных, читается
  при пустом NVS — имена/категории/карта переживают полный сброс.
- Прошивка/скрипты: полная очистка чипа теперь **опциональна**
  (`flash_firmware.bat erase`), по умолчанию NVS/настройки сохраняются; README
  Flash Download Tool генерируется из `partitions.csv`; standalone-флешер берёт
  смещения `ota_0`/`otadata` из `partitions.csv`.

Изменённые области: `partitions.csv`, `sdkconfig.defaults`, `components/storage`,
`components/web`, `components/settings` (+ `track_manifest.c`), `components/auxio`,
`components/provision`, `main/app_main.c`, `*.bat`/`*.ps1`, `test/test_settings`.

---

## 2026-09-18 — BUGFIX — нестабильное Wi-Fi соединение (Soft_Decoder_V3)


Область: стабильность Wi-Fi/веб-соединения (AP + HTTP + настройки).
Дата: 2026-09-18.

Результат проверки после правок:
- Сборка прошивки — **SUCCESS** (RAM 13.8%, Flash 89.7%).
- Нативные тесты (`test/run_tests.ps1`) — **все наборы PASSED** (в т.ч. `test_settings` 32/32, `test_web_util` 23/23).

Ниже — список найденных проблем и что сделано по каждой.

---

### 1. Запись в NVS из горячих путей (стопор радио) — ИСПРАВЛЕНО

**Проблема.** Каждое действие UI (движение слайдера громкости, выбор слота, play)
вызывало `settings_save()` → `nvs_commit()` (`components/settings/src/settings.c:160`).
Коммит NVS = программирование/стирание флеша, на время которого падает кэш
инструкций и Wi-Fi MAC не успевает обслуживаться → периодические «залипания» и
пропадание страницы.

**Исправление.**
- Добавлен отложенный коммит: `settings_save_deferred()` и `settings_pending_flush()`
  (`settings.c`). Значения складываются в кэш NVS, а `nvs_commit()` выполняется
  после паузы 2 с без изменений.
- `settings_pending_flush()` вызывается из `safety_task` каждые 50 мс
  (`main/app_main.c`).
- На отложенную запись переведены громкость (`audio_volume_post`) и активный слот
  (`audio_play_post`) в `components/web/src/web.c`.
- Критичные настройки (Wi-Fi, имя, источник управления) по-прежнему коммитятся сразу.

Файлы: `components/settings/include/settings.h`, `components/settings/src/settings.c`,
`components/settings/CMakeLists.txt`, `main/app_main.c`, `components/web/src/web.c`.

---

### 2. Устаревший `wifi_mode` (STA / AP+STA) — ИСПРАВЛЕНО

**Проблема.** `wifi_start()` доверял сохранённому `cfg->wifi_mode` (`web.c`). При
оставшемся в NVS режиме `2` (STA) AP не поднимался, при `3` (AP+STA) один
радиомодуль делил время между beacon AP и сканами STA → провалы соединения.

**Исправление.**
- `wifi_start()` теперь всегда поднимает **только точку доступа**: сохранённый
  STA/AP+STA игнорируется, STA-интерфейс используется лишь секунду для скана
  канала и затем отключается.
- `wifi_post()` принудительно ставит `wifi_mode = 1` и не даёт переключить режим
  устаревшим скриптом/страницей.
- Драйверу запрещена запись конфига в NVS: `esp_wifi_set_storage(WIFI_STORAGE_RAM)`.

Файлы: `components/web/src/web.c`.

---

### 3. HTTP-слой: сокеты, таймауты, порт — ИСПРАВЛЕНО

**Проблема.** `max_open_sockets = 6` при опросе статуса каждые 3 с + загрузке +
порте 81; `recv/send_wait_timeout = 60` — «зависший» клиент держал сокет минуту,
`lru_purge` рвал соединения (в UI это выглядело как «Офлайн»). Плюс сервер слушал
`s_cfg.port`, а captive-редирект указывал на порт 80 без порта.

**Исправление.**
- Основной сервер: `max_open_sockets` 6 → 8, `recv_wait_timeout` 60 → 15,
  `send_wait_timeout` 60 → 30 (`start_http_server`).
- Progress-сервер (порт 81): `max_open_sockets` 2 → 3, таймауты 10 с.
- HTTP-порт жёстко зафиксирован на 80 (`start_http_server`), как и в веб-UI.

Файлы: `components/web/src/web.c`.

---

### 4. Канал AP = 1, нет выбора канала — ЧАСТИЧНО ИСПРАВЛЕНО

**Проблема.** `ap.ap.channel = 1` жёстко; канал 1 обычно самый зашумлённый.

**Исправление.** При загрузке выполняется короткий скан и выбирается наименее
загруженный из неперекрывающихся каналов **1/6/11** (`wifi_pick_channel()`);
при любой ошибке скана — откат на канал 1 (как раньше).

**Не сделано:** `esp_wifi_set_country()` (Country Code) — сознательно не трогали,
это регуляторное решение, требует подтверждения региона.

Файлы: `components/web/src/web.c`.

---

### 5. Нет обработки Wi-Fi-событий и реального IP — ИСПРАВЛЕНО

**Проблема.** Не было ни одного `esp_event_handler` — отвал клиента/AP был невидим,
а `s_ap_ip` был захардкожен, нельзя было отделить RF-проблему от HTTP.

**Исправление.** Добавлен `wifi_event_handler` (`web.c`):
- логи `AP started/stopped`;
- лог подключения/отключения станции с MAC, `aid`, `reason` и числом клиентов;
- лог выдачи IP (`IP_EVENT_AP_STAIPASSIGNED`);
- `s_ap_ip` берётся из `esp_netif_get_ip_info()`.

Файлы: `components/web/src/web.c`.

---

### 6. `hold` и `auto_off_min` не были реализованы — ИСПРАВЛЕНО

**Проблема.** `hold` (`settings.h`: «answer captive probes with 204») и
`auto_off_min` только хранились в NVS и ни на что не влияли.

**Исправление.**
- `captive_handler`: при `hold` неизвестные пути отдают **204 No Content** (телефон
  считает, что интернет есть, и не переключается на другую сеть); иначе — прежний
  302-редирект на страницу.
- Автовыключение: задача `wifi_auto_off_task` останавливает AP, если клиентов не
  было `auto_off_min` минут (`esp_wifi_stop()`), затем завершается. Отключается
  значением 0 (по умолчанию).

Уточнение (обновлено): captive-портал всегда отдаёт 302 на страницу, поэтому
браузер телефона открывает её автоматически при подключении к AP. `hold`
больше не используется и сохраняется как 0. Адрес AP настраивается (см. ниже),
по умолчанию `192.168.100.1`.

Файлы: `components/web/src/web.c`.

---

### 7. Открытая сеть и короткий пароль — ИСПРАВЛЕНО

**Проблема.** Пароль короче 8 символов молча игнорировался, и AP оставался
открытым (`WIFI_AUTH_OPEN`), о чём пользователь не знал.

**Исправление.**
- `wifi_post`: пароль 1..7 символов теперь отклоняется с ошибкой
  (`{"ok":false,"error":"ap password must be >=8 chars"}`), сохранения/ребута нет.
- `wifi_start`: если по какой-то причине пришёл короткий пароль — в лог пишется
  предупреждение, что AP остаётся открытым.
- Чекбокс «Убрать пароль точки доступа» убран: пустое поле пароля при сохранении
  означает «без пароля» (UI отправляет `ap_password_clear=1`), заполненное —
  новый пароль.

Файлы: `components/web/src/web.c`.

---

### 8. Частота CPU и `double` в аудиомикшере — НЕ ИСПРАВЛЕНО (по требованию)

Частота процессора **оставлена 160 МГц** — повышение до 240 МГц увеличивает
потребление и нагрев. Оптимизация микшера (`double` → `float`, чтение сэмплов
блоками, `components/audio/src/audio.c`) не выполнялась: это не причина обрывов
Wi-Fi и несёт риск регрессии звука, который нельзя проверить автотестами.

---

### 9. Питание / brownout — НЕ ИСПРАВЛЕНО (аппаратная часть)

Детектор brownout включён на уровне 7. Если в логе после «глюка» виден
`BROWNOUT` в `esp_reset_reason()`, причина в просадке питания (мотор + I2S +
радио на 20 dBm), а не в прошивке. Программных правок нет; при необходимости —
снижение `CONFIG_ESP_PHY_MAX_WIFI_TX_POWER` или доработка питания.

---

### 10. Настраиваемый IP точки доступа и автооткрытие страницы — ДОБАВЛЕНО

- В UI панели Wi-Fi убрана строка статуса («Режим…», SSID, `ap_running`);
  остался один редактируемый адрес AP. Значение по умолчанию — `192.168.100.1`.
- Адрес хранится в `settings_config_t.ap_ip` (`SETTINGS_IP_MAX`) и применяется
  при старте AP: netif IP/gw, DHCP-DNS, captive-редирект и DNS-хайджек
  (`dns_build_response` теперь принимает IP параметром).
- Невалидный адрес при сохранении отклоняется (`invalid ap ip`).
- Автооткрытие страницы: captive-портал всегда отвечает 302 на страницу AP,
  поэтому телефон открывает её сам при подключении.

---

### Дополнительно исправлено после ревью

- **Автовыключение AP (п.6):** отсчёт ведётся от момента последнего
  подключения/отключения клиента (`s_last_client_us`), а не от старта AP —
  иначе после долгого сеанса AP выключался сразу после ухода телефона.
- **`wifi_post` (п.7):** параметры разбираются в локальную копию и применяются
  только после успешной валидации, поэтому отклонённый (короткий) пароль больше
  не может частично уйти в NVS отложенным коммитом.
- **Выбор канала (п.4):** при раннем выходе скана список AP освобождается через
  `esp_wifi_clear_ap_list()`.
- **Мёртвый код:** удалён разбор `sta_ssid`/`sta_password` из `wifi_post` —
  после перехода на AP-only эти поля нигде не используются.

### Изменённые файлы

- `components/settings/include/settings.h`
- `components/settings/src/settings.c`
- `components/settings/CMakeLists.txt`
- `main/app_main.c`
- `components/web/src/web.c`

### Что проверить на железе (не автотестируемо)

1. Автовыбор канала AP (скан при загрузке, откат на канал 1 при ошибке).
2. Автовыключение AP через 3 минуты без клиентов (и что при этом поведении
   дальше — только передёргивание питания).
3. `hold = 1` → телефон не уходит с сети, страница открывается вручную.
4. Отсутствие `BROWNOUT` при старте мотора и всплесках TX.

---

## 2026-09-14 — Журнал исправлений (Soft_Decoder_V3)


Дата: 2026-09-14
Проект: DCC-декодер (ESP32-S3), прошивка `firmware/`

Статус проверки на момент записи:
- Сборка прошивки — **SUCCESS**.
- Нативные тесты — **35/35 PASSED** (`test_dcc` 25, `test_settings` 10).

---

### 1. F0: светодиоды моргали при включении

**Проблема.** При включении функции F0 из DCC передние светодиоды мигали, управление скоростью работало некорректно.

**Причина.** В `main/app_main.c` (`update_decoder_address`) бит CV29 трактовался инвертированно:
`bool mode_14 = (cv29 & 0x02U) != 0;`. По NMRA бит 1 CV29: `0` = 14 шагов, `1` = 28/128 шагов.
Значение по умолчанию `CV29 = 0x02`, поэтому декодер ошибочно включал 14-шаговый разбор.
В 14-шаговой ветке `dcc.c` бит 4 инструкции скорости трактуется как F0, а пульт шлёт 28-шаговые пакеты,
где бит 4 — младший бит кода скорости. В результате F0 дёргался при каждом обновлении скорости.

**Исправление.** Полярность исправлена: `bool mode_14 = (cv29 & 0x02U) == 0;`.

**Файлы.** `main/app_main.c`.

---

### 2. Мотор: кривая скорости (переполнение unsigned)

**Проблема.** При немонотонных CV2/CV5/CV6 (например, Vmax < Vmid) ШИМ уходил в мусорное огромное значение — рывок мотора.

**Причина.** В `components/motor/src/motor.c` (`speed_duty`) разности `(vm - vs)`, `(vh - vm)` — беззнаковые;
при немонотонности они переполнялись.

**Исправление.** Введён принудительный порядок `Vstart <= Vmid <= Vhigh` до интерполяции.

**Файлы.** `components/motor/src/motor.c`.

---

### 3. Мотор: калибровка BEMF не использовалась

**Проблема.** Кривая калибровки BEMF вычислялась, сохранялась и отдавалась в API, но на регулятор не влияла.

**Причина.** В PID-цели использовались фиксированные `BEMF_TARGET_*`, а таблица `s_cal_frac_table` читалась только для флага валидности.

**Исправление.** PID-цель теперь берётся из калибровочной кривой `s_cal_frac_table[applied]` при валидной калибровке, иначе — линейный фолбэк.

**Файлы.** `components/motor/src/motor.c`.

**Риск.** Меняет динамику на низких ступенях — требуется проверка на железе.

---

### 4. Мотор: «подтормаживание и выход» при смене скорости

**Проблема.** При изменении скорости с крутилки ощущался шаг: мотор «притормаживал, потом выходил» на новую скорость; уменьшение скорости было слишком резким.

**Причина.** BEMF-PID сбрасывал интегратор на каждой смене применённой ступени
(`if (applied != prev_applied) { s_pid_integral = 0; s_pid_prev_error = 0; }`), из-за чего ШИМ сначала проваливался, затем снова набирался.

**Исправление.** Сброс интегратора при смене ступени убран (сброс остался только при полной остановке). Инерция CV3/CV4 снова работает как ожидается.

**Файлы.** `components/motor/src/motor.c`.

**Риск.** Возможен перебег на больших скачках уставки — проверить на железе.

---

### 5. Веб: переполнение буфера в DNS-хайджеке

**Проблема.** При DNS-запросе почти в 512 байт ответ писался за пределы буфера (до 16 байт за границей стека).

**Причина.** `dns_build_response` (`components/web/src/web.c`) копировал вопрос и добавлял 16 байт ответа без проверки итогового размера; дополнительно выставлялся неверный `NSCOUNT=1`.

**Исправление.** Добавлена проверка `qend + 16 > DNS_MSG_MAX → отклонение запроса`; `NSCOUNT = 0`; буферы задач переведены на `DNS_MSG_MAX`. Введён макрос `DNS_MSG_MAX`.

**Файлы.** `components/web/src/web.c`.

---

### 6. Веб: use-after-free / зависимость в конвейере загрузки

**Проблема.** При таймауте ожидания writer-задачи `pipe_upload` освобождал буферы и очереди, которые writer мог ещё использовать (use-after-free).

**Причина.** `xSemaphoreTake(done_sem, 30 c)` при таймауте продолжал освобождать ресурсы.

**Исправление.** Ожидание стало безусловным (`portMAX_DELAY`), плюс writer теперь завершается гарантированно:
- добавлен `volatile bool abort` в `pipe_ctx_t`;
- writer ждёт очередь с таймаутом `PIPE_WR_POLL_MS` (250 мс) и выходит по флагу `abort`;
- если маркер `last` не удалось поставить в очередь за 30 с, `pipe_upload` выставляет `abort=true`.

**Файлы.** `components/web/src/web.c`.

---

### 7. Веб: усечение порта при сохранении Wi-Fi

**Проблема.** Порт парсился с приведением к `uint16_t` до проверки диапазона (значения > 65535 усекались), плюс предупреждение компилятора `-Wtype-limits`.

**Исправление.** Разбор в `long`, проверка `1..65535`, затем приведение к `uint16_t`.

**Файлы.** `components/web/src/web.c` (`wifi_post`).

---

### 8. Громкости «Двигатель»/«Эффекты» не работали

**Проблема.** Слайдеры engine/effects сохранялись и отдавались в API, но к звуку не применялись: звуки функций играли с фиксированной громкостью `100`.

**Причина.** Не было потребителя `engine_volume`/`effects_volume` и категоризации слотов.

**Исправление.**
- Добавлен API `web_get_voice_volume(fn)` (`components/web/include/web.h`, `web.c`).
- Звуки функций (`main/app_main.c`, `on_dcc_function`) и превью слотов (`web.c`, `audio_play_post`) используют эту громкость.
- Введена покатегорийная привязка слота: `SETTINGS_TRACK_CAT_ENGINE` / `SETTINGS_TRACK_CAT_EFFECTS` (`components/settings/include/settings.h`).
- Категории хранятся в **отдельном ключе NVS `track_cat`** (`settings_track_cats_load/save`, `components/settings/src/settings.c`) — формат блоба `tracks` не меняется.
- Дефолт: слот 1 = «двигатель», слоты 2..20 = «эффекты».
- Добавлен роут `POST /api/track/category?slot=&cat=`; категории отдаются в `/api/audio/tracks` полем `cats`.
- UI (`web_ui.html`): кнопка «ДВ»/«ЭФ» у каждого слота, `toggleSlotCat()`.

---

### 9. Аудит маппинга CV: несоответствие прошивки и веб-UI

**Проблема.** Веб-UI показывал CV, которые прошивка не читает, и не показывал реально используемые.

**Что сделано.**
- **Исправлены CV регулятора:** UI показывал `113/114/115`, прошивка читает `54/55/56` → UI переведён на `54/55/56`.
- **Убраны фиктивные CV:** `30`, `31`, `32`, `33..46` (маппинг функций зашит в коде), `257..261` (Wi-Fi хранится не в CV).
- **Добавлены реально используемые:** `CV11` (таймаут пакетов) и `CV67..CV94` (таблица скоростей, генерируются циклом в UI).
- **CV8** сделан записываемым (значение `8` = заводской сброс), `CV7` — только чтение (как в прошивке).
- Обновлены названия/описания (CV2 = 24 по умолчанию, CV3/CV4 — время в секундах, расшифровка битов CV29) и «Избранное».
- Убраны устаревшие комментарии в `settings.h`, приписывавшие `hold`/`auto_off_min` номера CV259/CV260.

**Итоговый набор CV = прошивка:** `1,2,3,4,5,6,7,8,11,17,18,19,29,54,55,56,65,67..94`.

**Файлы.** `web_ui.html`, `components/settings/include/settings.h`.

---

### 10. Веб-запись адресных CV не применялась без перезагрузки

**Проблема.** Запись CV1/17/18/19/29 через веб-UI меняла NVS, но рантайм-конфиг декодера (адрес, режим шагов, состав) не обновлялся до перезагрузки.

**Исправление.**
- В компонент `dcc` добавлена `dcc_reload_config()` (`components/dcc/include/dcc.h`, `src/dcc.c`): читает CV через зарегистрированный колбэк и применяет адрес/режим/состав. `main/app_main.c` теперь использует её.
- `components/web/src/web.c` (`cv_write_post`) вызывает `dcc_reload_config()` для `1/8/17/18/19/29`.
- Компонент `web` получил зависимость `dcc` (`components/web/CMakeLists.txt`).

---

### 11. Веб-запись CV: ошибки игнорировались

**Проблема.** `cv_write_post` не проверял результат `settings_cv_write` и всегда отвечал «OK».

**Исправление.** Ошибка возвращается как `{"ok":false,"error":"..."}`; UI (`writeCv`) показывает причину. Также `value` стал обязательным параметром.

**Файлы.** `components/web/src/web.c`, `web_ui.html`.

---

### 12. Итоговое ревью изменений (найденные и устранённые замечания)

**M1. CV8 не обновлял рантайм.** Заводской сброс через CV8=8 не перечитывал конфиг. → В условие `cv_write_post` добавлен `idx == 8`.

**M2. Лимит маршрутов исчерпан.** `max_uri_handlers` было `32` при 32 же маршрутах; новые регистрировались молча-неудачно. → Поднято до `40`, `register_route` логирует ошибку регистрации.

**M3. Возможный вечный hang в `pipe_upload`.** См. п.6 (bounded receive + `abort`).

**L1. Переполнение при сборке JSON.** Паттерн `used += snprintf(json+used, sizeof(json)-used, ...)` при переполнении давал underflow `sizeof - used`. → Добавлен `buf_appendf(buf, cap, &used, fmt, ...)`, `audio_tracks_get` переведён на него.

**L2. Гонка при обновлении рантайм-конфига.** → В `dcc_reload_config` публикация адреса/режима/состава выполняется в критической секции (`portMUX_TYPE s_cfg_mux` + `portENTER_CRITICAL`/`portEXIT_CRITICAL`).

**L3. Неверный дефолт категорий до загрузки.** До `web_init` массив нулевой (=«двигатель» для всех слотов). → Добавлен флаг `s_track_cat_loaded`; до загрузки используется дефолтная раскладка.

**L4. Молчаливый сброс через CV8.** → UI запрашивает подтверждение перед записью CV8 = 8.

**Файлы.** `components/web/src/web.c`, `components/dcc/src/dcc.c`, `web_ui.html`.

---

### 13. Инфраструктура тестов (native)

**Проблема.** Нативные тесты не запускались.

**Причины и решения.**
- **Нет host-компилятора** → установлен GCC 15.2.0 (`scoop install gcc`).
- **`InvalidSchema: Missing dependencies for SOCKS support`** — в системе SOCKS-прокси, в Python venv не было PySocks → установлен `pysocks` в venv (офлайн-колесо).
- **`-Dstatic=` ломал `static inline` в заголовках MinGW** (`multiple definition of sscanf/printf`) → флаг убран из конфигурации сборки; снятие `static` ограничено белым ящиком (`#define static` / `#undef static` вокруг `#include .../dcc.c` и `.../settings.c` в тестах).
- В `test_libs/teststubs/include/freertos/FreeRTOS.h` добавлены no-op `portMUX_TYPE`, `portENTER_CRITICAL`, `portEXIT_CRITICAL` для сборки `dcc_reload_config` в тестах.

**Файлы.** `test/test_dcc/test_dcc.c`, `test/test_settings/test_settings.c`, `test_libs/teststubs/include/freertos/FreeRTOS.h`.

---

### 14. Требует проверки на железе (не автотестируемо)

1. Поведение BEMF-PID с калибровочной кривой на самых низких ступенях (возможен «недопривод»).
2. Отсутствие сброса интегратора PID при смене уставки (возможен перебег на больших скачках).
3. Настройка инерции: CV3/CV4 = секунды на полный диапазон 0→max; при CV=0 — без инерции.
4. Низкая скорость: регулируется CV2 (по умолчанию 24 ≈ 9.4% ШИМ); при трудном старте — кикстарт CV65.

---

### Сводка изменённых файлов

- `main/app_main.c`
- `components/dcc/include/dcc.h`, `components/dcc/src/dcc.c`
- `components/motor/src/motor.c`
- `components/web/include/web.h`, `components/web/src/web.c`, `components/web/CMakeLists.txt`
- `components/settings/include/settings.h`, `components/settings/src/settings.c`
- `web_ui.html` (генерирует `components/web/include/web_html.h`)
- `test/test_dcc/test_dcc.c`, `test/test_settings/test_settings.c`
- `test_libs/teststubs/include/freertos/FreeRTOS.h`

---

## 2026-08-27 — Отчёт о доработке прошивки Soft_Decoder_V3


Дата: 27.08.2026

Цель: привести прошивку к полному рабочему состоянию — корректный приём DCC
«как в V0», полный доступ к CV, управление мотором с BEMF-PID, DC-режим,
функции F0–F28, защита по току (аппаратная), таймаут потери сигнала,
контрольная сумма CV и сервисные входы. Прошивка остаётся минимальной.

Сборка: `idf.py build` → SUCCESS (чистая сборка).
RAM 15.8% (51 908 / 327 680 байт), Flash 83.7% (767 665 / 917 504 байт).

---

### 1. DCC: порядок бит (последовательность на рельсе)

`components/dcc/src/dcc.c` — байт собирается MSB-first (командная станция
передаёт старшим битом вперёд; при LSB-first адрес 3 читался как 0xC0).

### 2. DCC: тайминги и фильтр глитчей

Один порог 82 мкс (без мёртвой зоны), глитч < 35 мкс, максимум 292 мкс.
В ISR интервал короче 35 мкс игнорируется без сдвига опорного времени.

### 3. DCC: полный доступ к CV + consist

- Service Mode (Direct): Write 0x7C + ACK, Verify 0x74 (ACK при совпадении),
  Bit Manipulation 0x78 (111 K D BBB).
- Ops Mode: Write CC=11, Bit Manipulation CC=10, Verify CC=01 (без ACK),
  короткая форма CV23/CV24 (0xF2/0xF3).
- Consist CV19 (адрес состава + автореверс направления).
- Колбэк `dcc_register_cv_read_cb` для чтения CV.

### 4. Мотор: кривая, разгон/торможение, kickstart, BEMF-PID

`components/motor/src/motor.c` (переписан).

- `motor_set_speed()` задаёт целевую скорость; задача `motor` (10 мс) плавно
  подводит фактическую.
- Разгон/торможение CV3/CV4 (0 = мгновенно), целочисленное накопление.
- Кривая: таблица CV67–94 (CV29 бит 4) или Vstart/Vmid/Vhigh (CV2/5/6) → ШИМ 0..1023.
- Kickstart CV65 (буст 120 мс).
- **BEMF-PID** (замкнутый контур, как в V0):
  - окно измерения: на 400 мкс мост переводится в HiZ, считываются ADC1
    BEMF1 (GPIO4)/BEMF2 (GPIO5), затем ШИМ восстанавливается;
  - ПИД Kp/Ki/Kd из CV54/55/56 (по умолчанию 47/17/32 → 0.45/0.02/0.10);
  - целевое значение ПДС = (скорость/126)·(rail·0.70); отбраковка отсчётов в
    свободном выбеге (bemf_hi > 0.85·rail);
  - защита: интеграл ограничен ±20000, ШИМ не опускается ниже 50 % кривой,
    при недостоверной выборке интеграл затухает, при отсутствии BEMF ПИД не
    включается (разомкнутый контур) — исключён разгон мотора на полную.
- `motor_set_rail_voltage_mv()` + `motor_bemf_lock()/unlock()` — общий ADC1
  между задачей мотора и задачей трека.

### 5. DC-режим (аналог) — по умолчанию DCC

`components/track` — бит 2 CV29 включает DC. ADC1 (GPIO6, RAIL_SENSE) всегда
измеряет напряжение рельс (питает BEMF-PID) и в DC-режиме задаёт скорость;
направление по уровню DCC_IN (GPIO9) с дебаунсом. Переключение — через CV29
или веб (`GET/POST /api/mode`, панель «Режим работы»).

### 6. Функции F0–F28 + направленный свет

`components/web/src/web.c` — 29 состояний функций. F0 — направленный свет
(F0F вперёд / F0R назад по направлению мотора), F1–F7 → AUX1–AUX7. Задача
`outputs` (20 мс) применяет состояния к выходам.

### 7. Надёжность и защита

- **Контрольная сумма CV** (`settings.c`, FNV-1a): при повреждении хранилища
  в NVS декодер восстанавливает заводские CV.
- **Сброс к заводским** — запись 8 в CV8 (NMRA) или `/api/cv/write`.
- **CV11 таймаут** — при отсутствии адресованных пакетов (значение × 20 мс)
  мотор останавливается и функции гасятся.
- **Полный диапазон CV** — веб-редактор CV теперь адресует CV 1..512 (uint16).
- **PIN_CURRENT_SENSE (GPIO7)** — аппаратная защита (вход компаратора), кодом
  не читается.

### 8. Аудио: многоголосый микшер + привязка звуков к функциям DCC

`components/audio/src/audio.c` переписан на многоголосый микшер:

- Один канал I2S на фиксированной частоте 22050 Гц моно; до 20 голосов.
- Каждый голос читает свой WAV (моно/стерео, любая частота) и линейно
  ресемплируется в частоту микшера, громкости голоса и мастер складываются,
  сумма ограничивается (анти-клиппинг).
- Открытие/закрытие файлов выполняется только в задаче микшера через очереди
  запросов под мьютексом — безопасно вызывать `audio_voice_play/stop` из любой
  задачи (в т.ч. из DCC-парсера).
- Привязка: F1–F20 ↔ голоса 0–19 ↔ слоты 1–20; функция вкл → зацикленное
  воспроизведение слота, выкл → остановка голоса (`app_main.c`).
- Совместимость: `audio_play()/audio_stop()` оставлены как обёртки для веба.

### 9. Свет: ШИМ-диммирование и эффекты (мигание/плавное)

Новый компонент `components/auxio` — 9 выходов AUX с аппаратным ШИМ:

- Первые 6 (F0F, F0R, AUX1–AUX4) на свободных каналах LEDC 2–7 (таймер 1,
  8 бит, 20 кГц); мотор остаётся на LEDC 0–1 (не тронут).
- AUX5–AUX7 на MCPWM_UNIT_0 (3 оператора) — RMT на ESP32-S3 имеет только
  4 TX-канала, поэтому для последних трёх выходов использован MCPWM.
- Эффекты: steady / blink / breath (`auxio_set_effect`), задача `aux_fx`
  (20 мс) пересчитывает скважность.
- Привязка: F0 — направленный свет (F0F/F0R по направлению мотора),
  F1–F7 → AUX1–AUX7 через `auxio_set_enabled` (состояние функции не
  затирает настроенный эффект).
- Конфигурация эффекта: веб-endpoint `POST /api/aux/effect`
  (ch, on, pwm_on, pwm_off, mode, period).

### 10. Что намеренно НЕ делалось

- Статусный светодиод — вывод занят (общий с UART0 TX, GPIO43); требуется
  решение по пину.
- Indexed CV (страницы CV31/32 сверх 512) — плоское хранилище 512 CV уже
  покрывает CV257–512 напрямую.
- mDNS, USB MSC, PCA9685, Secure Boot — вне рамок (по анализу отложены).

### 11. Тесты (перенесены из V0 и проверены)

Тесты V0 (test_dcc, test_settings, test_funcmap, test_web, test_zip) разобраны.
`test_funcmap`/`test_web`/`test_zip` привязаны к компонентам V0 (funcmap,
settings.json-парсер, zip_extract), которых в V3 нет — не переносились.
`test_dcc` и `test_settings` переписаны под внутреннее устройство V3.

Инфраструктура (host-тесты на PC, без железа):

- `test_libs/teststubs/` — заглушки ESP-IDF (gpio, freertos, esp_timer,
  esp_log, nvs) + `stubs.c`.
- `test/test_dcc/test_dcc.c` — 25 тестов декодера DCC (классификация
  полупериодов, порядок бит MSB-first, контрольная сумма, 14/28/128 скорость,
  функции F0–F28, длинные адреса, consist, Service Mode write/verify/bit,
  Ops Mode, broadcast reset/estop, глитчи, сквозной конвейер полупериодов).
- `test/test_settings/test_settings.c` — 8 тестов CV-хранилища (FNV-1a CRC,
  заводские по умолчанию, read-only CV7, factory reset через CV8=8, границы).
- `test/run_tests.ps1` — запуск через TCC + Unity (как в V0).
- нативные тесты запускаются через `test/run_tests.ps1`.

Запуск:

```
powershell -ExecutionPolicy Bypass -File test\run_tests.ps1
```

Результат: **33 теста, 0 ошибок** (25 DCC + 8 settings).

Тесты выявили и подтвердили **реальный баг** в `dcc.c`: для Service Mode
пакета с адресом 0xFF извлечение адреса считало его длинным (бит 7 установлен)
и читало инструкцию не с того смещения — запись/проверка/бит-операции CV в
Service Mode не работали. Исправлено: адрес 0xFF / 112..126 в Service Mode
никогда не трактуется как длинный.

### Изменённые/созданные файлы

- `components/dcc/{src/dcc.c, include/dcc.h}`
- `components/motor/{src/motor.c, include/motor.h, CMakeLists.txt}`
- `components/track/{src/track.c, CMakeLists.txt}` (создан ранее)
- `components/audio/{src/audio.c, include/audio.h}`
- `components/auxio/{src/auxio.c, include/auxio.h, CMakeLists.txt}` (новый)
- `components/settings/{src/settings.c, include/settings.h}`
- `components/web/{src/web.c, CMakeLists.txt}` и `web_ui.html`
- `components/pinmap/{include/pinmap.h, src/pinmap.c}`
- `main/{app_main.c, CMakeLists.txt}`
- `components/provision/{src/provision.c, include/provision.h, CMakeLists.txt}` (новый)
- `test/test_dcc/test_dcc.c`, `test/test_settings/test_settings.c` (новые)
- `test/run_tests.ps1`, `test_libs/teststubs/**` (новые)
- `flash_uart.bat`, `build_ota_bin.bat`, `build_flash_tool_files.bat`,
  `flash_firmware_and_sounds.bat`, `provision_sounds.ps1` (новые)

### 12а. Исправление: зависание декодера при включении света (28.08.2026)

Симптом: при работающем DCC-сигнале первый же пакет функции (включение света
F0) «вешал» декодер — после этого команды скорости игнорировались.

Причина — переполнение стека задачи DCC. `on_dcc_function()` в `app_main.c`
объявлял `settings_track_t tracks[SETTINGS_MAX_TRACKS]` (20 × ~200 Б ≈ 4 КБ) в
локальной памяти. Стек задачи `dcc` составлял 4096 Б, и кадр этой функции
резервировался на входе при ЛЮБОМ пакете функции (включая F0/свет) — стек
переполнялся, задача DCC падала/портила память, скорость переставала
декодироваться.

Исправление:
- `app_main.c`: буфер `tracks` вынесен из стека в статическую память
  (`static`, обращается к нему только единственная задача DCC).
- `dcc.c`: стек задачи `dcc` увеличен 4096 → 8192 Б (запас под цепочку
  колбэков с NVS/flash-операциями).

Проверка: сборка SUCCESS, все 35 host-тестов проходят; символ `tracks$0`
теперь в `.bss`, а не на стеке. Образы в `release/` пересобраны.

Дополнение (28.08, стенд): при живом DCC-сигнале старый образ падал уже на
старте с `assert failed: xTaskRemoveFromEventList ... (pxUnblockedTCB)` —
это та же причина (командная станция непрерывно повторяет пакеты функций,
первый же такой пакет переполнял стек 4096 Б). Прошивка без фикса ловила
переполнение мгновенно после включения. Дополнительно включён контроль
переполнения стека задач (`CONFIG_FREERTOS_CHECK_STACKOVERFLOW=1`, canary на
переключении контекста): теперь любое переполнение даёт явный panic с именем
задачи, а не молчаливую порчу кучи (которая выглядела как случайный assert
FreeRTOS). Образы в `release/` обновлены (SHA `1FF34F3F...`).

### 12б. Изоляция источника управления: «Рельсы» ⇄ «Веб» (28.08.2026)

Требование: при управлении «Рельсы» всем управляет только DCC (скорость,
функции, свет, AUX); при управлении «Веб» — только веб-интерфейс (включая AUX).

До этого задействован был только мотор (скорость): функции/свет/AUX от DCC
применялись всегда, а из веба AUX можно было менять даже в режиме «Рельсы».

Исправлено:
- `app_main.c`:
  - `on_dcc_function` — пакеты функций DCC применяются только в режиме
    «Рельсы» (в веб-режиме DCC не трогает функции/свет/AUX);
  - `on_dcc_reset` — broadcast reset (00 00 00) действует только в режиме
    «Рельсы»;
  - `safety_task` — таймаут CV11 (пропадание DCC-пакетов) останавливает мотор
    и гасит функции только в режиме «Рельсы» (в веб-режиме отсутствие DCC
    не должно стопить веб-управляемый мотор).
- `web.c`: `POST /api/function` и `POST /api/aux/effect` отклоняются
  (`{"ok":false,"error":"rails control active"}`), когда активен режим
  «Рельсы» — из веба нельзя управлять функциями/AUX.

Программирование CV по DCC (service/ops mode) работает в обоих режимах —
это настройка, а не управление. Образы в `release/` пересобраны
(SHA `0929E34D...`).

### 12в. Свет: F0 — передний, F1 — задний (GPIO46) (28.08.2026)

Привязка функций к выходам выправлена под логику веб-кнопок (FARAf/FARAr):

- F0 → F0F (передний свет, GPIO10);
- F1 → F0R (задний свет, GPIO46);
- F2..F8 → AUX1..AUX7.

Убрана направленная задача `f0_task` (раньше F0R включался только при
реверсе мотора, поэтому с веба на GPIO46 сигнала не было). Заодно устранён
сдвиг привязки AUX: кнопка «AUX1» в вебе раньше вела на GPIO40 (AUX2), а
F1 — на AUX1 (GPIO41) вместо заднего света.

Изменено: `components/web/src/web.c` — `web_set_function_state()` и
`outputs_init()` (без `f0_task`). Образ в `release/` пересобран
(SHA `CF9C04A2...`).

### 12. Версия ПО и скрипты сборки

Версия ПО — **v0.3** (отображается в веб-интерфейсе: `<title>` и бренд).

- `firmware/idf_build.ps1 -Flash` — сборка и прошивка МК по UART/USB-JTAG,
  порт определяется автоматически.
- `firmware/build_ota_bin.bat` — сборка и копирование образа приложения в
  `release/ADDITIPUS_v0.3.bin` для обновления по OTA (через веб-интерфейс).
- `firmware/build_flash_tool_files.bat` — сборка и копирование файлов для
  Espressif Flash Download Tool в `release/flash_download_tool/`:
  `bootloader.bin` (0x0000), `partitions.bin` (0x8000),
  `ota_data_initial.bin` (0x10000), `firmware.bin` (0x20000).
  Смещения и настройки — `release/flash_download_tool/README.txt`.

### 13. Прошивка + звуки на внешний NOR (провижининг по UART)

Новый компонент `components/provision` — загрузка звуков на внешний NOR
(W25Q128, 16 МБ) при прошивке:

- После загрузки прошивка ~8 с слушает UART0 в ожидании команды `PROV`.
- При приёме `PROV`: стирается внешний NOR (`storage_format`), монтируется
  свежий LittleFS, по протоколу принимаются WAV-файлы
  (`PUT <slot> <size> <label>` + данные, ACK-подтверждения), создаётся
  конфигурация слотов в NVS, затем устройство перезагружается.
- Ручное переключение: `firmware/flash_firmware_and_sounds.bat [COMx]` —
  прошивает прошивку и заливает звуки из `..\..\SOUND` в слоты 1..N
  (стирает существующие данные на внешнем NOR).
- Скрипт передачи: `firmware/provision_sounds.ps1`.
- Скорость передачи после рукопожатия — 921600 бод (~2.4 МБ за ~30 с).

---
