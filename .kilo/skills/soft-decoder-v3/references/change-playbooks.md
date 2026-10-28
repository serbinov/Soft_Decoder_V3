# Playbooks изменений

Рецепты для типовых правок. Перед началом прочитайте [architecture.md](architecture.md) и
`firmware/ARCHITECTURE.md`; после — [review-checklist.md](review-checklist.md). Команды — из корня.

## DCC
**Файлы:** `firmware/components/dcc/src/dcc.c`, `include/dcc.h`, `firmware/main/app_main.c`, `firmware/test/test_dcc/`, `firmware/test/test_main/`.

1. Найдите путь разбора в `dispatch()` и проверьте `repeated()`: повторный пакет
   не должен повторно писать CV или выдавать ACK.
2. Для service mode помните: вход — reset с преамбулой ≥20, пакет ровно 4 байта,
   окно 20 мс, инструкции `0x74/0x78/0x7C`; short address 124..127 конфликтует с
   Write Byte, есть отдельное разрешение конфликта; ACK асинхронный.
3. Проверьте 28-step: `code==1` — e-stop, `0` — stop, `2..28` — скорость,
   `29+` — 126; mapping `1 + (code-2)*125/27`. Для 14-step F0 приходит в пакете
   скорости и не должен сбрасываться group-one. В 128-step `raw==1` — e-stop,
   `raw==0` — stop, иначе `raw-1`.
4. Broadcast (`addr==0`) принимает только reset/e-stop и stop/e-stop скорости;
   никогда функции/CV. Long address `0xC000` не должен алиасить broadcast.
5. CV19: consist и реверс; CV21/22 маскируют только F0/F1..F8/F9..F12, F13+
   проходят всегда. CV11 timeout — 0,5 с на единицу, только rails и не DC;
   это отдельный inhibit, не снимайте чужие.
6. Не добавляйте блокирующие вызовы в колбэки; CV commit — deferred, ACK — worker.

```powershell
& .\firmware\test\run_tests.ps1 -Only test_dcc,test_main
```

## Мотор и BEMF
**Файлы:** `firmware/components/motor/src/motor.c`,
`include/motor.h`, `include/bemf_cal_base.h`, `firmware/components/settings/src/settings.c`,
`firmware/components/web/src/web.c`, `firmware/main/app_main.c`,
`firmware/test/test_motor/`, `firmware/test/test_motor_concurrency/`.

1. Сохраните stop/inhibit: boot-safe, inhibited после init, немедленный coast,
   независимые reason-флаги, feedback fault снимается только stop/disable.
2. Kickstart CV65 масштабирует duty верхней скорости; PID не опускает duty ниже
   kick-порога. Ramp CV3/CV4 и кривая CV2/CV5/CV6/CV67..94 — не ломайте порядок.
3. PID: rail >300 мВ, валидный BEMF, ненулевая скорость, нет калибровки; при
   потере feedback duty не растёт (bounded hold ≤100 мс, затем coast/fault).
   Не ослабляйте rail/ADC/saturation/plausibility проверки.
4. Калибровка: preflight (стоящий локомотив, статический сенсор, полярность) →
   optimal window → curve; dwell по умолчанию 1000 мс (max 5000). Плохая кривая
   не заменяет предыдущую; частичная кривая — только по правилам (≥2 точек,
   последняя доля ≥51/1024), иначе `CURVE`.
5. Любое изменение safety-порогов — только со стендовыми данными; приложите
   `BEMF-RAW`/`BEMF-COAST` и укажите двигатель.

```powershell
& .\firmware\test\run_tests.ps1 -Only test_motor,test_motor_concurrency,test_settings,test_main
```

## Звук и аудио
**Файлы:** `firmware/components/sound/src/sound.c`, `sound_graph.c`,
`sound_graph_runner.c`, `sound_graph_store.c`, `include/sound_graph*.h`,
`firmware/components/audio/src/audio.c`, `ima_adpcm.c`,
`firmware/components/web/src/web.c`, `firmware/docs/sound_scheme.md`,
`firmware/test/test_sound_graph/`, `test_sound_graph_store/`, `test_audio/`,
`test_web/`.

