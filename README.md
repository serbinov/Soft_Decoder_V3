# Soft Decoder V3 — ADITIPUS AURA-X

Прошивка DCC-декодера для моделей железных дорог на ESP32-S3 (ESP-IDF,
PlatformIO). Версия: **0.7** (`firmware/version.txt`).

Полное техническое описание — [`firmware/ARCHITECTURE.md`](firmware/ARCHITECTURE.md).
История изменений — [`CHANGELOG.md`](CHANGELOG.md).
Исправления по код-ревью — [`firmware/FIXES_LOG.md`](firmware/FIXES_LOG.md) и
[`firmware/CODE_REVIEW_REPORT.md`](firmware/CODE_REVIEW_REPORT.md).

## Возможности

**Движение**
- Приём DCC с рельс: короткий/длинный адрес, 14/28/128 шагов скорости,
  consist (CV19), функции F0–F28, broadcast reset/e-stop.
- BEMF-регулирование оборотов под нагрузкой; калибровка из веба.
- Кривая скорости, Vstart (CV2), Vmid (CV6), Vhigh (CV5), разгон/торможение
  (CV3/CV4), таблица CV67–CV94.
- Режим аналогового (DC) управления от напряжения на рельсах.
- Защита: аварийный стоп, таймаут пакетов (CV11), контроль живости задачи.

**Функции и свет**
- F0–F28, выходы: передний/задний свет (F0F/F0R), AUX1–AUX9.
- Эффекты AUX: включение/выключение, мигание (несколько режимов).

**Звук**
- WAV 22 кГц, микшер до 20 голосов; громкости: общая/двигатель/эффекты.
- Загрузка звуков через веб и по COM (PROV); хранение на внешней NOR.
- Метаданные слотов (имена/категории, карта F↔AUX) в манифесте `tracks.txt`,
  переживают полный сброс NVS.

**Веб-интерфейс и связь**
- Управление мотором и функциями, редактор CV, загрузка звуков, калибровка BEMF.
- OTA-обновление прошивки и звуков (в т.ч. одним файлом).
- Wi-Fi: точка доступа по умолчанию (без пароля, пока пользователь не задаст)
  либо режим клиента; встроенный веб-сервер, captive portal (перехват DNS).

**Сервис**
- Программирование CV в сервисном режиме DCC, сброс CV (CV8=8).
- Диагностика по COM (BEMF, CV), провижининг звуков.
- Неразрушающий `SELFTEST` и HIL-команды.

## Структура репозитория

```
firmware/            исходники прошивки и документация
  components/        модули: dcc, motor, track, audio, auxio, web, settings,
                     storage, pinmap, provision, selftest
  main/              app_main.c
  test/              host-тесты (Unity + gcc) и test/hil (по железу)
  test_libs/         заглушки ESP-IDF для host-тестов
  tools/             gen_web_html.py, bump_version.ps1, gen_flash_readme.ps1
  web_ui.html        исходник веб-страницы (сборка генерирует web_html.h)
release/             собранные образы (например ADDITIPUS_AURA-X_v0.7.bin)
web_flasher/         веб-flasher (Web Serial) для прошивки из браузера
.kilo/               агенты/команды код-ревью (см. .kilo/agent, .kilo/command)
```

## Сборка и прошивка

Требуется PlatformIO (`pio`). Из каталога `firmware/`:

```powershell
pio run -e esp32-s3-devkitc-1                       # сборка
pio run -e esp32-s3-devkitc-1 -t fullclean          # при смене CMake/sdkconfig/partitions
flash_firmware.bat [COMx] [erase]                   # сборка + прошивка (автопоиск порта)
build_ota_bin.bat                                   # OTA-образ в release/
build_flash_tool_files.bat                          # файлы для Espressif Flash Download Tool
build_ota_with_sounds.ps1                           # составной OTA (прошивка + звуки)
```

Скрипты прошивки/звуков: `flash_firmware_and_sounds.bat`, `provision_sounds.ps1`,
`flash_standalone.ps1`, `read_bemf.ps1`, `bump_version.bat`.

## Тесты

```powershell
powershell -ExecutionPolicy Bypass -File test\run_tests.ps1   # host-наборы (Unity/gcc)
powershell -ExecutionPolicy Bypass -File test\coverage.ps1    # покрытие (gcov), 100% first-party
```

HIL по железу (нужна плата): `test\hil\run_hil.ps1` и `test\hil\run_hil_web.ps1`.
Подробности и «подводные камни» — в `firmware/ARCHITECTURE.md` §4.4.

## Git-хуки

После клонирования один раз выполните `setup_git_hooks.bat` — тогда каждый коммит
автоматически дописывает строку в `CHANGELOG.md`.
