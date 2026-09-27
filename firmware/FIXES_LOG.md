# Журнал исправлений по итогам архитектурного ревью

Референс: `ARCHITECTURE.md`. Здесь фиксируются найденные проблемные места и
статус их исправления. Формат записи:

```
## <ID> <Краткое имя>
- Статус: ПЛАН | В РАБОТЕ | ГОТОВО | ОТЛОЖЕНО
- Файлы: ...
- Проблема: ...
- Решение: ...
- Проверка: ...
```

Легенда статусов: ПЛАН — принято, ещё не начато; В РАБОТЕ — правки идут;
ГОТОВО — код собран и тесты пройдены; ОТЛОЖЕНО — сознательно отложено.

---

## Сводка

| ID | Проблема | Приоритет | Статус |
|----|----------|-----------|--------|
| FIX-1 | Блокирующий 6 мс service-mode ACK в задаче DCC | Высокий | ГОТОВО |
| FIX-2 | Разрушительные команды по UART без подтверждения (`PROV`) | Высокий | ГОТОВО |
| FIX-3 | OTA без rollback-защиты | Высокий | ГОТОВО |
| FIX-4 | Мелкие правки web.c (гонка `s_fn`, обрезка JSON в `/api/log`) | Средний | ГОТОВО |
| FIX-5 | 14-step маппинг отдаёт speed > 126 | Низкий | ГОТОВО |
| FIX-6 | Разделяемые данные `s_cfg` без синхронизации | Средний | ОТЛОЖЕНО (нужен отдельный батч) |
| FIX-7 | DCC-колбэк делает NVS/FS и блокируется на flash | Средний | ОТЛОЖЕНО |
| FIX-8 | Аудио: чтение по 1 сэмплу, клиппинг по голосам | Средний | ОТЛОЖЕНО |
| FIX-9 | Flash-wear: commit NVS на каждый CV при service-mode | Средний | ОТЛОЖЕНО |
| FIX-10 | Нет task watchdog | Высокий | ОТЛОЖЕНО |
| FIX-11 | `storage_benchmark()` — разрушительный мёртвый код | Низкий | ОТЛОЖЕНО |

---

## FIX-1 Non-blocking service-mode ACK

- Статус: ГОТОВО
- Файлы: `components/dcc/src/dcc.c`, `test/test_dcc/test_dcc.c`,
  `test_libs/teststubs/stubs.c`, `ARCHITECTURE.md`
- Проблема:
  - `dcc_service_ack()` вызывается из задачи разбора DCC (`dcc_task`, prio 10) и
    делает `vTaskDelay(6 мс)`. На эти 6 мс задача не читает очередь
    полупериодов (256 элементов, ~25 мс при 10 кГц) — серия service-команд может
    переполнить очередь и сбросить кадрирование.
  - Пин ACK (`PIN_ACK_LOAD`) переконфигурируется (`gpio_config`) на каждый ACK.
- Решение:
  - Пин ACK конфигурируется один раз в `dcc_init()`.
  - `dcc_service_ack()` только кладёт токен в очередь `s_ack_queue` (не блокирует
    декодер).
  - Отдельная задача `dcc_ack` (prio 5) выдаёт импульс: high → 6 мс → low.
  - Если очередь не создана (хост-тесты / ранний вызов) — синхронный fallback,
    как раньше (сохраняет поведение и покрытие тестами).
- Проверка:
  - `test\run_tests.ps1` — все 13 наборов PASSED; добавлены тесты
    `test_dcc_service_ack_queued`, `test_dcc_service_ack_queue_full`,
    `test_dcc_ack_task_pulse`, `test_dcc_init_ack_queue_fail`,
    `test_dcc_init_ack_task_fail`.
  - `pio run -e esp32-s3-devkitc-1` — SUCCESS (Flash 818 181 Б, 41.6 %).

## FIX-2 Подтверждение команды PROV

- Статус: ГОТОВО
- Файлы: `components/provision/src/provision.c`, `provision_sounds.ps1`,
  `flash_standalone.ps1`, `test/test_provision/test_provision.c`,
  `ARCHITECTURE.md`
- Проблема: любая строка `PROV` по UART0/USB-Serial-JTAG стирает внешнюю NOR
  (`storage_format()`) со всеми звуками — без подтверждения.
