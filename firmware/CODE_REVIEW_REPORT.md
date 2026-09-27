# Отчёт код-ревью прошивки (Soft_Decoder_V3)

Read-only ревью first-party кода (`components/`, `main/`) с последующими
исправлениями. Исходный план: `.kilo/plans/1790518367801-firmware-code-review-agents-and-fixes.md`.
Агенты ревью: `.kilo/agent/review-*.md` (оркестратор `review-lead`, команда
`/review-firmware`).

Дата ревью: 2026-09-27. Ветка/состояние на момент правок.

Легенда статусов: **FIXED** — исправлено и покрыто тестами; **OPEN** — принято,
не сделано; **DEFERRED** — сознательно отложено (см. `FIXES_LOG.md`).

## 1. Сводка

| ID | Severity | Файл:строка | Проблема | Статус |
|----|----------|-------------|----------|--------|
| A1 | CRITICAL | `dcc.c:161,174` | Service Mode читал инструкцию из `packet[1]`, реальный NMRA-пакет не декодировался | FIXED |
| A3 | CRITICAL | `motor.c:148-150` | Таблица CV67..CV94: unsigned-wrap при немонотонных CV → произвольный duty | FIXED |
| A4 | CRITICAL | `motor.c:326,601-633` | После калибровки `s_applied_speed=126` → проброс на CV4-ramp | FIXED |
| B1 | CRITICAL | `web.c:662,2447+` | Открытый AP + неаутентифицированные OTA/reset/delete | OPEN (по дизайну) |
| A2 | HIGH | `dcc.c:153-154` | Короткие адреса 112..126 трактовались как Service Mode | FIXED |
| A5 | HIGH | `motor.c:457-460` | Нет fail-safe: PWM глушит только живая `motor_task` | FIXED |
| A6 | HIGH | `motor.c:354` | Реверс на полном duty (броcок тока) | FIXED |
| A7 | HIGH | `storage.c:26,68-75` | Внешний раздел жёстко 16 МБ, размер чипа игнорировался | FIXED |
| A8 | HIGH | `provision.c:521` | `storage_format()` при живых пользователях FS | FIXED |
| A9 | HIGH | `track_manifest.c:81-110` | Неатомарная запись манифеста, ошибки отброшены | FIXED |
| A10 | MEDIUM | `settings.c:250-275` | `s_cv[]` пишется/сбрасывается без `s_lock` | FIXED |
| B2 | MEDIUM | `web_util.c:97-107` | `json_escape` не экранировал C0-управляющие | FIXED |
| B3 | MEDIUM | `web.c:1105-1110` | ~12.6 КБ стека в `audio_tracks_get` (httpd 16 КБ) | FIXED |
| B4 | MEDIUM | `web.c:270-308,442-477` | `s_func_map`/`s_motion_*`/`s_fn` без `s_func_mutex` | FIXED |
| B5 | MEDIUM | `settings.c:476-490` | `settings_bemf_cal_load` не проверял длину blob | FIXED |
| B6 | MEDIUM | `settings.c:319-347` | `tracks` blob без валидации длины/NUL; `_save` без клампа count | FIXED |
| B7 | MEDIUM | `audio.c:224-287` | EOF не закрывает `FILE*`; нет проверки format/rate; RIFF padding | FIXED |
| B8 | MEDIUM | `app_main.c:219,226` | `web_init()` до `recover_tracks_from_storage()` → устаревшие категории/map | FIXED |
| B9 | MEDIUM | `web.c:1266-1285,1378,1838+` | Утечки queue/sem; `slot` без диапазона; reboot без `motor_stop` | FIXED |
| B10 | MEDIUM | `dcc.c:57,608-625` | 64-бит timestamp cross-core; `s_cfg` писатель-only mux | FIXED |
| B11 | MEDIUM | `auxio.c:116` | Mars: `(pos*255)/half` оборачивался в `uint8_t` на нечётном периоде | FIXED |
| B12 | MEDIUM | `audio.c:249` | 20×8 КБ stdio-буферов в internal DRAM | FIXED |
| B13 | MEDIUM | `pinmap.c:11-37` | Не отвергались GPIO22, дубли, диапазон | FIXED |
| B14 | MEDIUM | `motor.c:419-453` | Derivative kick при пропадании BEMF | FIXED |
| B15 | LOW | `provision.c:166` | `s_uart_driver_installed` ставился при ошибке install | FIXED |
| B16 | LOW | `settings.c:84-94` | `nvs_read_str` подменял дефолт только при NOT_FOUND | FIXED |
| C-L1 | LOW | `dcc.c:194-217` | Service bit-write не выдавал ACK | FIXED |

## 2. Ключевые исправления

