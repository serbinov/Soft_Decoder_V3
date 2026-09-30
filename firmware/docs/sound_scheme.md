# Sound scheme engine (`components/sound`)

Version: firmware 0.9. Reference design: `SOUND_ENGINE_IMPLEMENTATION.md`;
delivery plan and status: `SOUND_ENGINE_ROADMAP.md`.

## 1. Overview

The sound engine turns the model's motion and the function (F) keys into WAV
playback through the `audio` mixer. It owns its own scheme data, a 20 ms RTOS
task (priority 6, core 1, 4 KB stack) and the reserved engine voices 18/19.

```
DCC / web ─▶ web_apply_function ─▶ sound_function(fn,state) ─┐
motor ─────▶ sound_task (20 ms) ─▶ motor_get_applied_speed ──┼─▶ sound_tick
                                                              │
                              settings (bindings, CV, .mds) ◀─┘
                                        │
                                        ▼
                              audio_voice_play/set_rate/stop
```

With `SOUND_SCHEME_NONE` (or `SOUND_SCHEME_LEGACY`) the engine is completely
inert: all legacy behaviour (F1..F20 → audio slots, AUX outputs) is unchanged.

## 2. Data model (`components/settings/include/sound_types.h`)

- `sound_scheme_t` — root: `type`, up to 32 `sound_table_t`, `engine`, up to 24
  `sound_extra_t`, `brake`.
- `sound_table_t` — `name`, flag `used`, tracks `init[]/loop[]/end[]` (4 groups
  for steam cylinders), `min_speed`/`max_speed`, `rate_scale`, `min_plays`/
  `max_plays`, graph links `end_table`/`next_accel`/`next_decel`.
- `sound_engine_t` — start/stop/shutdown tables, `drive[5]`/`accel[5]`/`coast[5]`,
  `engine_start_fn`, `cyl_shift_*`, `flags` (CV30), `sync_motion`.
- `sound_extra_t` — key-driven or automatic (`fn == SOUND_FN_NONE`) sound with
  `mode` (`RANDOM`/`STATE`), `dir`, `state`, `volume`, random interval.
- `func_binding_t` — canonical F-key binding (see §4).

Track cells hold **storage-relative file names**, resolved at play time as
`<storage_root>/<name>`. Because the model stores names, `sizeof(sound_scheme_t)`
is ≈ 26 KB — never place it on a task stack (16 KB); use the heap or the module
static.

## 3. Storage

- Binary scheme file `[magic "MDS1"][u16 ver][u16 size][u32 crc32][scheme]`
  under `<storage_root>/projects/<name>.mds`.
- Written atomically (temp file + `fflush`/`fsync`/`rename`).
- The active file name is kept in NVS (`active_scheme`).
- Schemes are managed from the web UI: create/select/delete a project and
  export/import the raw `.mds` file. A scheme name is 1..63 chars of
  `[A-Za-z0-9_-]` (no path separators).
- Function bindings live in NVS (`func_bind`) and are mirrored as `B;` records
  in the track manifest (`/userdata/audio/tracks.txt`, version 2).

## 4. Function bindings

`func_binding_t` records a many-to-many mapping from an F key to a target:

| `target_type` | target |
|---|---|
| `FUNC_TARGET_OUTPUT` | AUX/F0F/F0R output bit (0..8) |
| `FUNC_TARGET_SOUND`  | sound table index |
| `FUNC_TARGET_SLOT`   | legacy audio slot 1..20 |
| `FUNC_TARGET_LOGIC`  | engine logic id |

Gates `dir` (`ANY`/`FWD`/`REV`) and `state` (`ANY`/`MOVING`/`STOPPED`) are
checked before a binding fires, in both the pure evaluator `func_eval()` and the
engine's sound path.

Playback modes:

| mode | behaviour |
|---|---|
| `ONE_SHOT` | play once on press |
| `TRIGGER` | play once, ignores presses while still sounding |
| `LOOP_HELD` | loop while held, stop on release |
| `SHORT_LONG` | short tap (`< short_ms`) → `short_table`; long hold → loop |
| `LATCHED` | toggle on each press |

Legacy `settings_func_map_t` is migrated once (`settings_func_bind_legacy_convert`),
including the directional head-light rule (F0F+F0R → F0F on FWD, F0R on REV).

## 5. Engine state machine and sequencer

