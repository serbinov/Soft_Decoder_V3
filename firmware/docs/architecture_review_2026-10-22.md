# Architecture review, 2026-10-22

Scope: current `main` firmware (`firmware/`), sound block editor, web control,
DCC callbacks, maintenance/OTA and the host test harness. This is a working
document: findings are kept with stable IDs and updated as fixes land.

Legend: `fixed` = code and tests are in this tree; `partial` = a concrete
symptom is fixed but the underlying architecture item remains; `planned` = not
changed yet.

| ID | Severity | Finding | Status |
| --- | --- | --- | --- |
| AR-01 | HIGH | Web and DC motor commands do not update the function motion mirror, so direction/state-gated AUX, headlights and logic use stale state | fixed |
| AR-02 | HIGH | DCC emergency stop and broadcast reset are ignored while the decoder is in web control mode | fixed |
| AR-03 | MEDIUM-HIGH | Emergency stop and control-source switch clear functions but leave physical AUX/sound latched | fixed |
| AR-04 | MEDIUM | Canonical bindings with `FUNC_TARGET_SOUND`/`FUNC_TARGET_SLOT` are accepted by the API but not executed | fixed |
| AR-05 | HIGH | `web_maintenance_end()` can leave the maintenance owner and motor/sound inhibit latched (known issue) | partial |
| AR-06 | HIGH | DCC real-time path shares locks with HTTP and flash/NVS writes can stall packet processing | partial |
| AR-07 | MEDIUM-HIGH | Firmware OTA rollback does not roll back sounds/graph on external NOR | partial |
| AR-08 | HIGH | Open SoftAP, no API authentication/CSRF protection, unsigned OTA, secure boot and flash encryption disabled | planned |
| AR-09 | HIGH | Editor JS and firmware C duplicate the graph schema; preview mock validates nothing; limits/token limits are not shared | partial |
| AR-10 | MEDIUM | Graph allows 32 effect channels while audio has 19 dynamic voices | partial |
| AR-11 | MEDIUM | Shared ADC1 for BEMF/rail sense, unpinned motor/track tasks, instant default acceleration and no current telemetry | planned |
| AR-12 | MEDIUM | No AUX state readback endpoint; HIL checks command acceptance, not physical outputs | partial |
| AR-13 | MEDIUM | `web.c` is a 4.8k-line monolith with many shared globals and lock paths | planned |
| AR-14 | MEDIUM | Editor applies graph and output bindings as separate requests; a partial failure leaves an inconsistent device | planned |

## Confirmed defects

### AR-01. Motion mirror is only updated by the DCC speed callback

Evidence:
- `web.c:743` defines `web_motion_changed()`, which updates
  `s_motion_speed`/`s_motion_forward` and re-evaluates output/logic bindings.
- `app_main.c:64` calls it from the DCC speed callback only.
- `web.c:1551` `motor_post()` calls `motor_set_speed()` but never
  `web_motion_changed()`.
- `track.c:145` DC control calls `motor_set_speed()` directly as well.
- `web.c:702` `func_apply_all_locked()` and `web.c:674`
  `logic_apply_locked()` read `s_motion_speed`/`s_motion_forward`.

Failure scenario: in web mode, set direction to reverse and press F0. The
headlight pair is evaluated with the old forward state; `dir: auto` headlights
do not swap, `state: moving/stopped` outputs do not change, and
`MUTE_STOP`/`MUTE_MOVE` logic is computed from the previous speed. The same
happens in DC mode and after emergency stop.

Fix plan: update motion state from every accepted motor command
(`/api/motor`, DC track, emergency, source switch), preferably through one
small motion-state service rather than caller discipline. Add tests that go
through the public API, not only `web_motion_changed()`.

Implemented: `motor_post()` now calls `web_motion_changed()` on an accepted
command; `track.c` does the same for DC drive and for the delayed DC stop;
`web_emergency_reset()` zeroes the motion mirror. Regression test:
`test_motor_post_updates_motion_and_headlights` plus the DC track assertion.

### AR-02. DCC safety events are gated on the control source

Evidence: `app_main.c:138-144` returns from `on_dcc_emergency_stop()` when
`web_control_source_is_rails()` is false. `on_dcc_reset()` also requires a
rails lease. A layout emergency stop is a safety broadcast and must not depend
on which source currently owns throttle control.

Failure scenario: a locomotive is driven from the web UI, the layout e-stop or
broadcast reset is asserted, and the decoder keeps its last motor command and
function state.

Fix plan: always stop motor/audio/sound and reset outputs/functions on DCC
e-stop/reset; only DCC-specific bookkeeping (motor ownership note) remains
source-gated. Add source-isolation tests for both rails and web mode.

Implemented: both callbacks now check only `web_actuation_ready()` (to stay
fail-closed before boot admission), then stop and reset regardless of the
control source. Rails lease is used only for the motor-ownership note.
`test_callbacks_source_leases_and_accepted_motion_only` covers the web-mode
case; the boot-denied test still verifies that callbacks are inert before
admission.

### AR-03. Emergency and source switch leave outputs latched

