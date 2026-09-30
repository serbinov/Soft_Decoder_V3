# Роадмап: звуковой движок ADDITIPUS AURA-X

> Примечание: документ исторический. Сборка переведена на ESP-IDF 6.0
> (`firmware\idf_build.ps1`); упоминания `pio run`/PlatformIO относятся к
> ранним версиям.

Рабочий пошаговый документ внедрения. По нему вносятся изменения в код.
Проект: `firmware/` (Soft_Decoder_V3), версия-база **0.8**.
Проектирование и обоснования: `SOUND_ENGINE_IMPLEMENTATION.md` (особенно **§17–§21**).
Формат каждого шага: **Цель / Файлы / Действия / Проверка / DoD**.

При расхождении с `SOUND_ENGINE_IMPLEMENTATION.md` приоритет у этого документа по
**порядку и объёму работ**, а по деталям структур — у §5/§8/§20.

---

## Статус реализации

Этапы **R0–R6 реализованы** (версия прошивки `0.9`). Проверка на хосте:
`test\run_tests.ps1` — все наборы зелёные; `test\coverage.ps1` — покрытие
first-party **100 %** (включая новый набор `test_sound`). `pio run` в среде
выполнения недоступен, поэтому сборка прошивки/CMake-связи проверены только
статически.

Реализовано:
- **R0** — `audio`: `rate_permille/rate_cur`, сглаживание ресемплера,
  `audio_voice_set_rate/is_active/position/alloc/release`, анти-щёлчок
  (фейд 2 блока, тест-хук `s_fade_blocks`).
- **R1** — `settings/include/sound_types.h`, компонент `sound` с бинарным
  хранилищем `.mds` (`sound_store`), NVS `active_scheme`, `func_bind`
  (+`legacy_convert`), манифест v2 с записями `B;`.
- **R2** — `func_eval()` в `web_util`, привязки в `web.c` (миграция + fallback),
  REST `GET /api/func-map?view=bind|matrix` и `POST ?bind=1`.
- **R3** — `components/sound/src/sound.c`: секвенсер Init/Loop/End, автомат
  5 ступеней с гистерезисом, rate/чuff, 4 цилиндра, линт, режимы F-клавиш
  (ONE_SHOT/LOOP_HELD/SHORT_LONG/LATCHED/TRIGGER).
- **R4** — `sound_init()` в `app_main` (после `audio_init`), `motor_get_applied_speed`
  и опрос применённой скорости в задаче `sound`, вызов `sound_function` из
  `web_apply_function` с отключением legacy-слотов при активной схеме,
  резерв голосов 18/19, HIL-команды `HIL-ENGINE`, `HIL-SCHEME-SPEED`.
- **R5** — `max_uri_handlers=64`, маршруты `/api/sound/{state,scheme,table,extra,lint}`,
  панели «Звук: Схема/Функции/Доп. звуки/Параметры» и модальный редактор таблицы
  в `web_ui.html`.
- **R6** — CV30 (флаги Skip StoD1/D1toS), CV63 (алиас мастер-громкости),
  CV114/115/116 (chuff/bell/dynamic-brake rate, дефолты 57/5/30; см.
  `docs/cv_sound.md`), тормозной скрип, случайные и состояний-звуки
  (обязательный `max_plays`), mute-логики (`MUTE_STOP/MOVE/LIGHT`),
  `sync_motion`, имена CV в UI, версия 0.9. Документация: `docs/sound_scheme.md`,
  `docs/cv_sound.md`, `CHANGELOG.md`.

Упрощения/остаток относительно §5/§20:
- Loop-треки воспроизводятся как one-shot с перезапуском (секвенсер считает
  проигрыши для `min/max_plays`); бесшовность — задача предзагрузки P2.
- Расчётный chuff-интервал (формула JMRI) и фазировка цилиндров не реализованы:
  скорость chuff — через CV114 и `rate_scale` таблицы; 4 Init-группы играются
  последовательно.
- Модель таблиц хранит имена файлов (R1.1), из-за чего `sizeof(sound_scheme_t)`
  ≈ 26 КБ; при нехватке DRAM структуру можно вынести в PSRAM.
- Тормозной скрипт использует Stop-таблицу; «переход на End заранее» и явная
  привязка к CV3/CV4 не реализованы.
- `DRIVE_HOLD`/`COAST`/`DYNAMIC_BRAKE`/`NOTCH_UP/DOWN` распознаются, но пока
  no-op.
- Приёмка на железе (`pio run`, HIL) в этой среде не выполнялась — `pio`
  недоступен.

---

## 0. Как работать по документу

- Выполнять шаги **строго по порядку**; следующий шаг не начинать, пока не выполнен DoD
  предыдущего.
- После каждого шага: `pio run -e esp32-s3-devkitc-1`,
  `powershell -File test\run_tests.ps1`, `powershell -File test\coverage.ps1` (покрытие
  first-party = 100 %).
- Один шаг = один осмысленный коммит (`feat(sound): …`, `feat(web): …`).
- Новый тест-набор регистрировать в **ОБОИХ** скриптах: `test/run_tests.ps1` и
  `test/coverage.ps1` (`$inc`, `$suites`).
