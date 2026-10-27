# Архитектура Soft_Decoder_V3 (0.12)

Референс для правок. Точные константы, стеки и приоритеты всегда сверяйте с
исходниками (`firmware/CMakeLists.txt` фиксирует бюджет задач). Полное описание
с картой выводов и API — `firmware/ARCHITECTURE.md`.

## Платформа

- ESP32-S3FH4R2, 2 × Xtensa LX7, 160 МГц, 4 МБ flash, 2 МБ PSRAM в heap.
- ESP-IDF 6.0, C11, FreeRTOS 1000 Гц, `-Os` (кроме audio — `-O2`).
- Драйвер мотора DRV8870: LEDC 20 кГц, 10 бит, IN1/IN2.
- 9 выходов: F0F/F0R + AUX1..AUX4 на LEDC, AUX5..AUX7 на MCPWM.
- Аудио: I2S0 TX, 22050 Гц, моно, 16 бит → MAX98357A; 20 голосов.
- Внешняя NOR W25Q128 16 МБ на SPI2 → LittleFS `/userdata`.
- DCC: GPIO9, ISR по любому фронту, level 3, очередь 256 полупериодов.
- Питание/защита: brownout level 7, аппаратный current sense (кодом не читается),
  core dump, OTA rollback; power management выключен.
- **Все номера GPIO — только в `components/pinmap/include/pinmap.h`.** Не
  дублируйте таблицу выводов в новой документации; используйте `pinmap_validate()`.

## Задачи и периоды

| Задача | Период/событие | Приоритет | Стек | Источник |
|---|---|---|---|---|
| `dcc` | очередь полупериодов | 10 | 8192 | `dcc.c`, CPU1 |
| `dcc_ack` | запрос ACK | 5 | 2048 | `dcc.c` |
| `motor` | 10 мс | 7 | 3072 | `motor.c` |
| `bemf_cal` | по запросу | 6 | 3072 | `motor.c` |
| `track_adc` | 50 мс | 7 | 3072 | `track.c` |
| `audio_mix` | блок 256 сэмплов (~11,6 мс) | 7 | 4096 | `audio.c`, CPU1 |
| `sound` | 20 мс | 6 | 4096 | `sound.c`, CPU1 |
| `safety` | 10 мс | 6 | 3072 | `app_main.c` |
| `settings` (persistence) | 100 мс | 3 | 4096 | `app_main.c` |
| `aux_fx` | 20 мс | 6 | 3072 | `auxio.c` |
| `dns_hijack` | UDP :53 | 9 | 4096 | `web_wifi.c` |
| `web_transfer` | upload/OTA | 6 | 16384 | `web.c` |
| `prov_listen` | UART | 4 | 8192 | `provision.c` |
| `wifi_off` | 10 с (опц.) | 4 | 3072 | `web_wifi.c` |
| httpd 80/81 | запросы | IDF | 16384/4096 | `esp_http_server` |

Порядок важен: DCC выше мотора, мотор выше инерционных задач. Перенос задач с
CPU1 может ухудшить латентность ISR/аудио.

## Потоки данных

1. **Мотор.** DCC speed callback (`on_dcc_speed`) или `POST /api/motor`
   (`web.c`) → `motor_set_speed()` → ramp CV3/CV4 → `speed_duty()`
   (CV2/CV5/CV6 или CV67..94) → kickstart CV65 → BEMF PID (если включён) →
   LEDC duty → `motor_get_applied_speed()`.
2. **Звук.** `sound` раз в 20 мс читает **applied** скорость/направление →
   `sg_runner_tick()` → состояния/переходы → `audio_voice_play_owned()` →
   микшер `audio_mix` → I2S. **Звук никогда не командует мотором.**
3. **Функции.** DCC function callback и веб вызывают единый
   `web_apply_function()` → `func_eval_ex()` по `func_binding_t` → AUX/свет и
   логика; при активном графе звук идёт через `sound_function()`, а не через
   legacy-слоты.
