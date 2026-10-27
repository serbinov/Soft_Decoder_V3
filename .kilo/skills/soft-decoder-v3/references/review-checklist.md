# Чек-лист ревью

Порядок — по риску. Сначала безопасность, затем конкурентность, затем
функциональность и релиз. Ссылки: [architecture.md](architecture.md),
[change-playbooks.md](change-playbooks.md).

## 1. Безопасность (блокер)

- [ ] Нет новых путей к flash/erase/format/OTA/PROV без явного подтверждения.
- [ ] `motor_boot_safe`, boot-inhibit и немедленный coast сохранены; stop/inhibit/
      e-stop не восстанавливают старую скорость.
- [ ] Inhibit-флаги `CONTROL`/`SAFETY`/`DCC_TIMEOUT` независимы; чужой флаг не
      снимается; feedback fault снимается только `speed=0` или BEMF disable.
- [ ] Калибровка владеет мостом (`motor_bemf_cal_active`), DCC-скорость в это
      время не форвардится; watchdog не ждёт мьютексов.
- [ ] Emergency-путь не блокируется длинной мутацией/ADC/FS.
- [ ] Не ослаблены rail/BEMF/ADC/saturation/plausibility/fault-проверки без
      стендовых данных.
- [ ] Нет секретов и отладочных обходов аутентификации.

## 2. Конкурентность и владение

- [ ] Нет блокирующих NVS/FS/долгих waits в DCC-колбэках; CV commit deferred,
      ACK worker.
- [ ] ADC1 сериализован `motor_bemf_lock/unlock`; unlock только после успешного lock.
- [ ] Storage lease удерживается на всё время FILE/FS-операции; maintenance
      закрывает admission и ждёт quiescence с timeout.
- [ ] Нет lock-order inversion: не держите mutex, ожидая аудио/сеть/задачу.
- [ ] Stale handles/generation не останавливают нового владельца голоса.
- [ ] Новые задачи не ломают бюджет `firmware/CMakeLists.txt` (safety 10 мс,
      motor 10 мс, sound 20 мс, audio, DCC, provision).

## 3. DCC / NMRA

- [ ] 28-step: e-stop `code==1`, stop `0`, `2..28` и `29+→126`; mapping не съехал.
- [ ] 14-step F0 из пакета скорости не сбрасывается group-one; 128-step
      `raw==1` — e-stop.
- [ ] Broadcast принимает только reset/e-stop и stop/e-stop скорости, не функции/CV;
      `0xC000` не алиасит broadcast.
- [ ] Service mode: reset + preamble ≥20, 4-байтный пакет, окно 20 мс,
      Direct/Bit, ACK только по правилам, repeated-пакет не пишет повторно.
- [ ] CV19 consist/реверс; CV21/22 только F0/F1..F12; F13+ всегда.
- [ ] CV11 timeout (0,5 с × значение) только rails/не DC; отдельный inhibit.
- [ ] Callback не теряет e-stop/reset при занятом control mutex.

## 4. Мотор и BEMF

- [ ] Ramp CV3/CV4, кривая CV2/CV5/CV6 или CV67..94, kickstart CV65 — без
      изменения порядка и без PWM-скачков.
- [ ] Closed-loop: rail >300 мВ, свежий rail, валидный BEMF, нет калибровки;
      при потере feedback duty не растёт (hold ≤100 мс → coast/fault).
- [ ] Cold-start/open-loop fallback не выводит duty на средние значения при
      малой команде; saturation/plausibility не отключены.
- [ ] Калибровка: preflight (покой, сенсор, полярность) → optimal window →
      curve; dwell 1000 мс по умолчанию; плохая/частичная кривая по правилам
      (≥2 точек, конечная доля ≥51/1024), иначе `CURVE`.
- [ ] Stop/inhibit/e-stop отменяют калибровку; сохранение не воскрешает старую
      кривую после неудачи.
- [ ] Изменения порогов подкреплены `BEMF-RAW`/`BEMF-COAST` на реальном двигателе.

## 5. Хранилище и атомарность

- [ ] NVS layout/`cv_record`/legacy migration не сломаны; CRC/длины проверяются.
- [ ] `tracks.txt`, `volumes.txt`, `selection`, ревизии графа и `bemf_win`
      читаются/пишутся согласованно; манифест переживает сброс NVS.
- [ ] Upload: reserved names (`.pb*`, `*.tmp`, `tracks.txt`, `volumes.txt`),
      sanitize, lower-case `.wav`, staging + canonical rename.
- [ ] Pack: journal PREPARED/COMMITTED, fsync, backup; recovery не удаляет
      доказательства при неуверенности; NVS+manifest подтверждаются до COMMITTED.
- [ ] Внешняя NOR не форматируется автоматически; нет внутреннего fallback.
- [ ] Ошибки записи сохраняют dirty/retry и не теряют старые данные молча.

## 6. Веб и безопасность

- [ ] `Origin/Referer` same-origin (`http`, порт 80) не ослаблен; без заголовков —
      только не браузерные клиенты.
