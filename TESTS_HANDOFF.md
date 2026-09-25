# Тесты прошивки — статус и итог (handoff)

Проект: `Soft_Decoder_V3` / прошивка ESP32-S3. Дата: 2026-09-25.

---

## 1. Цель

Довести host-тесты (Unity, gcc) до **100 % покрытия** кода первого контура
(`components/` + `main/`), работая **без железа**. **Достигнуто.**

## 2. Текущий статус

Покрытие first-party: **100.0 % (4271/4271 строк)** — union по строкам (gcov).
Тестовые наборы: **14, все зелёные** (453 теста). Прошивка собирается.

| Модуль | Покрытие |
|---|---:|
| `dcc.c` | 100% |
| `motor.c` | 100% |
| `audio.c` | 100% |
| `auxio.c` | 100% |
| `track.c` | 100% |
| `pinmap.c` | 100% |
| `settings/track_manifest.c` | 100% |
| `storage.c` | 100% |
| `track/track_recover.c` | 100% |
| `settings.c` | 100% |
| `web/web_util.c` | 100% |
| `provision.c` | 100% |
| `selftest/selftest.c` | 100% |
| **`web/web.c`** | **100%** |

## 3. Git

- Репозиторий один, корень `Soft_Decoder_V3`, ветка `main`, remote `origin`.
- Локальный `.githooks/post-commit` дописывает строку в `CHANGELOG.md` и делает
  `--amend`; `DCC_AUTOCHANGELOG=1` пропускает авто-строку.

## 4. Инфраструктура тестов

```powershell
powershell -ExecutionPolicy Bypass -File test\run_tests.ps1
powershell -ExecutionPolicy Bypass -File test\coverage.ps1
powershell -ExecutionPolicy Bypass -File test\coverage.ps1 -Only test_web
```

- `test/run_tests.ps1` и `test/coverage.ps1`: список наборов в `$suites`.
  **При добавлении нового набора обновляйте `$inc` (include-пути) и `$suites`
  в ОБОИХ файлах.**
- Метод — white-box: `#define static` + `#include "<component>/src/x.c"`
  (+ `test_libs/teststubs/stubs.c`, для web — ещё `web_stubs.c`).
- Покрытие агрегируется **по строкам (union)**; частично исполненные строки
  (`N*` в gcov) считаются покрытыми.

### Стабы с флагами отказа
`stubs.c`: `mock_sem_take_fail`, `mock_mutex_create_fail`, `mock_nvs_*`,
`mock_task_create_ok`, `mock_task_create_fail_after`, `mock_adc1_config_*`,
`mock_i2s_*`, `mock_queue_create_fail`, `mock_queue_create_fail_after`,
`mock_queue_send_fail`, `mock_queue_send_fail_after`, `mock_nvs_get_u32_err`,
`mock_nvs_set_u32_err`, `mock_nvs_u32_corrupt`, `mock_gpio_isr_install_err`,
`mock_uart_*`, `mock_usbjtag_*`, `mock_ota_*` (включая `mock_ota_write_fail_after`),
`mock_spi_*`, `mock_flash_*`, `mock_lfs_*`, `mock_partition_register_err`;
мок UART (`mock_uart_feed`/`mock_uart_tx`/`mock_uart_reset`); `mock_timer_now_us`.

`web_stubs.c` (только `test_web`): мок `esp_http_server` (тело/query/заголовок,
захват ответа, таблица маршрутов), `esp_wifi`/`esp_netif`/`esp_event`, скрипт
сокетов (`mock_recv_script_*`), инъекция OOM (`mock_web_malloc/calloc`),
`fsync`/`fwrite`/`fflush`/`fclose` (флаги `mock_ota_*_fail`), моки
settings/audio/motor/auxio/storage/dcc.

