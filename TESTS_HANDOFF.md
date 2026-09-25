# Тесты прошивки — статус и план (handoff)

Документ для продолжения работы над покрытием тестами в новой сессии.
Проект: `Soft_Decoder_V3` / прошивка ESP32-S3. Дата: 2026-09-25.

---

## 1. Цель

Довести host-тесты (Unity, gcc) до **100% покрытия** кода первого контура
(`components/` + `main/`), работая **без железа**.

## 2. Текущий статус

Покрытие first-party: **99.7% (2520/2528 строк)**. Тестовые наборы: **12,
все зелёные**. Прошивка собирается (`pio run` → SUCCESS).

| Модуль | Покрытие | Примечание |
|---|---:|---|
| `dcc.c` | 100% | |
| `motor.c` | 100% | |
| `audio.c` | 100% | |
| `auxio.c` | 100% | |
| `track.c` | 100% | |
| `pinmap.c` | 100% | |
| `settings/track_manifest.c` | 100% | |
| `storage.c` | 100% | |
| `track/track_recover.c` | 100% | новый модуль (вынесен из app_main) |
| `settings.c` | 99.4% | 2 строки — артефакт gcov (общий TU с манифестом) |
| `web/web_util.c` | 99.5% | 1 недостижимая защитная строка (`vsnprintf<0`) |
| `provision.c` | 98.7% | 1 артефакт статик-стрипа + 4 строки защитной ошибки `fwrite` |
| **`web/web.c`** | **0%** | **осталось (самый крупный, ~2457 строк)** |

## 3. Git

- Репозиторий один, корень `Soft_Decoder_V3`, ветка `main`, remote `origin`
  (GitHub, приватный), локальный `.githooks/post-commit` пишет строку в
  `CHANGELOG.md`.
- **Не запушено** — по условию заказчика: push только после завершения всех
  доработок (после шага 4). Перед push: `git push origin main`.
- Последние коммиты: `6794864` (track_recover), `59416ef` (storage),
  `e0858bc` (provision), `1a70ac9` (fuzz/sanitize), `c426ff4` (dcc),
  `6027d78` (motor), `e84c039` (audio), `df26225` (auxio/settings),
  `8073a99` (coverage union), `dc17f51` (coverage tool + manifest).

## 4. Инфраструктура тестов

Запуск (из `firmware/`):
```powershell
powershell -ExecutionPolicy Bypass -File test\run_tests.ps1     # все наборы
powershell -ExecutionPolicy Bypass -File test\coverage.ps1      # покрытие (gcov)
powershell -ExecutionPolicy Bypass -File test\coverage.ps1 -Only test_dcc
```
- `test/run_tests.ps1` и `test/coverage.ps1` — список наборов в `$suites`.
  **При добавлении нового набора обновите `$inc` (include-пути) и `$suites` в
  ОБОИХ файлах.**
- Метод — white-box: `#define static` + `#include "<component>/src/x.c"` (+
  `test_libs/teststubs/stubs.c`). Стабы ESP-IDF — в
  `test_libs/teststubs/include/**`.
- Покрытие агрегируется **по строкам (union)** — файл, включаемый несколькими
  наборами, не считается дважды.
- Флага `-Sanitize` добавляет `-fsanitize=address,undefined`. **В текущем MinGW
  gcc нет `libasan`/`libubsan`** — флаг для CI/Linux.

### Стабы с флагами отказа (для негативных ветвей)
`stubs.c` предоставляет флаги: `mock_sem_take_fail`, `mock_mutex_create_fail`,
`mock_nvs_flash_init_err`, `mock_nvs_open_fail`, `mock_nvs_get_u8_err`,
`mock_task_create_ok`, `mock_adc1_config_*_ok`, `mock_i2s_*_err`,
`mock_queue_create_fail`, `mock_queue_send_fail`, `mock_gpio_isr_install_err`,
`mock_uart_*`, `mock_usbjtag_*`, `mock_ota_*`, `mock_spi_*`, `mock_flash_*`,
`mock_lfs_*`, `mock_partition_register_err`; мок UART (`mock_uart_feed` /
`mock_uart_tx` / `mock_uart_reset`); `mock_timer_now_us` (продвигается в
`vTaskDelay`).

### Тест-хуки в коде (ограничение бесконечных циклов)
`track_adc_step` + `s_iter_cap` (track), `s_fx_iter_cap` (auxio),
`s_mix_iter_cap` (audio), `s_motor_iter_cap` (motor), `s_dcc_iter_cap` (dcc),
`s_listen_iter_cap` (provision). В проде = 0 (вечно).

### Переопределяемые пути (для host)
`MANIFEST_DIR` (track_manifest), `STORAGE_MOUNT_POINT` (storage),
`PROV_AUDIO_DIR`/`PROV_MKDIR` (provision).

## 5. Что сделано по шагам

