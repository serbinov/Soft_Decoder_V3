# AURA-X web preview

Local sandbox for the decoder web interface without a board.

The server serves the **real** page `firmware/web_ui.html` and the built sound
editor from `firmware/sound_editor/dist`:

- <http://127.0.0.1:8080/> — main decoder UI
- <http://127.0.0.1:8080/sound-editor/> — sound graph editor (effect library + patch panel)

Every `/api/*` request is answered with in-memory mock data. The graph API
(`/api/sound/graph/{capabilities,projects,project,state,asset,validate,save,apply}`)
returns a v2 authoring project (effect tables + `sources`/`blocks`/`sinks`/`wires`
plus the compiled `states`/`transitions`/`effects`), so the editor's library and
patch panel are populated; Save/Apply and the live state poll work. Function
bindings are served by `/api/func-map?view=bind`.

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
- The built sound editor (`firmware/sound_editor/dist`) is committed, so a fresh
  `git clone` serves `/sound-editor/` without running npm. After editing
  `firmware/sound_editor/src` you must rebuild it with
  `cd firmware/sound_editor && npm run build` and commit the updated bundle —
  otherwise the device build rejects the stale assets (`build-inputs.json`
  freshness check).
- `__VERSION__` is shown as `PREVIEW`; state resets when the server restarts.
- Unknown `/api/*` paths return `{ "ok": false, "error": "..." }` instead of
  breaking the page.
