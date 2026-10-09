# AURA-X web preview

Local sandbox for the decoder web interface without a board.

The server serves the **real** page `firmware/web_ui.html` and the built sound
block editor from `firmware/sound_editor/dist`:

- <http://127.0.0.1:8080/> — main decoder UI
- <http://127.0.0.1:8080/sound-editor/blocks.html> — block constructor (locomotive sound blocks)

The block constructor lists test sounds from the committed `SOUND/` folder via
`/api/sound-files` and plays them from `/sound-files/<name>` for preview
auditioning.

Every `/api/*` request is answered with in-memory mock data. The graph API
(`/api/sound/graph/{capabilities,projects,project,state,asset,validate,save,apply}`)
returns a sound-graph project (`states`/`transitions`/`effects`/`assets`), so
Save/Apply and the live state poll work. Function bindings are served by
`/api/func-map?view=bind`.

## Run

Windows:

```
web_preview\start_preview.bat
```

Or directly:

```
python web_preview\mock_server.py            # http://127.0.0.1:8080/
python web_preview\mock_server.py 9000       # custom port
```

Then open <http://127.0.0.1:8080/>.

## Notes

- The source file `firmware/web_ui.html` is served with `no-store`, so editing it
  and reloading the browser shows the change immediately.
- The built block editor (`firmware/sound_editor/dist`) is committed, so a fresh
  `git clone` serves `/sound-editor/blocks.html` without running the build. After
  editing `firmware/sound_editor/public/blocks.html` you must rebuild it with
  `cd firmware/sound_editor && npm run build` (or `build_sound_editor.ps1`) and
  commit the updated bundle — otherwise the device build rejects the stale
  assets (`build-inputs.json` freshness check).
- `__VERSION__` is shown as `PREVIEW`; state resets when the server restarts.
- Unknown `/api/*` paths return `{ "ok": false, "error": "..." }` instead of
  breaking the page.

## Sound pack upload

The sound page («Загрузка звуков») can upload an AURA Sound Pack (`.asp`) in the
emulator too:

- `POST /api/audio/pack` parses and validates the container, decodes every entry
  (IMA ADPCM or PCM16) and replaces the in-memory library — the same rules as the
  device (magic/version/count, index bounds, contiguous payloads, duplicate
  names, mono ADPCM).
- `GET /api/audio/library` lists the uploaded sounds with per-file volumes.
- `POST /api/audio/library/volume` updates a file volume.
- `GET /sound-files/<name>` returns a browser-playable **PCM16** WAV (ADPCM is
  decoded on the fly in Python), so previews play in the browser.
- `GET /api/sound-files` (used by the block editor) lists uploaded + `SOUND/`
  files.

An invalid pack returns `{ "ok": false, "error": "..." }` and is written to the
log.

### Verify it loaded

```
python web_preview\test_mock_pack.py                     # builds a 3-tone pack
python web_preview\test_mock_pack.py release\ADDITIPUS_sounds.asp
```

The self-check runs the server, uploads the pack, and asserts the library size,
the browser-playable `/sound-files/<name>` WAV, and that a malformed pack is
rejected.