- Обновлять `ARCHITECTURE.md` при добавлении компонента/файла/маршрута.
- Отмечать `- [x]` по завершении шага.

### Инварианты (проверять на каждом шаге)
1. При `SOUND_SCHEME_NONE` поведение и все текущие тесты — без изменений.
2. Нет кода, скопированного из GPL-проектов (см. `SOUND_ENGINE_IMPLEMENTATION.md` §19.6).
3. `register_route(...)` не превышает `max_uri_handlers` (поднять до 64 в R5).
4. Никаких `//`-комментариев в `web_ui.html` (minifier), править только `web_ui.html`.
5. Задачи не блокируют `dcc`/`motor` (приоритеты: sound ≤ 6).

### Команды
```
pio run -e esp32-s3-devkitc-1
powershell -ExecutionPolicy Bypass -File test\run_tests.ps1
powershell -ExecutionPolicy Bypass -File test\coverage.ps1
python tools\gen_web_html.py
```

### 0.1. Проектные соглашения (проверено по коду) — обязательные точки правки
Эти факты определяют, **что именно менять**, помимо C-логики.

1. **Компоненты ESP-IDF линкуют файлы явно** (`idf_component_register(SRCS …)`), не по
   glob. Значит при добавлении `.c` править `CMakeLists.txt`:
   - `components/settings/CMakeLists.txt` — если добавляем файл в `settings`;
   - `components/sound/CMakeLists.txt` — новый компонент
     `idf_component_register(SRCS "src/sound.c" INCLUDE_DIRS "include"
     REQUIRES audio motor settings storage esp_timer)`;
   - `main/CMakeLists.txt` — добавить `sound` в `REQUIRES` (сейчас
     `pinmap storage settings dcc motor audio web track provision`).
2. **Хранение схемы — в компоненте `sound`, не в `settings`.** `settings` — только NVS
   (`REQUIRES nvs_flash esp_timer`), без `storage`. Файлы `.mds` читает/пишет `sound`
   (`REQUIRES storage`), используя `storage_is_mounted()` и `storage_get_root()`
   (`components/storage/include/storage.h`). `track_manifest.c` в `settings` работает с
   файлами через VFS/stdout — но это существующий код; новый файловый I/O кладём в `sound`.
3. **Общий заголовок типов** — `components/settings/include/sound_types.h` (без зависимостей).
   Подключают `settings` (для `func_binding`), `sound`, `web`.
4. **Привязки функций (`func_bind`) — в `settings.c`**, рядом с `func_map`/`tracks`
   (NVS-ключ `func_bind`, namespace `decoder`); по образцу `settings_func_map_load/save`.
   При этом `settings.c` **не кэширует** конфиг: `settings_load/save(settings_config_t*)`
   работают со структурой вызывающего.
5. **CV-хранилище**: `static uint8_t s_cv[SETTINGS_CV_COUNT+1]` (индекс 1..512, `0` не
   используется), дефолты — в `cv_set_defaults()`; `settings_cv_read/write` —
   границы 1..512, `CV7` только для чтения. CV63-алиас: держать `master_volume` в
   `settings.c` кэшем (обновлять в `settings_load/save` и при записи CV63), иначе
   `settings_cv_read` не сможет отдать громкость без чтения NVS.
6. **Нативные тесты — white-box**: каждый набор `test/<suite>/*.c` компилируется с Unity
   и `#define static` + `#include "../../components/…/X.c"`; заглушки —
   `test_libs/teststubs/stubs.c` (+ `web_stubs.c` для `web`). Новые API, которые дергает
   `web.c`/`sound.c`, надо застабить (иначе `test_web`/`test_sound` не соберутся).
7. **Регистрация набора — в ДВУХ скриптах**: `$inc` (добавить
   `-I$root\components\sound\include`) и `$suites` (добавить `"test_sound"`) в
   `test/run_tests.ps1` **и** `test/coverage.ps1`.
8. **`app_main.c`**: порядок `motor_init → audio_init → [sound_init] → dcc_init →
   recover_tracks_from_storage → web_init → track_init → safety_task`; добавить
   `#include "sound.h"`.
9. **Сброс/таймаут**: `on_dcc_reset()` и ветка `CV11 timeout` в `safety_task()` уже зовут
   `motor_emergency_stop()` + `clear_functions()` — туда добавить `sound_stop_all()`.
   `clear_functions()` гоняет `web_apply_function(fn,false)` по всем F — этого достаточно
   для эффектов, но флаг «двигатель вкл» сбрасывать явно.
10. **Web-хелперы** уже есть: `send_json(req,json)`, `buf_appendf(buf,n,&used,fmt,…)`,
    `web_log_event(tag,fmt,…)`; парсеры `parse_u8/parse_u16/parse_bool` — в
    `web_util.c` (покрыты `test_web_util`). Регистрация — `register_route(uri,method,cb)`.
    `WIN_FN_COUNT = WEB_FN_COUNT = SETTINGS_FUNC_MAP_COUNT = 29`; UI сейчас рисует только
    F0..F10.
11. **HIL-консоль — в `provision.c::run_hil_act_console`** (`strncmp`+`sscanf`+`send_line`),
    вызывающая `selftest_act_*` из `components/selftest/src/selftest.c`. Новые команды
    (`HIL-ENGINE`, `HIL-SCHEME-SPEED`) добавлять там же.