Evidence: `web.c:1599` `emergency_post()` stops motor/audio/sound and clears
`s_fn`, but never calls `func_apply_all_locked()`; `s_outputs_applied` and the
physical AUX channels stay on. `web.c:1283` `control_source_post()` only calls
`motor_emergency_stop()` and does not clear functions or sound.

Failure scenario: F2 horn and AUX1 are on in web mode; press emergency or
switch to rails. The UI shows all F off, but the horn/AUX can remain active
until a later function event.

Fix: central `web_emergency_reset()` that clears functions, forces all nine
AUX channels off, resets logic state and zeroes the motion mirror; call it from
web emergency, source/mode switch, and DCC e-stop/reset. Stop audio/sound in
the same transitions.

Implemented: `web_emergency_reset()` in `web.c`/`web.h`; used by
`emergency_post()`, `control_source_post()`, `mode_post()` and both DCC
callbacks. `test_web_emergency_reset_clears_outputs` asserts every AUX channel
and the motion/logic state; the existing emergency test still verifies best
effort when the function mutex is unavailable.

### AR-04. Unsupported canonical sound/slot targets

`func_map_post()` accepts `FUNC_TARGET_SOUND`/`FUNC_TARGET_SLOT` while the
graph is inactive. `logic_collect_cb()` (`web.c:662`) only collects
`FUNC_TARGET_LOGIC`, and output handling only covers `FUNC_TARGET_OUTPUT`.
The records persist but do nothing.

Fix plan: either implement these targets or reject them at save time with an
explicit error; keep legacy sound mapping separate.

Implemented: canonical binding POST now rejects `FUNC_TARGET_SOUND` and
`FUNC_TARGET_SLOT` with `400 Bad Request`; the graph engine or legacy
`/api/func-map` slot map remains the supported route. Old persisted records are
still readable/removable but are not applied.

### AR-05. Maintenance can latch inhibit forever

Known issue: `firmware/docs/known_issues.md:40`. `web_maintenance_end()`
(`web.c:524`) returns if it cannot take `s_control_mutex` within 100 ms,
leaving `s_maintenance_owner` set and motor/sound inhibited until reboot.

Fix plan (this iteration): do not give up on the mutex in `end`; handle a
missing mutex and clear ownership safely. Longer term: maintenance manager with
lease timeout, forced recovery and an explicit fault surface.

Implemented: `web_maintenance_end()` now initializes the control mutex if
needed and takes it with `portMAX_DELAY` instead of giving up after 100 ms; a
mutex initialization failure clears the owner and leaves actuation closed. The
remaining item is stale-owner recovery if a task dies while owning maintenance.

## Systemic risks

### AR-06. Real-time DCC vs HTTP and flash

`on_dcc_function()` holds the control mutex across `web_apply_function()`,
which takes `s_func_mutex` with `portMAX_DELAY` and can touch AUX/sound. The DCC
half-period queue is 256 entries (`dcc.c:30`), roughly 15-25 ms at DCC rates.
NVS/LittleFS writes while driving (CVs, settings, graph saves) can disable the
flash cache and delay both the DCC ISR/task and the motor loop.

Recommendation: measure worst-case flash write latency; defer NVS commits until
standstill; move DCC callbacks to a lock-free/snapshot path or bounded waits;
keep DCC ISR/task code in IRAM; verify queue watermark under load.

Implemented partially: `GET /api/dcc/status` exposes the cumulative ISR
half-period overrun count and age of the last valid packet, so a board test can
detect missed DCC frames while running audio, Wi-Fi and flash writes. The
latency/queue design itself is unchanged.

### AR-07. OTA and external NOR rollback are independent

`web.c:4383-4390` publishes sounds and the manifest before setting the new boot
partition. If the new firmware fails to confirm, the bootloader rolls back
firmware but external NOR keeps the new sounds/graph/selection. Old firmware
may then fail to parse the graph (sound fail-closed) or use an incompatible set.

Recommendation: sound-set generation/version stored on both sides, boot-time
compatibility gate, keep the previous sound revision until the new firmware is
confirmed, and test a forced rollback.

Implemented partially: the editor now saves and exports a canonical wire graph
(numeric `observers`/`hold`, no editor-only `hasFn`/`label` in `effects`), which
the pre-fix firmware parser can also read. This reduces rollback incompatibility
but does not provide a sound-set rollback transaction.

### AR-08. Security perimeter

Known issues 8-12: open SoftAP by default, no authentication or CSRF checks,
destructive endpoints and motor control reachable by any client, plain OTA
image accepted, `CONFIG_SECURE_BOOT` and `CONFIG_FLASH_ENCRYPTION` disabled
(`sdkconfig:803,805`). Unauthenticated writes also create flash-wear/DoS risk.

Minimum: require an AP password, add API authentication and rate limits for
mutations, sign OTA images. Secure boot/flash encryption if the device leaves a
trusted room.

### AR-09. Schema and mapping contracts

