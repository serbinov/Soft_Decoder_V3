# Sound-related CVs

Firmware 0.10. All values are 0..255 unless noted. CV7 is read-only (decoder
version, mirrors `version.txt`); CV8 = 8 triggers a CV factory reset.

| CV | Name | Default | Meaning |
|---|---|---|---|
| 63 | Master volume (alias) | 20 | Bidirectional alias of the persisted master volume (`mvol`). Read/write via the normal CV API; applied by the audio mixer. |
| 30 | Sound options (legacy) | 0 | Stored for compatibility; the graph engine does not read it. |
| 114 | Chuff / exhaust rate (legacy) | 57 | Stored for compatibility; not consumed in 0.10. |
| 115 | Bell ring rate (legacy) | 5 | Stored for compatibility; not consumed in 0.10. |
| 116 | Dynamic brake rate (legacy) | 30 | Stored for compatibility; not consumed in 0.10. |

## Current sound control (0.10)

The graph sound engine is the only sound behaviour model; the legacy MDS/table
subsystem that used CV30 and the CV114–116 rate mapping was removed
(see `sound_scheme.md`). In 0.10:

- playback `rate` and `volume` are properties of a graph state, authored in the
  block editor (`/sound-editor/blocks.html`);
- `engine_volume` / `effects_volume` scale the corresponding channels;
- CV63 (`mvol`) is the final master volume applied by the mixer.

CV30/CV114/CV115/CV116 remain in NVS and in the CV editor so existing dumps and
older documentation do not break. They have no runtime effect.

## Editing

- CV63 is in the CV editor and in the «Звук: Параметры» panel.
- CV63 writes are staged like other CVs and committed by the deferred path
  (`safety_task`), never synchronously in the DCC callback.