12. **Веб-страница** — только `web_ui.html` (+ `python tools/gen_web_html.py`); `web_html.h`
    генерируется и в git не хранится.

### 0.2. Проверка работоспособности новой схемы (по коду)
**Вердикт: рабочая.** Архитектурных блокеров нет; механизмы подтверждены чтением кода.
Ниже — что подтверждено и какие правки обязательны.

Подтверждено кодом (можно опираться):
| Механизм | Доказательство | Статус |
|---|---|---|
| Управляемая частота воспроизведения | `voice_fill()` считает `step = sample_rate/MIX_RATE` заново каждый блок (`audio.c:222-240`) | ✅ умножаем на `rate_cur` |
| Детект конца one-shot (Init/End) | `voice_next_sample()` → false ⇒ `st->active=false` (`audio.c:244-252`) | ✅ `audio_voice_is_active` |
| Бесшовный loop | `voice_next_sample()` делает `fseek(data_start)` и продолжает (`audio.c:196-199`) | ✅ loop не кончается |
| Резерв голосов | `audio_voice_play(voice,…)` принимает любой `< AUDIO_MAX_VOICES` (`audio.c:408-412`) | ✅ двигатель 18/19 |
| Фактическая (ramped) скорость мотора | `s_applied_speed`/`s_applied_forward` существуют, `motor_tick()` рампит всегда по CV3/CV4 (`motor.c:40-44,339-393`) | ⚠️ нужно API |
| Разгон/замедление при BEMF off | рампа не зависит от BEMF (только PID/нагрузка) | ✅ accel работать будет |
| Хранилище | `storage_get_root()` = `/userdata`, монтируется извне (`storage.h:21`, `storage.c:159`) | ✅ `.mds` в `sound` |
| Потоки без инверсии локов | `web_apply_function` держит `s_func_mutex`; звук не вызывает `web` | ✅ при правиле |
| Деградация без платы/звука | `audio_init()`/`storage_mount()` не фатальны (`app_main.c:203-227`) | ✅ |

Обязательные правки (иначе не заработает корректно):
1. **Конфликт голоса 0** — сегодня `voice = fn-1` (F1→0) и ручной play/HIL тоже пишут в 0
   (`web.c:384,1208`, `selftest.c`). Ввести резерв 18/19 под двигатель и в scheme-режиме
   **не использовать legacy-путь** слотов для этих голосов.
2. **`audio_is_playing()` глобальный** (`audio.c:466`) — зацикленный двигатель сделает
   `playing=true` навсегда → `/api/audio/status` (web.c:1146) и подсветка слотов сломаются.
   Считать `playing` по не-engine голосам (нужен `audio_voice_is_active`).
3. **`audio_stop_all()` гасит двигатель** (`audio.c:446`; `/api/audio/stop`, `web.c:1220`) —
   в scheme-режиме стоп = только эффекты; двигатель — отдельным `sound_engine_off()`.
4. **Путь к WAV**: `voice_start()` делает `fopen(path)` (`audio.c:268`) — таблицы должны
   отдавать **абсолютный путь** `/userdata/audio/<file>`; хранить имена файлов и собирать
   путь через `storage_get_root()` + `"/audio/"`.
5. **Громкость двигателя/эффектов**: `voice_fill` умножает только `st.volume * s_volume`
   (`audio.c:230`); движок должен сам ставить `volume = engine_volume` / `effects_volume`
   (иначе регуляторы «Двигатель/Эффекты» не подействуют).
6. **`audio_voice_loop_count`** (или суммарные проигранные сэмплы) — для `min/max_plays`:
   loop никогда не завершается, `is_active` остаётся true, нужен счётчик витков.
7. **Схемный режим отключает старый маппинг слотов** в `web_apply_function`: при
   `type != NONE` звуковые цели идут через `sound_*`, а не через `slot_a/slot_b → voice`.
8. **`motor_get_applied_speed(&speed,&fwd)`** — иначе ускорение считается по target-скорости
   (мгновенный скачок), переходы A/CX сработают неверно (см. §18 C-4).

Ограничения/риски (не блокеры, учесть):
- Нагрузка (BEMF) доступна только при `bemf_use=1`; иначе `load=0`.
- Микропауза `fopen()` при переходе Init→Loop на NOR; смягчить короткими Init (P2 — предзагрузка).
- Лимит одновременных файлов LittleFS (внутренний, в sdkconfig не задан) — сегодня 20 голосов
  уже используются, новая схема держит 1–2 двигателя + несколько эффектов, т.е. не хуже.
- Обратного воспроизведения нет: направление — отдельными таблицами (как в SoundGT).

---

## R0. ✅ Аудио: управляемая частота и сервисные запросы

Владелец компонента: `components/audio`. Ничего извне не ломается (rate по умолчанию 1000).

- [x] **R0.1. Поля состояния голоса.**
  Файлы: `components/audio/src/audio.c`.
  Действия: в `voice_state_t` добавить `uint16_t rate_permille` (целевой, 1000=номинал),
  `double rate_cur`, `uint32_t played`; в `voice_t` — `uint16_t req_rate` (по умолчанию 1000).
  Проверка: сборка; при `rate=1000` звук идентичен прежнему.
  DoD: компилируется, `test_audio` зелёный без правок тестов.

