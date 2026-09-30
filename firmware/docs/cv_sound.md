# Sound-related CVs

Firmware 0.9. All values are 0..255 unless noted. CV7 is read-only (decoder
version, mirrors `version.txt`); CV8 = 8 triggers a CV factory reset.

| CV | Name | Default | Meaning |
|---|---|---|---|
| 30 | Sound options | 0 | Bit 0 `Skip StoD1` — do not start the D1 (lowest drive) table from a standstill. Bit 1 `Skip D1toS` — do not play the Stop table on stopping. Other bits reserved. |
| 63 | Master volume (alias) | 20 | Bidirectional alias of the persisted master volume (`mvol`). Read/write via the normal CV API. |
| 114 | Chuff / exhaust rate | 57 | Playback-rate modifier for the engine (chuff/exhaust) loop. |
| 115 | Bell ring rate | 5 | Playback-rate modifier for random/automatic sounds. |
| 116 | Dynamic brake rate | 30 | Playback-rate modifier for the brake-squeal cue. |

## Rate mapping

CV114/115/116 are converted to a playback-rate permille:

```
permille = 500 + cv * 2500 / 255
```

| CV | rate | note |
|---|---|---|
| 0 | 500 ‰ | 0.5× (slowest) |
| 51 | 1000 ‰ | 1.0× (nominal) |
| 255 | 3000 ‰ | 3.0× (fastest) |

Defaults: CV114 = 57 ≈ 1.06×, CV115 = 5 ≈ 0.55×, CV116 = 30 ≈ 0.79×.

Per-table `rate_scale` (0..127, edited in the sound table UI) adds further
speed-dependent scaling on top of CV114 for that table.

## Editing

- CV30/63/114/115/116 are in the CV editor and in the «Звук: Параметры» panel.
- CV63 writes are staged like other CVs and committed by the deferred path
  (`safety_task`), never synchronously in the DCC callback.
