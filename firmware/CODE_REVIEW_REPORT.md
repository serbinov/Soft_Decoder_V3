# Отчёт код-ревью прошивки (Soft_Decoder_V3)

> Примечание: документ исторический. Сборка переведена на ESP-IDF 6.0
> (`firmware\idf_build.ps1`); упоминания `pio run`/PlatformIO относятся к
> ранним версиям.
>
> Дополнение 2026-10-02: выполнено новое полное ревью и начата реализация
> его исправлений. Текущая матрица приведена в разделе 5; исходная проверка
> BEMF сохранена в [BEMF_DIAGNOSTICS.md](BEMF_DIAGNOSTICS.md). Статусы FIXED ниже
> относятся к конкретным исправлениям, а не к полной верификации моторного
> контура или гарантии его устойчивости на реальном двигателе.

Read-only ревью first-party кода (`components/`, `main/`) с последующими
исправлениями.
Агенты ревью: `.kilo/agent/review-*.md` (оркестратор `review-lead`, команда
`/review-firmware`).

Дата ревью: 2026-09-27. Ветка/состояние на момент правок.
Новейшее повторное ревью — 2026-10-17, см. §6.

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

- `test\run_tests.ps1` — все 14 наборов PASSED (471 тест; +тесты на A1..A10,
  B1..B16, FIX-7/9/10/11 и LOW-пункты).
- `test\coverage.ps1` — first-party **100,0 %** (4399/4399 строк).
- `pio run -e esp32-s3-devkitc-1` — SUCCESS; RAM 50 408 Б (15.4 %),
  Flash 824 597 Б (41.9 %).

Не проверено на железе (нет платы): service-mode ACK/программирование CV с
командной станции (A1/A2), реверс под нагрузкой и аварийный стоп (A5/A6),
поведение при просадке питания. Формальная проверка HIL изложена в
`FIXES_LOG.md` и `ARCHITECTURE.md` §4.4.

## 4. Остаётся

Все пункты ревью от 2026-09-27 закрыты в коде и покрыты тестами, кроме B1.
Статус новых пунктов проверки 2026-10-02 приведён в разделе 5. Исторические
примеры неисправного BEMF относятся к исходникам до этих исправлений.

- **B1 — открытый AP и неаутентифицированные разрушительные endpoint'ы.**
  Оставлено осознанно по требованию: пароль Wi-Fi по умолчанию **не задаётся**
  и не генерируется, пока пользователь сам не установит его (веб `/api/wifi`
  POST), после чего AP работает как WPA2. Варианты усиления (WPA2 по умолчанию
  со сгенерированным паролем или токен на мутирующие маршруты) отклонены.

Требует аппарата:

- HIL-прогон на плате: `run_hil.ps1` (SELFTEST/BEMF), `run_hil_web.ps1`
  (при открытом AP `-ApPass` не нужен; при заданном пароле — передать его).
- Проверка Task Watchdog (`CONFIG_ESP_TASK_WDT_PANIC`, 10 с) и дебаунса CV на
  реальных flash-операциях: сначала измерить задержки/lock ownership и
  проверить progress. Не скрывать зависание увеличением watchdog timeout.
- Service-mode ACK/программирование CV с командной станции (A1/A2), реверс под
  нагрузкой и аварийный стоп (A5/A6), поведение при просадке питания.
- Формат coredump-раздела (128 КБ) ограничен разметкой 4 МБ (это последний
  раздел до конца flash) — расширить нельзя без уменьшения OTA; при необходимости
   измерить фактический размер dump и high-water marks. Стеки нельзя уменьшать
   вслепую; изменение разметки потребует отдельной USB-прошивки partition table.

## 5. Исправления Ревью 2026-10-02

База: `5cd9b1b`; изменения находятся в рабочем дереве, не закоммичены.
**Программные исправления 78 пунктов реализованы и прошли общий host-прогон
и сборку ESP-IDF 6.0. Аппаратная работоспособность не подтверждена.**

`HOST` означает реализованное исправление с профильной host-регрессией;
вся итоговая прошивка также собрана для ESP32-S3 на ESP-IDF 6.0.
Аппаратные измерения не проводились ни для одного пункта.