The editor (`blocks.html`) compiles the graph and the device parses it in C
(`sound_graph.c`) with independent implementations. `web_preview` returns
`valid` without running the C parser. The 2026-10-22 `observers`/`hold`/
`hasFn`/`label` mismatch passed all JS tests and failed only on device. Editor
limits are hardcoded while firmware also has a 16384-token parser limit.

Recommendation: single schema source or generated constants, plus a CI test
that compiles a project in headless Node and parses the payload with the real C
parser. Add token count and all firmware limits to client validation.

Implemented partially: the parser now accepts the documented boolean form of
`observers`/`hold` and the editor's `hasFn`/`label` effect metadata; regression
tests are in `test_sound_graph`. `test_blocks_editor.js` now compiles the
default editor project and feeds the exact payload to the C parser through
`test_tools/graph_contract_check.c`. The remaining gap is a single generated
schema/limit source shared by JS and C.

### AR-10. Audio voices

`SG_MAX_EFFECTS` is 32 (`sound_graph.h:13`) but only 19 dynamic voices exist
(`audio.h:13`); the editor warns at >19, the device does not enforce it.
Complex schemes can exhaust voices and drop transient sounds. Reserve voices
for horn/whistle or expose/limit effective concurrency.

Implemented partially: `/api/sound/graph/state` now reports `retryChannels`, the
bitmask of channels waiting for a free voice. This makes voice exhaustion
visible during board testing; priority/reservation is still to be designed.

### AR-11. Motor/BEMF timing and power

Motor and track tasks are priority 7 and unpinned; audio/sound are pinned to
CPU1. ADC1 is shared by BEMF and rail sense, so track sampling can perturb the
BEMF window. CV3 defaults to 0 (instant acceleration), which can cause inrush
and brownout; the current-sense comparator is not read by software.

Recommendation: pin the motor loop, measure scheduling jitter and BEMF quality
with audio/Wi-Fi active, defer flash writes, consider a nonzero default ramp,
and add current/stall telemetry.

### AR-12. AUX observability and HIL

There is no `GET /api/aux/state`; `auxio_get_enabled()` is internal. Web HIL
presses every AUX and function but only checks HTTP success, so a mapping or
output fault is invisible. `/api/aux/cfg` returns configuration, not the
effective output state.

Fix plan: expose readback of all nine channels and verify it after each HIL
step; if hardware supports it, add current feedback.

Implemented: `GET /api/aux/state` returns `ch`, `on` and `error` for all nine
channels. `run_hil_web.ps1` now checks the on/off readback for every channel;
`test_aux_state_get` covers the JSON contract.

### AR-13. Monolith and shared state

`web.c` combines Wi-Fi/DNS, REST, OTA/upload transactions, graph apply,
function mapping, control and logging. This encourages lock coupling and makes
error paths hard to audit. Suggested split: control/safety service, transfer
service, graph service, HTTP API. Audit `s_cfg` reads outside the control lock.

### AR-14. Multi-request editor apply

The editor saves the graph, applies it, then writes output bindings and AUX
config in separate requests. A failure between them leaves a graph with old or
partial outputs. A local browser draft can also be newer than the device state
and silently overwrite it on save.

Recommendation: a single server-side apply transaction (graph + bindings + AUX
+ volumes) or a resumable job with an explicit "partially applied" state; show
device revision vs draft revision before saving.

## Fix order for the current iteration

1. AR-01: motion state from all accepted motor commands; API-level tests. Done.
2. AR-02/AR-03: safety reset and source-independent e-stop/reset; tests. Done.
3. AR-05: do not leak maintenance ownership on mutex timeout. Done; stale-owner
   recovery remains planned.
4. AR-12: `/api/aux/state` plus HIL readback. Done.
5. AR-04: unsupported canonical sound/slot targets are rejected. Done.
6. AR-09: cross-language editor→C contract test is in place; a single
   generated schema/limit source remains a follow-up.

Deferred work: AR-06 (real-time redesign), AR-07 (full sound-set rollback),
AR-08 (product security policy), AR-10 (voice priority), AR-11 (BEMF timing
tuning), AR-13 (module split), AR-14 (atomic apply transaction).

## Verification for this iteration

- `firmware/test/run_tests.ps1`: all 18 C suites pass (818 tests), plus the
  Node.js and Python suites (`ALL TEST SUITES PASSED`). The block editor suite
  now includes 23 tests, including a cross-language contract test: canonical
  wire payload -> real C `sg_parse`/`sg_validate`.
- Board diagnostics added: `/api/dcc/status` (ISR overruns, last packet age),
  `/api/aux/state` (all nine outputs), and `retryChannels` in
  `/api/sound/graph/state`.
- New regression coverage: motion update through `POST /api/motor`, DC track
  motion callback, all-F/all-AUX mapping, headlight direction, all logic
  actions, emergency reset of all nine AUX channels, `/api/aux/state`, DCC
  all-function round trip, all-F forwarding in `app_main`, and all 29 sound
  effect channels in the runner.
- HIL was not run in this workspace (no board/COM port). The updated
  `run_hil_web.ps1` now verifies AUX readback after each on/off command.
- Firmware images in `release/` were not rebuilt; the device needs a fresh
  build/flash before hardware validation.
