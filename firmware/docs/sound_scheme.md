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

`states` 128 (`SG_MAX_STATES`, up to 96 sounding + 64 silent), `transitions` 250,
`effects` 32 channels (19 can sound simultaneously; one voice is reserved for
the engine), `assets` 64, `variables` 16, JSON body ≤ 128 KiB.

---

## 6. Архитектура и спецификация JSON для автоматического или ручного создания звуковых схем

Звуковая схема в AURA-X существует на двух уровнях:
1. **Исполняемый граф устройства (`format: "sound-graph"`, `schemaVersion: 1`)** — конечный автомат состояний и переходов, который понимает прошивка (`sound_graph.c`) и который упаковывается в `.asp` (`scheme.json`).
2. **Редакторский проект блоков (`format: "sound-blocks"`, `schemaVersion: 2`)** — визуальное представление («Миксы» + «Звуковые схемы»), которое вкладывается внутрь поля `editor.project` исполняемого графа, чтобы при открытии «Конструктора Блок Схем» схема отображалась со всеми блоками и связями.

Ниже описана полная структура обоих уровней, по которой можно **вручную или автоматически (скриптом/генератором)** создавать звуковые схемы.

### 6.1. Структура исполняемого графа (`format: "sound-graph"`)

```json
{
  "format": "sound-graph",
  "schemaVersion": 1,
  "id": "locomotive_default",
  "name": "Тепловоз ЧМЭ3",
  "engine": {
    "entry": "engine_1_off",
    "fn": 255,
    "hold": false
  },
  "hysteresis": 3,
  "observers": false,
  "variables": [
    { "name": "speed",   "kind": "speed",   "init": 0 },
    { "name": "forward", "kind": "forward", "init": 0 },
    { "name": "moving",  "kind": "moving",  "init": 0 },
    { "name": "engine",  "kind": "engine",  "init": 0 },
    { "name": "cab_door", "kind": "user",   "init": 0 }
  ],
  "states": [
    {
      "id": "engine_1_off",
      "name": "Выкл",
      "file": "",
      "loop": false,
      "volume": 100,
      "rate": 1000
    },
    {
      "id": "idle_cue-idle_loop_play",
      "name": "Цикл",
      "file": "567_ChME3_idle_loop.wav",
      "loop": true,
      "volume": 80,
      "rate": 1000
    },
    {
      "id": "horn_cue-h_start_off",
      "name": "Старт",
      "file": "",
      "loop": false,
      "volume": 100,
      "rate": 1000
    },
    {
      "id": "horn_cue-h_start_play",
      "name": "Старт",
      "file": "horn-low-init.wav",
      "loop": false,
      "volume": 90,
      "rate": 1000
    },
    {
      "id": "horn_cue-h_loop_play",
      "name": "Цикл",
      "file": "horn-low-loop.wav",
      "loop": true,
      "volume": 90,
      "rate": 1000
    },
    {
      "id": "horn_cue-h_end_end",
      "name": "Конец",
      "file": "horn-low-exit.wav",
      "loop": false,
      "volume": 90,
      "rate": 1000
    }
  ],
  "transitions": [
    {
      "id": "t1",
      "source": "engine_1_off",
      "target": "idle_cue-idle_loop_play",
      "priority": 60,
      "timing": "after_sample",
      "condition": { "type": "speed", "min": 0, "max": 0 }
    },
    {
      "id": "t2",
      "source": "idle_cue-idle_loop_play",
      "target": "engine_1_off",
      "priority": 60,
      "timing": "after_sample",
      "condition": { "type": "speed", "min": 1, "max": 255 }
    },
    {
      "id": "t3",
      "source": "horn_cue-h_start_off",
      "target": "horn_cue-h_start_play",
      "priority": 60,
      "timing": "immediate",
      "condition": { "type": "fn_on", "fn": 2 }
    },
    {
      "id": "t4",
      "source": "horn_cue-h_start_play",
      "target": "horn_cue-h_loop_play",
      "priority": 60,
      "timing": "immediate",
      "condition": { "type": "sample_done" }
    },
    {
      "id": "t5",
      "source": "horn_cue-h_loop_play",
      "target": "horn_cue-h_end_end",
      "priority": 60,
      "timing": "immediate",
      "condition": { "type": "fn_off", "fn": 2 }
    },
    {
      "id": "t6",
      "source": "horn_cue-h_end_end",
      "target": "horn_cue-h_start_off",
      "priority": 60,
      "timing": "immediate",
      "condition": { "type": "sample_done" }
    }
  ],
  "effects": [
    {
      "id": "horn_cue-h_start",
      "entry": "horn_cue-h_start_off",
      "fn": 2
    }
  ],
  "assets": [],
  "editor": {
    "project": { ... }
  }
}
```