- [ ] `web_flasher` nonce (`X-Aura-Nonce`/cookie) и `Origin` панели сохранены.
- [ ] Нет нового CORS `*` (кроме read-only `/progress`), нет обхода maintenance.
- [ ] Upload/OTA: лимиты (`UPLOAD_MAX` 2 МиБ, OTA ≤8 МиБ/20 файлов), проверка
      путей/WAV, `AURAOTA2`, raw `firmware.bin`.
- [ ] XSS: имена/логи/проекты экранируются (`esc`, `json_escape`, `textContent`).
- [ ] Wi-Fi остаётся SoftAP-only; captive DNS не сломан; лимит 64 маршрутов.

## 7. Звуковой граф

- [ ] Лимиты схемы (192/144/96/250/32/128/16, JSON ≤128 КиБ) и ID/имена/файлы
      валидируются.
- [ ] Engine F-key/hold/hysteresis; effects не дублируют Fx; transition priority
      и `immediate|after_sample` корректны.
- [ ] Граф читает applied speed/direction и **не командует мотором**.
- [ ] Голос 19 — движок, эффекты 0..18; stale handle/EOF/IDLE/FAILED обработаны;
      rate 500..3000, анти-щёлчок.
- [ ] Apply только при стоящем моторе; `expected_revision`/конфликт ревизий;
      выбранная ревизия не удаляется; store recovery не подменяет граф legacy.

## 8. Release, build и тесты

- [ ] `version.txt`, `SETTINGS_CV7_VERSION`, веб-версия и `manifest.json`
      согласованы; bump сделан `bump_version.ps1`.
- [ ] `sdkconfig.defaults` — источник настроек; `sdkconfig` не правился вручную.
- [ ] `idf_build.ps1 -Release` публикует и проверяет release; BIN не
      переименовывались вручную.
- [ ] `release_artifacts.py verify --release release` проходит.
- [ ] Затронутые тесты + полный `run_tests.ps1`; Node/Python доступны, иначе
      прогон неполный. Coverage не выдаётся за аппаратную проверку.
- [ ] Diff просмотрен; нет мусора/секретов; коммит только по просьбе.

## Do-not-regress (исправления 0.12)

- [ ] 28-step e-stop и mapping (`code 1/0/2..28/29+`).
- [ ] Broadcast: только reset/e-stop и stop/e-stop скорости.
- [ ] DCC-колбэки не блокируются; CV commit/ACK не в hot path.
- [ ] BEMF cold-start и open-loop fallback без роста duty при потере feedback.
- [ ] Частичная кривая BEMF принимается только по правилам; плохая не заменяет
      предыдущую.
- [ ] Preflight/optimal window/dwell калибровки сохранены.
- [ ] Reserved upload names и path sanitization.
- [ ] Pack journal/backup и recovery.
- [ ] CSRF Origin/Referer и XSS-экранирование.
- [ ] Release 0.12 и его manifest/версия не откатываются случайно.

## Остаточные и открытые пункты (`known_issues.md`)

- BEMF обязателен к проверке на конкретном двигателе; host/HIL PASS не
  доказывает обороты/устойчивость.
- HIL-калибровка BEMF — только opt-in в `run_hil_web.ps1 -CalibrateBemf`.
- Pack journal/recovery — best-effort, не транзакционная БД.
- `sound.c` runtime coverage низкое (~28,6 %); `provision.c`/`web.c` — следующие.
- SoftAP-only, один клиент, по умолчанию без пароля; API без аутентификации.
- `web_log_event()` до `web_init()` теряет часть сообщений.
- Legacy CV30/114–116, `mode`/`DUCK`, слоты 1..20 хранятся, но не исполняются;
  индексированные CV и RailCom не реализованы.

## Подводные камни target-build

- `-Werror=format-truncation`: `snprintf` в маленький буфер валит сборку; проверьте
  длины имён/путей/JSON.
- Забытый `PRIV_REQUIRES esp_driver_gpio` (или другой IDF-компонент) проявляется
  только в target-build, не в host-тестах.
- Устаревший `sdkconfig`/`build/` после смены `sdkconfig.defaults`, версии, CV7,
  таблицы разделов — используйте `-Clean`.
- `version.txt`/CV7/встроенная версия приложения должны совпадать; `publish`
  отвергает stale app.
- `manifest.json`/`SHA256SUMS.txt`/BIN не правятся вручную; `firmware.bin` — raw
  application, не merged.
- Генераторы `gen_web_html.py`/`gen_sound_editor.py` могут упасть на устаревшем
  `sound_editor/dist/` или запрещённом синтаксисе `web_ui.html`.

## Как проверить PR

```powershell
git status --short
git diff --stat
git diff
& .\firmware\test\run_tests.ps1 -Only test_dcc,test_motor,test_web   # затронутые
& .\firmware\test\run_tests.ps1                                      # полный
& .\firmware\idf_build.ps1 -Release                                  # target + release
python firmware\tools\release_artifacts.py verify --release release
```

Дополнительно: убедитесь, что не изменены `.kilo/worktrees/`, `release/`
вручную, чужие worktree и firmware-код вне задачи; сверьте `version.txt`,
`known_issues.md` и `ARCHITECTURE.md` с новым поведением.