4. **BEMF.** Калибровка: preflight (статический сенсор, поиск первого
   движения, полярность) → adaptive window при первой движущейся скорости →
   curve (open-loop точки, settle, samples, повторы) → dwell по умолчанию
   1000 мс на новую точку → сохранение. Потолок/пропуски дают валидированную
   частичную кривую (≥2 точек, последняя доля ≥51/1024), иначе `CURVE`.
5. **Хранилище.** NVS (`decoder`): конфиг, `cv_record`, треки, категории,
   F-привязки, AUX cfg, `bemf_cal`, `bemf_use`, `bemf_win`. LittleFS:
   `/userdata/audio/*.wav`, `tracks.txt` (манифест метаданных), `volumes.txt`,
   `selection`, ревизии графа, `.packjournal`. Манифест переживает сброс NVS и
   восстанавливает имена/категории/карту F↔AUX.
6. **Веб-обслуживание.** Мутации проходят `mutation_dispatch()`; тяжёлые
   операции берут `web_maintenance_begin()` и/или storage lease
   (`storage_access_begin()`). OTA/upload/pack идут через `web_transfer` и
   `transfer_start()`, затем проверяют staging и делают canonical rename или
   транзакцию с backup/journal.
7. **UART-провижининг.** `provision_listener_start()` ловит `PROV`, ставит
   флаг и перезагружает; `provision_try()` на следующем boot после
   `PROV-CONFIRM` стирает внешнюю NOR, принимает `PUT <slot> <size> <label>`
   (до 20) или `PACK <size>` (`PACK-DATA` кадрами 32 КБ, до 400 звуков),
   пишет метаданные и перезагружается. Wi-Fi не используется.

## Модель звукового графа

- Схема JSON v1: `states`, `transitions`, `effects`, `variables`, `assets`,
  `engine`. Парсит/валидирует `sound_graph.c`, исполняет
  `sound_graph_runner.c`, хранит ревизии `sound_graph_store.c`.
- **State** — звук/тишина + `loop`, `volume`, `rate`, `loops`, `set_var`.
  **Transition** — source/target, priority, `immediate|after_sample`, условие.
  **Engine** — входная F-клавиша (`engine_fn`), `engine_hold` (hold вместо
  toggle), `hysteresis` по скорости. **Effect** — F-триггер → entry state.
- Условия: F press/release/on/off, engine on/off, speed, accel, decel,
  sample done, direction, random, timeout, variable compare, state enter.
- Лимиты из `sound_graph.h`: 192 states (≤144 звучащих, ≤96 тихих), 250
  transitions, 32 effects, 128 assets, 16 variables, JSON ≤128 КиБ; ID ≤31,
  имя ≤63, файл ≤63. Store: 64 проекта, 1024 записи, 8 ревизий на проект,
  выбранная ревизия не удаляется.
- Store API: `sg_store_save()` с `expected_revision` (0 = создать),
  `sg_store_read/list/select/delete`, `sg_store_file_referenced()`. Apply
  требует стоящий мотор; выбранный граф не подменяется legacy при ошибке.

## Инварианты безопасности и владения

- **Inhibit-флаги независимы:** `MOTOR_INHIBIT_CONTROL`, `_SAFETY`,
  `_DCC_TIMEOUT`; никто не снимает чужой флаг. Feedback fault снимается только
  `speed=0` или явным BEMF disable. Stall (нет тика >200 мс) — до restart.
- **Калибровка владеет мостом:** `motor_bemf_cal_active()`; DCC-скорость в это
  время не пересылается в `motor_set_speed()` (иначе повторный speed 0 отменит
  прогон). Stop/inhibit/e-stop отменяют калибровку; `motor_bemf_cal_watchdog()`
  работает в safety и не ждёт мьютексов.
- **E-stop асинхронен:** `motor_emergency_stop()` не зависит от задачи мотора;
  DCC reset/e-stop и веб-стоп не должны ждать длинную мутацию.