> **Примечание про `assets[]`:** при упаковке через `tools/pack_sounds.py` поле `"assets"` можно оставлять пустым `[]` — упаковщик сам вычислит `size`, `sampleRate`, `channels`, `bits`, `durationMs` и `crc32` для каждого используемого `.wav` и запишет их в `scheme.json` внутри `.asp`.

### 6.2. Правила построения состояний (`states[]`), переходов (`transitions[]`) и каналов (`effects[]`)

1. **Каналы исполнения:**
   * **Канал двигателя (`engine.entry`):** единый канал, управляющий дизелем (холостой ход + ступени скорости). Его точка входа (`engine_1_off`) **обязана быть тихим состоянием** (`"file": ""`). Поле `"fn": 255` означает, что двигатель активен постоянно.
   * **Каналы эффектов (`effects[]`):** каждый независимый звуковой эффект (свисток, тифон, компрессор, торможение и т.д.) получает одну запись в `effects[]`:
     * `"id"`: уникальный идентификатор (`[A-Za-z0-9_-]{1,31}`);
     * `"entry"`: ID тихого входного состояния этого эффекта (`"file": ""`);
     * `"fn"`: номер функции `0..28` (для информационных целей; реальный запуск определяют переходы `transitions[]`).
2. **Состояния (`states[]`):**
   * `"id"`: `[A-Za-z0-9_-]{1,31}`, уникален.
   * `"name"`: строка до 63 байт UTF-8.
   * `"file"`: `""` для тихого состояния ожидания или `"имя_файла.wav"` для звучащего состояния.
   * `"loop"`: `true` (бесшовный цикл) или `false` (однократное воспроизведение, по окончании генерирует событие `sample_done`).
   * `"loops"` *(опционально)*: `1..255` — число повторов для цикла по счётчику (после N повторов срабатывает `sample_done`).
   * `"volume"`: `0..100` (громкость состояния).
   * `"rate"`: `250..2000` (`1000` = 1.0× нормальная скорость воспроизведения).
   * `"setVar"` / `"setValue"` *(опционально)*: индекс пользовательской переменной в массиве `variables[]` (`0..15`) и целое значение `-32768..32767`, присваиваемое при входе в это состояние.
3. **Типы условий переходов (`condition.type`):**
   * Функциональные клавиши (`fn: 0..28`):
     * `"fn_press"` — нажатие клавиши (фронт включения, для режима `toggle`);
     * `"fn_release"` — отпускание клавиши (фронт выключения);
     * `"fn_on"` — клавиша удерживается включённой (уровень, для режима `hold`);
     * `"fn_off"` — клавиша выключена (уровень).
   * Скорость и ускорение (`min: 0..255`, `max: 0..255`, где `0..100%` переводится как `round(pct * 255 / 100)`):
     * `"speed"` — текущая скорость в диапазоне `[min, max]`;
     * `"accel"` — ускорение в диапазоне `[min, max]`;
     * `"decel"` — замедление (торможение) в диапазоне `[min, max]` (обычно `min: 1, max: 255`).
   * Двигатель и направление (без параметров):
     * `"engine_on"`, `"engine_off"`, `"dir_fwd"`, `"dir_rev"`.
   * Завершение сэмпла и таймер:
     * `"sample_done"` — WAV-файл доиграл до конца;
     * `"timeout"` — прошло `min` тиков по 50 мс (`min: 1..255`, т.е. `50..12750 мс`);
     * `"state_enter"` — безусловный переход на следующем тике после входа в тихое состояние.
   * Случайный выбор и переменные:
     * `"random"` — случайный переход с весом `min: 1..100` (переходы из одного состояния с одинаковым `priority` и типом `"random"` образуют группу случайного выбора);
     * `"var_cmp"` — сравнение переменной `"var"` (индекс `0..15` в `variables[]`) целочисленным оператором `"op"` (`0`=`==`, `1`=`!=`, `2`=`<`, `3`=`<=`, `4`=`>`, `5`=`>=`) с числом `"value"` (`-32768..32767`) или со второй переменной `"var2"` (индекс `0..15`).
4. **Обязательные инварианты валидатора (`sg_validate`):**
   * У всех переходов, выходящих из одного и того же `source`, поле `priority` (`0..255`) должно быть **разным** (кроме группы `"random"`).
   * Не должно быть циклов из тихих состояний (`file: ""`) с мгновенными (`timing: "immediate"`) уровневыми условиями.
   * Каждое состояние в `states[]` должно быть достижимо по переходам из `engine.entry` или из одного из `effects[].entry`.

