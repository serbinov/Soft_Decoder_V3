# Soft Decoder V3 — ADDITIPUS AURA-X

Прошивка DCC-декодера для моделей железных дорог на ESP32-S3 (ESP-IDF 6.0).
Версия: **0.10** (`firmware/version.txt`).

Полное техническое описание — [`firmware/ARCHITECTURE.md`](firmware/ARCHITECTURE.md).
История изменений — [`CHANGELOG.md`](CHANGELOG.md).
Диагностика и калибровка BEMF — [`firmware/BEMF_DIAGNOSTICS.md`](firmware/BEMF_DIAGNOSTICS.md).
Актуальные ограничения и открытые вопросы — [`firmware/docs/known_issues.md`](firmware/docs/known_issues.md).

## Возможности

**Движение**
- Приём DCC с рельс: короткий/длинный адрес, 14/28/128 шагов скорости,
  consist (CV19), функции F0–F28, broadcast reset/e-stop.
- BEMF-регулятор и калибровка из веба. Устойчивость на конкретном двигателе
  требует проверки. Исправлены freshness измерений, расчёт калибровки и
  небезопасный fallback при потере feedback; PID без измерений не перенастраивался.
- Кривая скорости, Vstart (CV2), Vmid (CV6), Vhigh (CV5), разгон/торможение
  (CV3/CV4), таблица CV67–CV94.
- Режим аналогового (DC) управления: CV29.bit2 разрешает обнаружение DC,
  но не отключает продолжающийся DCC-поток.
- Защита: аварийный стоп, таймаут пакетов (CV11), контроль живости задачи.
  Stop/inhibit отменяют калибровку. Движение запрещено до готовности safety;
  persistent feedback fault снимается STOP или явным BEMF disable, а не
  повтором ненулевой команды. Это не аппаратная сертификация безопасности.

**Функции и свет**
- F0–F28, всего 9 световых выходов: F0F/F0R и AUX1–AUX7.
- Эффекты AUX: включение/выключение, мигание (несколько режимов).

**Звук**
- Выход 22,05 кГц; PCM16 WAV 1–192 кГц, 1–2 канала. До 20 голосов: голос 19
  зарезервирован под двигатель, эффектам доступны голоса 0..18 (до 19
  одновременных эффектов). Допустимость формата не гарантирует throughput любой
  комбинации файлов. Громкости: общая/двигатель/эффекты.
- Загрузка звуков через веб и по COM (PROV); хранение на внешней NOR.
- Пакетная загрузка звуков одним файлом: контейнер `.asp` (до 400 звуков,
  по умолчанию IMA ADPCM ~4×, `--pcm` для PCM16), собирается на ПК
  `firmware/tools/pack_sounds.py`; пароль защищает пакет через scrypt +
  ChaCha20 + HMAC-SHA256; при загрузке страница «Загрузка звуков»
  показывает компактный список имён с кнопкой ▶ и пофайловой громкостью, а
  старые звуки удаляются. Формат — `firmware/docs/sound_pack.md`.
- Прослушивание звуков по умолчанию — в браузере компьютера (переключатель
  «На компьютере / На декодере»); конструктор блоков тоже играет на ПК через
  `GET /sound-files/<name>`.
- Конструктор блоков открывается двумя пунктами меню — **Миксы** и
  **Звуковые схемы** (один редактор, `?mode=mix`/`?mode=scheme`); в каждом
  режиме видны только блоки своего раздела. Двигатель работает всегда и имеет
  только выходы ступеней скорости — звук подключается к ним блоками «Звук».
  При первом открытии загружается типовой локомотив: двигатель с четырьмя
  схемами по ступеням скорости, семь звуковых схем (гудок, звонок, компрессор,
  сцепка, песочница, двери открыть/закрыть), логика Mixes через cue-блоки,
  ambient-тормоз, фары/AUX1..4 и notch; F14 — условный AUX4 «Вентилятор»
  (`speed > 20`). **Файл → Типовой локомотив** загружает его повторно,
  **Новый** создаёт пустой проект.
- Метаданные слотов (имена/категории, карта F↔AUX) в манифесте `tracks.txt`,
  переживают полный сброс NVS.
- CV30/CV114–CV116 сохранены для совместимости, но в 0.10 не влияют на
  воспроизведение: rate/volume задаются в блоках, общая громкость — CV63
  (`firmware/docs/cv_sound.md`).

**Веб-интерфейс и связь**
- Управление мотором и функциями, редактор CV, загрузка звуков, калибровка BEMF.
- OTA-обновление прошивки и звуков (в т.ч. одним файлом).
- Wi-Fi: только точка доступа SoftAP (без пароля, пока пользователь не задаст);
  встроенный веб-сервер, captive portal (перехват DNS). Режим подключения
  к роутеру (STA/APSTA) не реализован.

**Сервис**
- Программирование CV в сервисном режиме DCC, сброс CV (CV8=8).
- Диагностика по COM (BEMF, CV), провижининг звуков.
- Неразрушающий `SELFTEST` и HIL-команды.

## Установка с нуля

Проект собирается через **ESP-IDF 6.0** (`idf.py`).
Для чистой машины нужны:

| Требование | Зачем |
|---|---|
| Windows 10/11 + PowerShell 5.1 | обёртки `*.bat` / `*.ps1` |
| Git | клонирование проекта и ESP-IDF |
| Python 3.10+ | `install.bat` и `idf.py` (минимум для ESP-IDF 6.0) |
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

