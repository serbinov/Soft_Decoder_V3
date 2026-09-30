# Soft Decoder V3 — ADDITIPUS AURA-X

Прошивка DCC-декодера для моделей железных дорог на ESP32-S3 (ESP-IDF 6.0).
Версия: **0.9** (`firmware/version.txt`).

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

## Установка с нуля

Проект собирается через **ESP-IDF 6.0** (`idf.py`); PlatformIO не используется.
Для чистой машины нужны:

| Требование | Зачем |
|---|---|
| Windows 10/11 + PowerShell 5.1 | обёртки `*.bat` / `*.ps1` |
| Git | клонирование проекта и ESP-IDF |
| Python 3.9+ | `install.bat` и `idf.py` |
| Интернет | первая установка ESP-IDF (~2 ГБ инструментов) |
| (опц.) C-компилятор: TCC или gcc/clang в PATH | только host-тесты |

**Шаг 1. Клонировать репозиторий**

```powershell
git clone <URL> Soft_Decoder_V3
cd Soft_Decoder_V3
```

**Шаг 2. Поставить окружение (ESP-IDF 6.0 + `.idf_path` + VS Code + первая сборка)**

```powershell
.\setup.ps1              # найдёт ESP-IDF в типовых путях
.\setup.ps1 -Install     # если ESP-IDF 6.0 не установлен — клонирует и поставит esp32s3
```

Скрипт проверяет `git`/`python`, пишет путь к IDF в `firmware\.idf_path`, ставит
рекомендуемые расширения VS Code и делает первую сборку (создаёт
`firmware\build\compile_commands.json` для IntelliSense).

ESP-IDF также можно поставить вручную:

```powershell
git clone -b v6.0 --recursive https://github.com/espressif/esp-idf.git C:\esp\v6.0\esp-idf
C:\esp\v6.0\esp-idf\install.bat esp32s3
```

**Шаг 3. Собрать и прошить**

```powershell
cd firmware
.\idf_build.ps1 -Flash              # сборка + прошивка (COM определяется автоматически)
```

## Сборка и прошивка

Из каталога `firmware/`. Путь к ESP-IDF 6.0 задаётся через `-IdfPath`,
переменную `$env:IDF_PATH` или файл `firmware\.idf_path`:

```powershell
.\idf_build.ps1                     # сборка (idf.py build)
.\idf_build.ps1 -Flash              # сборка + прошивка (автопоиск COM)
.\idf_build.ps1 -Flash -Monitor     # + монитор порта
.\idf_build.ps1 -Erase -Flash       # полное стирание чипа + прошивка
.\idf_build.ps1 -Clean              # fullclean
.\build_idf.bat / .\flash_idf.bat   # то же из cmd.exe
```

Готовые образы и вспомогательные скрипты:

```powershell
flash_firmware.bat [COMx] [erase]                   # сборка + прошивка (автопоиск порта)
build_ota_bin.bat                                   # OTA-образ в release/
build_flash_tool_files.bat                          # файлы для Espressif Flash Download Tool
build_ota_with_sounds.ps1                           # составной OTA (прошивка + звуки)
```

Скрипты прошивки/звуков: `flash_firmware_and_sounds.bat`, `provision_sounds.ps1`,
`flash_standalone.ps1`, `read_bemf.ps1`, `bump_version.bat`.
Звуковые скрипты читают WAV из папки `SOUND` рядом с репозиторием (`..\..\SOUND`).

## VS Code

Конфигурация лежит в `.vscode/` (открывать нужно **корень репозитория**):

- `extensions.json` — рекомендуются `ms-vscode.cpptools` и `espressif.esp-idf-extension`;
- `c_cpp_properties.json` / `settings.json` — IntelliSense через
  `firmware/build/compile_commands.json` (создаётся сборкой);
- `tasks.json` — задачи **Запуск задачи**: `ESP-IDF: build` (Ctrl+Shift+B),
  `flash`, `flash + monitor`, `erase + flash`, `fullclean`, `Host tests`;
- `launch.json` — отладка через расширение ESP-IDF (`ESP-IDF: debug`).

## Структура репозитория

```
firmware/            исходники прошивки и документация
  components/        модули: dcc, motor, track, audio, auxio, sound, web,
                     settings, storage, pinmap, provision, selftest
  main/              app_main.c
  test/              host-тесты (Unity + gcc) и test/hil (по железу)
  test_libs/         заглушки ESP-IDF для host-тестов
  tools/             gen_web_html.py, bump_version.ps1, gen_flash_readme.ps1
  web_ui.html        исходник веб-страницы (сборка генерирует web_html.h)
  idf_build.ps1      сборка/прошивка через idf.py
release/             собранные образы (ADDITIPUS_AURA-X_v<ver>.bin)
web_flasher/         веб-flasher (Web Serial) для прошивки из браузера
.vscode/             конфигурация VS Code (задачи, IntelliSense, отладка)
setup.ps1            автонастройка окружения (ESP-IDF, .idf_path, VS Code)
.kilo/               агенты/команды код-ревью (см. .kilo/agent, .kilo/command)
```

## Тесты

```powershell
powershell -ExecutionPolicy Bypass -File test\run_tests.ps1   # host-наборы (Unity + TCC/gcc)
powershell -ExecutionPolicy Bypass -File test\coverage.ps1    # покрытие (gcov), 100% first-party
```

Первый запуск скачает Unity (нужен Git/интернет). Компилятор ищется так:
`$env:DCC_TEST_CC` → портативный TCC в `%TEMP%\tcc` → `gcc`/`clang` в PATH.

HIL по железу (нужна плата): `test\hil\run_hil.ps1` и `test\hil\run_hil_web.ps1`.
Подробности и «подводные камни» — в `firmware/ARCHITECTURE.md` §4.4.

## Git-хуки

После клонирования один раз выполните `setup_git_hooks.bat` — тогда каждый коммит
автоматически дописывает строку в `CHANGELOG.md`.
