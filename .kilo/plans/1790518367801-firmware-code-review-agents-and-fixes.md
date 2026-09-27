# План: агенты код-ревью + отчёт + план исправлений (прошивка Soft_Decoder_V3)

## 1. Цель

1. Создать постоянные профили агентов ревью в `.kilo/agent/` (повторяемое ревью).
2. Зафиксировать результат уже проведённого анализа (отчёт по дефектам).
3. Дать поэтапный план исправлений найденных дефектов (код здесь не меняется —
   исполняет implementation-агент).

Рабочая область: `C:\Oleg\OwnCloud\Oleg\KIRILL\Decoder_DCC\Soft_Decoder_V3`.
Код прошивки — `firmware/` (ESP-IDF 5.1.4, ESP32-S3, C11, PlatformIO).

## 2. Границы

- Ревью уже выполнено по всему first-party коду: `components/*` (dcc, motor,
  web, settings, storage, audio, auxio, track, pinmap, provision, selftest) и
  `main/app_main.c`. Сторонние компоненты (`esp_littlefs`) и `test_libs/` не
  ревьюились.
- Известные отложенные пункты (`FIXES_LOG.md` FIX-6..FIX-11) не считаются
  новыми; там, где найдено, что они серьёзнее описанного, это отмечено.
- Агенты ревью — read-only (не редактируют исходники). Исправления выполняет
  implementation-агент по разделу 5.

## 3. Deliverable 1 — агенты ревью в `.kilo/agent/`

Создать в корне репозитория (не в `firmware/.kilo/`): `.kilo/agent/*.md`.
Формат: YAML-frontmatter (`description`, `mode: subagent`, `permission`) + промпт.
Модель не переопределять (наследуется). Общий контракт промпта:

- Только чтение; ничего не менять. Разрешено запускать `test\run_tests.ps1` и
  `pio run` при необходимости.
- Искать конкретные дефекты, не стиль. Формат вывода: список
  `[SEVERITY: CRITICAL/HIGH/MEDIUM/LOW/INFO] file:line — title`,
  затем `Evidence` (короткий сниппет + сценарий отказа) и `Suggested fix`.
  Завершать списком `Areas reviewed and found OK`.
- Ссылаться на строки точно; не репортить FIX-6..FIX-11 как новые.
- Игнорировать сгенерированные артефакты: `web_html.h`, `.pio/`, `components.zip`.

Файлы и зоны ответственности:

| Файл агента | Зона | Чек-лист (Bug classes) |
|---|---|---|
| `.kilo/agent/review-dcc.md` | `components/dcc`, `components/track`, DCC-колбэки в `main/app_main.c` | IRAM/ISR-safety; классификация полупериодов и рамминг; NMRA-адресация/шаги/функции; checksum; границы буферов; CV11-таймаут и wrap; гонки dcc/dcc_ack/ISR |
| `.kilo/agent/review-motor.md` | `components/motor` | PWM/Hi-Z окно, ADC; PID windup/derivative kick/NaN; деление на 0; знак BEMF; fail-safe стоп; реверс на полном duty; bounds 0..126/0..1023; гонки motor/bemf_cal |
| `.kilo/agent/review-web.md` | `components/web`, `web_ui.html` | длина тела/query, переполнения буферов, JSON-escape, парсинг чисел, path traversal, OTA-контейнер, auth/CSRF, утечки на ошибках, shared-state, стек httpd |
| `.kilo/agent/review-storage.md` | `components/settings`, `components/storage` | NVS blob длина/CRC/версия, границы CV/слотов/AUX, атомарность манифеста, поведение без внешней NOR, гонки deferred-flush |
| `.kilo/agent/review-media.md` | `components/audio`, `components/auxio`, `components/pinmap` | WAV-парсинг и ресемплер, границы эффектов, дескрипторы, overflow/underflow, валидация pinmap |
| `.kilo/agent/review-rtos.md` | `main/app_main.c` + все `*/src/*.c` (кросс-домен), `sdkconfig*`, `partitions.csv`, `platformio.ini` | WDT/starvation/priority inversion, стеки задач, инвентарь shared-state, brownout, heap/PSRAM, порядок инициализации |
| `.kilo/agent/review-lead.md` | агрегатор | Запускает 6 доменных агентов, дедуплицирует, присваивает ID/приоритет, проверяет высокие находки по коду, формирует единый отчёт |

Шаблон frontmatter (одинаковый для 6 доменных агентов, меняются `description`
и промпт):

```yaml
---
description: Read-only code reviewer for <ZONE>. Use for reviewing <FILES>.
mode: subagent
permission:
  edit: deny
  webfetch: deny
  websearch: deny
---
<промпт с чек-листом и форматом вывода>
```

