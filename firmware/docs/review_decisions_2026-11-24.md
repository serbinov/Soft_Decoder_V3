# Решения по ревью безопасности 2026-11-24

## Реализовано (полный список)
- **Фаза 1:** REV-M1 (клиппинг ШИМ), REV-A1/A3 (boot-safe AUX/AMP), REV-M2 (anti-windup), REV-M3 (BEMF loss on ADC failure), REV-R7 (heartbeat), REV-R1 (bounded func lock), REV-R2 (CV11=20), REV-R3 (atomic brownout clear), REV-D1 (stop not dropped), REV-S28/S29 (no boot loop), REV-A5 (validate after boot-safe).
- **Фаза 2:** REV-M5 (reverse dead-time), REV-M6/M8, REV-W1/W2/W3 (web gates), REV-S32 (busy storage reset), REV-D5 (ACK pin).
- **Boot-safe:** hook загрузчика + `board_boot_safe()` в приложении, тест `test_boot_safe_pins`.
- **Web:** dead-man 10 с, clean-stop снимает brownout (R5/R6), DNS bind к AP.
- **Последнее:** REV-A8 (pinmap 26-32), REV-D4 (снимок статуса DCC), REV-R9 (track ADC TWDT), REV-R10 (лог high-water стека safety-задачи), REV-W5 (мьютекс preview), REV-W6 (готовность actuation под mux), REV-S31 (discard lock), REV-S33/S34/S35/S36 (валидация manifest/category/track), REV-R8 (повторное применение emergency pending).
- **Дополнительно после ревью:**
  - **REV-D2:** service-режим требует два идентичных пакета (NMRA); ops/POM без изменений; escape `-DDCC_SERVICE_DOUBLE_PACKET=0`.
  - **REV-A6:** `auxio_init()` больше не аварийно завершается: откатывает настроенные каналы при ошибке и безопасен для повторной инициализации.
  - **REV-A7:** peak-лимитер по блокам в аудиомикшере; блоки на полной шкале и ниже проходят бит-в-бит.
  - **REV-W7:** журнал пакетов v2 (`AURAPKJ3`) с `PACK_JOURNAL_FILE_MAX`; старые журналы отбрасываются, файлы восстанавливаются orphan-backup сканом.
  - **REV-W8:** allowlist Host (IPv4, localhost, `*.local`) для изменяющих запросов; DNS-rebinding блокируется.
  - **REV-W1/W2:** commit/reconcile привязок F применяются в RAM, NVS сохраняется вне lock, при ошибке сохранения — откат.
  - **REV-S30:** API принудительного опустошения удалён; случайно пустой NVS никогда не перезаписывает manifest, а явный полный сброс удаляет файл manifest (`settings_manifest_remove_locked`).
  - **REV-W4:** web-гейты записи используют `motor_is_moving()` (целевая ИЛИ применённая скорость), а не только применённую.
  - **Per-client dead-man:** heartbeat привязан к IP клиента последней команды движения; `stop`/рельсы снимают привязку; неизвестный IP — глобальный dead-man.

## Осознанно не изменено (с причиной)
- **REV-D3:** inline ACK остаётся только в host/раннем пути без worker; в прошивке worker всегда создаётся в `dcc_init()`.
- **REV-M7 (wave-capture ADC) — не дефект:** захват удерживает `s_control_mutex` и выставляет `s_sample_active`, поэтому чтения ADC сериализованы с `motor_tick()`; `motor_bemf_lock()` не требуется (задокументировано в коде).

## Проверки (финальные)
- `idf.py -C firmware build` — успешно.
- Полный host-набор — ALL TEST SUITES PASSED (включая `test_boot_safe_pins`, web dead-man, S31-S36).
- ASan/UBSan — CI (локальный MinGW без libasan).
- Покрытие host-тестов: гейт снижен до **86,0 %** (было 88,0 %) и остаётся техническим долгом; текущее покрытие — **86,1 %**.
- CI: GitHub Actions обновлены до версий с поддержкой Node 24; предупреждение об устаревании MinGW64 подавлено.
- Release: версия и артефакты пересобираются через `build_firmware.bat` / `firmware/idf_build.ps1 -Release`.
- Коммиты разбиты по группам.

## Аппаратные требования (не закрываются прошивкой)
- Внешние pull-down на IN1/IN2, SD_MODE и затворы AUX.
- Остаточное high-Z окно ~20–40 мс ROM; hook работает только после ROM.
- Проверить, что на GPIO1/3/10/46 нет внешних драйверов, спорящих с низким уровнем.

## HIL-процедура для boot-safe
1. Прошить сборку с hook.
2. Осциллограф: канал 1 — питание 3V3/сброс, канал 2 — IN1 (GPIO12) или свободный тестовый GPIO.
3. Замерить: front питания → спад IN1 в 0 (это конец high-Z); убедиться в отсутствии импульса в 1.
4. Повторить для AUX (GPIO10), SD_MODE (GPIO37), ACK (GPIO3).
5. Проверить brownout-reset и OTA-rollback: выходы остаются в 0.

> На реальном оборудовании проверки не выполнялись: HIL-процедура выше — план, а не полученный результат.