- [x] **R0.2. Шаг ресемплинга с учётом rate.**
  Файлы: `audio.c`.
  Действия: в `voice_start()` задать `rate_cur = rate_permille/1000.0`; в `voice_fill()`
  заменить `double step = (double)st->sample_rate/(double)MIX_RATE;` на
  `step = ((double)st->sample_rate/(double)MIX_RATE) * st->rate_cur;`.
  Проверка: `test_audio` — при `rate=2000` за блок читается вдвое больше сэмплов.
  DoD: тест на rate-шаг проходит.

- [x] **R0.3. Сглаживание rate (анти-щелчок).**
  Файлы: `audio.c`.
  Действия: в начале обработки блока `rate_cur += (target - rate_cur) * 0.25;`; ресемплер
  **не отключать** при 1.0 (всегда активен).
  Проверка: ступенчатое изменение rate не даёт скачка амплитуды.
  DoD: тест на отсутствие разрыва.

- [x] **R0.4. API установки частоты.**
  Файлы: `components/audio/include/audio.h`, `audio.c`.
  Действия: добавить `esp_err_t audio_voice_set_rate(uint8_t voice, uint16_t permille);`
  (клип 500..3000; запись через `s_req_mutex` в `req_rate`, применение в `mixer_task`).
  Проверка: `test_audio` (валид/невалид голос).
  DoD: новый API покрыт тестом.

- [x] **R0.5. Опрос состояния голоса.**
  Файлы: `audio.h`, `audio.c`.
  Действия: `bool audio_voice_is_active(uint8_t voice);` и
  `uint32_t audio_voice_position(uint8_t voice);` (сумма `played`).
  Проверка: `test_audio`.
  DoD: тесты обоих методов.

- [x] **R0.6. Простой аллокатор голосов.**
  Файлы: `audio.h`, `audio.c`.
  Действия: `uint8_t audio_voice_alloc(void);` / `void audio_voice_release(uint8_t voice);`
  на массиве флагов занятости; при остановке голоса (`stop`/самозавершение) — освобождать.
  Проверка: `test_audio` — выдача/повторное использование/утечки.
  DoD: тесты аллокатора.

- [x] **R0.7. Анти-щелчок старта/стопа.**
  Файлы: `audio.c`.
  Действия: хранить последний сэмпл голоса; на старте/стопе — линейный фейд 2–4 блока.
  Проверка: `test_audio` (нет резких переходов), ручной прогон.
  DoD: тест + отсутствие слышимых щелчков.

- [x] **R0.8. Заглушки и реестр тестов.**
  Файлы: `test_libs/teststubs/*`, `test/test_audio/test_audio.c`.
  Действия: заглушки rate/alloc; новые кейсы; при необходимости обновить `$inc/$suites`.
  Проверка: `run_tests.ps1`, `coverage.ps1` = 100 %.
  DoD: R0 закрыт.

---

## R1. ✅ Типы и хранение схемы

- [x] **R1.1. Общие типы схемы.**
  Файлы: новый `components/settings/include/sound_types.h`.
  Действия: перенести структуры из `SOUND_ENGINE_IMPLEMENTATION.md` §5/§8.4:
  `sound_scheme_t`, `sound_table_t`, `sound_track_ref_t`, `sound_engine_t`, `sound_extra_t`,
  `sound_brake_t`, `sound_mode_t`, `func_binding_t` + константы
  (`SOUND_MAX_TABLES`, `SOUND_MAX_TRACKS_PER_TABLE`, `SOUND_ENGINE_STEPS`, `FUNC_BIND_MAX`).
  Поля треков — **имена файлов** `init[4]/loop[4]/end[4]` (см. §20.6).
  Учесть поля из §5/§20.9: `sound_engine_t.cyl_shift_min/max/inc` и `flags`
  (`SOUND_ENG_SKIP_STOD1`/`SKIP_D1TOS` = биты CV30), `func_binding_t.short_table/short_ms/
  min_ms/fade_ms`; доп. звук без клавиши — `sound_extra_t.fn = 0xFF` (state/random).
  Проверка: компилируется у `settings`, `sound`, `web`.
  DoD: заголовок подключён, сборка зелёная.

- [x] **R1.2. Бинарное хранение `.mds` (в компоненте `sound`).**
  Файлы: новый `components/sound/include/sound_store.h`, `components/sound/src/sound_store.c`,
  и `components/sound/CMakeLists.txt`:
  `idf_component_register(SRCS "src/sound_store.c" INCLUDE_DIRS "include"
  REQUIRES settings storage esp_timer)` (R3.1 добавит `src/sound.c`).
  Действия: формат `[magic "MDS1"][u16 ver][u16 size][u32 crc32][sound_scheme_t]`;
  `sound_store_load(path,&out)`, `sound_store_save(path,const*)`,
  `sound_store_default(sound_scheme_t*)`. Каталог `storage_get_root()/projects`
  (создать при старте, если `storage_is_mounted()`). При несовпадении magic/ver/size/crc —
  дефолт. **Без текстового парсера.** `settings` не трогаем (там только NVS).
  Проверка: `test_settings`-подобный набор (см. R3.8) с временным каталогом.
  DoD: round-trip + битый файл → дефолт; компонент собирается.

