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