| ID | Реализация | Статус |
|---|---|---|
| REV-D1 | Analog permission отдельно от обнаруженного DC; отсутствие DCC/фронтов и устойчивая полярность | HOST |
| REV-D2 | Отдельный emergency callback с прямым снятием PWM | HOST |
| REV-D3 | Исправлены reserved stop/e-stop и 28-step mapping | HOST |
| REV-D4 | F5..8: B*, F9..12: A* | HOST |
| REV-D5 | Классификация locomotive/accessory/reserved адресов | HOST |
| REV-D6 | Подтверждение service и long Ops Write; short CV23/24 не требует повтора | HOST |
| REV-D7 | Service deadline 20 мс и выход по non-service пакетам любого адреса | HOST |
| REV-D8 | Consist speed, CV21/22 и запрет long CV access через consist | HOST |
| REV-D9 | CV callback возвращает результат; ACK только при ESP_OK | HOST |
| REV-D10 | Точные длины; неподдерживаемый XPOM отвергается целиком | HOST |
| REV-D11 | Stretched/asymmetric zero halves до 10000 мкс | HOST |
| REV-D12 | IRAM interrupt, internal queue, DRAM ISR state | HOST |
| REV-D13 | CV1 side effects атомарны; CV8 обновляет runtime DCC и volume alias | HOST |
| REV-D14 | Rail sample freshness 100 мс и проверка перед DC-командой | HOST |
| REV-D15 | Source lease/generation и отметка принятой DCC motor-команды | HOST |
| REV-D16 | CV11 только в активном DCC; отдельный persistent inhibit | HOST |
| REV-D17 | Hard Reset меняет только CV19/29/31/32, с source admission | HOST |
| REV-D18 | Инверсия CV29.bit0 в опубликованной конфигурации | HOST |
| REV-D19 | ISR startup handshake; control disabled до полной готовности | HOST |
| REV-M1 / BEMF-01 | Отмена калибровки, запрет stale PWM и guarded save admission | HOST |
| REV-M2 / BEMF-02 | Только свежая полная пара ADC; persistent loss приводит к coast/fault | HOST |
| REV-M3 / BEMF-03 | Цель ограничена допустимым диапазоном; invalid feedback не повышает duty | HOST |
| REV-M4 / BEMF-04 | Калибровка усредняет raw magnitudes, нормированные на rail каждой выборки | HOST |
| REV-M5 / BEMF-05 | Убран half-base floor; kickstart сохранён | HOST |
| REV-M6 | Epoch/inhibit и двухканальные LL-записи под коротким output mux | HOST |
| REV-M7 | Snapshot команды; одинаковые DCC refresh не отменяют sampling | HOST |
| REV-M8 | Требуются остановка, готовые ADC/rail; неработающая кривая не заменяет старую | HOST |
| REV-M9 | Reservation до task creation и до конца save/cleanup | HOST |
| REV-M10 | COAST сериализован с владельцем и не восстанавливает stale output | HOST |
| REV-M11 | Единый валидатор count/speed/fraction/минимального конечного сигнала | HOST |
| REV-M12 | Проверенный rail snapshot для деления | HOST |
| REV-M13 | Атомарная публикация кривой; owner-applied reset и CV snapshot | HOST |
| REV-M14 | Защищённые 64-битные heartbeat reads/writes | HOST |
| REV-M15 | Просроченные циклы пропускаются без catch-up burst | HOST |
| REV-M16 | Экспорт всех 16 точек | HOST |
| REV-S20 | Deferred config/CV только в RAM до worker flush | HOST |
| REV-S21 | Setter/commit errors возвращаются; dirty остаётся до успеха, retry ограничен | HOST |
| REV-S22 | Versioned CV+CRC32 в одном blob; чтение shipped legacy cv/cv_crc | HOST |
| REV-S23 | Ошибка rename сохраняет live и temp; исправлено также для MDS | HOST |
| REV-S24 | Валидный пустой tracks store авторитетен | HOST |
| REV-S25 | Empty bindings отличаются от missing; legacy не возрождает удалённые записи | HOST |
| REV-S26 | Persistent recovery marker и retry всех metadata keys | HOST |
| REV-S27 | Backup failure возвращается, manifest dirty повторяется worker-ом | HOST |
| REV-R1 | Safety/WDT/heartbeat readiness обязательны до actuation и OTA acceptance | HOST |
| REV-R2 | Safety не выполняет NVS; отдельный persistence/cleanup worker | HOST |
| REV-R3 | Maintenance admission, file leases и bounded quiescence перед format | HOST |
| REV-R4 | ADC error/timeout не считается нулевым успешным измерением | HOST |
| REV-R6 | Callbacks/actuation допускаются после готовности AUX/map/safety | HOST |
| REV-R7 | HIL-MOTOR немедленно coasts на deadline, независимо от CV4 | HOST |
| REV-A3 | Семантическая проверка MDS после CRC, включая raw bool bytes и строки | HOST |
| REV-A4 | PENDING отличается от PLAYING/FINISHED | HOST |
| REV-A5 | Owned voice handles с generation; stale release не влияет на нового владельца | HOST |
| REV-A6 | FX allocator не использует зарезервированные 18/19 | HOST |
| REV-A7 | Нормализация fractional phase до переполнения индекса | HOST |
| REV-A8 | Fixed-point EMA затухает к нулю | HOST |
| REV-A9 | Смена/disable/delete сбрасывают старый runtime/owned voices | HOST |
| REV-A10 | Mute блокирует starts, но не releases; gates для автоматических extras | HOST |
| REV-A11 | Legacy web использует owned handles вместо пересекающихся индексов | HOST |
| REV-A12 | Общий WAV parser проверяет fmt, RIFF/data bounds и frame alignment | HOST |
| REV-A13 | Init rollback освобождает mutex/I2S и отключает amplifier | HOST |
| REV-L1 | Нормированная incandescent envelope, масштаб PWM один раз | HOST |
| REV-L2 | Отдельный scheduled flag, корректный initial deadline при большом uptime | HOST |
| REV-W20 | Отмена debounce при stop, очередь команд и отбрасывание stale replies | HOST |
| REV-W21 | Пустой пароль сохраняет старый; удаление отдельным явным флажком | HOST |
| REV-W22 | Private staging, canonical MDS-compatible имена, backups и rollback вместо truncate | HOST |
| REV-W23 | Все файлы проверяются до publication/boot; ошибки возвращают recovery evidence, не ok | HOST |
| REV-W24 | Подтверждения deletion/overwrite ожидаются через await | HOST |
| REV-W25 | Decode до проверки длины; malformed/NUL/overflow отвергаются | HOST |
| REV-W26 | Upload slots ограничены 1..20 | HOST |
| REV-W27 | Общий OR desired outputs и немедленное применение bindings | HOST |
| REV-W28 | Deadline до полного чтения JSON body | HOST |
| REV-W29 | Reporter errors поглощаются и rate-limited | HOST |
| REV-W30 | Пригодный unicast AP host address и проверка netif/DHCP ошибок | HOST |
| REV-W31 | Полные 32-byte SSID/64-hex PSK без тихого обрезания | HOST |
| REV-W32 | Async transfer worker, maintenance и конечный total deadline | HOST |
| REV-W33 | Bounded/nonoverlapping reconnect, upload timeout/abort, visibility guards | HOST |
| REV-W34 | Cursor последней действительно переданной записи | HOST |
| REV-W35 | Single-question DNS A/IN; AAAA получает корректный пустой ответ | HOST |