- [x] **R1.3. Указатель активной схемы.**
  Файлы: `settings.h/.c`.
  Действия: NVS-ключ `active_scheme` (строка/индекс); `settings_active_scheme_get/set`.
  Проверка: `test_settings`.
  DoD: сохранение/чтение.

- [x] **R1.4. Дефолты и совместимость.**
  Действия: `type=SOUND_SCHEME_NONE` по умолчанию; при `NONE` звуковой движок выключен.
  Проверка: тесты не меняются.
  DoD: регресс отсутствует.

- [x] **R1.5. Привязки функций: хранение + миграция.**
  Файлы: `settings.h/.c`.
  Действия: NVS-ключ `func_bind` (`func_binding_t[FUNC_BIND_MAX]`);
  `settings_func_bind_load/save` и `settings_func_bind_legacy_convert()` (из старого
  `func_map`). Манифест `track_manifest.c` — записи `B;` (а обратно совместимо с v1).
  Примечание: `settings.c` подключает `sound_types.h` из своего `include/`;
  `CMakeLists.txt` менять не нужно (файл уже в `SRCS`).
  Проверка: `test_settings`, `test_track_manifest`.
  DoD: конверсия и round-trip.

- [x] **R1.6. Реестр тестов.**
  Проверка: `run_tests.ps1`, `coverage.ps1` = 100 %.
  DoD: R1 закрыт.

---

## R2. ✅ Карта функций (привязки + eval)

- [x] **R2.1. API привязок.**
  Файлы: `settings.h/.c`.
  Действия: `settings_func_bind_add/remove/find` с лимитом `FUNC_BIND_MAX`.
  Проверка: `test_settings`.
  DoD: тесты add/remove/overflow.

- [x] **R2.2. Чистый оценщик `func_eval`.**
  Файлы: `components/web/src/web_util.c` (+ прототип в `web_util.h`).
  Действия: `uint16_t func_eval(const func_binding_t*, size_t n, uint8_t fn,
  uint8_t state, uint8_t dir, void (*on_sound)(void*,uint8_t,uint8_t,bool), void *ctx)`
  по алгоритму §8.5 (маска выходов + колбэк на звук). Без побочных эффектов.
  Проверка: `test_web_util` (dir/state/mode/несколько привязок).
  DoD: покрытие всех веток.

- [x] **R2.3. Замена прежнего пути в web.**
  Файлы: `web.c` (`func_apply_output_locked`, `web_apply_function`).
  Действия: считать `desired` через `func_eval` по привязкам; `s_func_last_mask[fn]`
  оставить для дельты AUX; старый `settings_func_map_t` — только для legacy/миграции.
  Проверка: `test_web` (F0..F10 сценарии проходят).
  DoD: регресс `test_web`.

- [x] **R2.4. REST карты.**
  Файлы: `web.c`.
  Действия: `GET /api/func-map` — привязки; `GET /api/func-map?view=matrix` — обе матрицы;
  `POST /api/func-map` — приём привязок **и** старого формата (конвертация).
  Проверка: `test_web`.
  DoD: тесты get/set/matrix/legacy.

- [x] **R2.5. Тесты и реестр.**
  Проверка: `run_tests.ps1`, `coverage.ps1` = 100 %.
  DoD: R2 закрыт.

---

## R3. ✅ Ядро компонента `sound`

- [x] **R3.1. Каркас компонента.**
  Файлы: `components/sound/include/sound.h`, `components/sound/src/sound.c`;
  в `components/sound/CMakeLists.txt` добавить `src/sound.c` в `SRCS`
  (`REQUIRES audio motor settings storage esp_timer`).
  Действия: задача `sound` (стек 4096, prio 6, ядро 1, период 20 мс); мьютекс схемы.
  Проверка: сборка, задача стартует (лог).
  DoD: компонент в сборке, задача создана.

- [x] **R3.2. Загрузка схемы и доступ к таблицам.**
  Файлы: `sound.c/.h`.
  Действия: `sound_init()`, `sound_scheme_get/set`, `sound_table_get/set`,
  `sound_extra_get/set` (через `sound_store_load/save`).
  Проверка: `test_sound` (round-trip).
  DoD: тесты доступа.

- [x] **R3.3. Секвенсер таблицы Init/Loop/End.**
  Файлы: `sound.c`.
  Действия: `tb_phase_t {TB_STOP,TB_INIT,TB_LOOP,TB_END}` (§20.2); проигрывание через
  `audio_voice_play` + `audio_voice_is_active`; учёт `min/max_plays`, `end_table`.
  Та же модель для доп. звуков (у MD Prog — `begin/loop/end`, §22.1); допускаются одиночные
  треки (`Init`+`End` без `Loop`).
  Проверка: `test_sound`.
  DoD: тесты фаз и переходов.

- [x] **R3.4. Автомат 5 ступеней + гистерезис.**
  Файлы: `sound.c`.
  Действия: полный список состояний/переходов из §6.1 (в т.ч. `CX1 to Stop`); пороги
  `max/min_speed`, `max_accel/decel` с дедбендом (§20.3); EMA α=0.3; опции пропуска
  `Stop→D1` / `D1→Stop` — `sound_engine_t.flags` (биты CV30 `Skip StoD1` / `Skip D1toS`).
  Проверка: `test_sound` (последовательности speed/accel).
  DoD: покрытие переходов.