- Решение: перед стиранием `provision_run()` шлёт `PROV-CONFIRM?` и ждёт строку
  `PROV-CONFIRM` (таймаут 10 с), иначе шлёт `PROV-ABORT` и ничего не стирает.
  Хостовые скрипты (`provision_sounds.ps1`, `flash_standalone.ps1`) обновлены:
  после `PROV-CONFIRM?` отвечают `PROV-CONFIRM` и ждут `PROV-OK`.
- Проверка: `test\run_tests.ps1` — `test_provision` 29 тестов PASS; добавлен
  `test_provision_run_requires_confirm` (без подтверждения
  `storage_format()` не вызывается). Все существующие сценарии провижининга
  адаптированы под рукопожатие.

## FIX-3 OTA rollback

- Статус: ГОТОВО
- Файлы: `sdkconfig.defaults`, `sdkconfig.esp32-s3-devkitc-1`, `main/app_main.c`,
  `ARCHITECTURE.md`
- Проблема: `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` выключен, образ не
  подтверждается — неудачная OTA-прошивка не откатывается.
- Решение: `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` (и производный
  `CONFIG_APP_ROLLBACK_ENABLE=y`); в конце `app_main`
  `esp_ota_mark_app_valid_cancel_rollback()` (no-op при обычной USB-прошивке,
  когда состояние не PENDING_VERIFY).