1. Схема: states/transitions/effects/variables/assets/engine. Соблюдайте лимиты
   192/144/96/250/32/128/16 и JSON ≤128 КиБ; ID/имена/файлы проверяйте
   `sg_id_valid`/`sg_filename_valid`.
2. Engine: `engine_fn`, `engine_hold`, `hysteresis`; F13+ и Fx-триггеры не
   должны дублировать эффекты. Граф не командует мотором — только читает
   applied speed/direction.
3. Аудио: 20 голосов, голос 19 — движок, эффектам 0..18; I2S 22050 Гц, моно,
   блок 256. Проверяйте stale handles/generation, EOF → переalloc, rate
   500..3000 permille, анти-щёлчок фейд.
4. Ошибки I2S/декодера не должны «залипать» канал: `SG_AUDIO_IDLE` освобождает
   голос и повторяет состояние; `FAILED` — устойчивый отказ.
5. Apply требует стоящий мотор; выбранная ревизия не удаляется; store-конфликт
   (`expected_revision`) не перезаписывайте молча.

```powershell
& .\firmware\test\run_tests.ps1 -Only test_sound_graph,test_sound_graph_store,test_audio,test_audio_pack,test_web
```

## Хранилище и persistence
**Файлы:** `firmware/components/settings/src/settings.c`, `track_manifest.c`,
`firmware/components/storage/src/storage.c`, `firmware/components/web/src/web.c`,
`firmware/main/app_main.c`, `firmware/test/test_settings/`, `test_track_manifest/`,
`test_storage/`, `test_track_recover/`, `test_web/`, `test_main/`.

1. NVS namespace `decoder`; ключи: `wifi_mode`, `ap_*`, `auto_off`, `mvol/evol/fvol`,
   `slot`, `csrc`, `dev_name`, `cv_record`, legacy `cv`/`cv_crc`, `tracks`,
   `track_cat`, `func_map`, `func_bind`, `aux_cfg`, `bemf_cal`, `bemf_use`,
   `bemf_win` (100..2000 мкс), `recover`.
2. `cv_record` версионирован и CRC32; legacy `cv` — FNV-1a. Не меняйте layout без
   миграции и теста old→new.
3. LittleFS: `/userdata/audio/*.wav`, `tracks.txt`, `volumes.txt`, `selection`,
   ревизии графа. Манифест переживает сброс NVS; при неполном recovery маркер
   `recover` повторяет все metadata keys.
4. Зарезервированные имена upload: `.pb*`, `*.tmp`, `tracks.txt`, `volumes.txt`.
   Не разрешайте пользователю их создать.
5. Pack: journal `.packjournal` (`AURAPKJ1`, PREPARED/COMMITTED) пишется и
   fsync до перемещения старой библиотеки; COMMITTED — после NVS+manifest.
   Recovery best-effort; не удаляйте backup/journal при неуверенности.
6. Форматирование внешней NOR — только явно и с подтверждением.

```powershell
& .\firmware\test\run_tests.ps1 -Only test_settings,test_track_manifest,test_storage,test_track_recover,test_web,test_main
```

## Веб, API и безопасность
**Файлы:** `firmware/components/web/src/web.c`, `web_wifi.c`, `web_util.c`,
`include/web.h`, `include/web_util.h`, `firmware/test/test_web/`, `test_web_util/`,
`test_main/`.

1. Каждая мутация проходит `mutation_dispatch()`: `Origin/Referer` должен
   совпадать с `Host` (scheme `http`, порт 80); без заголовков запрос — не
   браузерный. Не ослабляйте `web_util_same_origin()`.
2. Панель `web_flasher` дополнительно требует per-run nonce (`X-Aura-Nonce` или
   cookie `aura_nonce`) и `Origin == http://127.0.0.1:<port>`; не отключайте.
3. Upload: `sanitize_name`, lower-case `.wav`, `UPLOAD_MAX` 2 МиБ, reserved names,
   staging + canonical rename, lease/maintenance. OTA: `AURAOTA2` ≤8 МиБ,
   ≤20 файлов, проверка WAV и путей; `firmware.bin` — raw app.