Опционально (для повторяемости): `.kilo/command/review-firmware.md` —
команда-оркестратор (`agent: review-lead`, `subtask: true`), запускающая
сводное ревью, и `mode: primary` агент-агрегатор для ручного запуска.

## 4. Deliverable 2 — отчёт по дефектам (результаты анализа)

Проверено по коду автором плана (Verified=Да) либо получено доменными агентами
(Verified=нет, требует чтения при исправлении). Полный текст evidence — в
задачах-ревьюерах; здесь сведено и приоритезировано.

### 4.1 Critical

| ID | Файл:строка | Проблема | Verified |
|---|---|---|---|
| A1 | `dcc.c:161,174` | Service Mode читает инструкцию из `packet[1]` (`idx=1`), а не `packet[0]` — NMRA Direct Mode пакет `instr,CvLow,data,ck` не декодируется: записи/верификация/бит-оп CV с программатора молча игнорируются. Тесты маскируют, т.к. подают синтетический префикс `0xFF`. | Да |
| A3 | `motor.c:145-150` | Ветка таблицы CV67..CV94 не защищена от немонотонных CV: `(hi-lo)` при `hi<lo` даёт unsigned-wrap → произвольный duty (пример CV67=255,CV68=0, speed=1 → ≈801/1023). Достижимо через `/api/cv/write`. | Да |
| A4 | `motor.c:324-354,601-603,622-633` | По завершении калибровки `s_applied_speed=126`, затем `motor_stop()` (только `target=0`); при CV4≠0 идёт разгон вниз от 126 с фактическим PWM ≈ полного — «проброс» после калибровки. Плюс не сброшены `s_pid_integral/s_pid_prev_error`. | Частично |
| B1 | `web.c:662`, `web.c:2447-2456` | SoftAP по умолчанию `WIFI_AUTH_OPEN` (пароль по умолчанию пуст), а `POST /api/ota/update`, `/api/reset`, `/api/wifi/reset`, `/api/audio/delete` доступны без auth → полный захват устройства/wipe любым клиентом; нет CSRF/Origin-проверки. | Да |

### 4.2 High

| ID | Файл:строка | Проблема | Verified |
|---|---|---|---|
| A2 | `dcc.c:153-154,219` | Короткие адреса 112..126 при preamble≥20 ошибочно трактуются как Service Mode и отбрасываются — декодер с таким адресом «мёртв» для скорости/функций. | Да |
| A5 | `motor.c:457-460,575-578` | Нет fail-safe: PWM обнуляет только живая `motor_task`; `motor_stop()` — лишь запрос. Зависание/звёздная голодовка задачи → мотор остаётся на последнем duty (убегание). | Да |
| A6 | `motor.c:354` | Смена направления применяется мгновенно при полном duty (без ramp-to-zero) → броcок тока/brownout/срабатывание аппаратной защиты. | Да |
| A7 | `storage.c:26,68-75` | Размер внешнего раздела жёстко 16 МБ, `esp_flash_get_size` игнорируется → на 8 МБ чипе/при ошибке определения разметка уходит за физический конец (сбои/порча FS). | Да |
| A8 | `provision.c:521,733` | `storage_format()` размонтирует LittleFS, пока `audio_mix`/httpd/upload/OTA могут держать файлы → use-after-free/порча FS. Защита `s_provisioning` только от повторного входа. | Частично |
| A9 | `track_manifest.c:81-110` | Манифест пишется неатомарно (`fopen "w"`), все ошибки записи отброшены (`void`), при потере питания/ENOSPC остаётся усечённый «валидный» файл — теряется единственная копия восстановления. | Да |
| A10 | `settings.c:250-275` | `settings_cv_write` пишет `s_cv[]` без `s_lock`, а reset делает `memset` всей таблицы без лока; читатели (motor 10 мс, track, dcc) видят полу-обнулённую таблицу. | Да |

### 4.3 Medium