Из корня проекта **`build_firmware.bat`** собирает без прошивки и публикует
проверенный комплект в `release/`: `ADDITIPUS_AURA-X_v<ver>.bin`, `_OTA.bin`,
четыре фиксированных файла в `release/flash_download_tool/` и `manifest.json`
с версией, адресами и SHA256. Этот комплект использует `web_flasher`.
Для автоматизации: `build_firmware.bat --no-pause`. Полная офлайн-проверка:
`python firmware/tools/release_artifacts.py verify --release release`
(проверка обязательна после `build_firmware.bat`).

Из каталога `firmware/`. Путь к ESP-IDF 6.0 задаётся через `-IdfPath`,
переменную `$env:IDF_PATH` или файл `firmware\.idf_path`:

```powershell
.\idf_build.ps1                     # сборка (idf.py build)
.\idf_build.ps1 -Release            # сборка + публикация и проверка release
.\idf_build.ps1 -Flash              # сборка + прошивка (автопоиск COM)
.\idf_build.ps1 -Flash -Monitor     # + монитор порта
.\idf_build.ps1 -Erase -Flash       # полное стирание чипа + прошивка
.\idf_build.ps1 -Clean              # fullclean
```

Готовые образы и вспомогательные скрипты:

```powershell
build_firmware.bat                                  # сборка + публикация release/ (из корня)
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

## Предпросмотр веб-интерфейса

Без платы страницу и конструктор блоков можно открыть через mock-сервер:

```powershell
web_preview\start_preview.bat
# или: python web_preview\mock_server.py [port]
```

`http://127.0.0.1:8080/` отдаёт настоящий `firmware/web_ui.html` и собранный
`firmware/sound_editor/dist`; API отвечает тестовыми данными. После правки
`firmware/sound_editor/public/blocks.html` пересоберите бандл
(`cd firmware\sound_editor; npm run build`) и закоммитьте `dist/`, иначе сборка
прошивки отклонит устаревшие ассеты (см. `firmware/docs/sound_blocks_editor.md`).

## Структура репозитория

```
firmware/            исходники прошивки и документация
  components/        модули: dcc, motor, track, audio, auxio, sound, web,
                     settings, storage, pinmap, provision, selftest
  main/              app_main.c
  docs/              sound_scheme.md, sound_pack.md, sound_blocks_editor.md,
                     cv_sound.md, known_issues.md
  sound_editor/      исходник и собранный офлайн-редактор блоков
                     (public/ + scripts/, dist/ коммитится)
  test/              host-тесты (Unity + gcc), test/sound_editor_assets.py,
                     test/test_web_ui.js и test/hil (по железу)
  test_libs/         заглушки ESP-IDF для host-тестов
  tools/             gen_web_html.py, gen_sound_editor.py, pack_sounds.py,
                     release_artifacts.py, bump_version.ps1, gen_flash_readme.ps1
  web_ui.html        исходник веб-страницы (сборка генерирует web_html.h)
  idf_build.ps1      сборка/прошивка через idf.py
release/             собранные образы (ADDITIPUS_AURA-X_v<ver>.bin),
                     flash_download_tool/ и manifest.json
SOUND/               исходные WAV-сэмплы для провижининга звуков
web_flasher/         веб-панель (статус/настройка и запуск скриптов): start_flasher.bat
web_preview/         локальный mock-сервер для браузерной проверки web_ui.html
.vscode/             конфигурация VS Code (задачи, IntelliSense, отладка)
.githooks/           git-хуки (запись в CHANGELOG.md после коммита)
setup.ps1            автонастройка окружения (ESP-IDF, .idf_path, VS Code)
.kilo/               агенты/команды код-ревью (см. .kilo/agent, .kilo/command)
```

## Тесты

Команды выполняются из каталога `firmware/`:

```powershell
powershell -ExecutionPolicy Bypass -File test\run_tests.ps1   # C + JS/Python suites (Unity + TCC/gcc)
bash test/run_tests.sh                                        # Linux/CI C suites
bash test/run_tests.sh --sanitize                             # ASan + UBSan (CI)
powershell -ExecutionPolicy Bypass -File test\coverage.ps1    # покрытие строк host-сборок (gcov)
```

CI (`.github/workflows/host-tests.yml`) запускает C-наборы, JS/Python-тесты и
отдельный job с ASan+UBSan.

Итоговый прогон 2026-10-20: **18 C-наборов, 800 тестов PASS**
и **75 JavaScript/Python-тестов PASS** (37 web UI, 13 block editor,
8 pack integration, 17 Python asset/release/crypto). Для JS нужен Node.js (проверено
с 22.18); без него runner выдаёт предупреждение, а JS не считается проверенным.

```powershell
node --test test\test_web_ui.js
```

Покрытие (`test\coverage.ps1`, 2026-10-20): **92,9 % (8266/8893 строк)** —
union исполняемых строк first-party исходников (`components/` + `main/`),
попавших в gcov из host-сборок. Это не покрытие всех ветвей, всего кода или
физического поведения устройства; RTOS/аппаратная обвязка в host-gcov не входит.
Ветвевое покрытие gcov — **76,2 % (6449/8468)**; см. `ARCHITECTURE.md` §4.4.

Первый запуск скачает Unity (нужен Git/интернет). Компилятор ищется так:
`$env:DCC_TEST_CC` → портативный TCC в `%TEMP%\tcc` → `gcc`/`clang` в PATH.

HIL по железу (нужна плата): `test\hil\run_hil.ps1` и `test\hil\run_hil_web.ps1`.
PASS host/HIL не подтверждает реальные обороты и устойчивость BEMF:
нужны отдельные измерения на подключённом двигателе. Веб-HIL для мотора
проверяет только команду остановки.
Подробности и «подводные камни» — в `firmware/ARCHITECTURE.md` §4.4.

## Git-хуки

После клонирования один раз выполните `setup_git_hooks.bat` — тогда каждый коммит
автоматически дописывает строку в `CHANGELOG.md`.