1. **Закрыты дыры** в audio/auxio/dcc/motor/track/pinmap/settings/web_util;
   рефакторы под тестируемость; удалены/переписаны доказуемо мёртвые ветви.
2. **`test_storage`** + стабы `esp_flash`/`esp_partition`/`esp_littlefs`/
   `spi_master`; инъекция `malloc` для OOM-ветки бенчмарка.
3. **`test_provision`** + стабы `driver/uart`, `driver/usb_serial_jtag`,
   `esp_ota_ops`, `esp_system`, `esp_vfs_dev`.
4. **Шаг 5:** логика восстановления треков вынесена из `app_main` в
   `components/track/src/track_recover.c` (+API в `track.h`), `test_track_recover`.
5. **Шаг 6:** fuzz-тесты (`test_web_util`, `test_dcc`), флаг `-Sanitize`.

## 6. ЧТО ОСТАЛОСЬ — шаг 4: `test_web` (`components/web/src/web.c`)

`web.c` (~2457 строк) — HTTP-сервер: маршруты, обработчики API, OTA (FW +
составной `AURAOTA2`), Wi-Fi softAP, журнал событий, аплоад звуков.

План:
1. **Host-shim `esp_http_server`** в `test_libs/teststubs/include/esp_http_server.h`
   + реализации в `stubs.c`: mock `httpd_req_t` (`uri`, `content_len`, тело),
   `httpd_resp_send`/`send_err`/`set_hdr`/`set_status` (сбор ответа в буфер),
   `httpd_req_recv`, `httpd_req_get_hdr_value_str`, регистрация маршрутов
   (`httpd_register_uri_handler`) — сохранять список для вызова вручную.
2. Добавить стабы: `esp_wifi` (`esp_wifi_*`), `esp_netif`, `esp_event`,
   `esp_littlefs` (есть), `lwip`/`dhcps` (минимально), `app_update`/`esp_ota_ops`
   (есть частично), `mdns` если используется.
3. **Вынести «чистую» логику** из обработчиков в отдельные функции/модуль
   (парсинг и валидация параметров, сборка JSON, вычисление пути аплоада,
   разбор составного OTA-контейнера) — их и покрыть. Хендлеры вызывать через
   mock-`httpd_req_t` (проверять URI/тело → ответ).
4. По приоритету обработчиков: `GET /` (страница), `GET/POST /api/*`
   (device, audio tracks, upload/delete, categories, func map, aux, bemf, cv,
   log, wifi, ota firmware, ota combined), Wi-Fi старт (`wifi_start` — канал
   до старта AP), `web_log_event` (UTF-8-обрезка).
5. **On-target** (опционально): сокеты/LittleFS/реальный `esp_http_server`
   осмысленнее покрывать ESP-IDF unity/pytest на железе или в QEMU — host-shim
   для них не даёт настоящей уверенности.

После шага 4: обновить `ARCHITECTURE.md` (раздел про тесты/покрытие),
`CHANGELOG.md` (авто), пересобрать прошивку, обновить `release/` и **запушить**.

## 7. Грабли (важно для новой сессии)

- **Изменение `CMakeLists.txt` компонента** (новый `.c` в `SRCS`) требует
  пересборки CMake: `pio run -t clean` затем `pio run` (иначе undefined
  reference). Так было при добавлении `track_manifest.c` и `track_recover.c`.
- **Правки `sdkconfig.defaults`/`partitions.csv`** тоже требуют регенерации —
  удалить `sdkconfig.esp32-s3-devkitc-1` и собрать.
- Артефакты в `release/` не пересобираются автоматически: после изменения
  прошивки копировать `firmware.bin`/`bootloader.bin`/`partitions.bin`/
  `ota_data_initial.bin` в `release/flash_download_tool/` и `release/
  ADDITIPUS_AURA-X_v0.7.bin` (см. `firmware/build_flash_tool_files.bat`).
- White-box `#define static` делает function-local `static` автоматическими —
  из-за этого строки вида «early return if already installed» не покрываются
  (артефакт, не баг). Не гоняться за ними.
- `fopen`/`mkdir` на host: создавать каталоги в `setUp`, чистить в `tearDown`
  (файлы иначе «протекают» между тестами).
- Fuzz-тесты используют детерминированный LCG — не менять без причины.
- Локальный `.githooks/post-commit` при каждом коммите дописывает строку в
  `CHANGELOG.md` и делает `--amend`; при необходимости `DCC_AUTOCHANGELOG=1`
  чтобы пропустить.

## 8. Быстрые команды

```powershell
# тесты и покрытие
powershell -File test\run_tests.ps1
powershell -File test\coverage.ps1

# сборка прошивки (из firmware/)
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e esp32-s3-devkitc-1
# при изменениях CMake/sdkconfig/partitions:
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e esp32-s3-devkitc-1 -t clean
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e esp32-s3-devkitc-1
```