- Проверка: `pio run` — SUCCESS, `sdkconfig.esp32-s3-devkitc-1` сохраняет оба
  флага (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`).

## FIX-4 Мелкие правки web.c

- Статус: ГОТОВО
- Файлы: `components/web/src/web.c`
- Проблема: `functions_get()` читал `s_fn` без `s_func_mutex` (гонка с
  DCC-колбэком); `log_get()` мог обрезать JSON внутри строки.
- Решение: `functions_get()` снимает снимок `s_fn` под `s_func_mutex`;
  `log_get()` проверяет место под запись (400 Б) ДО её добавления.
- Проверка: `test\run_tests.ps1` — `test_web` 120 тестов PASS (весь набор
  PASSED).

## FIX-5 Клэмп 14-step скорости

- Статус: ГОТОВО
- Файлы: `components/dcc/src/dcc.c`
- Проблема: расчёт 14-step мог дать speed 135; клампился только в
  `motor_set_speed`, но в `web_motion_changed()` уходил 135.
- Решение: clamp 0..126 в `dcc.c`.
- Проверка: `test\run_tests.ps1` — `test_dcc` 61 тест PASS.

## FIX-6..FIX-11

Отложены на следующие батчи (см. сводку). Причина — объём и необходимость
отдельного тестирования (синхронизация состояния, рефакторинг задач,
watchdog).

---

## Итог батча 1

- Изменённые файлы:
  - `components/dcc/src/dcc.c`
  - `components/provision/src/provision.c`
  - `components/web/src/web.c`
  - `main/app_main.c`
  - `sdkconfig.defaults`, `sdkconfig.esp32-s3-devkitc-1`
  - `provision_sounds.ps1`, `flash_standalone.ps1`
  - `ARCHITECTURE.md`
- Тесты:
  - `test/test_dcc/test_dcc.c` — +5 тестов (ACK queue/worker/init failures)
  - `test/test_provision/test_provision.c` — +1 тест (`requires_confirm`),
    все сценарии адаптированы под рукопожатие
  - `test_libs/teststubs/stubs.c` — счётчики отказов queue/task create
- Проверка:
  - `test\run_tests.ps1` — **ALL TEST SUITES PASSED**, 433 теста, 0 ошибок.
  - `pio run -e esp32-s3-devkitc-1` — **SUCCESS**; Flash 818 161 Б (41.6 %),
    RAM 50 400 Б (15.4 %).

Ограничение: проверка на железе не выполнялась (нет подключённого модуля).
Изменения FIX-1 (тайминг ACK), FIX-2 (рукопожатие провижининга) и FIX-3
(rollback) следует проверить на реальной плате перед релизом:

- service-mode программирование (ACK и отсутствие потерь пакетов);
- `flash_firmware_and_sounds.bat` (провижининг со скриптом);
- OTA-обновление и принудительный откат (сброс на новой прошивке до
  подтверждения).

---

## Батч 2 — исправления по итогам ревью

Ревью незакоммиченных изменений (`/review uncommitted`) нашло три замечания;
все исправлены.

### REV-1 [WARNING] Подтверждение PROV не принимало CRLF

- Статус: ГОТОВО
- Файлы: `components/provision/src/provision.c`,
  `test/test_provision/test_provision.c`
- Проблема: `read_line()` не убирает `\r`, а проверка была точным `strcmp` —
  хост, завершающий строку CRLF/CR (Windows-терминал, PuTTY, `echo`), получал
  `PROV-ABORT` и провижининг не стартовал. Остальные команды (`PUT`/`FW`/`DONE`)
  к CR устойчивы (префикс/`sscanf`), так что это регрессия именно нового кода.
- Решение: перед сравнением срезать завершающий `\r`; добавлен тест
  `test_provision_run_confirm_crlf`.

### REV-2 [SUGGESTION] Очередь ACK освобождалась при работающей DCC-задаче

- Статус: ГОТОВО
- Файлы: `components/dcc/src/dcc.c`, `test/test_dcc/test_dcc.c`
- Проблема: при неудачном создании `dcc_ack` очередь `s_ack_queue` удалялась,
  хотя уже запущенная `dcc_task` могла в этот момент вызвать `xQueueSend` по
  освобождённому дескриптору (use-after-free).
- Решение: очередь больше не освобождается; добавлен флаг `s_ack_worker`
  (рабочая задача запущена), и `dcc_service_ack()` уходит в inline-ветку, если
  воркера нет. Тест `test_dcc_init_ack_task_fail` проверяет, что очередь
  остаётся, флаг сброшен, а ACK выдаётся inline.

### REV-3 [SUGGESTION] Fallback ACK не конфигурировал вывод

- Статус: ГОТОВО
- Файлы: `components/dcc/src/dcc.c`
- Проблема: после выноса `gpio_config` в `dcc_init()` fallback-ветка
  `dcc_service_ack()` (воркер не запущен / ранний вызов) дёргала вывод, который
  мог остаться входом, и ACK не выдавался.
- Решение: `ack_pin_config()` (идемпотентно) вызывается и в `dcc_init()`, и в
  fallback.

## Итог батча 2

- Проверка:
  - `test\run_tests.ps1` — **ALL TEST SUITES PASSED** (433 → 434 теста: +1 CRLF).
  - `pio run -e esp32-s3-devkitc-1` — **SUCCESS**; Flash 818 245 Б (41.6 %),
    RAM 50 392 Б (15.4 %).
- Открытых замечаний ревью нет; REV-1..REV-3 закрыты.

---

## Батч 3 — HIL по USB: команда SELFTEST + host-runner

- Статус: ГОТОВО (проверено на железе)
- Файлы:
  - `components/selftest/` (новый: `include/selftest.h`,
    `src/selftest.c`, `CMakeLists.txt`)
  - `components/auxio/include/auxio.h`, `components/auxio/src/auxio.c`
    (read-only `auxio_get_enabled()`)
  - `components/provision/src/provision.c`, `components/provision/CMakeLists.txt`
    (обработка команды `SELFTEST` в консоли)
  - `test/test_selftest/` (новый набор), `test/test_auxio/test_auxio.c`,
    `test/test_provision/test_provision.c`, `test_libs/teststubs/stubs.c`,
    `test_libs/teststubs/include/esp_system.h`
  - `test/run_tests.ps1`, `test/coverage.ps1` (+ `-Icomponents/selftest/include`,
    + набор `test_selftest`)
  - `test/hil/run_hil.ps1` (новый host-runner)
  - `ARCHITECTURE.md`
- Что сделано:
  - Команда `SELFTEST` по UART0/USB-Serial-JTAG запускает неразрушающие проверки:
    heap, pinmap, CV-хранилище (`settings_cv_read` и границы), NVS (scratch-ключ),
    LittleFS (scratch-файл), ADC-путь, громкость аудио, состояние AUX, DCC-API.
    Вывод машинночитаемый: `SELFTEST-BEGIN` / `TEST <name> PASS|FAIL|SKIP` /
    `SELFTEST-END <passed>/<total>`. Мотор/звук/настройки/хранилище не меняются.
  - `test/hil/run_hil.ps1` открывает порт (авто-детект VID_303A/PID_1001 или
    `-Port COMx`), шлёт `SELFTEST`, опц. `BEMF?`, парсит отчёт и возвращает код
    0/1 (FAIL или нет ответа → 1; SKIP допустим).
  - `auxio_get_enabled()` — read-only геттер для самотеста (и диагностики).
- Проверка:
  - `test\run_tests.ps1` — **ALL TEST SUITES PASSED**, 14 наборов, **444 теста**
    (новый `test_selftest` — 8; `test_auxio` 23→24; `test_provision` 30→31).
  - `test\coverage.ps1` — first-party **100,0 %** строк (4180/4180).
  - `pio run -e esp32-s3-devkitc-1` — **SUCCESS**; Flash 819 089 Б (41.7 %),
    RAM 50 384 Б (15.4 %).

### Как запускать HIL

```
powershell -ExecutionPolicy Bypass -File test\hil\run_hil.ps1
powershell -ExecutionPolicy Bypass -File test\hil\run_hil.ps1 -Port COM5
powershell -ExecutionPolicy Bypass -File test\hil\run_hil.ps1 -SkipBemf
```

### Результат на железе (ESP32-S3, USB-Serial-JTAG, COM13)

- Прошивка залита (`pio run -t upload --upload-port COM13`), `pio`-upload SUCCESS.
- `test\hil\run_hil.ps1 -Port COM13` → **HIL: PASSED** (exit 0):
  `SELFTEST-END 9/9` (heap, pinmap, settings_cv, nvs, storage, adc, audio, auxio,
  dcc — все PASS), `BEMF-OK 10 12 62 24 160 36 230 48 300 60 373 72 483 84 574 96
  661 108 687 126 725`. Внешняя NOR смонтирована (storage не SKIP), список треков
  в NVS — 17 слотов.
- Аппаратный сброс → boot-лог: `Storage: external NOR`, `track list present in NVS
  (17 slots)`, **`OTA image confirmed`**, `Boot complete`. Повторный HIL после
  сброса → снова **PASSED 9/9**: rollback-защита образ не откатила (нет boot-loop),
  FIX-3 подтверждён на железе.
- Итог: SELFTEST/HIL и защита rollback работают на реальной плате. Осталось при
  желании проверить на железе FIX-1 (service-mode ACK требует командной станции
  на рельсах) и FIX-2 (полный `flash_firmware_and_sounds.bat`).

---

## Батч 4 — Actuating HIL-команды (AUX / звук / мотор)

- Статус: ГОТОВО (проверено на железе)
- Файлы:
  - `components/selftest/include/selftest.h`, `src/selftest.c`
    (`selftest_act_aux/act_sound/act_motor`, границы по времени)
  - `components/provision/src/provision.c` (парсер `HIL-AUX/HIL-SOUND/HIL-MOTOR`)
  - `test/test_selftest/test_selftest.c` (11 тестов),
    `test/test_provision/test_provision.c` (34 теста)
  - `test/hil/run_hil.ps1` (флаги `-Actuate`, `-MotorSpeed`, `-AuxChannel`,
    `-SoundSlot`)
  - `ARCHITECTURE.md`
- Что сделано:
  - `HIL-AUX <ch> <ms>` — включает AUX-канал на `ms` (20..5000) и возвращает
    прежнее состояние; `HIL-SOUND <slot> <ms>` — проигрывает слот и
    останавливает тестовый голос; `HIL-MOTOR <spd> <ms>` — прогон мотора
    (`spd` 1..60) и стоп. Ответы `HIL-*-OK` / `HIL-*-ERR`.
  - В `run_hil.ps1` активация только по явным флагам: `-Actuate` (AUX+звук),
    `-MotorSpeed N` (мотор). По умолчанию тест неразрушающий.
- Проверка (на железе, ESP32-S3, COM13):
  - Прошивка залита; `run_hil.ps1 -Actuate` → **HIL: PASSED**:
    `SELFTEST 9/9`, `BEMF-OK`, `[ OK ] AUX 2`, `[ OK ] sound slot 1`.
  - `run_hil.ps1 -MotorSpeed 20` → **HIL: PASSED**: `[ OK ] motor`
    (мотор физически не подключён — проверен путь команды/PWM, не вращение).
  - `test\run_tests.ps1` — **ALL TEST SUITES PASSED**, **450 тестов**.
  - `test\coverage.ps1` — first-party **100,0 %** (4243/4243).
  - `pio run` — **SUCCESS**; Flash 820 017 Б (41.7 %), RAM 50 384 Б (15.4 %).

### Что ещё не проверено на железе

- Фактическое вращение мотора и замкнутый контур BEMF под нагрузкой (мотор не
  подключён).
- DCC-функции F0..F28 с командной станции (рельсы/пульт) и service-mode ACK
  (FIX-1).
- Веб-UI / REST API по SoftAP и OTA через веб.

---

## Батч 5 — Прогон всех F и AUX + веб/REST по SoftAP

- Статус: ГОТОВО (проверено на железе)
- Файлы:
  - `components/selftest/include/selftest.h`, `src/selftest.c`
    (`selftest_act_function`, `selftest_act_fn_sweep`, `selftest_act_aux_sweep`)
  - `components/provision/src/provision.c` (команды `HIL-FN`, `HIL-FN-SWEEP`,
    `HIL-AUX-SWEEP`)
  - `components/selftest/CMakeLists.txt` (+ `web` в REQUIRES: прогон F идёт
    через реальный `web_apply_function`)
  - `test/test_selftest/test_selftest.c` (14 тестов),
    `test/test_provision/test_provision.c` (34 теста)
  - `test/hil/run_hil.ps1` (+ `-Sweep`), `test/hil/run_hil_web.ps1` (новый)
  - `ARCHITECTURE.md`
- Что сделано:
  - `selftest_act_fn_sweep()` нажимает все F0..F28 (on→off) через
    `web_apply_function` — ту же функцию, что вызывают DCC и кнопки веб-UI;
    `selftest_act_aux_sweep()` включает/возвращает все 9 AUX.
  - `run_hil_web.ps1`: подключает ПК к SoftAP, прогоняет все safe REST-эндпоинты
    (read + POST: F0..F28, AUX effect/cfg, звук, громкость, CV write, func-map,
    track category, мотор, mode, control source, device, BEMF use, log) и
    возвращает изменённые настройки; опасные (`/api/reset`, `/api/ota/update`,
    `/api/wifi` POST, `/api/bemf/calibrate`, upload/delete) не вызываются.
- Замечание по сборке: после добавления зависимости `selftest → web` понадобился
  `pio run -t fullclean` — иначе использовался устаревший граф зависимостей
  (`web.h: No such file`).
- Проверка (на железе, ESP32-S3, COM13):
  - `run_hil.ps1 -Sweep`: `SELFTEST 9/9`, `BEMF-OK`,
    `HIL-FN-SWEEP-OK 29` (все F0..F28), `HIL-AUX-SWEEP-OK 9` (все AUX) —
    **HIL: PASSED**.
  - `run_hil_web.ps1 -RestoreSsid AlmaHome` — **WEB HIL: PASSED (45 проверок)**:
    все GET-эндпоинты OK; кнопки F0..F28 через `/api/function` OK; AUX0..8
    через `/api/aux/effect` OK; запись `/api/aux/cfg` для всех 9 каналов OK;
    звук/громкость/play/stop, CV write, func-map, track category, motor stop,
    device, mode, BEMF use — OK; control source возвращён; все функции после
    прогона сняты (`all functions released`). ПК возвращён на `AlmaHome`.
  - `test\run_tests.ps1` — **ALL TEST SUITES PASSED**, **453 теста**.
  - `test\coverage.ps1` — first-party **100,0 %** (4271/4271).
  - `pio run` — **SUCCESS**; Flash 820 629 Б (41.7 %), RAM 50 384 Б (15.4 %).

### Что ещё не проверено

- OTA через веб (`/api/ota/update`) и factory reset (`/api/reset`) — намеренно
  не запускались (разрушительные).
- Реальное вращение мотора под нагрузкой/BEMF и DCC с пульта (нет мотора и
  командной станции).

---

## Батч 6 — исправления по итогам код-ревью (агенты `.kilo/agent/review-*`)

- Статус: ГОТОВО (сборка + тесты + покрытие). Не проверено на железе.
- Источник: `.kilo/plans/1790518367801-firmware-code-review-agents-and-fixes.md`,
  отчёт: `CODE_REVIEW_REPORT.md`.
- Проверка:
  - `test\run_tests.ps1` — ALL TEST SUITES PASSED (14 наборов, 473 теста).
  - `test\coverage.ps1` — first-party **100,0 %** (4364/4364 строк).
  - `pio run -e esp32-s3-devkitc-1` — **SUCCESS**; RAM 50 416 Б (15.4 %),
    Flash 822 281 Б (41.8 %).

### Исправлено

| ID | Файлы | Что сделано |
|----|-------|-------------|
| A1, A2 | `dcc.c` | Service Mode: `[instr,CvLow,data,ck]` без адресного байта; вход через reset с длинной преамбулой (`s_service_mode`); адреса 112..126 больше не трактуются как сервис; bit-write выдаёт ACK |
| A3 | `motor.c` | Клэмп таблицы CV67..CV94 (нет unsigned-wrap) + ограничение `LEDC_MAX` |
| A4 | `motor.c` | Конец калибровки: applied/ramp/kick/PID/BEMF обнуляются — нет проброса по CV4 |
| A5 | `motor.c`, `motor.h`, `app_main.c` | `motor_emergency_stop()` + `motor_last_tick_us()`; `safety_task` глушит мост при зависании motor-задачи (>200 мс); reset/estop/CV11 → emergency stop |
| A6 | `motor.c` | Реверс при движении: торможение до нуля (CV4), затем смена полярности |
| A7 | `storage.c` | Раздел внешней NOR = `min(chip_size, 16 МБ)`; при неопределённом/малом чипе backend NONE |
| A9 | `track_manifest.c`, `settings.h`, `settings.c` | Атомарная запись манифеста (tmp + rename), проверка кодов, возврат `esp_err_t` |
| A10 | `settings.c` | `settings_cv_write`/`settings_cv_reset_to_factory` под `s_lock` |
| B2 | `web_util.c` | `json_escape` экранирует CR/TAB, прочие C0 → `?` (валидный JSON) |
| B5 | `settings.c` | `settings_bemf_cal_load` проверяет точную длину blob |
| B6 | `settings.c` | `tracks` load: кратность длины + NUL-терминация; save: кламп `count` (также cats/func_map/aux_cfg) |
| B8 | `app_main.c` | `recover_tracks_from_storage()` до `web_init()` (иначе категории/map остаются дефолтными) |
| B11 | `auxio.c` | Mars: треугольник клампится к 255 при нечётном периоде |
| B13 | `pinmap.c` | Отвергаются GPIO22, дубли и значения вне 0..48 |
| B14 | `motor.c` | `s_pid_prev_error` затухает при недоступном BEMF (нет derivative kick) |
| B15 | `provision.c` | `s_uart_driver_installed` ставится только при успехе install |
| B16 | `settings.c` | `nvs_read_str` подставляет дефолт при любом не-OK чтении |

Затронутые тесты: `test_dcc`, `test_motor`, `test_settings`, `test_storage`,
`test_web_util`, `test_pinmap`, `test_auxio`, `test_track_manifest`,
`test_provision`, `test_libs/teststubs/stubs.c` (новый
`mock_partition_registered_size`).

### Статус после батча 7

Все пункты батча 6 закрыты (см. батч 7 ниже).

---

## Батч 7 — закрытие отложенных пунктов

- Статус: ГОТОВО (сборка + тесты + покрытие). Не проверено на железе.
- Проверка:
  - `test\run_tests.ps1` — ALL TEST SUITES PASSED (14 наборов, 471 тест).
  - `test\coverage.ps1` — first-party **100,0 %** (4414/4414 строк).
  - `pio run -e esp32-s3-devkitc-1` — **SUCCESS**; RAM 50 416 Б (15.4 %),
    Flash 823 505 Б (41.9 %).

| ID | Файлы | Что сделано |
|----|-------|-------------|
| B1 | — (откат) | Пароль Wi-Fi по умолчанию **не задаётся** (AP открытый) — как и было; автогенерация пароля отменена. Пользователь устанавливает пароль сам (веб `/api/wifi` POST). Открытый AP и отсутствие аутентификации на OTA/reset/delete — принятое поведение по требованию, а не дефект |
| A8 | `provision.c`, `web.c/.h` | Перед `storage_format()`: `motor_emergency_stop()`, `audio_stop_all()`, задержка 100 мс и отказ (`PROV-ERR busy`), если активна web-передача (`web_fs_busy()` = upload/OTA) |
| B3 | `web.c` | `audio_tracks_get`: `tracks[]` и `json[8192]` перенесены из стека в heap (нет ~12.6 КБ на стеке httpd) с освобождением на всех путях |
| B4 | `web.c` | `web_motion_changed`, `web_func_map_set`, `web_get_function_state`, `func_apply_output_locked` работают под `s_func_mutex` |
| B7 | `audio.c` | EOF закрывает `FILE*`; `voice_start` проверяет PCM16, `sample_rate` 1..192000, каналы 1..2; RIFF padding через `wav_skip`; отклоняются размеры > INT32_MAX (нет обратного fseek) |
| B9 | `web.c` | `pipe_upload` освобождает queue/sem на всех ветках ошибок; отсутствующий слот (0) авто-назначается свободным (1..20); reboot-хендлеры (wifi/reset/factory/OTA) глушат мотор и аудио перед `esp_restart()` |
| B10 | `dcc.c` | 64-бит timestamp под `s_ts_mux` (`mark_packet`/`dcc_last_packet_us`); `s_cfg` читается под `s_cfg_mux` (`address_matches`, `cfg_speed_mode_14`, `cfg_consist_reverse`) |
| B12 | `audio.c` | stdio-буфер голоса 8192 → 2048; `setvbuf` проверяется с fallback на `_IONBF` |
| FIX-6 | `settings.c`/`web.c`/`dcc.c` | Синхронизация общего состояния закрыта в A10/B4/B10 |
| FIX-7, FIX-9 | `settings.c/.h`, `app_main.c` | DCC и сервисные записи CV коммитятся отложенно (`settings_cv_commit_deferred` + `settings_pending_flush`) — flash-commit не выполняется в real-time задаче и не на каждый CV |
| FIX-10 | `sdkconfig.defaults`, `app_main.c` | Включён Task Watchdog (`CONFIG_ESP_TASK_WDT_EN/PANIC`, таймаут 10 с); `safety_task` подписан (`esp_task_wdt_add`) и сбрасывает WDT каждый цикл |
| FIX-11 | `storage.c/.h`, `test_storage.c` | Удалён разрушительный `storage_benchmark` (raw erase @8 МБ + reformat LittleFS) вместе с тестами |
| LOW | `motor.c`, `auxio.c`, `web.c`, `web_stubs.c` | `apply_pwm` сначала выключает активный канал (нет both-high); kickstart в table-mode берётся из CV94, не CV5; `motor_bemf_coast_read` восстанавливает duty; Mars-треугольник клампится к 255; `used += snprintf` заменён на `buf_appendf` в `bemf_cal_get`/`bemf_base_get`/`functions_get`; OTA валидирует WAV перед привязкой трека; `/progress` сообщает реальный `ota`; DNS-задача завершается при auto-off AP (SO_RCVTIMEO + `s_dns_run`); firebox подмешивает `idx` в seed |

Сознательно не изменено:
- Пароль Wi-Fi по умолчанию: AP открытый, если пользователь не задал пароль
  (автогенерация отменена по требованию). Пользователь задаёт пароль сам через
  веб; после этого AP работает как WPA2.
- Размер coredump-раздела (128 КБ): это последний раздел, заканчивается ровно на
  4 МБ, расширять нечем без уменьшения OTA. Вместо этого уменьшены стеки/буферы.
- speed=1 duty=0: следствие CV2 (Vstart) = 0 — это намеренная настройка кривой,
  а не дефект.
- Аппаратная проверка (HIL, service-mode с командной станции, реверс под
  нагрузкой, поведение Task Watchdog на реальных flash-операциях).