- **ADC1 общий:** rail и BEMF сериализуются `motor_bemf_lock/unlock`; unlock
  только если lock вернул true. Emergency/inhibit не ждут control/ADC mutex.
- **Storage:** открытый FILE/FS-операция держит lease; maintenance закрывает
  admission и ждёт quiescence с bounded timeout. Не держите mutex, ожидая
  аудио; не форматируйте без подтверждения.
- **DCC callback:** только RAM-операции и короткие вызовы. CV commit —
  deferred; service ACK — отдельная задача; любые NVS/FS/долгие waits запрещены.

## Карта файлов и функций

| Подсистема | Файлы | Ключевые функции |
|---|---|---|
| Старт/safety | `main/app_main.c` | `app_main`, `safety_task`, `safety_step`, `persistence_task`, `on_dcc_*`, `recover_tracks_from_storage` |
| DCC | `components/dcc/src/dcc.c` | `dispatch`, `emit_function`, `dcc_service_ack`, `dcc_reload_config`, `dcc_set_control_enabled` |
| Мотор | `components/motor/src/motor.c` | `motor_set_speed`, `motor_stop`, `motor_emergency_stop`, `motor_set_inhibited`, `motor_set_inhibit_reason`, `motor_bemf_cal_*`, `motor_bemf_check` |
| Rail/DC | `components/track/src/track.c` | `track_init`, `track_is_dc_mode`, `track_recover_from_storage` |
| Звук-граф | `components/sound/src/sound*.c` | `sg_parse/validate`, `sg_runner_tick/function/power`, `sg_store_*`, `sound_function`, `sound_set_speed`, `sound_engine_power` |
| Аудио | `components/audio/src/audio.c` | `audio_voice_alloc/play/release_owned`, `audio_voice_set_rate_owned`, `audio_validate_wav`, `audio_inspect_wav` |
| AUX | `components/auxio/src/auxio.c` | `auxio_set_enabled`, `auxio_set_effect`, `auxio_config` |
| Веб | `components/web/src/web.c` | `web_init`, `web_apply_function`, `web_motion_changed`, `mutation_dispatch`, `web_maintenance_begin/end`, `graph_*_post`, `audio_pack_upload_post`, `ota_update_post` |
| Wi-Fi | `components/web/src/web_wifi.c` | `wifi_start`, `start_dns_hijack`, `wifi_start_auto_off`, `captive_handler` |
| Настройки | `components/settings/src/settings.c` | `settings_load/save`, `settings_cv_*`, `settings_bemf_*`, `settings_manifest_*`, `settings_recovery_*` |
| Хранилище | `components/storage/src/storage.c` | `storage_mount/format`, `storage_access_begin/end`, `storage_maintenance_begin/end`, `storage_is_quiescent` |
| Провижининг | `components/provision/src/provision.c` | `provision_try`, `provision_listener_start` |
| Самотест | `components/selftest/src/selftest.c` | `selftest_run`, `selftest_act_*` |

## Принятые ограничения

- Wi-Fi только SoftAP, один клиент; STA/APSTA не реализованы. Пароль AP по
  умолчанию пуст; API/OTA/удаление файлов без аутентификации — AP и есть граница.
- CV1..CV512; индексированные CV257+ и RailCom не реализованы. CV33..CV46 не
  используются: F→AUX задаётся редактором. CV12..14, CV28, CV31/32, CV105/106
  хранятся без влияния. CV30/114–116 — legacy-резерв, граф их не читает.
- `func_binding_t.mode` и `FUNC_FLAG_DUCK` сохраняются для формата, но legacy-
  режимы/ducking в рантайме не исполняются. Слоты 1..20 — совместимость.
- Без внешней NOR звук отключён; автоматического форматирования нет.
- BEMF-цель — доля rail, не RPM; достаточность gap/ceiling не доказана.
- `web_log_event()` до `web_init()` теряет сообщения журнала.