- [x] **R3.5. Rate и chuff.**
  Файлы: `sound.c`.
  Действия: `rate_permille` по T-03/§20.4; `audio_voice_set_rate`.
  Проверка: `test_sound` (монотонность от скорости).
  DoD: тест rate.

- [x] **R3.6. Пар: 4 цилиндра.**
  Файлы: `sound.c`.
  Действия: цикл `cylinder = (cylinder+1)&3`; chuff-интервал от скорости; фазировка —
  `cyl_shift_min/max/inc` (в MD Prog — «сдвиг цилиндров», §22.4/§20.4).
  Проверка: `test_sound`.
  DoD: тест цилиндра.

- [x] **R3.7. Линт графа.**
  Файлы: `sound.c/.h`.
  Действия: `sound_lint()` — нет стартовой таблицы, битые ссылки, `min_speed>max_speed`.
  Проверка: `test_sound`.
  DoD: тесты линта.

- [x] **R3.8. Длительность нажатия F (короткий/длинный гудок).**
  Файлы: `components/sound/src/sound.c`,
  `components/settings/include/sound_types.h` (расширить `func_binding_t`),
  `components/web/src/web.c`.
  Действия: режимы `LOOP_HELD / SHORT_LONG / ONE_SHOT / LATCHED / TRIGGER`; поля
  `short_table / short_ms / min_ms / fade_ms`; замер `esp_timer_get_time()` между фронтами;
  дебаунс 20–30 мс (реагировать только на смену состояния); fade при отпускании
  (анти-щёлчок, R0.7). Алгоритм — `SOUND_ENGINE_IMPLEMENTATION.md` **§20.9**.
  Проверка: `test_sound` (`dt < short_ms` → `short_table`; иначе loop+fade; `LATCHED` не
  останавливается), HIL.
  Подтверждено MD Prog: отдельный сэмпл `whistle short` (§22.2) — `short_table` ссылается
  на таблицу «короткий».
  DoD: короткое и длинное нажатие дают разный звук; длинный гудок затухает.

- [x] **R3.9. Набор `test_sound` + реестр.**
  Файлы: `test/test_sound/test_sound.c` (+ при необходимости `test/test_sound/*.c`),
  `test/run_tests.ps1`, `test/coverage.ps1`.
  Действия: white-box шаблон — `#define static`, `#include
  "../../components/sound/src/sound.c"`, `#include
  "../../components/sound/src/sound_store.c"`, `#undef static`, затем
  `#include "../../test_libs/teststubs/stubs.c"` и локальные заглушки
  `audio_*`/`motor_*`/`settings_*`, которые вызывает `sound`.
  В **обоих** скриптах: `$inc` += `"-I$root\components\sound\include"`,
  `$suites` += `"test_sound"`. Также добавить заглушки `sound_*` в тестовые
  зависимости `test_web` (`test_libs/teststubs/web_stubs.c` или локально).
  Проверка: `run_tests.ps1`, `coverage.ps1` = 100 %.
  DoD: R3 закрыт, оба скрипта обновлены.

---

## R4. ✅ Интеграция в текущий декодер

- [x] **R4.1. Порядок инициализации.**
  Файлы: `main/app_main.c`, `main/CMakeLists.txt`.
  Действия: `#include "sound.h"`; в `app_main()` вызвать `sound_init()` сразу после
  `audio_init()` и **до** `dcc_init()` (страницы: `motor_init → audio_init → sound_init →
  dcc_init → recover_tracks_from_storage → web_init → track_init`); ошибка схемы/хранилища —
  не фатальна (`ESP_LOGW`, как для `storage_mount`). В `main/CMakeLists.txt` добавить
  `sound` в `REQUIRES`.
  Проверка: сборка, лог порядка.
  DoD: старт без регресса, `sound` в REQUIRES.

- [x] **R4.2. Фактическая скорость мотора.**
  Файлы: `components/motor/include/motor.h`, `components/motor/src/motor.c`.
  Действия: добавить `void motor_get_applied_speed(uint8_t *speed128, bool *forward);`
  (возврат `s_applied_speed`/`s_applied_forward`). Причина — §18 C-4.
  Проверка: `test_motor`.
  DoD: API + тест.

- [x] **R4.3. Движение → звук.**
  Файлы: `main/app_main.c` (или `web.c::web_motion_changed` вне мьютекса).
  Действия: вызывать `sound_on_motion(applied_speed, forward)`; периодический тик — внутри
  задачи `sound` (опрос `motor_get_applied_speed`), колбэк — только ускорение реакции.
  Проверка: `test_web`, HIL `-MotorSpeed`.
  DoD: звук реагирует на изменение скорости.

