# AURA-X web preview

Local sandbox for developing the decoder web interface without a board.

The server serves the **real** page `firmware/web_ui.html` (so the layout,
themes and logic are exactly the device ones) and answers every `/api/*`
request with in-memory mock data. Controls, settings, log, tracks and the
sound scheme all behave like on the decoder; state resets when the server
restarts.

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

Then open <http://127.0.0.1:8080/>. The page is served with `no-store`, so
after editing `firmware/web_ui.html` just reload the browser.

The sound graph editor is served from its built `sound_editor/dist` at
<http://127.0.0.1:8080/sound-editor/>.

## Notes

- The source file `firmware/web_ui.html` is the single source of truth; the
  preview never modifies it. `__VERSION__` is shown as `PREVIEW`.
- Defaults are chosen to be interactive: control source is `web`, mode `dcc`,
  20 tracks, a 20-table diesel scheme and calibration idle.
- Mocked firmware contracts mirror `firmware/components/web/src/web.c`, so
  field names and shapes match the device responses.
- Unknown `/api/*` paths return `{ "ok": false, "error": "..." }` instead of
  breaking the page.