| ID | Файл:строка | Проблема | Verified |
|---|---|---|---|
| B2 | `web_util.c:97-107` | `json_escape` не экранирует C0-управляющие (CR, TAB, 0x00-0x1F) → невалидный JSON из имени/SSID/меток; `JSON.parse` в UI падает. | Да |
| B3 | `web.c:1105-1110` | `audio_tracks_get`: `tracks[20]` (~3.9 КБ) + `json[8192]` (+`f[256]`,`l[128]`) ≈ 12.6 КБ на стеке httpd 16 КБ → переполнение стека (remote DoS через метки). | Да |
| B4 | `web.c:270-308,442-477` | `s_func_map`, `s_motion_*`, `s_func_last_mask`, `s_fn` мутируются/читаются без `s_func_mutex` (пути DCC-колбэка и map-set) → AUX может «залипнуть». | Да |
| B5 | `settings.c:476-490` | `settings_bemf_cal_load` не проверяет `len == sizeof(...)`; при коротком legacy-blob вернётся OK с мусором → мусорная кривая в PID. | Да |
| B6 | `settings.c:319-347` | `tracks` blob: нет проверки кратности/лимита длины, нет NUL-терминации `file`/`label`; `_save` не клампит `count` к `SETTINGS_MAX_TRACKS` (тихая потеря списка). | Да |
| B7 | `audio.c:224-231,237-287,167,283` | EOF не закрывает `FILE*` (накопление дескрипторов); `voice_start` не проверяет `format/bits/sample_rate` (играет не-PCM16 как 16-бит); RIFF-walk игнорирует word-padding на нечётных чанках → валидный WAV отвергается. | Да |
| B8 | `app_main.c:219,226,247` | `web_init()` вызывается до `recover_tracks_from_storage()`, поэтому категории/func-map из манифеста не подхватываются в RAM до перезагрузки; OTA-образ подтверждается даже если web/audio/storage/track не поднялись. | Да |
| B9 | `web.c:1266-1285,1378-1379,1838-1872,2277-2279` | Утечки queue/semaphore при неудаче upload; `slot` парсится без проверки диапазона; reboot-хендлеры не глушат мотор/AUX перед `esp_restart()`. | Да |
| B10 | `dcc.c:57,623-625`,`app_main.c:134`; `dcc.c:608-612,108-124` | 64-битный `s_last_packet_us` читается cross-core без синхронизации (torn read → ложный/пропущенный CV11-timeout); `s_cfg` публикуется писателем через `portMUX`, читатели (CPU1) лок не берут. | Да |
| B11 | `auxio.c:116`; `auxio.c:168` | Mars: `(pos*255)/half` может превысить 255 и обернуться `uint8_t` при нечётном периоде ≤511 → провал на пике; firebox мерцает синхронно (seed от `now_ms`, `idx` не учтён). | Да |
| B12 | `audio.c:249`,`sdkconfig:903` | 20 голосов × 8 КБ stdio-буфера `setvbuf` = до 160 КБ внутренней DRAM (аллокации <16 КБ не идут в PSRAM), `setvbuf` не проверяется → риск OOM при многих голосах. | Частично |
| B13 | `pinmap.c:11-37` | Не отвергается GPIO22 (нет на S3); нет проверки дублей и диапазона 0..48 → конфликт выводов проходит валидацию. | Да |
| B14 | `motor.c:419-421,449-453` | Derivative kick: `s_pid_prev_error` не сбрасывается при недоступности BEMF → одиночный выброс duty до ~800. | Частично |
| B15 | `provision.c:521`/FS | См. A8; плюс `s_uart_driver_installed` ставится `true` даже при неудаче `uart_driver_install` — консоль молча не работает. | Да |
| B16 | `settings.c:84-94` | `nvs_read_str` подменяет дефолт только при `NOT_FOUND`; при `INVALID_LENGTH` поле остаётся пустым (SSID/имя теряют дефолт). | Да |

### 4.4 Low / Info (выборочно, чинить пачкой)

- `dcc.c:458-461` gap не сбрасывает рамминг парсера; `dcc.c:194-217` бит-запись
  в Service Mode не выдаёт ACK (NMRA S-9.2.3); `dcc.c:176-183` ACK безусловен для
  невалидных CV.
- `motor.c:185-193` на переходе reverse→forward возможен короткий both-high
  (для DRV8870 = brake, для дискретного моста — shoot-through); `motor.c:373-376`
  кикстарт берётся из CV5 даже в табличном режиме; `motor.c:784-803` `BEMF-COAST`
  оставляет мост в coast; `motor.c:174-176` speed=1 даёт duty 0.
- `web.c:887-922` `used += snprintf(...)` без клампа (сейчас недостижимо);
  `web.c:1803-1807` SSID в буфере размера пароля; `web.c:2216-2261` OTA+звуки не
  валидирует WAV; `/progress` всегда `"ota":false`.
- `audio.c:155-158` `audio_validate_wav` принимает `sample_rate==0`; `audio.c:101-127`
  утечка I2S/мьютекса при ошибке init; `auxio.c:263-265` утечка мьютекса при
  ошибке task-create; `storage.c:44-66` утечка SPI-шины при частичном init.