---

### 6.3. Типовые шаблоны цепочек (для генератора схем)

1. **Одиночный звук по нажатию F-клавиши (`Старт`):**
   * Тихое состояние `S_off` (`file: ""`) и звучащее `S_play` (`file: "bell.wav", loop: false`).
   * `S_off -> S_play` по `{"type": "fn_press", "fn": 5}`.
   * `S_play -> S_off` по `{"type": "sample_done"}`.
2. **Удерживаемый звук из 3 фаз (`Старт -> Цикл -> Конец`, например Тифон на `F2`):**
   * Состояния: `H_off` (тихое), `H_start` (`init.wav`, `loop: false`), `H_loop` (`loop.wav`, `loop: true`), `H_end` (`exit.wav`, `loop: false`).
   * `H_off -> H_start` (`fn_on, fn: 2`, prio 60);
   * `H_start -> H_loop` (`sample_done`, prio 60);
   * `H_start -> H_end` (`fn_off, fn: 2`, prio 70 — раннее отпускание клавиши до окончания атаки);
   * `H_loop -> H_end` (`fn_off, fn: 2`, prio 60);
   * `H_end -> H_off` (`sample_done`, prio 60).
3. **Ступень скорости двигателя (`Холостой ход` или `Дизель X..Y%`):**
   * Общий вход двигателя `engine_1_off` (тихий).
   * Для каждой ступени `i` с диапазоном `[lo, hi]` (в шкале `0..255`):
     * `engine_1_off -> step_i_play` (`speed: [lo, hi]`, `timing: "after_sample"`);
     * `step_i_play -> engine_1_off` при выходе скорости ниже `lo` (`speed: [0, lo-1]`, `timing: "after_sample"`) и выше `hi` (`speed: [hi+1, 255]`, `timing: "after_sample"`).
4. **Звук торможения по замедлению (`Торможение`):**
   * Отдельный эффект в `effects[]` со своим тихим входом `brake_off` и звуком `brake_play` (`bremse.wav`, `loop: false`):
   * `brake_off -> brake_play` (`{"type": "decel", "min": 1, "max": 255}`);
   * `brake_play -> brake_off` (`{"type": "sample_done"}`).

---

### 6.4. Структура визуального проекта (`editor.project`, формат `"sound-blocks"`)

Чтобы сгенерированная схема открывалась в визуальном редакторе `/sound-editor/blocks.html` с разделением на «Миксы» и «Звуковые схемы», в поле `editor.project` графа помещается объект:

```json
{
  "format": "sound-blocks",
  "schemaVersion": 2,
  "id": "locomotive_default",
  "name": "Тепловоз ЧМЭ3",
  "vars": [
    { "name": "cab_door", "kind": "user", "init": 0 }
  ],
  "schemes": [
    {
      "id": "horn_scheme",
      "name": "Тифон (низкий)",
      "blocks": [
        { "id": "h_start", "type": "start", "name": "Старт", "volume": 90, "files": { "sound": "horn-low-init.wav" }, "outEvent": "finish", "outDelay": 0, "x": 60, "y": 80 },
        { "id": "h_loop",  "type": "loop",  "name": "Цикл",  "volume": 90, "files": { "sound": "horn-low-loop.wav" }, "loopMode": "hold",   "outDelay": 0, "x": 360, "y": 80 },
        { "id": "h_end",   "type": "end",   "name": "Конец", "volume": 90, "files": { "sound": "horn-low-exit.wav" }, "x": 660, "y": 80 }
      ],
      "links": [
        { "id": "l1", "from": "h_start", "to": "h_loop", "kind": "after", "cond": "out" },
        { "id": "l2", "from": "h_loop",  "to": "h_end",  "kind": "after", "cond": "out" }
      ],
      "texts": []
    }
  ],
  "mix": {
    "blocks": [
      { "id": "engine_1", "type": "engine", "name": "Двигатель", "run": [ { "from": 0, "to": 14, "file": "" } ], "x": 40, "y": 240 },
      { "id": "vol_1",    "type": "volume", "name": "Громкость", "master": 85, "engine": 85, "effects": 75, "x": 40, "y": 480 },
      { "id": "f2",       "type": "fx_in",  "name": "F2 Тифон",  "fn": 2, "fxMode": "hold", "x": 40, "y": 1280 },
      { "id": "horn_cue", "type": "sound_cue", "name": "Тифон (низкий)", "scheme": "horn_scheme", "x": 420, "y": 1280 }
    ],
    "links": [
      { "id": "ml1", "from": "f2", "to": "horn_cue", "kind": "after", "cond": "out" }
    ],
    "texts": []
  }
}
```