- [x] **R4.4. Функции → звук + reset/estop.**
  Файлы: `components/web/src/web.c`, `main/app_main.c`.
  Действия: `web_apply_function()` дополнительно вызывает `sound_on_function(fn,state)`;
  в `on_dcc_reset()` и в ветке `CV11 timeout` внутри `safety_task()` (после
  `motor_emergency_stop()` + `clear_functions()`) добавить `sound_stop_all()` и сброс
  `engine_on`. `clear_functions()` уже прогоняет `web_apply_function(fn,false)` по всем F —
  эффекты глушатся сами.
  Проверка: `test_web`, `test_sound`, HIL `-Sweep`.
  DoD: F-старт двигателя и глушение по reset/таймауту.

- [x] **R4.5. Резерв голосов.**
  Файлы: `sound.c`, `web.c`.
  Действия: двигатель — фиксированные голоса 18 (и 19 для crossfade); эффекты/доп. —
  0..17; при нехватке отбрасывать низкоприоритетный доп. звук.
  Проверка: `test_sound`, ручной сценарий «гудок+двигатель».
  DoD: двигатель не вытесняется F-эффектами.

- [x] **R4.6. Selftest/HIL.**
  Файлы: `components/selftest/src/selftest.c`, `components/provision/src/provision.c`,
  `test/hil/run_hil.ps1`.
  Действия: `selftest_act_sound_scheme(...)`/`selftest_act_engine(...)` в `selftest.c`;
  маршрутизация строк `HIL-ENGINE <0|1>` и `HIL-SCHEME-SPEED <n>` в
  `provision.c::run_hil_act_console` (шаблон `strncmp`+`sscanf`+`send_line`, ответы
  `HIL-*-OK`/`HIL-*-ERR`); отчёт активной таблицы/состояния двигателя.
  Проверка: HIL PASS (`run_hil.ps1`).
  DoD: HIL-команды работают через serial-консоль.

- [x] **R4.7. Тесты и реестр.**
  Проверка: `run_tests.ps1`, `coverage.ps1`, `run_hil.ps1` = 100 %.
  DoD: R4 закрыт.

---

## R5. ✅ REST и веб-интерфейс

> Дизайн-макеты, компоненты, адаптивность и JS-структура — `SOUND_ENGINE_WEB_DESIGN.md`.
> Шаги ниже реализуют этот дизайн.

- [x] **R5.0. Поднять лимит маршрутов.**
  Файлы: `components/web/src/web.c`.
  Действия: `cfg.max_uri_handlers = 64`; при необходимости объединять маршруты.
  Проверка: сборка, все маршруты регистрируются (лог ошибок пуст).
  DoD: нет отказа регистрации.

- [x] **R5.1. REST схемы (5 маршрутов).**
  Файлы: `web.c`.
  Действия: `GET/POST /api/sound/scheme`, `GET/POST /api/sound/table?id=N`,
  `GET/POST /api/sound/extras`, `GET/POST /api/sound/params`, `GET /api/sound/state`
  (+ `GET /api/sound/lint`). Использовать существующие хелперы: `send_json`,
  `buf_appendf(buf,n,&used,…)`, `parse_u8/parse_u16/parse_bool` (из `web_util.c`),
  `web_log_event`; регистрация — `register_route(uri,method,cb)`. JSON ≤ 8 КБ
  (ориентир `audio_tracks_get`). Учесть `WEB_FN_COUNT = 29`.
  Проверка: `test_web`.
  DoD: тесты каждого маршрута.

- [x] **R5.2. Панель «Звук: Схема».**
  Файлы: `web_ui.html`.
  Действия: сворачиваемая панель; тип проекта, клавиша старта, «синхронизировать движение»,
  карточки состояний (`.cv-card`), live-индикатор; `togglePanel('sound_scheme')`.
  Проверка: `run_hil_web.ps1`, ручной.
  DoD: панель грузит/сохраняет схему.

- [x] **R5.3. Редактор таблицы (модально).**
  Файлы: `web_ui.html`.
  Действия: поля §10.2 — Init/Loop/End ×4, max/min скорость, max ускорение/замедление,
  окончание, ускорение звука, min/max проигрышей, «не включать без двигателя»; ссылки на
  таблицы текстом; кнопка `▶ Прослушать` (`/api/audio/play`).
  Проверка: ручной + HIL web.
  DoD: сохранение таблицы.

- [x] **R5.4. Матрица «Карта выходов».**
  Файлы: `web_ui.html`.
  Действия: 9 выходов × (F0..F28 × В/Н), горизонтальный скролл + sticky, клик создаёт/
  удаляет привязку.
  Проверка: правки сохраняются после reload.
  DoD: матрица синхронна с `/api/func-map`.

- [x] **R5.5. Матрица «Карта звуков».**
  Файлы: `web_ui.html`.
  Действия: строки — таблицы/слоты, столбцы F×В/Н, имя таблицы в ячейке.
  Проверка: ручной.
  DoD: привязки сохраняются.

- [x] **R5.6. Панель «Звук: Доп. звуки» и «Звук: Параметры».**
  Файлы: `web_ui.html`.
  Действия: список доп. таблиц (группа 1), привязки F×направление, состояния, случайные,
  колодки; параметры CV/громкости.
  Проверка: HIL web.
  DoD: обе панели работают.

- [x] **R5.7. Генерация и тесты.**
  Действия: `python tools/gen_web_html.py`; `test_web`; `run_hil_web.ps1`.
  Проверка: страница собирается, покрытие 100 %.
  DoD: R5 закрыт.

---

