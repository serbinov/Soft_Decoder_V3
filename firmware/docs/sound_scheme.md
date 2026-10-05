# Sound engine (`components/sound`)

Version: firmware 0.10. The graph engine is the **single** sound behaviour
model. The legacy MDS/table sound-scheme subsystem (`sound_store`, `sound_scheme_t`,
`.mds` files, `/api/sound/*`) has been removed; there is no migration path.

## 1. Overview

```
DCC / web ─▶ web_apply_function ─▶ sound_function(fn,state) ─┐
motor ─────▶ sound_task (20 ms) ─▶ motor_get_applied_speed ──┼─▶ sg_runner_tick
                                                              │
                              graph (states/transitions/effects) ◀─┘
                                        │
                                        ▼
                              audio_voice_play/set_rate/stop
```

- `components/sound/src/sound.c` — RTOS task, runtime inputs, audio voice I/O.
- `components/sound/src/sound_graph.c` — editor JSON parse + validation.
- `components/sound/src/sound_graph_runner.c` — dependency-free semantic runner.
- `components/sound/src/sound_graph_store.c` — atomic versioned JSON store.

## 2. Data model

`sg_graph_t` (`sound_graph.h`): `states[]`, `transitions[]`, `effects[]`,
`assets[]`, an `engine` entry and `hysteresis`.

- A **state** is one WAV (`file`, `loop`, `volume`, `rate`); an empty file is
  deliberate silence.
- A **transition** connects two states and carries one typed condition
  (`fn_press/release/on/off`, `engine_on/off`, `speed/accel/decel` ranges,
  `sample_done`), a unique per-source `priority` and a timing
  (`immediate`/`after_sample`).
- An **effect** is an F-key entry point (`fn`) into a silent subgraph; the
  engine (voces 18/19 reserved for the prime mover) plays effects on independent
  voices.

`func_binding_t` (`components/settings/include/func_types.h`) is the runtime
routing for outputs and logic: `FUNC_TARGET_OUTPUT` (F0F/F0R/AUX1..7),
`FUNC_TARGET_SOUND`, `FUNC_TARGET_LOGIC` (mute stop/move/light, drive hold,
coast, dynamic brake, notch), gated by `dir` and `state`.

## 3. Editor authoring layer (`sound_editor`, schema v2)

The web editor (`/sound-editor/`) authors two layers that compile to the
device graph:

- **Effect-table library** (`effectTables`) — reusable sound definitions:
  `kind` (horn/motor/brake/bell/coupler/custom), `preset`
  (oneShot/loopHeld/shortLong/latched/random/state), `init`/`loop`/`end`/`short`
  WAVs, `shortMs`, `volume`, `rate`, and an optional `behavior` entry.
- **Routing** (`sources`, `blocks`, `sinks`, `wires`) — the three-column patch
  panel: F/engine/motion/direction/state sources, sound or logic blocks, and
  AUX/audio sinks, connected by wires that carry an event and direction/state
  gates. Sound blocks are pure references to a library table.

On save/apply the editor sends the schema-v1 device payload
(`states/transitions/effects/assets`) so the device runner is unchanged; the
routing compiles to `func_binding_t` for outputs/logic. v1 graphs load and are
migrated to the v2 authoring layer (`migrateV1toV2`).

A second, simpler authoring path is the **block constructor**
(`/sound-editor/blocks.html`, see `docs/sound_blocks_editor.md`): the user places
ready-made locomotive blocks (engine, horn, whistle, bell, compressor, coupler,
…) and the page compiles them to the same schema-v1 payload. It is vanilla JS,
has no Svelte dependency, and is reachable from the device UI under
Настройки → «Конструктор блоков (простой)».

## 4. REST

| Route | Method | Purpose |
|---|---|---|
| `/api/sound/graph/capabilities` | GET | format/schema + limits |
| `/api/sound/graph/projects` | GET | stored projects |
| `/api/sound/graph/project` | GET | one project (raw JSON) |
| `/api/sound/graph/state` | GET | live status (engine, table, speed) |
| `/api/sound/graph/asset` | GET | WAV manifest (size/CRC/format) |
| `/api/sound/graph/validate` | POST | device-side validation |
| `/api/sound/graph/save` | POST | save a new revision |
| `/api/sound/graph/apply` | POST | activate a saved revision |
| `/api/func-map?view=bind` | GET | canonical function bindings |
| `/api/func-map?bind=1...` | POST | add/remove a binding |

## 5. Limits

`states` 57 (31 sounding + 26 silent), `transitions` 128, `effects` 24,
`assets` 31, JSON body ≤ 128 KiB. Editor limits add `effectTables` 16,
`blocks` 64, `sinks` 9.
