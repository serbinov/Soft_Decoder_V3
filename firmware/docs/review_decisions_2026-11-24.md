# Решения по ревью безопасности 2026-11-24

## Реализовано (полный список)
- **Фаза 1:** REV-M1 (клиппинг ШИМ), REV-A1/A3 (boot-safe AUX/AMP), REV-M2 (anti-windup), REV-M3 (BEMF loss on ADC failure), REV-R7 (heartbeat), REV-R1 (bounded func lock), REV-R2 (CV11=20), REV-R3 (atomic brownout clear), REV-D1 (stop not dropped), REV-S28/S29 (no boot loop), REV-A5 (validate after boot-safe).
- **Фаза 2:** REV-M5 (reverse dead-time), REV-M6/M8, REV-W1/W2/W3 (web gates), REV-S32 (busy storage reset), REV-D5 (ACK pin).
- **Boot-safe:** hook загрузчика + `board_boot_safe()` в приложении, тест `test_boot_safe_pins`.
- **Web:** dead-man 10 с, clean-stop снимает brownout (R5/R6), W4 (DNS bind к AP).
- **Последнее:** REV-A8 (pinmap 26-32), REV-D4 (снимок статуса DCC), REV-R9 (track ADC TWDT), REV-R10 (лог high-water стека safety-задачи), REV-W5 (мьютекс preview), REV-W6 (готовность actuation под mux), REV-S31 (discard lock), REV-S33/S34/S35/S36 (валидация manifest/category/track), REV-R8 (повторное применение emergency pending).

## Осознанно не изменено (с причиной)
- **REV-D2:** строгие 2 пакета service/ops — нужен список command station; иначе риск сделать декодер непрограммируемым. Дизайн готов: счётчик подтверждений в `dcc_confirmation_t`, запись на 2-м совпадении.
- **REV-S30:** нельзя отличить «намеренно пустую библиотеку» от «ключи отсутствуют/повреждены» без отдельного маркера; нужно продуктовое решение.
- **REV-W7:** имя в журнале пакетов (`.wav`) может обрезаться при имени >60 символов; исправление требует смены формата журнала (magic bump) и миграции. Orphan-backup fallback уже покрывает восстановление.
- **REV-A6:** rollback `auxio_init` конфликтует с текущим тестовым харнессом; на устройстве ошибка инициализации и так аварийная (`ESP_ERROR_CHECK`).
- **REV-A7:** headroom/лимитер аудио — изменение звучания, нужно прослушивание/HIL.
- **REV-D3:** inline ACK остаётся только в host/раннем пути без worker; в прошивке worker всегда создаётся в `dcc_init()`.
- **REV-W8:** устройство AP-only без интернета; DNS-rebinding риск минимален, ужесточение Host может сломать mDNS/кастомные имена.

## Проверки (финальные)
- `idf.py -C firmware build` — успешно.
- Полный host-набор — ALL TEST SUITES PASSED (включая `test_boot_safe_pins`, web dead-man, S31-S36).
- ASan/UBSan — CI (локальный MinGW без libasan).
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