## R6. ✅ CV, параметры, колодки, финализация

- [x] **R6.1. Новые CV.**
  Файлы: `components/settings/src/settings.c` (дефолты — в `cv_set_defaults()`).
  Действия: CV 30 (биты опций — `Skip StoD1`/`Skip D1toS`, синхронизировать с
  `sound_engine_t.flags`), 114 (chuff rate), 115 (bell rate), 116 (dynamic brake
  rate) — как обычные CV массива `s_cv[1..512]`, дефолты 0/57/5/30. CV63 — **алиас
  `master_volume`**: держать громкость кэшем в `settings.c`, синхронизируя её в
  `settings_load/save(settings_config_t*)` и при записи CV63; в `settings_cv_read/write`
  отдавать/принимать `vol = v*100/255` (обратно `v = vol*255/100`). Сохранить инварианты:
  границы 1..512, `CV7` read-only, поведение `CV8` (factory) не менять.
  Проверка: `test_settings`.
  DoD: CV читаются/пишутся, алиас двусторонний и не расходится с `master_volume`.

- [x] **R6.2. Отображение CV в UI.**
  Файлы: `web_ui.html` (`cvDB`).
  Действия: добавить описания CV 30/63/114/115/116.
  Проверка: ручной.
  DoD: CV видны и редактируются.

- [x] **R6.3. Тормозной скрип.**
  Файлы: `sound.c`.
  Действия: `sound_brake_t` — пороги, «End заранее», привязка к CV3/CV4.
  Проверка: `test_sound`.
  DoD: тесты порогов.

- [x] **R6.4. Random/авто-звуки и звуки по состоянию.**
  Файлы: `sound.c`, `sound_types.h`, `web.c`.
  Действия: `mode=RANDOM` (min/max интервал — как «мин./макс. время между случайными
  звуками» у MD Prog, §22.4) и `mode=STATE` (таблицы «`Стоим`/`Едем`/`Свет` × вперёд/назад»,
  §22.4: `SOUND`-привязки с `fn=0xFF`, `state`, `dir`); mute-клавиши
  (`MUTE_STOP/MOVE/LIGHT`) как `LOGIC`-привязки; обязательный `max_plays`.
  Проверка: `test_sound` с детерминированным seed.
  DoD: тесты random, state и mute.

- [x] **R6.5. Документация и релиз.**
  Файлы: `ARCHITECTURE.md`, `README` (при наличии), `docs/sound_scheme.md`,
  `docs/cv_sound.md`, `version.txt` + CHANGELOG.
  Действия: описать компонент `sound`, `.mds`, шкалу состояний и CV; поднять версию.
  Проверка: полный прогон + HIL.
  DoD: R6 закрыт.

---

## Что НЕ делать в этой итерации
- Generic-граф/DSL, байткод-VM (как MRRSoundDecoder).
- Микс нескольких сэмплов двигателя, ducking, sync-motion — post-MVP.
- 8-бит WAV, предпросмотр WAV в браузере, drag-and-drop граф.
- Текстовый формат схемы, отдельные страницы, SPA, MQTT.
- Копирование кода GPL-проектов; изменение partition table; gzip веб-страницы.

## Файлы: сводка изменений
| Файл | Тип | Этапы |
|---|---|---|
| `components/audio/{src/audio.c,include/audio.h}` | изм. | R0 |
| `components/settings/include/sound_types.h` | новый | R1 |
| `components/settings/{include/settings.h,src/settings.c}` | изм. | R1, R2, R6 |
| `components/settings/src/track_manifest.c` | изм. | R1 |
| `components/sound/include/sound_store.h` | новый | R1 |
| `components/sound/src/sound_store.c` | новый | R1 |
| `components/sound/{include/sound.h,src/sound.c}` | новый | R3 |
| `components/sound/CMakeLists.txt` | новый | R1, R3 |
| `components/motor/{include/motor.h,src/motor.c}` | изм. | R4 |
| `components/web/{src/web.c,src/web_util.c,include/web_util.h}` | изм. | R2, R4, R5 |
| `components/selftest/src/selftest.c` | изм. | R4 |
| `components/provision/src/provision.c` | изм. | R4 (HIL) |
| `main/{app_main.c,CMakeLists.txt}` | изм. | R4 |
| `web_ui.html` | изм. | R5, R6 |
| `test/test_sound/*` | новый | R3–R6 |
| `test_libs/teststubs/web_stubs.c` (+ stubs) | изм. | R2, R3 |
| `test/run_tests.ps1`, `test/coverage.ps1` | изм. | R0, R1, R3 |
| `components/settings/CMakeLists.txt` | **не меняется** | — |
| `ARCHITECTURE.md`, `docs/*` | изм./нов. | R6 |

## Definition of Done (общий)
1. `pio run -e esp32-s3-devkitc-1` без ошибок.
2. `test/run_tests.ps1` зелёный; `test/coverage.ps1` first-party = 100 %.
3. Затронутые REST/HIL проверены (`run_hil*.ps1` при наличии платы).
4. `web_ui.html` сгенерирован в `web_html.h`.
5. При `SOUND_SCHEME_NONE` поведение идентично текущему (регресс отсутствует).
6. `ARCHITECTURE.md` и тест-скрипты обновлены.
