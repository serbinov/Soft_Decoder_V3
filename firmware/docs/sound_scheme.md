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
`assets[]`, an `engine` entry and `hysteresis`. `engine.fn` is kept for
compatibility with older graphs; the editor always emits `255` — the engine runs
continuously (`engine.hold` is then irrelevant).

- A **state** is one WAV (`file`, `loop`, `volume`, `rate`); an empty file is
  deliberate silence.
- A **transition** connects two states and carries one typed condition
  (`fn_press/release/on/off`, `engine_on/off`, `speed/accel/decel` ranges,
  `sample_done`), a unique per-source `priority` and a timing
  (`immediate`/`after_sample`).
- An **effect** is an entry point into a silent subgraph. The editor creates one
  only for real entry points (an Input/Fx trigger, a cue with an incoming input
  link, a direct-link target or an ambient speed/decel block); blocks reached
  only by a chain do not consume a channel. When a chain finishes, its effect
  channel returns to the entry of the chain root, so the next F press replays
  the whole sequence. A timeout loop keeps `elapsed_ms` across internal sample
  replays: the timeout covers the whole loop run, not one WAV.
  The engine (voice 19 reserved for the prime mover) plays effects on
  independent voices 0..18 (19 simultaneous effects).

`func_binding_t` (`components/settings/include/func_types.h`) is the runtime
routing for outputs and logic: `FUNC_TARGET_OUTPUT` (F0F/F0R/AUX1..7),
`FUNC_TARGET_SOUND`, `FUNC_TARGET_LOGIC` (mute stop/move/light, drive hold,
coast, dynamic brake, notch), gated by `dir` and `state`. For graph output/logic
blocks the binding can additionally carry a compact variable condition
(`FUNC_FLAG_COND`): up to two value comparisons, `AND`/`OR`, standard operators
and inversion of the Logic False branch. `func_eval_ex()` resolves variable
values through `sound_variable_get()`; `func_eval()` remains the condition-free
compatibility wrapper. Logic actions are executed by `web_apply_function()`:
mutes select graph silence, drive hold freezes the speed seen by the runner,
coast/brake force idle, and notch edges shift that speed by ±1 step (clamped to
±24).

## 3. Authoring (`/sound-editor/blocks.html`)

The sound scheme is authored with the **block constructor**
(`/sound-editor/blocks.html`, see `docs/sound_blocks_editor.md`). The device UI
opens it in one of two modes: **Миксы** (`?mode=mix`) or **Звуковые схемы**
(`?mode=scheme`); both use the same editor and a mode switch remains in its header.
In Mixes the user places inputs/outputs/logic and `Sound` cue blocks that reference
named schemes. In Sound schemes the user edits the scheme primitives (Start/End/
Simple/Loop/Mute) plus shared Logic/Variable/Delay blocks; legacy presets are not
shown in that mode. The engine block is always active and exposes only its speed
step outputs — connect those outputs to `Sound` blocks with schemes; it has no
F-key, internal WAV files or Margin in the UI. The page is vanilla HTML/CSS/JS
with no external dependencies. On first open it loads a ready locomotive template:
engine with four speed-band schemes (0–25/25–50/50–75/75–100 %), seven named
sound schemes (horn, bell, compressor, coupler, sander, doors open/close), Mixes
cues for them, ambient brake, lights/AUX and notch.
**Файл → Типовой локомотив** reloads it and **Новый** creates an empty project.

On save/apply it compiles the blocks to the schema-v1 device payload
(`states`/`transitions`/`effects`/`assets`), and compiles output/logic blocks to
`func_binding_t` for outputs and logic. A direct Logic -> Output/LogicOut link
also encodes its condition into the binding; `rhsVar` and more than two
conditions are rejected fail-closed because the compact binding has no room for
them.

The engine condition set is `fn_press/release/on/off`, `engine_on/off`, `speed`,
`accel`/`decel`, `sample_done`, `timeout`, `var_cmp`, `state_enter` (true for the
tick after a channel enters a state), plus `dir_fwd`/`dir_rev` (travel direction,
passed into `sg_runner_tick`) and `random` (chance in `min`; equal-priority
`random` transitions from one state form a random group picked uniformly).
`observers=true` makes the runner tick from boot so global Logic/Var/Delay
blocks without an input are evaluated once at startup; their outgoing links use
`state_enter` and therefore fire once instead of every 20 ms. While
any effect voice is audible the engine voice is ducked to `SOUND_DUCK_PCT` (40 %)
through `audio_voice_set_volume_live` and restored when effects end. A state's
`volume` is first scaled by the settings `engine_volume` or `effects_volume`
(depending on the channel) and then by `master_volume` in the audio mixer.

## 4. REST

| Route | Method | Purpose |
|---|---|---|
| `/api/sound/graph/capabilities` | GET | format/schema + limits |
| `/api/sound/graph/projects` | GET | stored projects |
| `/api/sound/graph/project` | GET | one project (raw JSON) |
| `/api/sound/graph/state` | GET | live status (engine, revision, speed, armed/fault) |
| `/api/sound/graph/asset` | GET | WAV manifest (size/CRC/format) |
| `/api/sound/graph/validate` | POST | device-side validation |
| `/api/sound/graph/save` | POST | save a new revision |
| `/api/sound/graph/apply` | POST | activate a saved revision |
| `/api/sound/graph/delete` | POST | delete a project and all its revisions |
| `/api/func-map?view=bind` | GET | canonical function bindings |
| `/api/func-map?bind=1...` | POST | add/remove a binding |

## 5. Limits

`states` 57 (31 sounding + 26 silent), `transitions` 128, `effects` 24 channels
(19 can sound simultaneously; one voice is reserved for the engine), `assets` 31,
JSON body ≤ 128 KiB.