### Итоговая Проверка

- Все **16 C-наборов: 705 PASS**, zero ignored, zero failures.
- JavaScript: **13 PASS** (`node --test test/test_web_ui.js`).
- Финальная сборка ESP32-S3 / ESP-IDF 6.0 прошла. `soft_decoder_v3.bin`:
  `0xef890` (981136 байт), около 50% минимального OTA-раздела остаётся свободно.
- ELF-проверка интеграционной сборки: `dcc_isr`, `esp_timer_get_time` и
  `xQueueGenericSendFromISR` находятся в `.iram0.text`, `s_last_edge_us` в DRAM.
- Прошивка, motion/format HIL и испытания power-loss на плате не запускались.

### Существенные Контракты

- CONTROL, SAFETY и DCC_TIMEOUT являются независимыми inhibit-владельцами.
  Maintenance/HIL не могут очистить чужой safety/timeout запрет.
- При enabled BEMF потеря обратной связи не повышает PWM; возможен только
  ограниченный по возрасту hold прежнего duty до 100 мс, затем coast и fault.
  Fault снимается STOP или явным BEMF disable, но не повтором ненулевой команды.
- Проверка разрешения сохранения калибровки после получения settings mutex
  является save linearization point. Отмена до неё сохраняет старую запись;
  уже допущенная полностью измеренная запись может закончиться после отмены.
