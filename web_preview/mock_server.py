#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Local preview server for the AURA-X decoder web UI.

Serves the real firmware page (firmware/web_ui.html) and answers every
/api/* endpoint with in-memory mock data, so the interface can be developed
and tested in a browser exactly like it renders on the decoder -- without a
board. State is kept in process, so the controls, settings, log and sound
graph project behave as on the device.

Run:
    python mock_server.py            # http://127.0.0.1:8080/
    python mock_server.py 9000       # custom port
    set PREVIEW_PORT=9000 & python mock_server.py  (Windows CMD)
"""

import json
import os
import re
import struct
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs, unquote

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.normpath(os.path.join(HERE, "..", "firmware"))
UI_FILE = os.path.join(FW, "web_ui.html")
EDITOR_DIR = os.path.join(FW, "sound_editor", "dist")
SOUND_DIR = os.path.normpath(os.path.join(HERE, "..", "SOUND"))
VERSION = "PREVIEW"
CV_COUNT = 512
FN_COUNT = 29
TRACK_COUNT = 20
AUX_NAMES = ["F0F", "F0R", "AUX1", "AUX2", "AUX3", "AUX4", "AUX5", "AUX6", "AUX7"]

# Graph limits mirror firmware/components/sound/include/sound_graph.h so the
# preview cannot accept a graph the device would reject (or hide a new limit).
SG_HEADER = os.path.join(FW, "components", "sound", "include", "sound_graph.h")
SG_DEFAULTS = {
    "SG_MAX_STATES": 192, "SG_MAX_SOUND_STATES": 144, "SG_MAX_SILENT_STATES": 96,
    "SG_MAX_TRANSITIONS": 250, "SG_MAX_EFFECTS": 32, "SG_MAX_ASSETS": 128,
    "SG_MAX_VARIABLES": 16, "SG_MAX_JSON": 128 * 1024,
}


def _sg_defines():
    values = {}
    try:
        with open(SG_HEADER, "r", encoding="utf-8") as fh:
            for line in fh:
                match = re.match(r"\s*#define\s+(SG_[A-Z0-9_]+)\s+(.+?)\s*(?:/\*.*)?$", line)
                if not match:
                    continue
                name, expr = match.group(1), match.group(2).strip()
                product = re.match(r"^\(?\s*(\d+)U?\s*\*\s*(\d+)U?\s*\)?$", expr)
                if product:
                    values[name] = int(product.group(1)) * int(product.group(2))
                elif expr.isdigit():
                    values[name] = int(expr)
    except OSError:
        pass
    return values


_SG_DEFINES = _sg_defines()


def _sg(name):
    return _SG_DEFINES.get(name, SG_DEFAULTS[name])


SG_MAX_STATES = _sg("SG_MAX_STATES")
SG_MAX_SOUND_STATES = _sg("SG_MAX_SOUND_STATES")
SG_MAX_SILENT_STATES = _sg("SG_MAX_SILENT_STATES")
SG_MAX_TRANSITIONS = _sg("SG_MAX_TRANSITIONS")
SG_MAX_EFFECTS = _sg("SG_MAX_EFFECTS")
SG_MAX_ASSETS = _sg("SG_MAX_ASSETS")
SG_MAX_VARIABLES = _sg("SG_MAX_VARIABLES")
SG_MAX_JSON = _sg("SG_MAX_JSON")

GRAPH_LIMITS = {
    "states": SG_MAX_STATES, "soundStates": SG_MAX_SOUND_STATES,
    "transitions": SG_MAX_TRANSITIONS, "effects": SG_MAX_EFFECTS,
    "assets": SG_MAX_ASSETS, "variables": SG_MAX_VARIABLES,
    "jsonBytes": SG_MAX_JSON,
}
CONDITION_TYPES = [
    "fn_press", "fn_release", "fn_on", "fn_off", "engine_on", "engine_off",
    "speed", "accel", "decel", "sample_done", "dir_fwd", "dir_rev",
    "random", "timeout", "var_cmp", "state_enter",
]
VARIABLE_KINDS = ["speed", "forward", "moving", "engine"]
COMPARE_OPS = ["eq", "ne", "lt", "le", "gt", "ge"]
TRACK_LABELS = [
    "Тепловоз", "Отправление", "Свисток", "Тифон", "Компрессор", "Вентилятор",
    "Сцепка", "Тормоз", "Гудок", "Стрелка", "Колёса", "Песок", "Генератор",
    "Дизель", "Токоприёмник", "Шасси", "Дверь", "Кондиционер", "Дребезг",
    "Обрыв",
]

LOCK = threading.Lock()
START = time.time()

# Uploaded sound library (base name without .wav -> {"vol": int, "wav": bytes}),
# populated by POST /api/audio/pack. Mirrors the device's /userdata/audio.
LIBRARY = {}
PREVIEW = {"name": ""}

# IMA/DVI ADPCM tables, matching firmware/components/audio/src/ima_adpcm.c.
_IMA_INDEX = (-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8)
_IMA_STEP = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767)
IMA_BLOCK = 256          # 4-byte preamble + 252 data bytes
IMA_SAMPLES = 505        # 1 preamble sample + 504 nibbles


def _ima_decode_block(buf, off, size=IMA_BLOCK):
    """Decode one mono IMA ADPCM block to a list of int16 samples."""
    pred = int.from_bytes(buf[off:off + 2], "little", signed=True)
    index = min(88, max(0, buf[off + 2]))
    out = [pred]
    for i in range(off + 4, off + size):
        byte = buf[i]
        for half in (0, 1):
            nib = (byte & 0x0F) if half == 0 else (byte >> 4)
            step = _IMA_STEP[index]
            diff = step >> 3
            if nib & 1:
                diff += step >> 2
            if nib & 2:
                diff += step >> 1
            if nib & 4:
                diff += step
            pred = pred - diff if (nib & 8) else pred + diff
            pred = max(-32768, min(32767, pred))
            index = max(0, min(88, index + _IMA_INDEX[nib]))
            out.append(pred)
    return out


def _pcm16_wav(samples, rate):
    data = struct.pack("<%dh" % len(samples), *samples)
    block_align, byte_rate = 2, rate * 2
    header = (b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE" +
              b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, byte_rate, block_align, 16) +
              b"data" + struct.pack("<I", len(data)))
    return header + data


def _base(name):
    """Base sound name: strip the directory and a trailing .wav only (NOT any
    extension: a name like 'loop1.2' must keep its dot)."""
    base = os.path.basename(name or "")
    if base.lower().endswith(".wav"):
        base = base[:-4]
    return base


def _sanitize(name):
    """Match tools/pack_sounds.py and components/web/src/web_util.c."""
    out = []
    for ch in name:
        o = ord(ch)
        if ch.isascii() and ch.isalnum():
            out.append(ch)
        elif o >= 0x80 or ch in ("_", "-", "."):
            out.append(ch)
        else:
            out.append("_")
    return "".join(out)


def _sound_alias():
    """Map sanitized device filename -> source WAV in SOUND/."""
    aliases = {}
    try:
        names = os.listdir(SOUND_DIR)
    except OSError:
        names = []
    for name in sorted(names):
        if not name.lower().endswith(".wav"):
            continue
        aliases.setdefault(_sanitize(name[:-4]) + ".wav", name)
    return aliases


SOUND_ALIASES = _sound_alias()


def _seed_library_from_sound_dir():
    """Populate LIBRARY with WAV files from SOUND/ on startup so the Sounds
    section in the preview UI immediately displays available sounds."""
    for dev_wav, orig_name in sorted(SOUND_ALIASES.items()):
        base = _base(dev_wav)
        fpath = os.path.join(SOUND_DIR, orig_name)
        try:
            size = os.path.getsize(fpath)
        except OSError:
            continue
        LIBRARY[base] = {"vol": 100, "path": fpath, "size": size}


_seed_library_from_sound_dir()


_GRAPH_ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,31}$")
_GRAPH_TIMINGS = ("immediate", "after_sample")
_GRAPH_FN_CONDITIONS = ("fn_press", "fn_release", "fn_on", "fn_off")
_GRAPH_RANGE_CONDITIONS = ("speed", "accel", "decel")
_GRAPH_VAR_KINDS = ("user", "speed", "forward", "moving", "engine")


def _graph_int(value, low, high, what):
    if isinstance(value, bool) or not isinstance(value, int) or not low <= value <= high:
        raise ValueError("invalid %s" % what)
    return value


def _graph_id(value):
    return isinstance(value, str) and bool(_GRAPH_ID_RE.match(value))


def _graph_wav_name(value):
    if not isinstance(value, str) or not value.endswith(".wav"):
        return False
    raw = value.encode("utf-8")
    if not 5 <= len(raw) < 64:
        return False
    stem = value[:-4]
    for i, ch in enumerate(stem):
        if ord(ch) >= 0x80:
            continue
        if not (ch.isascii() and (ch.isalnum() or ch in "_-.")):
            return False
        if ch == "." and (i == 0 or stem[i - 1] == "."):
            return False
    return True


def _validate_graph(doc, available_files=None):
    """Mirror the core firmware parser/validator invariants for a compiled
    schema-v1 sound graph. Raises ValueError with a short reason on failure."""
    if not isinstance(doc, dict):
        raise ValueError("project must be an object")
    if doc.get("format") != "sound-graph":
        raise ValueError("format must be sound-graph")
    if doc.get("schemaVersion") != 1:
        raise ValueError("unsupported schemaVersion")
    if not _graph_id(doc.get("id")):
        raise ValueError("invalid graph id")
    if not isinstance(doc.get("name"), str):
        raise ValueError("invalid graph name")
    engine = doc.get("engine")
    if not isinstance(engine, dict) or not _graph_id(engine.get("entry")):
        raise ValueError("invalid engine entry")
    fn = engine.get("fn")
    if isinstance(fn, bool) or not isinstance(fn, int) or not (0 <= fn <= 28 or fn == 255):
        raise ValueError("invalid engine fn")
    if "hold" in engine and not isinstance(engine["hold"], bool):
        raise ValueError("invalid engine hold")
    if "hysteresis" in doc:
        _graph_int(doc["hysteresis"], 0, 32, "hysteresis")

    variables = doc.get("variables", [])
    if not isinstance(variables, list) or len(variables) > SG_MAX_VARIABLES:
        raise ValueError("variable capacity")
    var_names = []
    var_kinds = []
    for var in variables:
        if not isinstance(var, dict):
            raise ValueError("invalid variable")
        name = var.get("name")
        if not isinstance(name, str) or not name or len(name.encode("utf-8")) >= 24:
            raise ValueError("invalid variable name")
        if name in var_names:
            raise ValueError("duplicate variable name")
        kind = var.get("kind", "user")
        if kind not in _GRAPH_VAR_KINDS:
            raise ValueError("invalid variable kind")
        init = var.get("init", 0)
        if isinstance(init, bool) or not isinstance(init, int) or not -(1 << 31) <= init < (1 << 31):
            raise ValueError("invalid variable init")
        if kind != "user" and init != 0:
            raise ValueError("builtin variable cannot have an initial value")
        var_names.append(name)
        var_kinds.append(kind)

    states = doc.get("states")
    if not isinstance(states, list) or not states or len(states) > SG_MAX_STATES:
        raise ValueError("state capacity")
    state_ids = []
    audible = silent = 0
    for state in states:
        if not isinstance(state, dict) or not _graph_id(state.get("id")):
            raise ValueError("invalid state")
        sid = state["id"]
        if sid in state_ids:
            raise ValueError("duplicate state ID")
        if not isinstance(state.get("name"), str):
            raise ValueError("invalid state name")
        file = state.get("file", "")
        if not isinstance(file, str) or (file and not _graph_wav_name(file)):
            raise ValueError("invalid WAV basename")
        if not isinstance(state.get("loop"), bool):
            raise ValueError("invalid state loop")
        _graph_int(state.get("volume"), 0, 100, "state volume")
        _graph_int(state.get("rate"), 500, 3000, "state rate")
        if "loops" in state:
            _graph_int(state["loops"], 0, 255, "state loops")
        if "setVar" in state:
            _graph_int(state["setVar"], 0, max(0, len(variables) - 1), "state setVar")
            if state["setVar"] >= len(variables) or var_kinds[state["setVar"]] != "user":
                raise ValueError("state setVar must be a user variable")
        if "setValue" in state:
            _graph_int(state["setValue"], -(1 << 15), (1 << 15) - 1, "state setValue")
        if file:
            audible += 1
        else:
            silent += 1
        state_ids.append(sid)
    if audible > SG_MAX_SOUND_STATES or silent > SG_MAX_SILENT_STATES:
        raise ValueError("audible/silent state capacity")

    assets = doc.get("assets")
    if not isinstance(assets, list) or len(assets) > SG_MAX_ASSETS:
        raise ValueError("asset capacity")
    asset_files = []
    for asset in assets:
        if not isinstance(asset, dict) or not _graph_wav_name(asset.get("file")):
            raise ValueError("invalid asset metadata")
        file = asset["file"]
        if file in asset_files:
            raise ValueError("duplicate asset")
        crc = asset.get("crc32")
        if not isinstance(crc, str) or not re.match(r"^[0-9A-Fa-f]{8}$", crc):
            raise ValueError("invalid CRC32")
        for key, low in (("size", 1), ("sampleRate", 1), ("channels", 1), ("bits", 1)):
            _graph_int(asset.get(key), low, (1 << 32) - 1, "asset %s" % key)
        asset_files.append(file)

    asset_set = set(asset_files)
    for state in states:
        file = state.get("file", "")
        if file and file not in asset_set:
            raise ValueError("referenced WAV requires an asset manifest")
        if file and available_files is not None and file not in available_files:
            raise ValueError("referenced WAV is missing from the pack")

    entry = engine["entry"]
    if entry not in state_ids or states[state_ids.index(entry)].get("file"):
        raise ValueError("engine entry must be silent")

    effects = doc.get("effects")
    if not isinstance(effects, list) or len(effects) > SG_MAX_EFFECTS:
        raise ValueError("effect capacity")
    effect_ids = []
    effect_entries = []
    for effect in effects:
        if not isinstance(effect, dict) or not _graph_id(effect.get("id")):
            raise ValueError("invalid effect")
        if effect["id"] in effect_ids:
            raise ValueError("duplicate effect ID")
        _graph_int(effect.get("fn"), 0, 28, "effect fn")
        target = effect.get("entry")
        if target not in state_ids or states[state_ids.index(target)].get("file"):
            raise ValueError("effect entry must be silent")
        effect_ids.append(effect["id"])
        effect_entries.append(target)

    transitions = doc.get("transitions")
    if not isinstance(transitions, list) or len(transitions) > SG_MAX_TRANSITIONS:
        raise ValueError("transition capacity")
    transition_ids = []
    for transition in transitions:
        if not isinstance(transition, dict) or not _graph_id(transition.get("id")):
            raise ValueError("invalid transition")
        if transition["id"] in transition_ids:
            raise ValueError("duplicate transition ID")
        if transition.get("source") not in state_ids or transition.get("target") not in state_ids:
            raise ValueError("dangling transition")
        _graph_int(transition.get("priority"), 0, 255, "transition priority")
        if transition.get("timing") not in _GRAPH_TIMINGS:
            raise ValueError("invalid transition timing")
        cond = transition.get("condition")
        if not isinstance(cond, dict) or cond.get("type") not in CONDITION_TYPES:
            raise ValueError("unknown condition")
        typ = cond["type"]
        if typ in _GRAPH_FN_CONDITIONS:
            _graph_int(cond.get("fn"), 0, 28, "condition fn")
        elif "fn" in cond:
            raise ValueError("condition fn mismatch")
        if typ in _GRAPH_RANGE_CONDITIONS:
            if "min" not in cond and "max" not in cond:
                raise ValueError("missing condition range")
            if "min" in cond:
                _graph_int(cond["min"], 0, 255, "condition min")
            if "max" in cond:
                _graph_int(cond["max"], 0, 255, "condition max")
            if cond.get("min", 0) > cond.get("max", 255):
                raise ValueError("inverted condition range")
        elif typ == "random":
            _graph_int(cond.get("min"), 0, 100, "random chance")
            if "max" in cond:
                raise ValueError("invalid random chance")
        elif typ == "timeout":
            _graph_int(cond.get("min"), 0, 255, "timeout")
            if "max" in cond:
                raise ValueError("invalid timeout")
        elif "min" in cond or "max" in cond:
            raise ValueError("unexpected condition range")
        if typ == "var_cmp":
            _graph_int(cond.get("var"), 0, max(0, len(variables) - 1), "var_cmp variable")
            if cond["var"] >= len(variables):
                raise ValueError("invalid var_cmp variable")
            if "var2" in cond:
                _graph_int(cond["var2"], 0, max(0, len(variables) - 1), "var_cmp second variable")
                if cond["var2"] >= len(variables):
                    raise ValueError("invalid var_cmp second variable")
            if "op" in cond:
                _graph_int(cond["op"], 0, 5, "var_cmp operator")
            if "value" in cond:
                _graph_int(cond["value"], -(1 << 15), (1 << 15) - 1, "var_cmp value")
        elif "var" in cond or "var2" in cond:
            raise ValueError("unexpected variable fields")
        transition_ids.append(transition["id"])

    reached = {entry}
    reached.update(effect_entries)
    for _ in range(len(states)):
        changed = False
        for transition in transitions:
            if transition["source"] in reached and transition["target"] not in reached:
                reached.add(transition["target"])
                changed = True
        if not changed:
            break
    if len(reached) != len(state_ids):
        raise ValueError("unreachable state")


def _parse_pack(body):
    """Validate an AURA Sound Pack (.asp) and return its index entries.
    Raises ValueError with a short reason on any structural problem."""
    if len(body) < 16:
        raise ValueError("файл меньше заголовка")
    if body[0:8] != b"AURASP01":
        raise ValueError("не контейнер .asp (магия)")
    version, count = struct.unpack_from("<HH", body, 8)
    data_off = struct.unpack_from("<I", body, 12)[0]
    if version != 1:
        raise ValueError("неподдерживаемая версия %d" % version)
    if not (1 <= count <= 400):
        raise ValueError("неверное число звуков: %d" % count)
    if data_off < 16 + count * 16:
        raise ValueError("неверное смещение данных")
    entries = []
    seen = set()
    pos = 16
    for _ in range(count):
        if pos + 16 > len(body):
            raise ValueError("оборванный индекс")
        nlen, vol, ch, bits = body[pos], body[pos + 1], body[pos + 2], body[pos + 3]
        rate = struct.unpack_from("<I", body, pos + 4)[0]
        doff, dlen = struct.unpack_from("<II", body, pos + 8)
        pos += 16
        if not (1 <= nlen <= 63) or vol > 100:
            raise ValueError("неверная запись индекса")
        if pos + nlen > len(body):
            raise ValueError("оборванное имя")
        name = body[pos:pos + nlen]
        pos += nlen
        try:
            name = name.decode("utf-8")
        except UnicodeDecodeError:
            raise ValueError("не UTF-8 имя")
        if name in seen:
            raise ValueError("дубликат имени: %s" % name)
        seen.add(name)
        if bits == 0:
            if name != "__aura_scheme__" or ch != 0 or rate != 0 or not (0 < dlen <= 131072):
                raise ValueError("неверная запись схемы")
        else:
            if ch not in (1, 2) or bits not in (4, 16):
                raise ValueError("формат %d/%d бит не поддержан" % (ch, bits))
            if bits == 4 and ch != 1:
                raise ValueError("ADPCM только моно")
            if not (0 < rate <= 192000):
                raise ValueError("неверная частота")
            frame = ch * 2 if bits == 16 else IMA_BLOCK
            if dlen < frame or dlen % frame != 0:
                raise ValueError("неверная длина данных")
        if doff + dlen > len(body):
            raise ValueError("данные выходят за файл")
        entries.append({"name": name, "vol": vol, "bits": bits, "channels": ch,
                        "rate": rate, "off": doff, "len": dlen})
    if pos != data_off:
        raise ValueError("длина индекса не совпадает")
    off = data_off
    for e in entries:
        if e["off"] != off:
            raise ValueError("данные не подряд")
        off += e["len"]
    if off != len(body):
        raise ValueError("размер не совпадает с заголовком")
    return entries


def _load_pack(body):
    """Replace the in-memory library (and optional bundled sound scheme) with
    an uploaded pack. Returns (files, bytes, encoding, scheme_id) or raises ValueError."""
    entries = _parse_pack(body)
    lib = {}
    encodings = set()
    scheme_doc = None
    scheme_id = ""
    for e in entries:
        payload = body[e["off"]:e["off"] + e["len"]]
        if e["bits"] == 0 and e["name"] == "__aura_scheme__":
            try:
                doc = json.loads(payload.decode("utf-8"))
            except (ValueError, UnicodeDecodeError) as exc:
                raise ValueError("некорректная схема в пакете: %s" % exc)
            if isinstance(doc, dict) and doc.get("format") == "sound-graph":
                scheme_doc = doc
            continue
        if e["bits"] == 4:
            blocks = e["len"] // IMA_BLOCK
            samples = []
            for b in range(blocks):
                samples.extend(_ima_decode_block(payload, b * IMA_BLOCK))
            encodings.add("ADPCM")
            stored_hdr = 60  # ADPCM WAV header (+fact)
        else:
            samples = list(struct.unpack("<%dh" % (e["len"] // 2), payload))
            encodings.add("PCM16")
            stored_hdr = 44  # PCM16 WAV header
        # "size" mirrors the .wav the device writes on unpack; "wav" is the
        # decoded PCM16 used only for browser playback.
        lib[e["name"]] = {"vol": e["vol"], "wav": _pcm16_wav(samples, e["rate"]),
                          "size": stored_hdr + e["len"], "raw": payload,
                          "rate": e["rate"], "channels": e["channels"], "bits": e["bits"]}
    if scheme_doc is not None:
        available = set()
        for name in lib:
            available.add(name)
            available.add(name + ".wav")
        try:
            _validate_graph(scheme_doc, available_files=available)
        except ValueError as exc:
            raise ValueError("некорректная схема в пакете: %s" % exc)
    LIBRARY.clear()
    LIBRARY.update(lib)
    PREVIEW["name"] = ""
    if scheme_doc is not None:
        GRAPH["project"] = scheme_doc
        GRAPH["revision"] = GRAPH.get("revision", 0) + 1
        GRAPH["active"] = True
        GRAPH["fault"] = False
        scheme_id = scheme_doc.get("id", "locomotive_default")
        PROJECTS[scheme_id] = {"project": scheme_doc, "revision": GRAPH["revision"]}
        _runner_reset()
        _save_persisted()
    return len(lib), len(body), "/".join(sorted(encodings)), scheme_id



def _new_tracks():
    tracks = []
    cats = []
    for i in range(TRACK_COUNT):
        tracks.append({
            "slot": i + 1,
            "file": "audio/slot%02d.wav" % (i + 1),
            "label": TRACK_LABELS[i],
            "enabled": True,
        })
        cats.append(0 if i < 6 else 1)
    return tracks, cats


def _asset(name):
    size = 44144
    sample_rate = 22050
    channels = 1
    bits = 16
    duration_ms = 1000
    base = _base(name)
    raw = None
    if base in LIBRARY:
        entry = LIBRARY[base]
        if "wav" in entry:
            raw = entry["wav"]
            size = len(raw)
        elif "path" in entry and os.path.isfile(entry["path"]):
            try:
                with open(entry["path"], "rb") as fh:
                    raw = fh.read(64)
                size = os.path.getsize(entry["path"])
            except OSError:
                raw = None
    else:
        safe = os.path.normpath(os.path.join(SOUND_DIR, name or ""))
        if not (safe.startswith(SOUND_DIR) and os.path.isfile(safe)):
            alias = SOUND_ALIASES.get(name or "")
            if alias:
                safe = os.path.join(SOUND_DIR, alias)
        if os.path.isfile(safe):
            try:
                with open(safe, "rb") as fh:
                    raw = fh.read(64)
                size = os.path.getsize(safe)
            except OSError:
                raw = None
    if raw and len(raw) >= 44 and raw[0:4] == b"RIFF":
        if size == 44144:
            size = len(raw)
        channels = max(1, struct.unpack_from("<H", raw, 22)[0])
        sample_rate = max(1, struct.unpack_from("<I", raw, 24)[0])
        bits = max(4, struct.unpack_from("<H", raw, 34)[0])
        bytes_per_sec = sample_rate * channels * max(1, bits) / 8.0
        if bytes_per_sec > 0 and size > 44:
            duration_ms = max(20, int(round((size - 44) * 1000.0 / bytes_per_sec)))
    return {"file": name, "size": size, "crc32": "12345678",
            "sampleRate": sample_rate, "channels": channels, "bits": bits, "durationMs": duration_ms}


def _state(sid, name, filename, loop, volume=80):
    return {"id": sid, "name": name, "file": filename, "loop": loop,
            "volume": volume, "rate": 1000}


def _edge(eid, source, target, priority, timing, condition):
    return {"id": eid, "source": source, "target": target,
            "priority": priority, "timing": timing, "condition": condition}


def _new_graph():
    """A v2 authoring project (effect-table library + patch-panel routing) with
    a compiled v1 behaviour layer, mirroring what the sound editor saves."""
    states = [
        {"id": "off", "name": "Выкл / тихий вход", "file": "", "loop": False, "volume": 100, "rate": 1000},
        _state("engine_idle", "Холостой ход ЧМЭ3", "s-loop1.1.wav", True),
        _state("engine_run", "Тяга ЧМЭ3", "notch_4-loop1.1.wav", True),
        _state("engine_stop", "Останов дизеля", "ms-end.wav", False),
        {"id": "horn_off", "name": "Тифон / тихий вход", "file": "", "loop": False, "volume": 100, "rate": 1000},
        _state("horn_start", "Атака тифона", "horn-low-init.wav", False),
        _state("horn_hold", "Удержание тифона", "horn-low-loop.wav", True),
        _state("horn_end", "Отпускание тифона", "horn-low-exit.wav", False),
    ]
    transitions = [
        _edge("t1", "off", "engine_idle", 10, "immediate", {"type": "engine_on"}),
        _edge("t2", "engine_idle", "engine_run", 1, "after_sample", {"type": "speed", "min": 1, "max": 255}),
        _edge("t3", "engine_run", "engine_idle", 1, "after_sample", {"type": "speed", "min": 0, "max": 0}),
        _edge("t4", "engine_idle", "engine_stop", 255, "immediate", {"type": "engine_off"}),
        _edge("t5", "engine_run", "engine_stop", 255, "immediate", {"type": "engine_off"}),
        _edge("t6", "engine_stop", "off", 10, "immediate", {"type": "sample_done"}),
        _edge("t7", "horn_off", "horn_start", 10, "immediate", {"type": "fn_press", "fn": 2}),
        _edge("t8", "horn_start", "horn_hold", 10, "immediate", {"type": "sample_done"}),
        _edge("t9", "horn_start", "horn_end", 200, "immediate", {"type": "fn_off", "fn": 2}),
        _edge("t10", "horn_hold", "horn_end", 10, "immediate", {"type": "fn_off", "fn": 2}),
        _edge("t11", "horn_end", "horn_off", 10, "immediate", {"type": "sample_done"}),
    ]
    effect_tables = [
        {"id": "horn", "name": "Тифон", "kind": "horn", "preset": "shortLong",
         "init": "horn-low-init.wav", "loop": "horn-low-loop.wav", "end": "horn-low-exit.wav", "short": "horn-low-exit.wav",
         "shortMs": 400, "volume": 80, "rate": 1000, "behavior": "horn_off"},
        {"id": "motor", "name": "Дизель ЧМЭ3", "kind": "motor", "preset": "loopHeld",
         "init": "sm.wav", "loop": "s-loop1.1.wav", "end": "ms-end.wav", "short": "",
         "shortMs": 400, "volume": 85, "rate": 1000, "behavior": ""},
        {"id": "brake", "name": "Тормоза", "kind": "brake", "preset": "state",
         "init": "", "loop": "", "end": "bremse-anlegen_lokbremse.wav", "short": "",
         "shortMs": 400, "volume": 75, "rate": 1000, "behavior": ""},
        {"id": "bell", "name": "Радиостанция", "kind": "bell", "preset": "random",
         "init": "piepton-rangierfunk.wav", "loop": "", "end": "", "short": "",
         "shortMs": 400, "volume": 75, "rate": 1000, "behavior": ""},
        {"id": "coupler", "name": "Автосцепка", "kind": "coupler", "preset": "oneShot",
         "init": "ankuppeln.wav", "loop": "", "end": "", "short": "",
         "shortMs": 400, "volume": 80, "rate": 1000, "behavior": ""},
    ]
    sources = [
        {"id": "src_engine", "role": "engine", "fn": 8, "label": "Двигатель"},
        {"id": "src_horn", "role": "fn", "fn": 2, "label": "F2 Тифон"},
        {"id": "src_f3", "role": "fn", "fn": 5, "label": "F5 Радиостанция"},
    ]
    blocks = [
        {"id": "blk_motor", "kind": "sound", "table": "motor", "op": "and", "min": 0, "max": 0},
        {"id": "blk_horn", "kind": "sound", "table": "horn", "op": "and", "min": 0, "max": 0},
        {"id": "blk_bell", "kind": "sound", "table": "bell", "op": "and", "min": 0, "max": 0},
    ]
    sinks = [
        {"id": "sink_head", "output": "F0F"},
        {"id": "sink_aux3", "output": "AUX3"},
    ]
    wires = [
        {"id": "w1", "from": "src_engine", "to": "blk_motor", "event": "fn_on", "dir": "any", "state": "any"},
        {"id": "w2", "from": "src_horn", "to": "blk_horn", "event": "fn_press", "dir": "any", "state": "any"},
        {"id": "w3", "from": "src_f3", "to": "blk_bell", "event": "fn_press", "dir": "any", "state": "any"},
        {"id": "w4", "from": "blk_motor", "to": "sink_head", "event": "", "dir": "fwd", "state": "moving"},
        {"id": "w5", "from": "blk_horn", "to": "sink_aux3", "event": "", "dir": "any", "state": "any"},
    ]
    files = set()
    for st in states:
        if st["file"]:
            files.add(st["file"])
    for tbl in effect_tables:
        for role in ("init", "loop", "end", "short"):
            if tbl[role]:
                files.add(tbl[role])
    positions = {}
    for node in states + sources + blocks + sinks:
        positions[node["id"]] = {"x": 200, "y": 80}
    return {
        "format": "sound-graph", "schemaVersion": 2, "id": "preview",
        "name": "Тепловоз ЧМЭ3 (превью)",
        "engine": {"entry": "off", "fn": 8}, "hysteresis": 3,
        "effectTables": effect_tables, "sources": sources, "blocks": blocks,
        "sinks": sinks, "wires": wires,
        "states": states, "transitions": transitions,
        "effects": [{"id": "horn", "entry": "horn_off", "fn": 2}],
        "assets": [_asset(name) for name in sorted(files)],
        "editor": {"positions": positions, "viewport": {"x": 0, "y": 0, "zoom": 0.8}},
    }


def _new_state():
    tracks, cats = _new_tracks()
    return {
        "control_source": "web",
        "mode": "dcc",
        "motor": {"speed": 0, "forward": True},
        "fn": [0] * FN_COUNT,
        "audio": {"playing": False, "active_slot": 0, "master": 100, "engine": 100, "effects": 20},
        "device_name": "ТЕПЛОВОЗ",
        "wifi": {
            "mode": 1, "ap_ssid": "AURA-X", "ap_password": "12345678",
            "auto_off_min": 0, "status": "ap_running", "ap_ip": "192.168.100.1",
        },
        "bemf": {"active": False, "progress": 0, "total": 10, "valid": True,
                 "stored": True, "use": False, "runId": 1, "result": "idle",
                 "error": "none", "errorCode": 0, "apply_pid": False,
                 "profile": {"mode": 255, "planned": 10, "measured": 10,
                             "stalled": 0, "ceiling": 0, "first": 12,
                             "firstFrac": 200, "settle": 0, "gain": 0,
                             "kp": 0, "ki": 0, "kd": 0, "quality": 0,
                             "pidApplied": False}},
        "cv": [0] * CV_COUNT,
        "aux": [{"level": 100, "effect": 0} for _ in range(9)],
        "fmap": _new_fmap(),
        "binds": _new_binds(),
        "tracks": tracks,
        "cats": cats,
        "log": [],
        "logseq": 0,
    }


def _new_fmap():
    fmap = []
    for f in range(FN_COUNT):
        entry = {"a": 0, "b": 0, "aux": 0, "dir": 0, "speed": 0}
        if f == 0:
            entry["aux"] = 1
        elif 1 <= f <= TRACK_COUNT:
            entry["a"] = f
        fmap.append(entry)
    return fmap


def _bind(fn, btype, bid, direction, state, mode=0):
    return {
        "fn": fn, "type": btype, "id": bid, "dir": direction, "state": state,
        "mode": mode, "flags": 0, "short": 0, "short_ms": 400,
        "min_ms": 150, "fade_ms": 80,
    }


def _new_binds():
    """Effective bindings mirroring the device defaults after legacy migration:
    F0 head light (bit0 forward, bit1 reverse), F1..F20 -> audio slots, plus a
    couple of sound-table bindings so the "Звук: Функции" matrix shows dots."""
    binds = [
        _bind(0, 1, 0, 1, 0),
        _bind(0, 1, 1, 2, 0),
        # Locomotive template outputs (F3/F4/F11/F14/F28 -> AUX1..AUX5).
        _bind(3, 1, 2, 0, 0),
        _bind(4, 1, 3, 0, 0),
        _bind(11, 1, 4, 0, 0),
        _bind(14, 1, 5, 0, 0),
        _bind(28, 1, 6, 0, 0),
    ]
    for n in range(1, TRACK_COUNT + 1):
        binds.append(_bind(n, 3, n, 0, 0))
    binds.append(_bind(3, 2, 17, 0, 2, 0))
    binds.append(_bind(5, 2, 15, 0, 2, 1))
    return binds


STATE = _new_state()
STATE["cv"][0] = 3        # CV1 primary address
STATE["cv"][28] = 2       # CV29 28/128 speed steps

# The active sound graph project and its revision.
GRAPH = {"project": _new_graph(), "revision": 4, "active": True, "fault": False}
# Saved projects by id; the firmware store keeps several revisions/projects, so
# the preview must do the same or "New project" appears to overwrite the old one.
PROJECTS = {GRAPH["project"]["id"]: {"project": GRAPH["project"], "revision": GRAPH["revision"]}}
PERSIST_PATH = None


def _load_persisted(port):
    global PERSIST_PATH
    PERSIST_PATH = os.path.join(tempfile.gettempdir(), "aura_preview_graph_%d.json" % port)
    try:
        with open(PERSIST_PATH, "r", encoding="utf-8") as fh:
            data = json.load(fh)
        projects = data.get("projects")
        if not isinstance(projects, dict):
            return
        loaded = {}
        for pid, ent in projects.items():
            if isinstance(pid, str) and isinstance(ent, dict) and isinstance(ent.get("project"), dict):
                loaded[pid] = {"project": ent["project"], "revision": max(1, int(ent.get("revision") or 1))}
        if not loaded:
            return
        PROJECTS.clear()
        PROJECTS.update(loaded)
        active_id = data.get("active_id") or ""
        if active_id in PROJECTS:
            GRAPH["project"] = PROJECTS[active_id]["project"]
            GRAPH["revision"] = PROJECTS[active_id]["revision"]
            GRAPH["active"] = bool(data.get("active", True))
        else:
            GRAPH["active"] = False
        GRAPH["fault"] = False
    except Exception:
        pass


def _save_persisted():
    if not PERSIST_PATH:
        return
    try:
        tmp = PERSIST_PATH + ".tmp"
        with open(tmp, "w", encoding="utf-8") as fh:
            json.dump({"projects": PROJECTS, "active": GRAPH["active"],
                       "active_id": GRAPH["project"].get("id", "")}, fh, ensure_ascii=False)
        os.replace(tmp, PERSIST_PATH)
    except Exception:
        pass
# Minimal graph runner for the preview: it keeps the same channel order as the
# firmware (engine at index 0, then effects) and evaluates the saved schema-v1
# transitions from the current F-key/motor state. This makes Live show the same
# active sound blocks as the editor simulator while controlling over the web.
RUNNER = {"channels": [], "state_by_id": {}, "assets": {}, "previous_fn": 0,
          "previous_speed": 0, "last_ms": 0.0}


def _runner_payload():
    p = GRAPH.get("project") or {}
    return p if isinstance(p.get("states"), list) else None


def _runner_channel(engine, index, states):
    st = states[index]
    return {"engine": engine, "state": index, "elapsed": 0.0,
            "done": not bool(st.get("file")), "loops_done": 0,
            "entered": True, "pending": "", "pending_prio": 0}


def _runner_reset():
    p = _runner_payload()
    if not p:
        RUNNER.update(channels=[], state_by_id={}, assets={})
        return
    states = p.get("states") or []
    RUNNER["state_by_id"] = {st.get("id"): i for i, st in enumerate(states)}
    RUNNER["assets"] = {a.get("file"): int(a.get("durationMs", 1000) or 1000)
                        for a in (p.get("assets") or []) if a.get("file")}
    chans = []
    eng = p.get("engine") or {}
    if eng.get("entry") in RUNNER["state_by_id"]:
        chans.append(_runner_channel(True, RUNNER["state_by_id"][eng["entry"]], states))
    for effect in (p.get("effects") or []):
        entry = effect.get("entry")
        if entry in RUNNER["state_by_id"]:
            chans.append(_runner_channel(False, RUNNER["state_by_id"][entry], states))
    RUNNER["channels"] = chans
    RUNNER["previous_fn"] = 0
    RUNNER["previous_speed"] = 0
    RUNNER["last_ms"] = time.monotonic() * 1000.0


def _runner_enter(channel, target):
    idx = RUNNER["state_by_id"].get(target)
    if idx is None:
        return
    st = (_runner_payload().get("states") or [])[idx]
    channel.update(state=idx, elapsed=0.0, done=not bool(st.get("file")),
                   loops_done=0, entered=True, pending="", pending_prio=0)


def _runner_var(p, name):
    return {"speed": int(RUNNER.get("speed255", 0)),
            "forward": 1 if STATE["motor"]["forward"] else 0,
            "moving": 1 if int(STATE["motor"]["speed"]) > 0 else 0,
            "engine": 1 if RUNNER.get("engine_on") else 0}.get(name, 0)


def _runner_cond(c, channel, press, release, speed, engine_on, accel, entered):
    if not c:
        return True
    typ = c.get("type")
    fn = c.get("fn")
    if typ == "state_enter":
        return bool(entered)
    if typ == "fn_press":
        return fn is not None and fn >= 0 and bool(press & (1 << int(fn)))
    if typ == "fn_release":
        return fn is not None and fn >= 0 and bool(release & (1 << int(fn)))
    if typ == "fn_on":
        return fn is not None and 0 <= int(fn) < FN_COUNT and bool(STATE["fn"][int(fn)])
    if typ == "fn_off":
        return fn is not None and 0 <= int(fn) < FN_COUNT and not STATE["fn"][int(fn)]
    if typ == "engine_on":
        return bool(engine_on)
    if typ == "engine_off":
        return not engine_on
    if typ == "dir_fwd":
        return bool(STATE["motor"]["forward"])
    if typ == "dir_rev":
        return not STATE["motor"]["forward"]
    if typ == "sample_done":
        return bool(channel["done"])
    if typ == "timeout":
        return channel["elapsed"] >= max(1, int(c.get("min", 0) or 0)) * 50
    if typ == "speed":
        if not engine_on:
            return False
        lo = 0 if c.get("min") is None else int(c["min"])
        hi = 255 if c.get("max") is None else int(c["max"])
        return lo <= speed <= hi
    if typ == "accel":
        lo = int(c.get("min", 0) or 0)
        hi = 255 if c.get("max") is None else int(c["max"])
        return lo <= accel <= hi
    if typ == "decel":
        lo = int(c.get("min", 0) or 0)
        hi = 255 if c.get("max") is None else int(c["max"])
        return lo <= -accel <= hi
    if typ == "var_cmp":
        p = _runner_payload() or {}
        def val(ref):
            if isinstance(ref, str):
                return _runner_var(p, ref)
            if isinstance(ref, int) and 0 <= ref < len(p.get("variables") or []):
                return _runner_var(p, (p["variables"][ref] or {}).get("name"))
            return 0
        a = val(c.get("var"))
        b = val(c.get("var2")) if c.get("var2") is not None else int(c.get("value", 0) or 0)
        op = int(c.get("op", 0) or 0)
        return {0: a == b, 1: a != b, 2: a < b, 3: a <= b,
                4: a > b, 5: a >= b}.get(op, False)
    if typ == "random":
        return True
    return False


def _runner_tick_channel(channel, p, press, release, speed, engine_on, accel, dt):
    states = p.get("states") or []
    if not (0 <= channel["state"] < len(states)):
        return
    st = states[channel["state"]]
    channel["elapsed"] += dt
    was_entered = bool(channel["entered"])
    channel["entered"] = False
    file = st.get("file") or ""
    loops = int(st.get("loops", 0) or 0)
    native_loop = bool(st.get("loop")) and loops == 0
    if not file:
        channel["done"] = True
    elif native_loop:
        channel["done"] = False
    elif not channel["done"]:
        dur = RUNNER["assets"].get(file, 1000)
        if channel["elapsed"] >= dur:
            if loops > 0 and channel["loops_done"] + 1 < loops:
                channel["loops_done"] += 1
                channel["elapsed"] = 0.0
            else:
                channel["done"] = True

    cands = []
    for tr in (p.get("transitions") or []):
        if tr.get("source") != st.get("id"):
            continue
        ok = _runner_cond(tr.get("condition"), channel, press, release,
                          speed, engine_on, accel, was_entered)
        if tr.get("timing") == "after_sample":
            if channel["done"]:
                if ok or channel["pending"] == tr.get("id"):
                    cands.append(tr)
            elif ok:
                # Endless engine loops have no sample end; waiting would freeze
                # the speed band. Only speed/motion edges switch immediately.
                ctype = (tr.get("condition") or {}).get("type")
                if channel.get("engine") and native_loop and ctype in ("speed", "accel", "decel"):
                    cands.append(tr)
                else:
                    prio = int(tr.get("priority", 0) or 0)
                    if not channel["pending"] or prio > channel["pending_prio"]:
                        channel["pending"] = tr.get("id")
                        channel["pending_prio"] = prio
        elif ok:
            cands.append(tr)
    if cands:
        channel["pending"] = ""
        channel["pending_prio"] = 0
        best = max(int(t.get("priority", 0) or 0) for t in cands)
        group = [t for t in cands if int(t.get("priority", 0) or 0) == best]
        rnd = [t for t in group if (t.get("condition") or {}).get("type") == "random"]
        pick = rnd[0] if rnd else group[0]
        _runner_enter(channel, pick.get("target"))
        return True
    if channel["done"]:
        channel["pending"] = ""
        channel["pending_prio"] = 0
        if st.get("loop") and (loops == 0 or channel["loops_done"] < loops):
            channel["elapsed"] = 0.0
            channel["done"] = False
            channel["entered"] = True
        else:
            channel["done"] = False
    return False


def _runner_states():
    p = _runner_payload()
    if not p:
        return [0]
    if not RUNNER["channels"]:
        _runner_reset()
    now = time.monotonic() * 1000.0
    last = RUNNER["last_ms"] or now
    dt = max(0.0, min(1000.0, now - last))
    RUNNER["last_ms"] = now
    fnmask = _function_mask()
    press = fnmask & ~RUNNER["previous_fn"]
    release = RUNNER["previous_fn"] & ~fnmask
    RUNNER["previous_fn"] = fnmask
    raw_speed = int(STATE["motor"].get("speed", 0) or 0)
    speed = int(round(raw_speed * 255.0 / 126.0))
    RUNNER["speed255"] = speed
    eng = p.get("engine") or {}
    eng_fn = int(eng.get("fn", 255) or 255)
    engine_on = speed > 0 or eng_fn > 28 or (0 <= eng_fn < FN_COUNT and bool(STATE["fn"][eng_fn]))
    RUNNER["engine_on"] = engine_on
    accel = speed - RUNNER["previous_speed"]
    RUNNER["previous_speed"] = speed
    for ch in RUNNER["channels"]:
        _runner_tick_channel(ch, p, press, release, speed, engine_on, accel, dt)
        # Settle immediate chains (engine idle -> off -> new speed band) in one
        # web poll instead of waiting for the next 700 ms request.
        for _ in range(3):
            if not _runner_tick_channel(ch, p, 0, 0, speed, engine_on, 0, 0.0):
                break
    out = [255] * (SG_MAX_EFFECTS + 1)
    for i, ch in enumerate(RUNNER["channels"]):
        if i < len(out):
            out[i] = ch["state"]
    return out



def add_log(tag, text):
    STATE["logseq"] += 1
    STATE["log"].append({"s": STATE["logseq"], "tag": tag, "text": text})
    if len(STATE["log"]) > 200:
        STATE["log"] = STATE["log"][-200:]


for _t, _x in [("Система", "предпросмотр запущен"), ("Режим", "DCC")]:
    add_log(_t, _x)


# --------------------------------------------------------------------------
# helpers
# --------------------------------------------------------------------------

def q1(query, key, default=None):
    vals = query.get(key)
    if not vals:
        return default
    return vals[0]


def as_int(value, default=0):
    try:
        return int(float(value))
    except (TypeError, ValueError):
        return default


def as_bool(value, default=False):
    if value is None:
        return default
    return str(value).lower() in ("1", "true", "yes", "on")


def ok(**kw):
    out = {"ok": True}
    out.update(kw)
    return out


def uptime_s():
    return int(time.time() - START)


def device_json():
    return ok(name=STATE["device_name"], version=VERSION, uptime=uptime_s())


def motor_json():
    m = STATE["motor"]
    return ok(speed=m["speed"], forward=m["forward"])


def functions_json():
    return ok(states=list(STATE["fn"]))


def _binding_active(b):
    """Apply the direction/motion gate used by the device for output bindings."""
    m = STATE["motor"]
    d = int(b.get("dir", 0) or 0)
    if d == 1 and not m["forward"]:
        return False
    if d == 2 and m["forward"]:
        return False
    st = int(b.get("state", 0) or 0)
    moving = int(m.get("speed", 0) or 0) > 0
    if st == 1 and not moving:
        return False
    if st == 2 and moving:
        return False
    return True


def _function_mask():
    mask = 0
    for fn, level in enumerate(STATE["fn"]):
        if level:
            mask |= 1 << fn
    return mask


def _output_mask():
    mask = 0
    for b in STATE["binds"]:
        if int(b.get("type", 0) or 0) != 1:
            continue
        fn = int(b.get("fn", -1))
        if 0 <= fn < FN_COUNT and STATE["fn"][fn] and _binding_active(b):
            ch = int(b.get("id", -1))
            if 0 <= ch <= 8:
                mask |= 1 << ch
    return mask


def _logic_mask():
    mask = 0
    for b in STATE["binds"]:
        if int(b.get("type", 0) or 0) != 4:
            continue
        fn = int(b.get("fn", -1))
        op = int(b.get("id", 0))
        if 0 <= fn < FN_COUNT and STATE["fn"][fn] and _binding_active(b) and 1 <= op <= 8:
            mask |= 1 << (op - 1)
    return mask


def audio_status_json():
    a = STATE["audio"]
    return ok(playing=a["playing"], active_slot=a["active_slot"],
              master_volume=a["master"], engine_volume=a["engine"],
              effects_volume=a["effects"])


def _bemf_start_config(query):
    name = str(q1(query, "mode", "auto") or "auto").lower()
    modes = {"auto": 0, "adaptive": 0, "fast": 1, "balanced": 2,
             "thorough": 3, "custom": 4}
    mode = modes.get(name, 0)
    default_total = {0: 16, 1: 10, 2: 12, 3: 16, 4: 16}.get(mode, 16)
    default_settle = {0: 600, 1: 400, 2: 500, 3: 600, 4: 500}.get(mode, 600)
    speeds = str(q1(query, "speeds", "") or "")
    if speeds:
        count = len([s for s in speeds.split(",") if s.strip()])
        if 10 <= count <= 16:
            mode = 4
            default_total = count
    total = as_int(q1(query, "points"), default_total)
    total = max(10, min(16, total))
    settle = as_int(q1(query, "settle"), default_settle)
    settle = max(100, min(2000, settle))
    apply_pid = as_bool(q1(query, "apply_pid"), False)
    return mode, total, settle, apply_pid


def bemf_json():
    b = STATE["bemf"]
    profile = dict(b.get("profile") or {})
    count = max(1, min(16, int(profile.get("measured") or 10)))
    points = []
    for i in range(count):
        speed = int(round(12 + (126 - 12) * i / (count - 1))) if count > 1 else 12
        frac = int(round(20 + 60 * i / (count - 1))) if count > 1 else 20
        points.append({"speed": speed, "frac": frac})
    return ok(active=b["active"], progress=b["progress"], total=b["total"],
              valid=b["valid"], stored=b["stored"], use=b["use"],
              runId=b["runId"], result=b["result"], error=b["error"],
              errorCode=b["errorCode"], points=points, profile=profile)


def tracks_json():
    return ok(cats=list(STATE["cats"]), tracks=[dict(t) for t in STATE["tracks"] if t["enabled"]])


def library_json():
    files = [{"name": n, "size": LIBRARY[n].get("size", 0), "vol": LIBRARY[n]["vol"]}
             for n in sorted(LIBRARY)]
    return ok(count=len(files), preview=PREVIEW["name"], files=files)


def fmap_json():
    return ok(map=[dict(m) for m in STATE["fmap"]])


def binds_json():
    return ok(count=len(STATE["binds"]), binds=[dict(b) for b in STATE["binds"]])


def aux_json():
    return ok(aux=[dict(a) for a in STATE["aux"]])


def wifi_json():
    w = STATE["wifi"]
    return ok(mode=w["mode"], ap_ssid=w["ap_ssid"], ap_password=w["ap_password"],
              auto_off_min=w["auto_off_min"], status=w["status"], ap_ip=w["ap_ip"])


def log_json(query):
    since = as_int(q1(query, "since", "0"), 0)
    entries = [e for e in STATE["log"] if e["s"] > since]
    return ok(entries=entries, seq=STATE["logseq"])


# --------------------------------------------------------------------------
# API dispatch
# --------------------------------------------------------------------------

def handle_api(method, path, query, body):
    s = STATE

    if path == "/api/device":
        if method == "POST":
            name = q1(query, "name")
            if name:
                s["device_name"] = name
                add_log("Имя", name)
            return 200, ok(name=s["device_name"])
        return 200, device_json()

    if path == "/api/control/source":
        if method == "POST":
            src = (q1(query, "source", "rails") or "rails").lower()
            s["control_source"] = "web" if src in ("web", "1") else "rails"
            add_log("Источник", "Веб" if s["control_source"] == "web" else "Рельсы")
        return 200, ok(source=s["control_source"])

    if path == "/api/mode":
        if method == "POST":
            mode = (q1(query, "mode", "dcc") or "dcc").lower()
            s["mode"] = "dc" if mode == "dc" else "dcc"
            s["motor"]["speed"] = 0
            add_log("Режим", "DC (аналог)" if s["mode"] == "dc" else "DCC")
        return 200, ok(mode=s["mode"])

    if path == "/api/motor":
        if method == "POST":
            if q1(query, "speed") is not None:
                s["motor"]["speed"] = max(0, min(126, as_int(q1(query, "speed"), 0)))
            if q1(query, "forward") is not None:
                s["motor"]["forward"] = as_bool(q1(query, "forward"), True)
        return 200, motor_json()

    if path == "/api/emergency":
        s["motor"]["speed"] = 0
        s["audio"]["playing"] = False
        s["audio"]["active_slot"] = 0
        s["fn"] = [0] * FN_COUNT
        add_log("Стоп", "ЭКСТРЕННЫЙ СТОП")
        return 200, ok()

    if path == "/api/functions":
        return 200, functions_json()

    if path == "/api/function":
        fn = as_int(q1(query, "fn"), -1)
        if 0 <= fn < FN_COUNT:
            s["fn"][fn] = 1 if as_bool(q1(query, "state"), False) else 0
            add_log("Функция", "F%d %s" % (fn, "вкл" if s["fn"][fn] else "выкл"))
            return 200, ok(fn=fn, state=bool(s["fn"][fn]))
        return 200, {"ok": False, "error": "fn out of range"}

    if path == "/api/audio/status":
        return 200, audio_status_json()

    if path == "/api/audio/volume":
        a = s["audio"]
        if q1(query, "master") is not None:
            a["master"] = max(0, min(100, as_int(q1(query, "master"), a["master"])))
        if q1(query, "engine") is not None:
            a["engine"] = max(0, min(100, as_int(q1(query, "engine"), a["engine"])))
        if q1(query, "effects") is not None:
            a["effects"] = max(0, min(100, as_int(q1(query, "effects"), a["effects"])))
        return 200, ok()

    if path == "/api/audio/tracks":
        return 200, tracks_json()

    if path == "/api/audio/play":
        name = q1(query, "name")
        if name:
            base = _base(name)
            PREVIEW["name"] = base
            s["audio"]["playing"] = True
            s["audio"]["active_slot"] = 0
            add_log("Звук", "плей: %s" % base)
        else:
            slot = as_int(q1(query, "slot"), 0)
            PREVIEW["name"] = ""
            s["audio"]["playing"] = True
            s["audio"]["active_slot"] = slot
            add_log("Звук", "слот %d" % slot)
        return 200, ok()

    if path == "/api/audio/stop":
        s["audio"]["playing"] = False
        s["audio"]["active_slot"] = 0
        PREVIEW["name"] = ""
        add_log("Звук", "стоп (все)")
        return 200, ok()

    if path == "/api/audio/library":
        return 200, library_json()

    if path == "/api/audio/library/volume":
        name = _base(q1(query, "name", ""))
        vol = max(0, min(100, as_int(q1(query, "vol"), 100)))
        if name in LIBRARY:
            LIBRARY[name]["vol"] = vol
        return 200, ok()

    if path == "/api/audio/library/delete":
        name = _base(q1(query, "name", ""))
        if not name:
            return 400, {"ok": False, "error": "bad name"}
        LIBRARY.pop(name, None)
        if PREVIEW.get("name") == name:
            PREVIEW["name"] = ""
        add_log("Звук", "удалён %s.wav" % name)
        return 200, ok(name=name)

    if path == "/api/audio/pack":
        try:
            files, size, encoding, scheme_id = _load_pack(body)
        except ValueError as exc:
            add_log("Звук", "пакет отклонён: %s" % exc)
            return 200, {"ok": False, "error": str(exc)}
        add_log("Звук", "пакет: %d звук(ов)%s, %d Б (%s)" %
                (files, " + схема " + scheme_id if scheme_id else "", size, encoding))
        res = ok(files=files, bytes=size, encoding=encoding)
        if scheme_id:
            res["scheme"] = scheme_id
            res["revision"] = GRAPH["revision"]
        return 200, res

    if path == "/api/audio/delete":
        slot = as_int(q1(query, "slot"), 0)
        for t in s["tracks"]:
            if t["slot"] == slot:
                t["enabled"] = False
                t["file"] = ""
                t["label"] = ""
        add_log("Звук", "слот %d удалён" % slot)
        return 200, ok(slot=slot)

    if path == "/api/audio/upload":
        if as_bool(q1(query, "asset"), False):
            hdr_name = unquote(q1(query, "name", ""))
            base = _sanitize(_base(hdr_name))
            if not base:
                return 400, {"ok": False, "error": "bad name"}
            LIBRARY[base] = {"vol": 100, "wav": body, "size": len(body),
                             "raw": body[44:] if len(body) > 44 else body,
                             "rate": 22050, "channels": 1, "bits": 16}
            add_log("Звук", "загружен %s.wav (%d байт)" % (base, len(body)))
            return 200, ok(file="audio/%s.wav" % base, label=base,
                           enabled=True, bytes=len(body),
                           recovery_cleanup_pending=False, recovery="")
        slot = as_int(q1(query, "slot"), 0)
        if slot <= 0:
            used = {t["slot"] for t in s["tracks"] if t["enabled"]}
            slot = next((i for i in range(1, TRACK_COUNT + 1) if i not in used), 0)
        label = "Загружено %d" % slot
        for t in s["tracks"]:
            if t["slot"] == slot:
                t["enabled"] = True
                t["file"] = "audio/slot%02d.wav" % slot
                t["label"] = label
        add_log("Звук", "загружен слот %d (%d байт)" % (slot, len(body)))
        return 200, ok(slot=slot, file="audio/slot%02d.wav" % slot, label=label,
                       enabled=True, bytes=len(body),
                       recovery_cleanup_pending=False, recovery="")

    if path == "/api/track/category":
        slot = as_int(q1(query, "slot"), 0)
        cat = as_int(q1(query, "cat"), 0)
        if 1 <= slot <= TRACK_COUNT:
            s["cats"][slot - 1] = cat
        add_log("Звук", "слот %d категория %d" % (slot, cat))
        return 200, ok(slot=slot, cat=cat)

    if path == "/api/cv/all":
        return 200, ok(count=CV_COUNT, values=list(s["cv"]))

    if path == "/api/cv/write":
        idx = as_int(q1(query, "index"), 0)
        val = max(0, min(255, as_int(q1(query, "value"), 0)))
        if 1 <= idx <= CV_COUNT:
            s["cv"][idx - 1] = val
        add_log("CV", "CV %d = %d" % (idx, val))
        return 200, ok(index=idx, value=val)

    if path == "/api/func-map":
        if q1(query, "view") in ("bind", "matrix"):
            return 200, binds_json()
        if method == "POST":
            if as_bool(q1(query, "bind"), False):
                if as_bool(q1(query, "remove"), False):
                    idx = as_int(q1(query, "idx"), -1)
                    if 0 <= idx < len(s["binds"]):
                        s["binds"].pop(idx)
                    return 200, ok()
                s["binds"].append({
                    "fn": as_int(q1(query, "fn"), 0), "type": as_int(q1(query, "type"), 2),
                    "id": as_int(q1(query, "id"), 0), "dir": as_int(q1(query, "dir"), 0),
                    "state": as_int(q1(query, "state"), 2), "mode": as_int(q1(query, "mode"), 0),
                    "flags": as_int(q1(query, "flags"), 0), "short": as_int(q1(query, "short"), 0),
                    "short_ms": as_int(q1(query, "short_ms"), 400), "min_ms": as_int(q1(query, "min_ms"), 150),
                    "fade_ms": as_int(q1(query, "fade_ms"), 80),
                })
                add_log("Функция", "привязка добавлена")
                return 200, ok(count=len(s["binds"]))
            fn = as_int(q1(query, "fn"), -1)
            if 0 <= fn < FN_COUNT:
                s["fmap"][fn] = {
                    "a": as_int(q1(query, "a"), 0), "b": as_int(q1(query, "b"), 0),
                    "aux": as_int(q1(query, "aux"), 0), "dir": as_int(q1(query, "dir"), 0),
                    "speed": as_int(q1(query, "speed"), 0),
                }
                add_log("Функция", "F%d карта обновлена" % fn)
                return 200, ok(fn=fn)
            return 200, {"ok": False, "error": "save failed"}
        return 200, fmap_json()

    if path == "/api/aux/cfg":
        if method == "POST":
            ch = as_int(q1(query, "ch"), 0)
            if 0 <= ch < len(s["aux"]):
                s["aux"][ch]["level"] = as_int(q1(query, "level"), s["aux"][ch]["level"])
                s["aux"][ch]["effect"] = as_int(q1(query, "effect"), s["aux"][ch]["effect"])
            add_log("AUX", "%s обновлён" % (AUX_NAMES[ch] if 0 <= ch < 9 else ch))
            return 200, ok(ch=ch)
        return 200, aux_json()

    if path == "/api/wifi":
        if method == "POST":
            w = s["wifi"]
            if q1(query, "ap_ssid") is not None:
                w["ap_ssid"] = q1(query, "ap_ssid")
            if q1(query, "ap_ip") is not None:
                w["ap_ip"] = q1(query, "ap_ip")
            add_log("Wi-Fi", "настройки сохранены (IP %s), перезагрузка" % w["ap_ip"])
            return 200, ok(reboot=1)
        return 200, wifi_json()

    if path in ("/api/wifi/reset", "/api/reset"):
        add_log("Сброс", "настройки сброшены, перезагрузка")
        return 200, ok(reboot=1)

    if path == "/api/storage":
        return 200, ok(free=6 * 1024 * 1024)

    if path == "/api/bemf/cal":
        b = s["bemf"]
        if b["active"] and b["progress"] < b["total"]:
            b["progress"] += 1
            if b["progress"] >= b["total"]:
                b["active"] = False
                b["result"] = "succeeded"
                b["stored"] = True
                b["valid"] = True
                p = b.setdefault("profile", {})
                p["measured"] = b["total"]
                p["stalled"] = 0
                p["ceiling"] = 0
                p["first"] = 12
                p["firstFrac"] = 200
                p["gain"] = 2400
                p["kp"] = 42
                p["ki"] = 21
                p["kd"] = 5
                p["quality"] = 88
                p["pidApplied"] = bool(b.get("apply_pid"))
                add_log("BEMF", "калибровка завершена и сохранена")
        return 200, bemf_json()

    if path == "/api/bemf/calibrate":
        b = s["bemf"]
        if as_bool(q1(query, "cancel"), False):
            b["active"] = False
            b["result"] = "cancelled"
            p = b.setdefault("profile", {})
            p["measured"] = b["progress"]
            p["pidApplied"] = False
            add_log("BEMF", "калибровка отменена")
        elif as_bool(q1(query, "reset"), False):
            b["active"] = False
            b["stored"] = False
            b["valid"] = False
            b["result"] = "idle"
            b["total"] = 10
            b["apply_pid"] = False
            b["profile"] = {"mode": 0, "planned": 0, "measured": 0,
                            "stalled": 0, "ceiling": 0, "first": 0,
                            "firstFrac": 0, "settle": 0, "gain": 0,
                            "kp": 0, "ki": 0, "kd": 0, "quality": 0,
                            "pidApplied": False}
            add_log("BEMF", "калибровка очищена")
        elif as_bool(q1(query, "start"), False):
            mode, total, settle, apply_pid = _bemf_start_config(query)
            b.update(active=True, progress=0, total=total, result="running",
                     error="none", errorCode=0, runId=b["runId"] + 1,
                     apply_pid=apply_pid)
            b["profile"] = {"mode": mode, "planned": total, "measured": 0,
                            "stalled": 0, "ceiling": 0, "first": 0,
                            "firstFrac": 0, "settle": settle, "gain": 0,
                            "kp": 0, "ki": 0, "kd": 0, "quality": 0,
                            "pidApplied": False}
            add_log("BEMF", "калибровка запущена")
        return 200, ok()

    if path == "/api/bemf/use":
        if method == "POST":
            s["bemf"]["use"] = as_bool(q1(query, "enabled"), False)
            add_log("BEMF", "регулятор %s" % ("включён" if s["bemf"]["use"] else "выключен, только ШИМ"))
        return 200, ok(enabled=s["bemf"]["use"])

    if path == "/api/bemf/base":
        return 200, ok(start=20, full=95,
                       points=[{"speed": i * 12, "frac": 20 + i * 7} for i in range(1, 11)])

    if path == "/api/sound/graph/capabilities":
        return 200, ok(format="sound-graph", schemaVersion=1, limits=dict(GRAPH_LIMITS),
                       conditions=list(CONDITION_TYPES), variableKinds=list(VARIABLE_KINDS),
                       compareOps=list(COMPARE_OPS))

    if path == "/api/sound/graph/projects":
        return 200, ok(projects=[{"id": pid, "name": ent["project"].get("name", pid),
                                  "revision": ent["revision"]}
                                 for pid, ent in PROJECTS.items()])

    if path == "/api/sound/graph/project":
        pid = q1(query, "id", "")
        ent = PROJECTS.get(pid) if pid else None
        if ent is None:
            ent = {"project": GRAPH["project"], "revision": GRAPH["revision"]}
        return 200, ok(project=ent["project"], revision=ent["revision"])

    if path == "/api/sound/graph/reference":
        GRAPH["active"] = False
        GRAPH["fault"] = False
        _runner_reset()
        _save_persisted()
        add_log("Граф", "выбран эталонный проект")
        return 200, ok()

    if path == "/api/sound/graph/delete":
        pid = q1(query, "id", "")
        if pid not in PROJECTS:
            return 200, {"ok": False, "error": "project not found"}
        was_active = GRAPH["project"].get("id") == pid
        del PROJECTS[pid]
        if was_active:
            if PROJECTS:
                nid = next(iter(PROJECTS))
                GRAPH["project"] = PROJECTS[nid]["project"]
                GRAPH["revision"] = PROJECTS[nid]["revision"]
                GRAPH["active"] = True
            else:
                GRAPH["project"] = _new_graph()
                GRAPH["revision"] = 0
                GRAPH["active"] = False
            GRAPH["fault"] = False
            _runner_reset()
        _save_persisted()
        return 200, ok()

    if path == "/api/sound/graph/state":
        m = s["motor"]
        active = bool(GRAPH["active"])
        return 200, ok(active=active, fault=GRAPH["fault"],
                       id=GRAPH["project"]["id"] if active else "",
                       projectId=GRAPH["project"]["id"] if active else "",
                       revision=GRAPH["revision"] if active else 0,
                       engine=False, armed=True, states=_runner_states() if active else [0, 0, 0, 0, 0],
                       failedChannels=0, speed=m["speed"],
                       functions=_function_mask(), outputs=_output_mask(),
                       logic=_logic_mask())

    if path == "/api/sound/graph/asset":
        return 200, ok(asset=_asset(q1(query, "file", "sample.wav")))

    if path == "/api/sound/graph/validate":
        return 200, ok(valid=True, diagnostics=[])

    if path == "/api/sound/graph/save":
        try:
            incoming = json.loads(body.decode("utf-8")) if body else {}
        except (ValueError, UnicodeDecodeError):
            return 200, {"ok": False, "error": "invalid json"}
        if not isinstance(incoming, dict):
            return 200, {"ok": False, "error": "invalid project"}
        pid = q1(query, "id", incoming.get("id", ""))
        expected = as_int(q1(query, "expectedRevision"), 0)
        existing = PROJECTS.get(pid)
        current = existing["revision"] if existing else 0
        if expected != current:
            return 200, {"ok": False, "error": "Stale expectedRevision"}
        incoming["id"] = pid
        # The editor sends the compiled schema-v1 payload; keep the v2
        # authoring layer so preview load/save round-trips the library.
        if not incoming.get("effectTables"):
            src = existing["project"] if existing else None
            if src:
                for key in ("effectTables", "sources", "blocks", "sinks", "wires"):
                    if key in src:
                        incoming[key] = src[key]
        revision = current + 1
        PROJECTS[pid] = {"project": incoming, "revision": revision}
        GRAPH["project"] = incoming
        GRAPH["revision"] = revision
        GRAPH["active"] = True
        GRAPH["fault"] = False
        _runner_reset()
        _save_persisted()
        add_log("Граф", "сохранена версия %d" % revision)
        return 200, ok(revision=revision)

    if path == "/api/sound/graph/apply":
        pid = q1(query, "id", "")
        ent = PROJECTS.get(pid) if pid else None
        if ent:
            GRAPH["project"] = ent["project"]
            GRAPH["revision"] = ent["revision"]
        GRAPH["active"] = True
        GRAPH["fault"] = False
        _runner_reset()
        _save_persisted()
        add_log("Граф", "применён проект %s" % GRAPH["project"].get("id", ""))
        return 200, ok()

    if path == "/api/ota/update":
        add_log("OTA", "прошивка записана, перезагрузка")
        return 200, ok(bytes=len(body), files=1, recovery_cleanup_pending=False, recovery="")

    if path == "/api/log":
        return 200, log_json(query)

    if path == "/api/sound-files":
        files = set(SOUND_ALIASES.keys())
        files |= set(n + ".wav" for n in LIBRARY)
        return 200, ok(files=sorted(files))

    if path == "/api/clientlog":
        sys.stderr.write("[client] %s\n" % (q1(query, "m", "") or ""))
        return 200, ok()

    if path == "/api/time":
        return 200, ok(now=int(time.time()))

    return 200, {"ok": False, "error": "preview: endpoint not implemented: %s" % path}


# --------------------------------------------------------------------------
# HTTP server
# --------------------------------------------------------------------------

CONTENT_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    ".txt": "text/plain; charset=utf-8",
    ".gz": "application/gzip",
    ".svg": "image/svg+xml",
    ".png": "image/png",
    ".wav": "audio/wav",
}


class Handler(BaseHTTPRequestHandler):
    server_version = "AuraPreview/1.0"
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    # -- low level ---------------------------------------------------------
    def _send(self, status, body, ctype="application/json; charset=utf-8", extra=None):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Access-Control-Allow-Origin", "*")
        if extra:
            for key, value in extra:
                self.send_header(key, value)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass

    def _json(self, status, obj):
        self._send(status, json.dumps(obj, ensure_ascii=False))

    def _serve_file(self, path):
        if not os.path.isfile(path):
            return self._json(404, {"ok": False, "error": "not found"})
        ext = os.path.splitext(path)[1].lower()
        with open(path, "rb") as fh:
            data = fh.read()
        ctype = CONTENT_TYPES.get(ext, "application/octet-stream")
        self._send(200, data, ctype)

    def _serve_ui(self):
        try:
            with open(UI_FILE, "r", encoding="utf-8") as fh:
                html = fh.read()
        except OSError as exc:
            return self._send(500, "web_ui.html not found: %s" % exc, "text/plain; charset=utf-8")
        html = html.replace("__VERSION__", VERSION)
        self._send(200, html, "text/html; charset=utf-8")

    # -- routing -----------------------------------------------------------
    def _route(self, method):
        parsed = urlparse(self.path)
        path = parsed.path
        query = parse_qs(parsed.query, keep_blank_values=True)

        if path == "/favicon.ico":
            return self._send(204, b"", "image/x-icon")
        if path in ("/", "/index.html"):
            return self._serve_ui()
        if path in ("/sound-editor", "/sound-editor/"):
            return self._serve_file(os.path.join(EDITOR_DIR, "blocks.html"))
        if path.startswith("/sound-editor/"):
            rel = unquote(path[len("/sound-editor/"):])
            safe = os.path.normpath(os.path.join(EDITOR_DIR, rel))
            if safe.startswith(EDITOR_DIR):
                return self._serve_file(safe)
            return self._json(404, {"ok": False, "error": "not found"})

        if path.startswith("/sound-files/"):
            rel = unquote(path[len("/sound-files/"):])
            base = _base(rel)
            if base in LIBRARY:
                entry = LIBRARY[base]
                if "wav" in entry:
                    return self._send(200, entry["wav"], "audio/wav")
                if "path" in entry and os.path.isfile(entry["path"]):
                    return self._serve_file(entry["path"])
            safe = os.path.normpath(os.path.join(SOUND_DIR, rel))
            if safe.startswith(SOUND_DIR):
                if os.path.isfile(safe):
                    return self._serve_file(safe)
                alias = SOUND_ALIASES.get(rel) or SOUND_ALIASES.get(base + ".wav")
                if alias:
                    return self._serve_file(os.path.join(SOUND_DIR, alias))
            return self._json(404, {"ok": False, "error": "not found"})

        if path.startswith("/api/audio/raw/"):
            rel = unquote(path[len("/api/audio/raw/"):])
            base = _base(rel)
            if base in LIBRARY:
                e = LIBRARY[base]
                if "raw" in e or "wav" in e:
                    raw = e.get("raw", e["wav"][44:] if len(e["wav"]) > 44 else e["wav"])
                    return self._send(200, raw, "application/octet-stream",
                                      extra=[("X-Audio-Rate", str(e.get("rate", 22050))),
                                             ("X-Audio-Channels", str(e.get("channels", 1))),
                                             ("X-Audio-Bits", str(e.get("bits", 16)))])
                if "path" in e and os.path.isfile(e["path"]):
                    try:
                        with open(e["path"], "rb") as fh:
                            wav_bytes = fh.read()
                        ch = struct.unpack_from("<H", wav_bytes, 22)[0] if len(wav_bytes) >= 44 else 1
                        rate = struct.unpack_from("<I", wav_bytes, 24)[0] if len(wav_bytes) >= 44 else 22050
                        bits = struct.unpack_from("<H", wav_bytes, 34)[0] if len(wav_bytes) >= 44 else 16
                        raw = wav_bytes[44:] if len(wav_bytes) > 44 else wav_bytes
                        return self._send(200, raw, "application/octet-stream",
                                          extra=[("X-Audio-Rate", str(rate)),
                                                 ("X-Audio-Channels", str(ch)),
                                                 ("X-Audio-Bits", str(bits))])
                    except OSError:
                        pass
            return self._json(404, {"ok": False, "error": "not found"})

        if path.startswith("/api/") or path == "/api":
            length = as_int(self.headers.get("Content-Length"), 0)
            body = self.rfile.read(length) if length > 0 else b""
            hdr_fname = self.headers.get("X-File-Name")
            if hdr_fname and "name" not in query:
                query = dict(query)
                query["name"] = [hdr_fname]
            try:
                status, payload = handle_api(method, path, query, body)
            except Exception as exc:  # keep the preview alive on bad input
                status, payload = 500, {"ok": False, "error": "preview error: %s" % exc}
            if isinstance(payload, (bytes, bytearray)):
                return self._send(status, bytes(payload), "application/octet-stream")
            return self._json(status, payload)

        return self._json(404, {"ok": False, "error": "not found"})

    def do_GET(self):
        self._route("GET")

    def do_HEAD(self):
        self._route("GET")

    def do_POST(self):
        self._route("POST")

    def do_OPTIONS(self):
        self._send(204, b"", "text/plain; charset=utf-8",
                   extra=[("Access-Control-Allow-Methods", "GET, POST, OPTIONS"),
                          ("Access-Control-Allow-Headers", "*")])


def main():
    port = as_int(os.environ.get("PREVIEW_PORT"), 0)
    if len(sys.argv) > 1:
        port = as_int(sys.argv[1], port)
    if port <= 0:
        port = 8080
    host = os.environ.get("PREVIEW_HOST", "127.0.0.1")
    _load_persisted(port)
    server = ThreadingHTTPServer((host, port), Handler)
    url = "http://%s:%d/" % (host, port)
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
    print("AURA-X preview server")
    print("  UI:     %s" % url)
    print("  source: %s" % UI_FILE)
    print("  stop:   Ctrl+C")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
