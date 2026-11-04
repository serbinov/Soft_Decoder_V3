# Каталог звуков

Каталог собран по фактическому аудио: длительность, частота дискретизации,
каналы, RMS/пик, zero-crossing rate, спектральный центроид, доминирующая
частота, rolloff 85 %, тренд громкости и SHA-256 (16 символов). Это позволяет
сравнивать дубликаты и варианты без прослушивания каждого файла.

- `sound_signatures.csv` — сигнатуры 374 файлов (`SOUND/` + `default_locomotive/sounds/`).
  Колонка `legacy_name` хранит прежнее имя для переименованных файлов.
- `sound_rename_map.csv` — карта старых имён в новые, причина и уверенность.

## Что переименовано

`SOUND/` содержал 232 имени в старом формате (пробелы, `.WAV`, `a2-1`,
`curve-squeal-speed1-loop1.7`, немецкие названия). Все приведены к нижнему
snake_case и разложены по семействам:

- `engine_accel_*`, `engine_decel_*`, `engine_transition_*` — переходы дизеля по ступеням;
- `engine_notch_N_loop_vX` — циклы ступеней;
- `curve_squeal_speedN_*`, `brake_squeal_02_*` — визг колёс/тормозов;
- `air_compressor_notch_N`, `air_release_*`, `sander_loop_*`, `turbo_loop_*`, `fan_loop_*`;
- `rail_clank_N_fast_M` / `rail_clank_N_slow_M`, `switch_flange_N`;
- `whistle_loop_N`, `whistle_short_1_N`, `whistle_start_2`, `whistle_fast_exit`;
- `announcement_ru_*` — русские объявления;
- `train_brake_*`, `loco_brake_apply`, `brake_button_N`, `conductor_whistle_N`;
- `traction_motor_deep_loop_N`, `high_speed_gear_shift`, `beep`, `silence_*`.

`default_locomotive/sounds/` уже содержал 71 понятное имя (`engine_accel_1_to_2`,
`fan_start`, `brake_squeal_loop` и т.д.) и оставлен без изменений.

## Файлы с низкой уверенностью

Их стоит проверить на слух перед использованием в схемах:

- `engine_start_failed_part_1.wav`, `engine_start_failed_part_2.wav` (бывшие `ms failed part-*`);
- `engine_idle_loop_v2/v3/v4.wav` (бывшие `s-loop1.2/1.3/1.4`);
- `engine_decel_to_idle_alt.wav` (бывший `sd.WAV`);
- `ambient_low_rumble_sm.wav` (бывший `sm.WAV`, 9,9 с низкого рокота);
- `whistle_short_1_1..9.wav` — короткие 0,1-секундные срезы whistle-цикла.

## Совместимость

`release/ADDITIPUS_sounds.asp` пересобран из `SOUND/` и содержит только новые
понятные имена: 303 записи, без дублей. Старые имена перечислены в
`sound_rename_map.csv`, но в пакет не включаются. Если на устройстве остались
проекты со старыми ссылками, переименуйте их по карте или откройте и сохраните
заново в редакторе.

Обычная сборка пакета:

```
python firmware/tools/pack_sounds.py SOUND -o release/ADDITIPUS_sounds.asp
```