- `pinmap`/`partitions.csv`: coredump 128 КБ может быть мал для дампов с 3×16 КБ
  стеками; DNS-задача web не завершается при auto-off AP.

## 5. Deliverable 3 — план исправлений

Порядок батчей: сначала safety/correctness (A), затем security/robustness (B),
затем отложенное (C). Внутри батча независимые правки можно вести параллельно
в отдельных worktree, но общие файлы (`web.c`, `settings.c`, `motor.c`,
`app_main.c`, `dcc.c`) сериализовать.

### Батч A — корректность и безопасность (P0)

- A1 DCC Service Mode: в `dispatch()` для `service_mode` использовать
  `instr=packet[0]`, `cv_low=packet[1]`, `data=packet[2]`; убрать `0xFF` как
  признак service (это idle); не выводить service из одного preamble-count.
  Обновить `test/test_dcc/test_dcc.c` (убрать синтетический `0xFF`-префикс),
  прогонять реальные 4-байтные пакеты `7C/74/78`. Проверить ACK для bit-write
  (см. Low) — добавить в тот же тест.
- A2 Признак service-режима — явное состояние (вход по ops-mode reset), а не
  `packet[0]∈112..126`; иначе адреса 112..126 работают как короткие. Тесты на
  границы.
- A3 `motor.c:145-150`: клампить `if (hi < lo) hi = lo;` (как в ветке
  CV2/5/6) и/или ограничить результат `LEDC_MAX`. Добавить тест немонотонных
  CV67..94.
- A4 `motor.c`: при выходе из калибровки выставить `s_applied_speed=0`,
  `s_ramp_acc=0`, `s_pid_integral=0`, `s_pid_prev_error=0` и `apply_pwm(0,true)`
  до `s_cal_active=false`; тест «после cal нет проброса при CV4≠0».
- A5 Fail-safe: по reset/estop/CV11-timeout/ошибке — прямой `apply_pwm(0,true)`
  из вызывающего; в `safety_task` — контроль «живости» motor-task (timestamp
  последнего тика + coast при пропуске). Тест на принудительный coast.
- A6 `motor.c:354`: при смене направления во время движения сначала
  ramp-to-zero (переиспользовать CV4-ramp), затем применять направление.
- A7 `storage.c`: регистрировать `min(chip_size, EXT_PARTITION_SIZE)`; при
  `chip_size==0`/аномально малом — backend NONE (без монтирования), логировать
  фактический размер. Тест `test_storage` на «малый/неопределённый» чип.
- A8 Provisioning: глобальный «FS maintenance»-гейт — `audio_stop_all()`,
  отказ/прерывание in-flight upload/OTA, ожидание дренажа перед `storage_format()`.
- A9 Манифест: писать в `tracks.txt.tmp`, `fflush`/`fsync`/`fclose` с проверкой
  кодов, затем `rename()`; `settings_manifest_sync` → `esp_err_t` и проброс.
  Тест «обрыв записи → старый манифест цел».
- A10 `s_cv`: взять `s_lock` вокруг `settings_cv_write` и вокруг
  `cv_set_defaults()`/reset, либо двойная буферизация для читателей.

### Батч B — security / robustness (P1)

- B1 Web-auth: по умолчанию WPA2 с per-unit случайным паролем (не
  `WIFI_AUTH_OPEN`); token/digest для мутирующих маршрутов (OTA/reset/wifi/
  delete/upload) и/или проверка `Origin`; CSRF-токен в UI. ВАЖНО: обновить
  `test/hil/run_hil_web.ps1` (сейчас подключается к открытому AP) и
  `web_ui.html` (передача токена). Решение о совместимости — см. §7.
- B2 `json_escape`: экранировать `\r \t \b \f` и все `<0x20` как `\u00XX`;
  опционально отвергать управляющие на входе (SSID/имя/метки).
- B3 `audio_tracks_get`: перенести `tracks[]`/`json[]` в heap (PSRAM через
  `heap_caps_malloc`) или стримить через `httpd_resp_send_chunk`.
- B4 Взять `s_func_mutex` в `web_motion_changed`, `web_func_map_set`,
  `web_get_function_state`, `func_apply_output`.
- B5 `settings_bemf_cal_load`: требовать `len == sizeof(settings_bemf_cal_t)`.
- B6 `tracks`/`track_cat`/`func_map`/`aux_cfg`: проверять кратность/лимит длины,
  NUL-терминацию, `count <= MAX` в `_save`.