- Порог 85%, ADC conversion, gap 1000 мкс и PID CV54/55/56 не перенастраивались
  без измерений. Устойчивость конкретного мотора и время свободного затухания
  остаются аппаратной проверкой.
- Старые NVS `cv`/`cv_crc` сохраняются при миграции, но будущие записи идут
  в новый record. Это чтение legacy, не обещание двустороннего downgrade-sync.
- Успешный host/target build не доказывает max latency, радиосовместимость,
  waveform ACK, coredump headroom или максимальную audio throughput.

### WAV И Combined OTA

- Имя `audio/idle.wav` остаётся каноническим: существующий MDS получает новый
  валидный asset, а не скрытый `u*.tmp`. Старые отличающиеся имена сохраняются,
  поскольку они могут использоваться другими сохранёнными проектами.
- Все payloads проверяются до публикации. Original copies `.bak` и record
  `WAVTXN1` синхронизируются до первого rename; workspace находится в heap.
- Некорректный/неполный контейнер, duplicate names/slots, недостаток места,
  ошибка metadata или boot selection не объявляются успехом. По возможности
  восстанавливаются metadata/файлы; uncertain результат сохраняет recovery path.
- **Совместная атомарность FS/NVS/boot при power loss не гарантируется.**
  Обрыв между отдельными rename может оставить валидные смешанные версии.
  `.bak`/record являются данными для **ручного** восстановления, не automatic replay.
- Cleanup после успеха может остаться pending. При partial/uncertain ответе
  нельзя удалять recovery artifacts или форматировать NOR до их разбора.
- Total deadline 120 с проверяется программно; nonpreemptible SDK/VFS calls
  и rollback могут превышать его. Максимальную реальную latency нужно измерить.

## 6. Ревью 2026-10-17 (рабочее дерево, база `0c5c027`)

Повторное read-only ревью first-party кода (`components/`, `main/`, `web_ui.html`,
тесты) с приоритетом на незакоммиченные изменения: графовый звук, split
`web.c` → `web_wifi.c`/`web_internal.h`, условия `func_binding_t`, редактор.
Оркестратор `review-lead`; в этом окружении дочерние агенты не запускались,
поэтому домены dcc/motor/web/storage/media/rtos пройдены последовательно.
Каждый CRITICAL/HIGH перепроверен по `file:line`. FIX-6..FIX-11 не
переоткрывались. Файлы ревьюером не изменялись; исправления ниже **не
применялись**.

Проверка после ревью: `18 C-наборов / 790 тестов PASS`, `27 JS PASS`
(`ALL TEST SUITES PASSED`), покрытие host-строк **92,8 % (8234/8872)**,
сборка ESP-IDF 6.0 — SUCCESS (`soft_decoder_v3.bin` 1 110 080 Б, 44 % OTA-слота
свободно), `release_artifacts.py verify` — 6 BIN OK. На железе не проверялось.

### 6.1 Сводка