### Тест-хуки в коде (ограничение бесконечных циклов)
`track_adc_step`/`s_iter_cap` (track), `s_fx_iter_cap` (auxio),
`s_mix_iter_cap` (audio), `s_motor_iter_cap` (motor), `s_dcc_iter_cap` (dcc),
`s_ack_iter_cap` (dcc ACK), `s_listen_iter_cap` (provision),
`s_autooff_iter_cap`/`s_dns_iter_cap`/`s_pipe_iter_cap` (web). В проде = 0
(вечно). Дополнительно в web: `s_pipe_write_err_inject`, макросы
`WEB_UP_PROGRESS_STEP`, `WEB_FWRITE`, `WEB_FFLUSH`, `WEB_FCLOSE`; в provision:
`PROV_FWRITE`; в selftest: `SELFTEST_FOPEN`/`SELFTEST_FWRITE`/`SELFTEST_FREAD`.

### Переопределяемые пути (для host)
`MANIFEST_DIR` (track_manifest), `STORAGE_MOUNT_POINT` (storage),
`PROV_AUDIO_DIR`/`PROV_MKDIR` (provision), `WEB_USERDATA_DIR`/`WEB_AUDIO_DIR`
(web), `SELFTEST_TMP_PATH` (selftest).

## 5. Что сделано по шагам

1. Закрыты дыры в audio/auxio/dcc/motor/track/pinmap/settings/web_util.
2. `test_storage` + стабы flash/partition/littlefs/spi.
3. `test_provision` + стабы uart/usb-serial-jtag/ota/system/vfs.
4. **`test_web`** (120 тестов) + host-shim `esp_http_server` и стабы
   wifi/netif/event/lwip; OTA (plain + составной `AURAOTA2`), аплоад звуков и
   deep-queue pipeline, все API-обработчики, Wi-Fi/AP, журнал, серверы.
5. Логика восстановления треков вынесена в `track_recover.c`.
6. Fuzz-тесты (`test_web_util`, `test_dcc`), флаг `-Sanitize` (для Linux/CI).
7. **`test_selftest`** (14 тестов) — новый компонент `selftest`: неразрушающий
   `SELFTEST` (heap/pinmap/CV/NVS/LittleFS/ADC/audio/AUX/DCC) и активирующие
   команды `HIL-AUX/SOUND/MOTOR/FN` + прогоны `-FN-SWEEP`/`-AUX-SWEEP`.
8. **HIL-раннеры по железу**: `test/hil/run_hil.ps1` (USB-Serial-JTAG/UART0) и
   `test/hil/run_hil_web.ps1` (SoftAP, все safe REST-эндпоинты).

## 6. Найденные баги (исправлены)

- `web.c: ota_safe_name()` — сравнение расширения до `out[w]='\0'` читало
  мусор за скопированным именем и могло добавить лишний `.wav`.
- `web.c: audio_upload_post()` — при ошибке `fsync()` шорт-цикл пропускал
  `fclose()`, оставляя открытый дескриптор и делая `remove()` неуспешным.
- `web_util.c: buf_appendf()` — недостижимая ветка `n<0` заменена на
  эквивалентную `if (n>0)`.
- `provision.c: ensure_uart_driver()` — guard вынесен из function-local
  static в файловый static (иначе white-box терял персистентность).

## 7. Быстрые команды

```powershell
# тесты и покрытие
powershell -File test\run_tests.ps1
powershell -File test\coverage.ps1

# сборка прошивки (из firmware/)
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e esp32-s3-devkitc-1
# при изменениях CMake/sdkconfig/partitions (или после смены REQUIRES):
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e esp32-s3-devkitc-1 -t fullclean
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e esp32-s3-devkitc-1

# HIL на железе (нужна плата)
powershell -File test\hil\run_hil.ps1 -Port COMx -Sweep            # все F и AUX
powershell -File test\hil\run_hil.ps1 -Port COMx -Actuate -MotorSpeed 20
powershell -File test\hil\run_hil_web.ps1 -RestoreSsid <домашний_SSID>  # REST
```