- B7 audio: закрывать `FILE*` в EOF-ветке; в `voice_start` проверять
  `format==1 && bits==16 && sample_rate` в разумном диапазоне (напр. 4..192 кГц)
  и `channels 1..2`; в RIFF-walk учитывать padding `size + (size&1)`.
- B8 `app_main.c`: вызвать `recover_tracks_from_storage()` до `web_init()` (или
  перечитать категории/map в web после восстановления); подтверждать OTA только
  при флаге `boot_ok` (web поднялся; по политике — storage/audio).
- B9 Закрывать queue/sem на всех ветках `pipe_upload`; валидировать
  `slot 1..SETTINGS_MAX_TRACKS`; в reboot-хендлерах звать
  `motor_stop()+audio_stop_all()` перед `esp_restart()`.
- B10 `s_last_packet_us` — 32-бит мкс или чтение под critical section; читателей
  `s_cfg` перевести на тот же `portMUX`/иммутабельный снапшот.
- B11 auxio: считать Mars в `uint32_t` и клампить к 255; в seed firebox добавить
  `idx`/предыдущее состояние.
- B12 Проверять `setvbuf`; голосовые буферы — фиксированный общий/PSRAM
  (`heap_caps_malloc(MALLOC_CAP_SPIRAM)`), либо поднять
  `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`/уменьшить буфер.
- B13 `pinmap_check`: отвергать 22, диапазон `<0 || >48`, дубли (O(n²)).
- B14 Сбрасывать/затухать `s_pid_prev_error` при недоступном BEMF.
- B15 `s_uart_driver_installed = (err==ESP_OK || err==ESP_ERR_INVALID_STATE)`.
- B16 `nvs_read_str`: дефолт при любом не-OK чтении + явная терминация.

### Батч C — отложенное и Low (P2)

- FIX-6 (`s_cfg` синхронизация) — закрыть вместе с B4/B10.
- FIX-7 (DCC-колбэк делает NVS/FS и блокируется на flash) — вынести commit из
  DCC-задачи.
- FIX-9 (flash-wear: commit NVS на каждый CV в service mode) — дебаунс/батч.
- FIX-10 (нет task watchdog) — подписать периодические задачи на TWDT,
  `CONFIG_ESP_TASK_WDT_PANIC=y`, конечные таймауты для `done_sem`.
- FIX-11 (`storage_benchmark` разрушителен) — удалить/спрятать.
- Остальные Low из §4.4 (ACK bit-write, both-high ordering, kickstart в table
  mode, `BEMF-COAST`, speed=1 duty, `used+=snprintf`, OTA WAV-валидация,
  `/progress ota`, coredump размер, DNS shutdown).

## 6. Валидация

Обязательное после каждого батча:

1. `powershell -ExecutionPolicy Bypass -File test\run_tests.ps1` — все наборы
   зелёные. При добавлении набора обновить `$inc` и `$suites` **в обоих**
   `test/run_tests.ps1` и `test/coverage.ps1`.
2. `powershell -ExecutionPolicy Bypass -File test\coverage.ps1` — удержать
   100 % first-party (union по строкам). Новые ветки (A1..A4, B5/B6/B13 и т.д.)
   покрыть тестами.
3. `pio run -e esp32-s3-devkitc-1` (при изменениях CMake/sdkconfig/partitions —
   `-t fullclean`). Следить за Flash/RAM.
4. HIL (нужна плата): `test\hil\run_hil.ps1` (SELFTEST/BEMF), по возможности
   `-Sweep`; `run_hil_web.ps1` — с учётом B1 (AP-пароль/токен).
5. Ручные проверки, не покрываемые хостом (по `FIXES_LOG`): service-mode ACK с
   командной станцией (A1), OTA + принудительный откат (B8), программирование
   CV через рельсы (A2).

## 7. Риски и решения, требующие подтверждения

- B1 меняет поведение AP (по умолчанию WPA2 + токен): ломает текущий
  `run_hil_web.ps1` и «нулевой» онбординг. Нужно решение: (а) WPA2 + случайный
  пароль в вебе/на шильдике, (б) оставить открытый AP, но защитить только
  опасные маршруты токеном. Рекомендация: (а) + токен на мутации.
- A7/A8/A9 — правки данных/FS; обязателен HIL с реальной внешней NOR.
- A4/A5/A6/A14 — safety-критичный мотор; без нагрузки/стенда проверить частично.
- Batch A/B конфликтуют по файлам (`web.c`, `motor.c`, `settings.c`, `dcc.c`,
  `app_main.c`): распараллеливать только по разным файлам или вести
  последовательно.
- Открытый вопрос: нужен ли обязательный CSRF-токен, если AP остаётся открытым
  (риск UX) — см. выше.