| ID | Severity | Файл:строка | Проблема | Verified |
|----|----------|-------------|----------|----------|
| REV-W36 | **HIGH** | `web.c:2217-2231`, `sound_graph.c:308-316`, `blocks.html:615-617,644-661` | Политика имён WAV рассогласована: валидатор графа отвергает имена, которые использует типовой проект и принимают upload/OTA/pack; `.WAV` виден в библиотеке, но не играет | Да |
| REV-W37 | MEDIUM | `web.c:1604-1620` | Аварийный стоп очищает `s_fn`, но не пересчитывает выходы: AUX/свет могут остаться включёнными, а API показывает «всё выключено» | Да |
| REV-S29 | MEDIUM | `sound.c:305-310`, `sound_graph_runner.c:62-77` | После apply графа восстанавливаются уровни F, но не `engine_on`/`armed`: двигатель молчит, если его F уже удерживалась | Да |
| REV-W38 | MEDIUM | `web.c:3565-3575,3868-3869` | Загрузка pack может вернуть успех и удалить старые файлы, даже если reconciliation track/binding провалился (функция `void`, ошибки игнорируются) | Да |
| REV-W39 | MEDIUM | `web.c:519-541` | `web_maintenance_end()` молча выходит при таймауте мьютекса 100 мс; maintenance owner и inhibit мотора/звука остаются до перезагрузки | Да |
| REV-S28 | LOW | `sound_graph.c:157,165` | На точной ёмкости токенов обход читает `tokens[capacity]` (scratch-строка вместо токена); крафтовый JSON может дать неверный разбор/цикл | Да (код) |
| REV-W40 | LOW | `web.c:212-216`, `app_main.c:397-399` | Boot-recovery события теряются: `web_log_event()` выходит до создания мьютекса, хотя комментарий обещает буферизацию | Да |
| REV-W41 | LOW | `web.c:2173-2176` | `/api/audio/volume` с одним параметром сбрасывает остальные категории в 20; malformed значение тоже молча даёт 20 | Да |
| REV-W42 | LOW | `web_wifi.c:190-200`, `web.c:85,3154` | Временный STA netif не уничтожается (в т.ч. на error paths); `s_sta_ip` нигде не присваивается, но STA-поля отдаются в API | Да |

### 6.2 Ключевые находки

**REV-W36 (HIGH) — единая политика имён WAV.** `sg_filename_valid()` требует
ровно `.wav` в нижнем регистре и только `[A-Za-z0-9_-]` в базе; при этом
типовой проект редактора использует `ms-start.WAV`, `Air Compressor Start.WAV`,
`türe-auf_führerstand.wav` (`sound_editor/public/blocks.html:615-617,644-646,659-661`),
а OTA принимает `.WAV` через `strcasecmp`, не нормализуя регистр
(`web.c:3406-3421`). Вдобавок выдача/список пересобирают путь как
`WEB_AUDIO_DIR "/%s.wav"` (`web.c:1976,2106,2231,3784`), поэтому файл `horn.WAV`
числится в `/api/sound-files`, но `/sound-files/horn` ищет `horn.wav`.
Следствие: первый же типовой проект не проходит asset-check и не сохраняется на
чистом устройстве; имена со пробелами/UTF-8/верхним расширением нефункциональны.
Решение: один канонический helper для upload, pack, OTA, редактора, выдачи и
валидатора; нормализовать к нижнему `.wav` и одному документированному набору
символов (ASCII или текущий UTF-8 sanitizer), отдавать файл по точному имени,
мигрировать существующие имена. Это блокер для 0.10.

**REV-W37.** В `emergency_post()` после `memset(s_fn,0)` нет
`func_apply_all_locked()`/сброса `s_outputs_applied`; привязки выходов не
пересчитываются. Исправление: вызвать пересчёт под `s_func_mutex` (или явно
погасить все mapped-выходы и очистить маску).

**REV-S29.** `sound_graph_prime_functions()` восстанавливает только `fn_levels`.
Если `graph->engine_fn` уже нажат, нужно выставить `engine_on = true` и
`armed = true` (для hold и toggle), не формируя `fn_press/fn_release`.

**REV-W38.** `pack_reconcile_metadata()` должна возвращать `esp_err_t` и
пробрасывать ошибки `settings_*_save`; при сбое — откат к backup или
degraded-ответ с сохранением recovery-файлов; не удалять старые файлы до
устойчивой согласованности метаданных.

**REV-W39.** При неудачном `xSemaphoreTake(...,100 ms)` нельзя оставлять
`s_maintenance_owner`/`s_maintenance_exclusive_fs`; нужен retry-worker или
аварийная очистка owner с последующим снятием inhibit.