### A1/A2 — DCC Service Mode (`components/dcc/src/dcc.c`)
Пакет Service Mode теперь разбирается как `[instr, CvLow, data, checksum]` без
адресного байта; вход в сервисный режим — явное состояние `s_service_mode`,
устанавливаемое reset-пакетом с удлинённой преамбулой (≥20) и сбрасываемое при
валидном main-track пакете. Это устраняет и неверный индекс инструкции (A1), и
ложное распознавание коротких адресов 112..126 (A2). Бит-запись сервисного
режима теперь выдаёт ACK (NMRA S-9.2.3).

### A3/A4/A5/A6/B14 — мотор (`components/motor/src/motor.c`)
- Таблица CV67..CV94 клампится как Vstart≤Vmid≤Vhigh-ветка (нет unsigned-wrap).
- По завершении калибровки состояние принудительно обнуляется (applied, ramp,
  PID, kick) — нет проброса.
- Добавлен `motor_emergency_stop()` (немедленно глушит мост из любой задачи) и
  `motor_last_tick_us()`; `safety_task` следит за живостью motor-задачи (порог
  200 мс) и глушит мост при зависании. Reset/estop/CV11-timeout используют
  emergency stop.
- Реверс при движении сначала тормозит до нуля (по CV4), затем меняет полярность.
- `s_pid_prev_error` затухает вместе с интегратором при недоступном BEMF.

### A7 — размер внешней NOR (`components/storage/src/storage.c`)
Раздел регистрируется размером `min(chip_size, 16 МБ)`; при неопределённом или
подозрительно малом чипе backend = NONE (без монтирования).

### A9 — атомарный манифест (`components/settings/src/track_manifest.c`)
Запись идёт в `tracks.txt.tmp` с проверкой `fflush/fsync/fclose`, затем
`rename()` поверх оригинала (с Windows-совместимым fallback). Функция теперь
возвращает `esp_err_t`.

### A10/B5/B6/B16 — настройки (`components/settings/src/settings.c`)
Запись/сброс CV под `s_lock`; `bemf_cal` проверяет точную длину blob; `tracks`
проверяет кратность длины и NUL-терминирует строки; `_save` клампят `count` к
ёмкости; `nvs_read_str` подставляет дефолт при любом не-OK чтении.

### B2/B11/B13/B15 — прочее
`json_escape` экранирует CR/TAB и заменяет прочие C0; Mars клампится к 255;
`pinmap_check` отвергает GPIO22, дубли и вне-диапазона; UART-флаг ставится
только при успешном install.

### B8 — порядок старта (`main/app_main.c`)
`recover_tracks_from_storage()` вызывается до `web_init()`, чтобы кэш
категорий/карты функций не оставался дефолтным после восстановления.

## 3. Проверка

- `test\run_tests.ps1` — все 14 наборов PASSED (470 тестов; +тесты на A1..A10,
  B1..B16, FIX-7/9/10/11 и LOW-пункты).
- `test\coverage.ps1` — first-party **100,0 %** (4414/4414 строк).
- `pio run -e esp32-s3-devkitc-1` — SUCCESS; RAM 50 416 Б (15.4 %),
  Flash 823 505 Б (41.9 %).

Не проверено на железе (нет платы): service-mode ACK/программирование CV с
командной станции (A1/A2), реверс под нагрузкой и аварийный стоп (A5/A6),
поведение при просадке питания. Формальная проверка HIL изложена в
`TESTS_HANDOFF.md`/`FIXES_LOG.md`.

## 4. Остаётся

Все пункты отчёта закрыты в коде и покрыты тестами, кроме B1:

- **B1 — открытый AP и неаутентифицированные разрушительные endpoint'ы.**
  Оставлено осознанно по требованию: пароль Wi-Fi по умолчанию **не задаётся**
  и не генерируется, пока пользователь сам не установит его (веб `/api/wifi`
  POST), после чего AP работает как WPA2. Варианты усиления (WPA2 по умолчанию
  со сгенерированным паролем или токен на мутирующие маршруты) отклонены.

Требует аппарата:

- HIL-прогон на плате: `run_hil.ps1` (SELFTEST/BEMF), `run_hil_web.ps1`
  (при открытом AP `-ApPass` не нужен; при заданном пароле — передать его).
- Проверка Task Watchdog (`CONFIG_ESP_TASK_WDT_PANIC`, 10 с) и дебаунса CV на
  реальных flash-операциях (OTA/провижининг): при ложных срабатываниях
  поднять `CONFIG_ESP_TASK_WDT_TIMEOUT_S`.
- Service-mode ACK/программирование CV с командной станции (A1/A2), реверс под
  нагрузкой и аварийный стоп (A5/A6), поведение при просадке питания.
- Формат coredump-раздела (128 КБ) ограничен разметкой 4 МБ (это последний
  раздел до конца flash) — расширить нельзя без уменьшения OTA; при необходимости
  уменьшить стеки задач, а не раздел.