Each 20 ms tick:

1. `sync_motion` (if set) makes the prime mover follow the wheels.
2. Speed/accel EMA → `sound_engine_pick()` chooses a table by speed band and
   accel/coast thresholds (with CV30 skip flags `Skip StoD1`/`Skip D1toS` and
   mute gates, §6).
3. The Init/Loop/End sequencer plays `init→loop→end` (4 cylinder groups for
   steam), counts loop plays against `min_plays`/`max_plays`, then stops.
4. Playback rate = `sound_rate_for(table.rate_scale, speed, CV114 rate)`.

## 6. Logic bindings

`FUNC_TARGET_LOGIC` bindings implement:

- `MUTE_STOP` — silence the engine while stopped.
- `MUTE_MOVE` — silence the engine while moving.
- `MUTE_LIGHT` — silence effect (non-engine) sounds.
- `DRIVE_HOLD`, `COAST`, `DYNAMIC_BRAKE`, `NOTCH_UP/DOWN` — recognised ids; the
  engine currently treats them as no-ops.

## 7. Rates, brake, extras

- CV114 = chuff/exhaust rate, CV115 = bell rate, CV116 = dynamic brake rate;
  each maps 0→0.5×, 51→1.0×, 255→3.0× (see `docs/cv_sound.md`).
- Brake: a hard deceleration below `brake.min_brake_speed` (and above
  `max_on_speed`) plays the Stop table on the secondary voice (CV116 rate).
- Extras: `RANDOM` (interval min/max, requires a finite target table —
  `max_plays != 0`) and `STATE` (play while the motion state/direction matches).

## 8. REST and UI

| Route | Method | Purpose |
|---|---|---|
| `/api/sound/state` | GET | live status (type, engine, table, speed) |
| `/api/sound/scheme` | GET | scheme + `drive`/`accel`/`coast` arrays + `brake` + tables (with group-0 file names) + extras |
| `/api/sound/scheme` | POST | edit scheme/engine fields (`drive0..4`, `accel0..4`, `coast0..4`, `brake_max`, `brake_min`; `flags` also mirrors CV30) |
| `/api/sound/table` | POST | edit one table (`init`/`loop`/`endf` = group-0 files) |
| `/api/sound/extra` | POST | edit one extra |
| `/api/sound/lint` | GET | graph validation report |
| `/api/sound/projects` | GET | list stored `.mds` schemes (+ active one) |
| `/api/sound/project` | POST | create (`create=NAME[&type=N]`), activate (`activate=NAME`) or delete (`delete=NAME`) a scheme |
| `/api/sound/download` | GET | export a scheme as a `.mds` attachment (`name=NAME`) |
| `/api/sound/upload` | POST | import a `.mds` body (`name=NAME[&activate=0]`) |
| `/api/func-map?view=bind[|matrix]` | GET | canonical bindings / matrix |
| `/api/func-map?bind=1...` | POST | add/remove a binding |

UI panels: «Звук: Схема», «Звук: Функции» (binding matrix), «Звук: Доп. звуки»,
«Звук: Параметры» (CV30/63/114/115/116).

## 9. Known limitations

- Loop tracks are played as one-shots with restart; truly seamless looping needs
  the P2 preload buffer.
- The computed chuff interval (`60000/rpm/cylinders/2`) and cylinder phase
  rotation are not implemented; the chuff speedup is handled by CV114 plus the
  per-table `rate_scale`. The 4 Init groups play sequentially.
- Brake uses the Stop table; "switch to End early" and explicit CV3/CV4 coupling
  are not implemented.
- `SOUND_SCHEME_LEGACY` is accepted but currently identical to `NONE`.
- Speed-band hysteresis is not implemented: a step change is taken from the raw
  speed band (only accel has the `SOUND_ACCEL_HYST` dead-band), so a speed that
  jitters exactly on a band edge can flip tables. Add a speed dead-band around
  `step*256/SOUND_ENGINE_STEPS` to smooth it.
- The graph fields `end_table`, `next_accel`, `next_decel`, `min_ms`/`fade_ms`
  and `cyl_shift_*` are stored/edited via REST but reserved: only `max_plays`
  (and `min_plays`) affect table sequencing today.
- `sound_scheme_t` (~26 KB) is a candidate to move to PSRAM if DRAM is tight.