**LOW.** REV-S28 — добавить sentinel-токен или границу `i + 1 < capacity`;
REV-W40 — создать лог-мьютекс до recovery или перенести его в начало
`app_main`; REV-W41 — отсутствующие поля сохранять, malformed — 400;
REV-W42 — уничтожать `sta_netif` после скана/на ошибках и убрать мёртвые
STA-поля из API (или реализовать STA).

### 6.3 Области без новых дефектов

- **DCC:** 28-step mapping, группы F (A*=F9-12, B*=F5-8), service/ops
  confirmation и разбиение адресов соответствуют тестам.
- **Motor:** PWM/PID/BEMF-калибровка, inhibit/emergency и отмена калибровки —
  новых дефектов нет.
- **Storage/settings:** LittleFS без авто-format, leases, атомарный манифест,
  компакция track/binding — новых HIGH нет (см. REV-W38).
- **Media/audio:** WAV/IMA-ADPCM, времена жизни голосов (19 FX + голос 19),
  pack-заголовок — новых memory-safety дефектов, кроме REV-W36, нет.
- **RTOS:** порядок блокировок `func → sound → audio`, `control →
  sound/audio`, maintenance/admission, размеры стеков — дедлока/инверсии не
  найдено. `sound_task` 4 КБ и локальные массивы `sg_runner_tick()` (~256 Б)
  стоит перепроверить при росте раннера.

### 6.4 Кандидаты на удаление/чистку (не дефекты)

Уже удалены в этой ревизии: пустые `*_tmp/`, кэши Python, `sdkconfig.old`,
`asset-sizes.json`, `blocks.html.gz`. Остаются кандидаты, требующие решения:

| Кандидат | Где | Причина |
|---|---|---|
| `settings_config_t.hold` | `settings.h:80`, `web.c:3153,3213` | Хранится/отдаётся, не читается; captive всегда 302 |
| STA-поля и `wifi_mode` 2/3 | `settings.h:73,77-78`, `web.c:85,3140-3154` | AP-only; `s_sta_ip` не присваивается |
| `settings_config_t.port` | `settings.h:79`, `web.c:4488` | Порт фиксирован 80 |
| `FUNC_FLAG_DUCK` | `func_types.h:25` | Нет runtime-читателя |
| `func_binding_t.mode` | `func_types.h:68` | Хранится, не исполняется |
| `web_get_function_state()` | `web.c:341`, `web.h:16` | Только тесты |
| `sound_engine_is_on()` | `sound.c:129` | Нет вызовов |
| `audio_voice_alloc/release` | `audio.c:737,758` | Нет production-вызовов; только тесты |
| `upload_wav` (i18n) | `web_ui.html:555,619` | Нет UI-элемента |
| CV30/114/115/116 | `web_ui.html:1377-1380` | Осознанно оставлены для совместимости |
| `web_wav_probe()` | `web.c:1885-1929` | Дублирует RIFF-walk `audio.c`, слабее по bounds |
| Четыре политики имён | `web_util.c:240`, `web.c:1767,3391`, `sound_graph.c:308` | Причина REV-W36; свести в один helper |
| `sound_editor/dist/blocks.html` | `sound_editor/dist/` | Идентичен `public/` (SHA-256 `5FD93DAF…F765`); генерируется, в git — для сборки |

Незакомпилированных/мёртвых `.c` не найдено: все исходники входят в CMake-цели.

### 6.5 Нужны решения

1. **REV-W36:** нормализовать все ingress к нижнему `.wav` + один charset
   (рекомендуется) или ослабить `sg_filename_valid()` под текущий sanitizer;
   от этого зависит миграция существующих имён.
2. **REV-W37:** должен ли веб-аварийный стоп гасить mapped AUX/свет? Рекомендуется
   да (сейчас API врёт о выключенных функциях).
3. **Мёртвая конфигурация:** удалить `hold`, `port`, STA-поля и `wifi_mode` 2/3
   или реализовать; удаление меняет формат `/api/wifi` и NVS-совместимость.
4. **`dist/blocks.html`:** оставить коммит для воспроизводимой сборки или
   генерировать на build и убрать из git.
