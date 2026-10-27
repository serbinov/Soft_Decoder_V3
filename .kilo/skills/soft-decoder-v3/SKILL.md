---
name: soft-decoder-v3
description: Use when working on ADDITIPUS AURA-X / Soft_Decoder_V3 (ESP32-S3 DCC decoder): firmware, DCC, motor/BEMF, sound graph, web UI/API, storage, provisioning, tests, builds and releases.
---

# ADDITIPUS AURA-X / Soft_Decoder_V3

## Что это

DCC-декодер для моделей железных дорог на ESP32-S3: приём DCC с рельсов,
мотор DRV8870 с ШИМ и BEMF-PID, 20 звуковых голосов через I2S → MAX98357A,
9 AUX/световых выходов, SoftAP с веб-страницей и REST API, NVS + внешняя
LittleFS (W25Q128), OTA и UART-провижининг звуков. Стек: ESP-IDF 6.0, C11,
FreeRTOS, без C++. Полное описание — `firmware/ARCHITECTURE.md`.

## Первые действия

1. Прочитайте `firmware/version.txt` (сейчас **0.12**), `firmware/docs/known_issues.md`, `firmware/ARCHITECTURE.md`.
2. Проверьте `git status` и `git diff` — не смешивайте свои правки с чужими.
3. Не открывайте и не редактируйте `.kilo/worktrees/`, `.kilo/agent-manager.json`, `release/` вручную; не трогайте чужие worktree.
4. Найдите затронутый компонент и его тесты до изменения кода. Источник истины для версии — только `firmware/version.txt`.
5. Для веба правьте `firmware/web_ui.html`; для редактора — `firmware/sound_editor/public/`, затем `npm run build` и коммит `dist/`.

## Незыблемые правила

- **Никаких flash/erase/format/reset без явного подтверждения пользователя.** Это `idf_build.ps1 -Flash/-Erase`, `storage_format()`, `PROV`, `/api/reset`, OTA и скрипты прошивки. `-Erase` и `PROV` уничтожают NVS и/или внешнюю NOR.
- **Мотор:** сохраняйте инварианты stop/inhibit/PWM-off. `motor_boot_safe()` вызывается в начале `app_main`; после `motor_init()` мотор inhibited до готовности safety. `motor_stop()`, `motor_emergency_stop()`, `motor_set_inhibited(true)` и e-stop должны немедленно переводить мост в coast и не восстанавливать старую скорость.
- **DCC-колбэки не блокируются:** никаких NVS/FS-операций, длинных ожиданий мьютексов, `vTaskDelay` и калибровки из `on_dcc_speed/function/cv_*`. Запись CV — через deferred commit; ACK — через `dcc_ack`.
- **Не ослабляйте проверки BEMF/rail/ADC/fault и safety-таймауты без стендовых измерений.** Host-тест не доказывает поведение на реальном двигателе.
- **Хранимые форматы и миграции неприкосновенны:** NVS keys, `cv_record`, legacy `cv`/`cv_crc`, `tracks`, манифест `tracks.txt`, `.asp`, `AURAOTA2`. Меняйте формат только с миграцией и тестами.
- **Не редактируйте release-артефакты вручную:** `manifest.json`, `SHA256SUMS.txt`, `*.bin`, `firmware.bin`. Их создаёт `release_artifacts.py`; `firmware.bin` — raw application, не merged-образ.
- **Не добавляйте секреты** (пароли, ключи, токены) в код, конфиги, тесты или документацию.
- **Веб-API намеренно без аутентификации:** граница безопасности — пароль SoftAP. Не расширяйте CORS и не убирайте проверки `Origin/Referer` (`web_util_same_origin`, `mutation_dispatch`) и nonce панели `web_flasher`.
- **`sdkconfig` генерируется:** правьте `sdkconfig.defaults`. `version.txt` и `SETTINGS_CV7_VERSION` синхронизирует `tools/bump_version.ps1`.

## Карта репозитория