4. XSS: имена треков/проектов/логи экранируйте (`esc`, `json_escape`,
   `textContent`), не вставляйте данные в HTML без экранирования.
5. Maintenance/lease: тяжёлые FS-операции — под `web_maintenance_begin()` и
   `storage_access_begin()`; не держите их во время аудио.
6. Wi-Fi только SoftAP; captive DNS; не добавляйте STA/APSTA и не расширяйте
   CORS (исключение — read-only `/progress` на порту 81).
7. Лимит httpd — 64 обработчика (занято ~62); новый маршрут проверяйте на лимит.

```powershell
& .\firmware\test\run_tests.ps1 -Only test_web,test_web_util,test_main
```

## UI и редактор блоков
**Файлы:** `firmware/web_ui.html`, `firmware/sound_editor/public/blocks.html`,
`firmware/sound_editor/scripts/`, `firmware/sound_editor/dist/`,
`firmware/tools/gen_web_html.py`, `gen_sound_editor.py`,
`firmware/test/test_web_ui.js`, `test_blocks_editor.js`, `test_sound_editor_assets.py`.

1. Страницу правьте только в `web_ui.html`; `web_html.h` генерируется.
2. Редактор — в `public/blocks.html` (и scripts), затем `npm run build` в
   `firmware/sound_editor`; коммитьте `dist/`, вручную его не редактируйте.
3. Экранируйте данные проекта (имена, ID, тексты) через `textContent`/эскейпер;
   лимиты — ID 31, имя 63, текст 1024, JSON ≤128 КиБ.
4. Сохранение: `expectedRevision`, конфликт ревизий, draft в localStorage,
   `capabilities` для лимитов; не теряйте несохранённые правки при reload.
5. `gen_sound_editor.py` сверяет хэши `dist/build-inputs.json`; устаревший
   `dist/` останавливает сборку прошивки.

```powershell
cd firmware\sound_editor; npm run build; cd ..\..
& .\firmware\test\run_tests.ps1 -Only test_web_util,test_web
node --test firmware\test\test_web_ui.js
node --test firmware\test\test_blocks_editor.js
python firmware\test\test_sound_editor_assets.py
```

## Провижининг, flasher и release
**Файлы:** `firmware/components/provision/src/provision.c`,
`firmware/provision_sounds.ps1`, `provision_pack.ps1`,
`firmware/flash_*.ps1`/`build_firmware.bat`, `firmware/tools/release_artifacts.py`,
`release_artifacts.py`, `bump_version.ps1`, `web_flasher/`, `firmware/test/test_provision/`,
`test_release_artifacts.py`, `test_pack_crypto.py`.

1. UART: `PROV` → подтверждение `PROV-CONFIRM` → стирание внешней NOR. `PUT
   <slot> <size> <label>` — до 20 слотов; `PACK <size>` + `PACK-DATA` по 32 КБ —
   до 400 звуков. Не меняйте порядок подтверждения/стирания.
2. После `PROV`/`-Erase` теряются NVS/настройки; это требует явного согласия.
3. **COM3 pitfall:** не хардкодьте COM3 (старые команды/примеры могут уйти не
   на тот порт). Скрипты ищут ESP32-S3 по VID_303A/PID_1001; `-Port` задавайте явно.
4. Release: `idf_build.ps1 -Release` → `publish` + `verify`; вручную
   `manifest.json`/`SHA256SUMS.txt`/BIN не правьте. Versioned `ADDITIPUS_AURA-X_v<ver>.bin` —
   единственная копия raw application (без merged flash и без дубля `_OTA.bin`).
5. Версия: `firmware/tools/bump_version.ps1` обновляет `version.txt`, CV7 и
   тестовое ожидание. После bump — `-Clean` сборка.
6. Flasher: `web_flasher\start_flasher.bat`; действия — только через
   `http://127.0.0.1:8765/...` (Origin), не `localhost`.

```powershell
& .\firmware\test\run_tests.ps1 -Only test_provision,test_selftest,test_main
python firmware\test\test_release_artifacts.py
python firmware\tools\release_artifacts.py verify --release release
```