| Компонент | Ключевые файлы | Ответственность |
|---|---|---|
| `pinmap` | `include/pinmap.h`, `src/pinmap.c` | Все GPIO и `pinmap_validate()` |
| `settings` | `src/settings.c`, `src/track_manifest.c` | NVS, CV 1..512, треки, F-привязки, AUX cfg, BEMF cal/window, манифест |
| `storage` | `src/storage.c` | Внешняя NOR, LittleFS `/userdata`, leases и maintenance |
| `dcc` | `src/dcc.c`, `include/dcc.h` | ISR, пакеты, 14/28/128, service/ops mode, CV19/21/22, e-stop, CV11 |
| `motor` | `src/motor.c`, `include/motor.h`, `include/bemf_cal_base.h` | DRV8870 PWM, ramp, kickstart, BEMF PID, calibration, preflight/window |
| `track` | `src/track.c`, `src/track_recover.c` | Rail ADC, DC-режим, восстановление метаданных |
| `audio` | `src/audio.c`, `src/ima_adpcm.c` | 20 голосов, ресемплинг, I2S 22050 Гц, ADPCM |
| `sound` | `src/sound.c`, `src/sound_graph*.c`, `include/sound_graph*.h` | Граф, раннер, store, движок, Fx |
| `auxio` | `src/auxio.c` | 9 выходов, гамма, световые эффекты |
| `web` | `src/web.c`, `src/web_wifi.c`, `src/web_util.c` | SoftAP, HTTP, REST, OTA, upload, pack, maintenance |
| `provision` | `src/provision.c` | UART `PROV`/`PUT`/`PACK`, BEMF/HIL-консоль |
| `selftest` | `src/selftest.c` | Неразрушающий `SELFTEST`, HIL-актуация |
| `main` | `main/app_main.c` | Порядок старта, DCC-колбэки, safety 10 мс, persistence |
| `tools`/`docs` | `tools/*.py`, `docs/*.md` | Генерация веба, pack, release, bump, схема звука, ограничения |

## Быстрые команды

Полный host-набор (C + Node/Python; ориентир 0.12 — **1074 теста**, coverage **88,7 %**; фактическое число сверяйте с итоговой строкой):

```powershell
& .\firmware\test\run_tests.ps1
```

Только затронутые наборы (быстрее в цикле правок):

```powershell
& .\firmware\test\run_tests.ps1 -Only test_motor,test_web,test_sound_graph
```

Целевая сборка (IDF 6.0 в `C:\esp\v6.0\esp-idf`, tools в `C:\Users\user\.espressif`):

```powershell
& .\firmware\idf_build.ps1 -Release
& .\firmware\idf_build.ps1 -Clean
```

`-Clean` нужен после смены версии, CV7, `sdkconfig.defaults`, таблицы разделов и при странных ошибках генерации. `-Release` только собирает и публикует; не совмещайте с `-Flash/-Erase/-Monitor`.

Проверка release без платы:

```powershell
python firmware\tools\release_artifacts.py verify --release release
```

Веб-панель прошивки:

```powershell
web_flasher\start_flasher.bat
```

Открывайте `http://127.0.0.1:8765/web_flasher/index.html`. Не `localhost`: панель сверяет `Origin` с `127.0.0.1`, иначе действия получают 403 (известное ограничение).

## Definition of done

- Затронутые наборы зелёные, затем полный `run_tests.ps1`; при недоступном Node/Python прогон считается неполным.
- Целевая сборка проходит. Она ловит `-Werror=format-truncation`, отсутствующий `PRIV_REQUIRES esp_driver_gpio` и устаревший `sdkconfig`.
- Если менялся релиз — `release_artifacts.py verify` и проверка версии/`manifest.json`.
- Версия, CV7, `version.txt`, веб-версия и документация согласованы; `known_issues.md`/`ARCHITECTURE.md` обновлены при изменении поведения.
- Нет секретов, отладочных паролей и мусорных файлов; diff просмотрен; коммит — только по явной просьбе.

## Текущее состояние и остаточные риски

- BEMF всё ещё требует стендовой валидации на конкретном двигателе; host-тесты не подтверждают обороты и устойчивость.
- HIL-калибровка BEMF — opt-in (`test/hil/run_hil_web.ps1 -CalibrateBemf`); обычный serial HIL её не покрывает.
- Журнал/транзакция звукового пакета — best-effort: recovery пытается откатить `PREPARED` или дочистить `COMMITTED`, но это не транзакционная БД.
- Runtime `sound.c` покрыт слабо (~28,6 %); `provision.c` и `web.c` — следующие цели.
- Целевая сборка и coverage на каждой локальной правке обычно не запускаются; перед релизом это обязательный шаг.

## Справочники

- [references/architecture.md](references/architecture.md) — платформа, задачи, потоки данных, модель графа, инварианты, карта функций. Читайте перед изменением межмодульного поведения.
- [references/change-playbooks.md](references/change-playbooks.md) — конкретные рецепты по подсистемам, файлам и тестам. Открывайте перед правкой соответствующего модуля.
- [references/review-checklist.md](references/review-checklist.md) — приоритетный чек-лист ревью, do-not-regress 0.12, подводные камни сборки и проверка PR.
