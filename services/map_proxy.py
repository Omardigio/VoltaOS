from flask import Flask, Response, jsonify, request, send_from_directory
import requests
import time
import math
import io
import colorsys
import urllib.parse
import os
import json
import copy
import hashlib
import threading
import collections
from concurrent.futures import ThreadPoolExecutor, wait as _cf_wait
from pathlib import Path
from PIL import Image

_DEBUG = bool(os.environ.get("VOLTA_DEBUG"))   # diagnostic log (VOLTA_DEBUG=1)
app = Flask(__name__, static_folder=None)   # /static is served by the route below, from assets/static
from local_security import protect
protect(app)

# --- API KEYS (from keys.env, see keys.env.example) ---
from keys import key
TOKEN = key("MAPBOX_TOKEN")
MAPTILER_KEY = key("MAPTILER_KEY")
OWM_KEY = key("OWM_KEY")
TOMTOM_KEY = key("TOMTOM_KEY")

# ═══ RETE VELOCE: sessione condivisa + cache (RAM + disco) ═══════════════
# Before, EVERY request (vector tile, font glyphs, sprite, style) opened
# a new TLS connection to Mapbox/MapTiler/Google and saved nothing:
# hence the slowness in loading the tiles. Now:
#  - a single Session with reused connections (keep-alive), 64 in parallel;
#  - disk cache (tile_cache/<type>/) + small RAM cache: what you have already
#    seen opens instantly, even offline;
#  - a single in-flight request for the same tile (no duplicates);
#  - if the network drops the old copy is served instead of an error.
_http = requests.Session()
_adapter = requests.adapters.HTTPAdapter(pool_connections=16, pool_maxsize=64, max_retries=1)
_http.mount("https://", _adapter)
_http.mount("http://", _adapter)
_http.headers.update({"User-Agent": "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36"})

_PROJECT = Path(__file__).resolve().parent.parent   # project root
_CACHE_ROOT = _PROJECT / 'data' / 'tile_cache'
_mem = collections.OrderedDict()
_MEM_MAX = 500
_mem_lock = threading.Lock()
_inflight = {}
_inflight_lock = threading.Lock()
_writes = collections.Counter()
_MAX_FILES = {'sat': 20000, 'sat_maptiler': 20000, 'vec': 40000, 'fonts': 4000, 'sprite': 200, 'tj': 100, 'style': 100}

def _mem_get(k):
    with _mem_lock:
        v = _mem.get(k)
        if v is not None:
            _mem.move_to_end(k)
        return v

def _mem_put(k, v):
    with _mem_lock:
        _mem[k] = v
        _mem.move_to_end(k)
        while len(_mem) > _MEM_MAX:
            _mem.popitem(last=False)

def _prune_dir(d, max_files):
    try:
        entries = list(os.scandir(d))
    except OSError:
        return
    if len(entries) <= max_files:
        return
    entries.sort(key=lambda e: e.stat().st_mtime)
    for e in entries[:len(entries) - max_files]:
        try: os.remove(e.path)
        except OSError: pass

def _cache_path(kind, name):
    d = _CACHE_ROOT / kind
    d.mkdir(parents=True, exist_ok=True)
    return d / name

def _fetch_cached(kind, name, url, ttl=None, headers=None, timeout=(3, 8), allow_empty=False):
    """(bytes|None, status). RAM cache -> disk -> network. ttl=None: forever."""
    k = (kind, name)
    v = _mem_get(k)
    if v is not None:
        return v, 200
    path = _cache_path(kind, name)
    stale = None
    try:
        st = path.stat()
        b = path.read_bytes()
        if ttl is None or (time.time() - st.st_mtime) < ttl:
            _mem_put(k, b)
            return b, 200
        stale = b
    except OSError:
        pass
    with _inflight_lock:
        ev = _inflight.get(k)
        leader = ev is None
        if leader:
            ev = _inflight[k] = threading.Event()
    if not leader:                      # someone else is already downloading it
        ev.wait(15)
        try:
            return path.read_bytes(), 200
        except OSError:
            return stale, (200 if stale is not None else 504)
    try:
        resp = None
        for _ in range(2):
            try:
                resp = _http.get(url, headers=headers, timeout=timeout)
            except requests.RequestException:
                resp = None
                continue
            if resp.status_code == 200 or resp.status_code in (400, 401, 403, 404):
                break
        if resp is not None and resp.status_code == 200 and (resp.content or allow_empty):
            try:
                tmp = path.with_name(path.name + ".tmp%d" % threading.get_ident())
                tmp.write_bytes(resp.content)
                os.replace(tmp, path)
                _writes[kind] += 1
                if _writes[kind] % 300 == 0:
                    threading.Thread(target=_prune_dir, args=(path.parent, _MAX_FILES.get(kind, 20000)), daemon=True).start()
            except OSError as e:
                print(f"[map_proxy] cache {kind} not saved: {e}", flush=True)
            _mem_put(k, resp.content)
            return resp.content, 200
        if stale is not None:           # network absent: better the old copy
            return stale, 200
        return None, (resp.status_code if resp is not None else 504)
    finally:
        with _inflight_lock:
            _inflight.pop(k, None)
        ev.set()

def _md5(s):
    return hashlib.md5(s.encode("utf-8")).hexdigest()

_CACHE_HDR = "public, max-age=86400"

def _haversine_km(lat, lon, plat, plon):
    R = 6371.0
    dlat = math.radians(plat - lat)
    dlon = math.radians(plon - lon)
    a = (math.sin(dlat / 2) ** 2 +
         math.cos(math.radians(lat)) * math.cos(math.radians(plat)) * math.sin(dlon / 2) ** 2)
    return R * 2 * math.atan2(math.sqrt(a), math.sqrt(1 - a))

# Cache reverse geocoding
_geocode_cache = {}
_GEOCODE_CACHE_TTL = 15

# Cache meteo
_weather_tile_cache = {}
_WEATHER_CACHE_TTL = 300

# ── EV-style palette for the Weather Radar
_VOLTA_STOPS = [
    (0.00, (26, 173, 26)),    # verde
    (0.30, (185, 230, 57)),   # verde-giallo
    (0.50, (255, 255, 0)),    # giallo
    (0.65, (255, 200, 0)),    # giallo-arancio
    (0.80, (255, 140, 0)),    # arancione
    (0.92, (255, 50, 0)),     # arancio-rosso
    (1.00, (139, 0, 0)),      # rosso scuro (severa)
]

def _volta_color_at(pos):
    pos = max(0.0, min(1.0, pos))
    for i in range(len(_VOLTA_STOPS) - 1):
        p0, c0 = _VOLTA_STOPS[i]
        p1, c1 = _VOLTA_STOPS[i + 1]
        if p0 <= pos <= p1:
            t = (pos - p0) / (p1 - p0) if p1 > p0 else 0
            return tuple(int(c0[k] + (c1[k] - c0[k]) * t) for k in range(3))
    return _VOLTA_STOPS[-1][1]

_DARKNESS_MIN = 0.05
_DARKNESS_MAX = 0.75

def _rescale_darkness(raw_darkness):
    if _DARKNESS_MAX <= _DARKNESS_MIN:
        return raw_darkness
    return max(0.0, min(1.0, (raw_darkness - _DARKNESS_MIN) / (_DARKNESS_MAX - _DARKNESS_MIN)))

_DARKNESS_LUT = [_volta_color_at(_rescale_darkness(i / 255.0)) for i in range(256)]

def _recolor_weather_tile(png_bytes):
    img = Image.open(io.BytesIO(png_bytes)).convert("RGBA")
    pixels = img.load()
    w, h = img.size
    for y in range(h):
        for x in range(w):
            r, g, b, a = pixels[x, y]
            if a == 0:
                continue
            hue, light, sat = colorsys.rgb_to_hls(r / 255.0, g / 255.0, b / 255.0)
            darkness_idx = min(255, int((1.0 - light) * 255))
            nr, ng, nb = _DARKNESS_LUT[darkness_idx]
            boosted_a = min(255, max(190, int(a * 1.4)))
            pixels[x, y] = (nr, ng, nb, boosted_a)
    out = io.BytesIO()
    img.save(out, format="PNG")
    return out.getvalue()

# ── WEATHER RADAR: RainViewer (free, no key) recolored in EV style ──
# RainViewer only serves the "Universal Blue" scheme and up to zoom 7 (since 2026).
# Here the tile is converted back to dBZ (the color table is fixed),
# smoothly enlarged up to zoom 10 and recolored with the
# green -> yellow -> orange -> red scale as on an EV display.
import numpy as np

_RV_MAX_Z = 7
_RV_OUT_MAXZ = 10
_RV_META = {"t": 0.0, "host": None, "path": None}
_rv_raw_cache = {}
_rv_png_cache = {}

# Universal Blue (RainViewer): dBZ -> RGB, opaque rows 15..65 (official table)
_UB_HEX = {
 15:"88ddee",16:"6cd1eb",17:"51c5e8",18:"36bae5",19:"1baee2",20:"00a3e0",21:"009ad5",22:"0091ca",
 23:"0088bf",24:"007fb4",25:"0077aa",26:"0070a3",27:"00699c",28:"006295",29:"005b8e",30:"005588",
 31:"005180",32:"004e78",33:"004a70",34:"004768",35:"ffee00",36:"ffe000",37:"ffd200",38:"ffc500",
 39:"ffb700",40:"ffaa00",41:"ff9f00",42:"ff9500",43:"ff8b00",44:"ff8100",45:"ff4400",46:"f23600",
 47:"e62800",48:"d91b00",49:"cd0d00",50:"c10000",51:"a80000",52:"8f0000",53:"760000",54:"5d0000",
 55:"ffaaff",56:"ff9fff",57:"ff95ff",58:"ff8bff",59:"ff81ff",60:"ff77ff",61:"ff6cff",62:"ff62ff",
 63:"ff58ff",64:"ff4eff",65:"ffffff"}
_UB_DBZ = np.array(sorted(_UB_HEX), dtype=np.float32)
_UB_RGB = np.array([[int(_UB_HEX[d][i:i+2], 16) for i in (0, 2, 4)] for d in sorted(_UB_HEX)], dtype=np.int32)

# Output scale: (dBZ, R, G, B, alpha)
_RADAR_STOPS = np.array([
    ( 5, 120, 225, 120,   0),
    (10, 120, 225, 120, 105),
    (20,  70, 205,  80, 165),
    (30,  35, 165,  55, 195),
    (35, 255, 235,   0, 210),
    (40, 255, 165,   0, 220),
    (45, 255,  75,   0, 228),
    (50, 225,   0,   0, 234),
    (58, 150,   0,  25, 240),
    (65, 205,   0, 160, 245),
], dtype=np.float32)

def _rv_frame():
    """(host, path) dell'ultimo fotogramma radar; cache 2 minuti."""
    now = time.time()
    if _RV_META["host"] and now - _RV_META["t"] < 120:
        return _RV_META["host"], _RV_META["path"]
    try:
        r = _http.get("https://api.rainviewer.com/public/weather-maps.json", timeout=(3, 6))
        if r.status_code == 200:
            j = r.json()
            past = (j.get("radar") or {}).get("past") or []
            if past:
                _RV_META.update(t=now, host=j.get("host", "https://tilecache.rainviewer.com"), path=past[-1]["path"])
                return _RV_META["host"], _RV_META["path"]
    except Exception as e:
        print("[map_proxy] RainViewer meta err:", repr(e), flush=True)
    return _RV_META["host"], _RV_META["path"]

def _decode_ub(png_bytes):
    """Universal Blue 256x256 tile -> dBZ matrix (float32; -10 = no echo)."""
    im = np.array(Image.open(io.BytesIO(png_bytes)).convert("RGBA"), dtype=np.int32)
    a = im[..., 3]
    dbz = np.full(a.shape, -10.0, dtype=np.float32)
    # eco debole: rampa beige semitrasparente (dBZ 0..14 ~ alpha 73..160)
    weak = (a > 0) & (a < 250)
    dbz[weak] = np.clip((a[weak] - 73) / 87.0 * 14.0, 0, 14)
    # strong echo: full color -> dBZ of the nearest row in the table
    strong = a >= 250
    if strong.any():
        rgb = im[..., :3][strong]
        packed = rgb[:, 0] * 65536 + rgb[:, 1] * 256 + rgb[:, 2]
        uniq, inv = np.unique(packed, return_inverse=True)
        u = np.stack([(uniq >> 16) & 255, (uniq >> 8) & 255, uniq & 255], axis=1)
        d2 = ((u[:, None, :] - _UB_RGB[None, :, :]) ** 2).sum(axis=2)
        dbz[strong] = _UB_DBZ[d2.argmin(axis=1)][inv]
    return dbz

def _colorize_dbz(dbz):
    out = np.zeros(dbz.shape + (4,), dtype=np.uint8)
    for ch in range(4):
        out[..., ch] = np.interp(dbz, _RADAR_STOPS[:, 0], _RADAR_STOPS[:, ch + 1]).astype(np.uint8)
    return out

def _rv_tile_png(z, x, y):
    host, path = _rv_frame()
    if not host: return None
    zp = min(z, _RV_MAX_Z)
    k = z - zp
    xp, yp = x >> k, y >> k
    key = (path, zp, xp, yp)
    dbz = _rv_raw_cache.get(key)
    if dbz is None:
        url = f"{host}{path}/256/{zp}/{xp}/{yp}/2/1_0.png"
        r = _http.get(url, timeout=(3, 8))
        if r.status_code != 200 or not r.headers.get("content-type", "").startswith("image"):
            return None
        dbz = _decode_ub(r.content)
        if len(_rv_raw_cache) > 200: _rv_raw_cache.clear()
        _rv_raw_cache[key] = dbz
    if k > 0:                       # zoom beyond 7: crop + smooth enlargement
        n = 256 >> k
        ox, oy = (x - (xp << k)) * n, (y - (yp << k)) * n
        # 1 px margin to avoid hard edges between neighboring tiles
        sub = dbz[oy:oy + n, ox:ox + n]
        im = Image.fromarray(sub, mode="F").resize((256, 256), Image.BICUBIC)
        dbz = np.asarray(im, dtype=np.float32)
    else:
        im = Image.fromarray(dbz, mode="F").resize((512, 512), Image.BICUBIC).resize((256, 256), Image.BILINEAR)
        dbz = np.asarray(im, dtype=np.float32)
    rgba = _colorize_dbz(dbz)
    buf = io.BytesIO()
    Image.fromarray(rgba, "RGBA").save(buf, format="PNG")
    return buf.getvalue()

def _owm_weather_tile(z, x, y):
    url = f"https://tile.openweathermap.org/map/precipitation_new/{z}/{x}/{y}.png?appid={OWM_KEY}"
    resp = _http.get(url, timeout=(3, 8))     # before, without timeout: a slow server blocked the thread
    if resp.status_code != 200:
        # no URL in the log: it contained the OWM key in clear
        print(f"[map_proxy] tile meteo OWM {z}/{x}/{y} fallita: HTTP {resp.status_code}", flush=True)
        return None
    return _recolor_weather_tile(resp.content)

@app.route('/weather-tile/<int:z>/<int:x>/<int:y>')
def get_weather_tile(z, x, y):
    now = time.time()
    cache_key = (_RV_META["path"], z, x, y)
    cached = _rv_png_cache.get(cache_key)
    if cached and (now - cached[0]) < _WEATHER_CACHE_TTL:
        return Response(cached[1], mimetype='image/png')
    png = None
    try:
        png = _rv_tile_png(z, x, y)
    except Exception as e:
        print("[map_proxy] radar RainViewer err:", repr(e), flush=True)
    if png is None:                               # ripiego: OpenWeatherMap
        try: png = _owm_weather_tile(z, x, y)
        except Exception: png = None
        if png is None:
            return Response(status=502)
        return Response(png, mimetype='image/png')
    if len(_rv_png_cache) > 400: _rv_png_cache.clear()
    _rv_png_cache[(_RV_META["path"], z, x, y)] = (now, png)
    return Response(png, mimetype='image/png')

# ── SATELLITE — proxy with disk cache ────────────────────────────────
# Before, the satellite layer (style "sat" below) pointed DIRECTLY to the
# provider from the native MapLibre plugin, which has no retry: a
# momentary connection drop was enough to leave the image grainy
# (tile of the previous zoom level, enlarged) until you
# went back over it. Now it goes through here: a couple more attempts on
# network drops, and once downloaded a tile stays saved on disk — the already
# seen areas reload instantly, even offline (useful above all
# for usual routes). It is not a full download of Italy (the
# satellite tiles of the whole of Italy at useful resolution would be too many for
# a car PC's disk), but a cache that grows with use.
_SAT_TILE_DIR = _CACHE_ROOT / 'sat_maptiler'      # new folder: the old tiles (from another source) are not reused
_SAT_TILE_DIR.mkdir(parents=True, exist_ok=True)
_sat_prefetch_pool = ThreadPoolExecutor(max_workers=6)

def _sat_url(z, x, y):
    # MapTiler Satellite (same MAPTILER_KEY key as the maps): 512 px tile on the same z/x/y
    return f"https://api.maptiler.com/tiles/satellite-v2/{z}/{x}/{y}.jpg?key={MAPTILER_KEY}"

def _sat_get(z, x, y):
    return _fetch_cached('sat_maptiler', f"{z}_{x}_{y}.jpg", _sat_url(z, x, y), timeout=(2.5, 5))

def _sat_prefetch(z, x, y):
    """After a NEW tile, downloads in the background only the lower-zoom one
    (zooming out it is already ready). Before it also fetched the 8 tiles
    around it: 9 extra downloads for every new tile, a lot of traffic on the hotspot
    and more requests on the MapTiler quota."""
    if z < 9 or z > 19 or _sat_prefetch_pool._work_queue.qsize() > 40:
        return
    cand = [(z - 1, x // 2, y // 2)]
    for (zz, xx, yy) in cand:
        if not (_SAT_TILE_DIR / f"{zz}_{xx}_{yy}.jpg").exists():
            _sat_prefetch_pool.submit(_sat_get, zz, xx, yy)

@app.route('/sat-tile/<int:z>/<int:x>/<int:y>')
def get_sat_tile(z, x, y):
    was_cached = (_SAT_TILE_DIR / f"{z}_{x}_{y}.jpg").exists()
    data, st = _sat_get(z, x, y)
    if data is None:
        return Response(status=404)
    if not was_cached:
        _sat_prefetch(z, x, y)
    r = Response(data, mimetype='image/jpeg')
    r.headers['Cache-Control'] = _CACHE_HDR
    return r

# ── MARTE (Toybox › Marte) ──────────────────────────────────────────────
# REAL surface of Mars (OpenPlanetaryMap, NASA/USGS data): Celestia color
# texture; if a tile is missing it falls back to Viking MDIM 2.1 and then to the
# colored MOLA relief. No more recolored terrestrial satellite (the "sepia"
# effect above the map). New path (/mars-real-tile) so MapLibre does not
# reuse from its cache the recolored tiles of the previous version.
MARS_TILE_SOURCES = [
    "http://s3-eu-west-1.amazonaws.com/whereonmars.cartodb.net/celestia_mars-shaded-16k_global/{z}/{x}/{y}.png",
    "http://s3-eu-west-1.amazonaws.com/whereonmars.cartodb.net/viking_mdim21_global/{z}/{x}/{y}.png",
    "http://s3-eu-west-1.amazonaws.com/whereonmars.cartodb.net/mola-color/{z}/{x}/{y}.png",
]
MARS_FLIP_Y = True     # if the map still appears striped, try False
MARS_MAX_Z = 6          # maximum texture resolution (beyond it is enlarged)
_mars_cache = {}
_MARS_CACHE_MAX = 800

def _mars_style():
    return {
        "version": 8,
        "name": "Mars",
        "sources": {
            "mars": {
                "type": "raster",
                "tiles": ["http://127.0.0.1:9098/mars-tile-v3/{z}/{x}/{y}"],
                "tileSize": 256,
                "maxzoom": MARS_MAX_Z,
                "attribution": "OpenPlanetaryMap · NASA/USGS"
            }
        },
        "layers": [
            {"id": "mars-bg", "type": "background", "paint": {"background-color": "#1A0E08"}},
            {"id": "mars-surface", "type": "raster", "source": "mars",
             "paint": {"raster-resampling": "linear"}}
        ]
    }

# OpenPlanetaryMap tiles are in TMS scheme: row 0 is at the BOTTOM
# (south pole), while MapLibre asks for tiles in XYZ scheme (row 0 at the top).
# Without this conversion every horizontal band was taken from
# another latitude: the map looked "striped" with bands that do not match.
@app.route('/mars-tile-v3/<int:z>/<int:x>/<int:y>')
def get_mars_tile(z, x, y):
    if MARS_FLIP_Y:
        y = (2 ** z - 1) - y      # XYZ → TMS
    key = (z, x, y)
    if key in _mars_cache:
        data, ctype = _mars_cache[key]
        return Response(data, mimetype=ctype)
    for tmpl in MARS_TILE_SOURCES:
        try:
            r = _http.get(tmpl.format(z=z, x=x, y=y), timeout=8)
            ctype = r.headers.get('Content-Type', '') or 'image/png'
            if r.status_code == 200 and (ctype.startswith('image/') or r.content[:4] == b'\x89PNG'):
                if len(_mars_cache) >= _MARS_CACHE_MAX:
                    _mars_cache.pop(next(iter(_mars_cache)))
                _mars_cache[key] = (r.content, ctype)
                return Response(r.content, mimetype=ctype)
        except Exception:
            continue
    return Response(status=404)



def _scale_width(w, k):
    """Multiplies a MapLibre line width: number, old function
    {"stops": [[z, v], ...]} or expression ["interpolate"/"step", ..., ["zoom"], z, v, ...]."""
    if isinstance(w, (int, float)):
        return w * k
    if isinstance(w, dict) and 'stops' in w:
        w = dict(w)
        w['stops'] = [[z, v * k if isinstance(v, (int, float)) else v] for z, v in w['stops']]
        return w
    if isinstance(w, list) and w and w[0] in ('interpolate', 'step'):
        out = list(w)
        start = 3 if w[0] == 'interpolate' else 2   # after type/curve and input
        if w[0] == 'step' and len(out) > 2 and isinstance(out[2], (int, float)):
            out[2] = out[2] * k                      # initial value of the step
            start = 3
        for i in range(start + 1, len(out), 2):     # positions of the values (the others are the zooms)
            if isinstance(out[i], (int, float)):
                out[i] = out[i] * k
        return out
    return w

def _compat_qt_maplibre(data):
    """Adapts the MapTiler style to MapLibre for Qt (libQMapLibre 4), which is older:
    - "icon-overlap"/"text-overlap" do not exist yet: they become *-allow-overlap
      ("layer doesn't support this property");
    - sources with neither "tiles" nor "url" (e.g. maptiler_attribution, it only serves for the
      credits text) are removed ("source must have tiles")."""
    for layer in data.get('layers', []):
        lay = layer.get('layout')
        if not isinstance(lay, dict):
            continue
        for kind in ('icon', 'text'):
            v = lay.pop(kind + '-overlap', None)
            if v is not None and (kind + '-allow-overlap') not in lay and isinstance(v, str):
                lay[kind + '-allow-overlap'] = (v == 'always')
    used = {l.get('source') for l in data.get('layers', [])}
    for name, src in list(data.get('sources', {}).items()):
        if (isinstance(src, dict) and src.get('type') in ('vector', 'raster')
                and 'tiles' not in src and 'url' not in src and name not in used):
            data['sources'].pop(name)


@app.route('/style.json')
def get_style():
    s = request.args.get('s', 'dark')

    if s == 'mars':
        response = jsonify(_mars_style())
        response.headers['Cache-Control'] = 'no-store'
        return response

    if s == "light":
        url = "https://api.maptiler.com/maps/01a059f2-13fd-7093-8f14-673805645690/style.json?key=" + MAPTILER_KEY
    else:
        url = "https://api.maptiler.com/maps/01a06434-8dc2-7506-b480-5e73d816c10d/style.json?key=" + MAPTILER_KEY

    base, _st = _fetch_cached('style', _md5(url), url, ttl=1800)
    if base is None:
        return jsonify({"error": "map style unreachable"}), 502
    data = json.loads(base)

    # Relief (raster-dem / hillshade): Qt MapLibre does not decode the tiles ("Unsupported
    # image type"), nothing shows on screen. Removed: no useless requests and errors.
    _dem = [n for n, sc in data.get('sources', {}).items()
            if n == 'terrain_rgb' or (isinstance(sc, dict) and sc.get('type') == 'raster-dem')]
    if _dem:
        for n in _dem:
            data['sources'].pop(n, None)
        data['layers'] = [l for l in data.get('layers', [])
                          if l.get('source') not in _dem and l.get('type') != 'hillshade']
        data.pop('terrain', None)
    
    for name, source in data.get('sources', {}).items():
        if 'url' in source and source['url'].startswith('mapbox://'):
            source_id = source['url'].replace('mapbox://', '')
            source['url'] = f"http://127.0.0.1:9098/tilejson/{source_id}"

    if 'glyphs' in data and data['glyphs'].startswith('mapbox://'):
        font_name = data['glyphs'].replace('mapbox://fonts/', '')
        data['glyphs'] = f"http://127.0.0.1:9098/fonts/{font_name}"

    if s == 'sat':
        # MapTiler satellite: goes through the local proxy (/sat-tile):
        # it retries on network drops and keeps a disk cache of the areas
        # already seen (see the route above for details).
        data.setdefault('sources', {})['google-sat'] = {
            "type": "raster",
            "tiles": ["http://127.0.0.1:9098/sat-tile/{z}/{x}/{y}?src=maptiler"],
            "tileSize": 256,
            "attribution": "© MapTiler"
        }
        sat_layer = {
            "id": "google-sat-layer",
            "type": "raster",
            "source": "google-sat",
            "minzoom": 0,
            "maxzoom": 22,
            "paint": {"raster-resampling": "linear", "raster-saturation": 0.18}
        }
        
        ROAD_LAYER_IDS = {"Road network"}

        filtered_layers = []
        for layer in data.get('layers', []):
            ltype = layer.get('type')
            lid = layer.get('id', '')
            if ltype in ('background', 'fill', 'fill-extrusion'):
                continue
            if ltype == 'line':
                if lid not in ROAD_LAYER_IDS:
                    continue
                layer = dict(layer)
                layer['paint'] = dict(layer.get('paint', {}))
                layer['paint']['line-color'] = "#D3D3D3"
                layer['paint']['line-opacity'] = 0.85
            if ltype == 'symbol':
                layer = dict(layer)
                layer['paint'] = dict(layer.get('paint', {}))
                layer['paint'].update({
                    "text-color": "#FFFFFF", "text-halo-color": "#000000",
                    "text-halo-width": 2.2, "text-halo-blur": 0
                })
            filtered_layers.append(layer)
        filtered_layers.insert(0, sat_layer)

        # Night effect (?n=1) — "city at night", not a switched-off photo:
        #  • satellite darker but with MORE contrast and slightly
        #    enlivened colors (before: less saturation + heavy blue veil = dead colors)
        #  • light night-blue veil, only to give the cold tone
        #  • roads as if lit by street lamps: warm, soft and blurred lines
        #  • sharp white labels with a dark halo
        if request.args.get('n') == '1':
            sat_layer['paint'].update({
                "raster-brightness-min": 0.0,
                "raster-brightness-max": 0.38,
                "raster-saturation": 0.16,
                "raster-contrast": 0.35,
            })
            filtered_layers.insert(1, {
                "id": "night-tint",
                "type": "background",
                "paint": {"background-color": "#050D24", "background-opacity": 0.24}
            })
            glow = []
            for layer in filtered_layers:
                if layer.get('type') == 'line':
                    base_w = layer['paint'].get('line-width', 1.5)
                    # wide, faint halo under the road (diffused light)
                    halo = dict(layer)
                    halo['id'] = layer.get('id', 'road') + '-night-glow'
                    halo['paint'] = {
                        "line-color": "#FFB347",
                        "line-opacity": 0.22,
                        "line-blur": 4,
                        "line-width": _scale_width(base_w, 3),
                    }
                    glow.append((layer, halo))
                    layer['paint']['line-color'] = "#FFD9A0"
                    layer['paint']['line-opacity'] = 0.6
                    layer['paint']['line-blur'] = 0.6
                elif layer.get('type') == 'symbol':
                    layer['paint'] = dict(layer.get('paint', {}))
                    layer['paint'].update({
                        "text-color": "#F4F4F4", "text-halo-color": "#05070C",
                        "text-halo-width": 1.8, "text-halo-blur": 0.4
                    })
            for road, halo in glow:
                filtered_layers.insert(filtered_layers.index(road), halo)
        data['layers'] = filtered_layers

    if request.args.get('w') == '1':
        data.setdefault('sources', {})['weather-radar'] = {
            "type": "raster",
            "tiles": [f"http://127.0.0.1:9098/weather-tile/{{z}}/{{x}}/{{y}}"],
            "tileSize": 256,
            "maxzoom": 10
        }
        weather_layer = {
            "id": "weather-layer",
            "type": "raster",
            "source": "weather-radar",
            "minzoom": 0,
            "maxzoom": 22,
            "paint": {
                "raster-opacity": 0.92,
                "raster-resampling": "linear",
            }
        }
        insert_idx = len(data['layers'])
        for idx, layer in enumerate(data['layers']):
            if layer.get('type') == 'symbol':
                insert_idx = idx
                break
        data['layers'].insert(insert_idx, weather_layer)

    _compat_qt_maplibre(data)

    poi_requested = request.args.get('poi') == '1'
    for layer in data.get('layers', []):
        lsrclayer = (layer.get('source-layer') or '').lower()
        lid = layer.get('id', '').lower()
        if layer.get('type') == 'symbol' and ('poi' in lsrclayer or 'poi' in lid):
            layout = dict(layer.get('layout', {}))
            layout['visibility'] = 'visible' if poi_requested else 'none'
            layer['layout'] = layout

    response = jsonify(data)
    response.headers['Cache-Control'] = 'no-store, no-cache, must-revalidate, max-age=0'
    response.headers['Pragma'] = 'no-cache'
    return response


# ═══ POIs ON THE MAP — our icons, OpenStreetMap data ═══════════════════
# The POIs from MapTiler/Mapbox show a sea of parking lots and almost nothing else.
# Here instead the POIs come from OpenStreetMap (Overpass), chosen by category
# useful for driving (gas, chargers, restaurants, pharmacies, supermarkets,
# hotels, attractions...) WITHOUT parking lots, and drawn by QML with the app's
# poi_*.svg icons. It downloads in z14 "tiles" (~2 km) saved to disk:
# an already seen area no longer needs the network.
_OVERPASS = ["https://overpass-api.de/api/interpreter",
             "https://overpass.kumi.systems/api/interpreter",
             "https://overpass.private.coffee/api/interpreter"]
_POI_TILE_Z = 14
_POI_TTL = 21 * 86400
_overpass_sem = threading.Semaphore(2)
_poi_pool = ThreadPoolExecutor(max_workers=4)

_POI_QUERY = """[out:json][timeout:25];
(
  nwr["amenity"~"^(restaurant|fast_food|cafe|bar|pub|biergarten|ice_cream|fuel|charging_station|pharmacy|hospital|clinic|doctors|dentist|cinema|theatre|school|university|college|place_of_worship|car_wash|bus_station|marketplace)$"]({s},{w},{n},{e});
  nwr["shop"]({s},{w},{n},{e});
  nwr["tourism"~"^(hotel|motel|hostel|guest_house|apartment|museum|gallery|attraction|viewpoint|zoo|theme_park|aquarium)$"]({s},{w},{n},{e});
  nwr["leisure"~"^(park|garden|stadium|sports_centre|fitness_centre|beach_resort)$"]({s},{w},{n},{e});
  nwr["natural"="beach"]({s},{w},{n},{e});
  nwr["historic"~"^(monument|castle|ruins|memorial|archaeological_site)$"]({s},{w},{n},{e});
  nwr["railway"="station"]({s},{w},{n},{e});
  nwr["aeroway"="aerodrome"]({s},{w},{n},{e});
);
out center tags 1500;"""

_AMENITY = {   # value -> (icon, category, level) — level 0 always, 1 main, 2 medium, 3 detail
    "fuel": ("poi_gas_station.svg", "Gas station", 1),
    "charging_station": ("poi_ev_station.svg", "Charging station", 1),
    "pharmacy": ("poi_pharmacy.svg", "Pharmacy", 1),
    "hospital": ("poi_hospital.svg", "Hospital", 1),
    "clinic": ("poi_hospital.svg", "Clinic", 2),
    "doctors": ("poi_hospital.svg", "Doctor's office", 3),
    "dentist": ("poi_dentist.svg", "Dentist", 3),
    "restaurant": ("poi_restaurant.svg", "Restaurant", 2),
    "fast_food": ("poi_fastfood.svg", "Fast food", 2),
    "cafe": ("poi_cafe.svg", "Cafe", 2),
    "ice_cream": ("poi_cafe.svg", "Ice cream shop", 2),
    "bar": ("poi_bar.svg", "Bar", 2),
    "pub": ("poi_bar.svg", "Pub", 2),
    "biergarten": ("poi_bar.svg", "Beer garden", 3),
    "cinema": ("poi_theater.svg", "Cinema", 2),
    "theatre": ("poi_theater.svg", "Theater", 2),
    "school": ("poi_school.svg", "School", 3),
    "university": ("poi_school.svg", "University", 2),
    "college": ("poi_school.svg", "University", 3),
    "place_of_worship": ("poi_church.svg", "Church", 3),
    "car_wash": ("poi_car_wash.svg", "Car wash", 3),
    "bus_station": ("poi_bus.svg", "Bus station", 2),
    "marketplace": ("poi_grocery.svg", "Market", 3),
}
_SHOP = {
    "supermarket": ("poi_grocery.svg", "Supermarket", 1),
    "mall": ("poi_mall.svg", "Shopping mall", 1),
    "department_store": ("poi_mall.svg", "Department store", 1),
    "convenience": ("poi_convenience_store.svg", "Convenience store", 3),
    "bakery": ("poi_bakery.svg", "Bakery", 2),
    "pastry": ("poi_bakery.svg", "Pastry shop", 2),
    "clothes": ("poi_clothing.svg", "Clothing", 3),
    "shoes": ("poi_clothing.svg", "Shoes", 3),
    "car_repair": ("poi_car_repair.svg", "Car repair", 3),
    "tyres": ("poi_car_repair.svg", "Tire shop", 3),
    "car": ("poi_car_repair.svg", "Car dealership", 3),
    "greengrocer": ("poi_grocery.svg", "Greengrocer", 3),
    "butcher": ("poi_storefront.svg", "Butcher", 3),
}
_TOURISM = {
    "hotel": ("poi_hotel.svg", "Hotel", 2), "motel": ("poi_hotel.svg", "Motel", 2),
    "hostel": ("poi_guesthouse.svg", "Hostel", 3), "guest_house": ("poi_guesthouse.svg", "B&B", 3),
    "apartment": ("poi_guesthouse.svg", "Apartments", 3),
    "museum": ("poi_attractions.svg", "Museum", 1), "gallery": ("poi_attractions.svg", "Gallery", 3),
    "attraction": ("poi_attractions.svg", "Attraction", 1), "viewpoint": ("poi_attractions.svg", "Viewpoint", 2),
    "zoo": ("poi_attractions.svg", "Zoo", 1), "theme_park": ("poi_attractions.svg", "Theme park", 1),
    "aquarium": ("poi_attractions.svg", "Aquarium", 1),
}
_LEISURE = {
    "park": ("poi_park.svg", "Park", 2), "garden": ("poi_park.svg", "Garden", 3),
    "stadium": ("poi_stadium.svg", "Stadium", 1), "sports_centre": ("poi_stadium.svg", "Sports center", 3),
    "fitness_centre": ("poi_gym.svg", "Gym", 3), "beach_resort": ("poi_beach.svg", "Beach", 2),
}

def _classify_poi(t):
    """OSM tags -> (icon, category, level) or None. Parking lots are NOT there, on purpose."""
    if t.get("aeroway") == "aerodrome": return ("poi_airport.svg", "Airport", 0)
    if t.get("railway") == "station" and t.get("station") != "subway": return ("poi_train.svg", "Station", 1)
    a = t.get("amenity")
    if a in _AMENITY:
        ic, cat, lv = _AMENITY[a]
        if a == "place_of_worship" and t.get("religion") == "muslim": ic, cat = "poi_mosque.svg", "Mosque"
        if a == "restaurant" and "pizza" in (t.get("cuisine") or ""): ic, cat = "poi_pizza.svg", "Pizzeria"
        return ic, cat, lv
    sh = t.get("shop")
    if sh:
        if sh in _SHOP: return _SHOP[sh]
        return ("poi_storefront.svg", "Shop", 4)
    tu = t.get("tourism")
    if tu in _TOURISM: return _TOURISM[tu]
    le = t.get("leisure")
    if le in _LEISURE: return _LEISURE[le]
    if t.get("natural") == "beach": return ("poi_beach.svg", "Beach", 2)
    if t.get("historic"): return ("poi_attractions.svg", "Monument", 2)
    return None

def _poi_from_osm(elements):
    out, seen = [], set()
    for el in elements:
        t = el.get("tags") or {}
        c = _classify_poi(t)
        if not c: continue
        icon, cat, lv = c
        lat = el.get("lat", (el.get("center") or {}).get("lat"))
        lon = el.get("lon", (el.get("center") or {}).get("lon"))
        if lat is None or lon is None: continue
        name = (t.get("name") or t.get("brand") or t.get("operator") or "").strip()
        if not name:
            if lv > 1: continue              # without a name only the essential services (gas, chargers, pharmacies...)
            name = cat
        key = (name.lower(), icon, round(lat, 3), round(lon, 3))
        if key in seen: continue
        seen.add(key)
        out.append({"id": "%s%s" % (el.get("type", "n")[0], el.get("id")), "lat": round(lat, 6), "lon": round(lon, 6),
                    "name": name, "icon": icon, "category": cat, "tier": lv, "named": bool(t.get("name"))})
    return out

def _tile_bounds(x, y, z):
    n = 1 << z
    w = x / n * 360.0 - 180.0
    e = (x + 1) / n * 360.0 - 180.0
    nl = math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * y / n))))
    sl = math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * (y + 1) / n))))
    return sl, w, nl, e

def _lonlat_tile(lon, lat, z):
    n = 1 << z
    x = int((lon + 180.0) / 360.0 * n)
    lat = max(-85.0, min(85.0, lat))
    y = int((1 - math.asinh(math.tan(math.radians(lat))) / math.pi) / 2 * n)
    return max(0, min(n - 1, x)), max(0, min(n - 1, y))

def _poi_tile(x, y, wait):
    """POI list of the z14 tile (disk cache). wait=False: does not block, None if missing."""
    path = _cache_path('poi', f"{x}_{y}.json")
    try:
        if time.time() - path.stat().st_mtime < _POI_TTL:
            return json.loads(path.read_text("utf-8"))
    except (OSError, ValueError):
        pass
    k = ('poi', x, y)
    with _inflight_lock:
        ev = _inflight.get(k)
        leader = ev is None
        if leader: ev = _inflight[k] = threading.Event()
    if not leader:
        if not wait: return None
        ev.wait(30)
        try: return json.loads(path.read_text("utf-8"))
        except (OSError, ValueError): return None
    try:
        s_, w_, n_, e_ = _tile_bounds(x, y, _POI_TILE_Z)
        q = _POI_QUERY.format(s=round(s_, 5), w=round(w_, 5), n=round(n_, 5), e=round(e_, 5))
        with _overpass_sem:
            for url in _OVERPASS:
                try:
                    r = _http.post(url, data={"data": q}, timeout=(4, 28))
                    if r.status_code == 200:
                        pois = _poi_from_osm(r.json().get("elements", []))
                        path.write_text(json.dumps(pois, ensure_ascii=False), "utf-8")
                        return pois
                except (requests.RequestException, ValueError):
                    continue
        try: return json.loads(path.read_text("utf-8"))     # rete assente: copia vecchia
        except (OSError, ValueError): return None
    finally:
        with _inflight_lock: _inflight.pop(k, None)
        ev.set()

def _tier_for_zoom(z):
    if z < 14: return -1
    if z < 15.2: return 1
    if z < 16.2: return 2
    if z < 17.2: return 3
    return 4

# ── POIs from the map's own vector tiles (same source, same cache) ──
# The tiles that the map already downloads contain the POIs ("poi_label"/"poi" layer): I
# decode them here (Mapbox Vector Tile format, no extra library) and
# redraw them myself, filtered (no parking lots) and with the app's icons. Being
# the same files as the map, they are usually already cached: zero extra network.
def _pb_varint(buf, i):
    r = sh = 0
    while True:
        b = buf[i]; i += 1
        r |= (b & 0x7F) << sh
        if b < 0x80: return r, i
        sh += 7

def _pb_fields(buf):
    """Iterates (field, type, value) of a protobuf message; for len-delimited value=bytes."""
    i, n = 0, len(buf)
    while i < n:
        key, i = _pb_varint(buf, i)
        f, t = key >> 3, key & 7
        if t == 0:
            v, i = _pb_varint(buf, i)
        elif t == 2:
            ln, i = _pb_varint(buf, i)
            v = buf[i:i + ln]; i += ln
        elif t == 1:
            v = buf[i:i + 8]; i += 8
        elif t == 5:
            v = buf[i:i + 4]; i += 4
        else:
            return
        yield f, t, v

def _mvt_value(buf):
    import struct
    for f, t, v in _pb_fields(buf):
        if f == 1: return v.decode("utf-8", "replace")
        if f == 2: return struct.unpack("<f", v)[0]
        if f == 3: return struct.unpack("<d", v)[0]
        if f in (4, 5): return v
        if f == 6: return (v >> 1) ^ -(v & 1)
        if f == 7: return bool(v)
    return None

def _mvt_poi_points(data, z, x, y):
    """Points of the layers whose name contains 'poi' -> [{...properties, lat, lon}]."""
    import gzip
    if data[:2] == b"\x1f\x8b":
        try: data = gzip.decompress(data)
        except OSError: return []
    out = []
    n = float(1 << z)
    for f, t, lay in _pb_fields(data):
        if f != 3 or t != 2: continue
        name, extent, keys, vals, feats = "", 4096, [], [], []
        for lf, lt, lv in _pb_fields(lay):
            if lf == 1: name = lv.decode("utf-8", "replace")
            elif lf == 2: feats.append(lv)
            elif lf == 3: keys.append(lv.decode("utf-8", "replace"))
            elif lf == 4: vals.append(_mvt_value(lv))
            elif lf == 5: extent = lv
        if "poi" not in name.lower(): continue
        for fb in feats:
            tags, gtype, geom = [], 0, []
            for ff, ft, fv in _pb_fields(fb):
                if ff == 2 and ft == 2:
                    j = 0
                    while j < len(fv):
                        v_, j = _pb_varint(fv, j); tags.append(v_)
                elif ff == 3: gtype = fv
                elif ff == 4 and ft == 2:
                    j = 0
                    while j < len(fv):
                        v_, j = _pb_varint(fv, j); geom.append(v_)
            if gtype != 1 or not geom: continue          # points only
            props = {}
            for k in range(0, len(tags) - 1, 2):
                if tags[k] < len(keys) and tags[k + 1] < len(vals):
                    props[keys[tags[k]]] = vals[tags[k + 1]]
            px = py = 0
            j = 0
            while j < len(geom):
                cmd, cnt = geom[j] & 7, geom[j] >> 3
                j += 1
                for _ in range(cnt):
                    if cmd in (1, 2) and j + 1 < len(geom):
                        dx, dy = geom[j], geom[j + 1]; j += 2
                        px += (dx >> 1) ^ -(dx & 1)
                        py += (dy >> 1) ^ -(dy & 1)
                        xf = (x + px / extent) / n
                        yf = (y + py / extent) / n
                        p2 = dict(props)
                        p2["lon"] = xf * 360.0 - 180.0
                        p2["lat"] = math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * yf))))
                        out.append(p2)
    return out

_MAKI = {   # maki (Mapbox) -> (icon, category, level). "parking", "bank", "atm"... are NOT there: discarded.
    "restaurant": ("poi_restaurant.svg", "Restaurant", 2), "fast-food": ("poi_fastfood.svg", "Fast food", 2),
    "cafe": ("poi_cafe.svg", "Cafe", 2), "bar": ("poi_bar.svg", "Bar", 2), "beer": ("poi_bar.svg", "Pub", 3),
    "ice-cream": ("poi_cafe.svg", "Ice cream shop", 2), "bakery": ("poi_bakery.svg", "Bakery", 2),
    "pharmacy": ("poi_pharmacy.svg", "Pharmacy", 1), "hospital": ("poi_hospital.svg", "Hospital", 1),
    "doctor": ("poi_hospital.svg", "Doctor's office", 3), "dentist": ("poi_dentist.svg", "Dentist", 3),
    "fuel": ("poi_gas_station.svg", "Gas station", 1), "charging-station": ("poi_ev_station.svg", "Charging station", 1),
    "grocery": ("poi_grocery.svg", "Groceries", 1), "marketplace": ("poi_grocery.svg", "Market", 3),
    "shop": ("poi_storefront.svg", "Shop", 4), "clothing-store": ("poi_clothing.svg", "Clothing", 3),
    "shoe": ("poi_clothing.svg", "Shoes", 3), "mall": ("poi_mall.svg", "Shopping mall", 1),
    "department-store": ("poi_mall.svg", "Department store", 1), "lodging": ("poi_hotel.svg", "Hotel", 2),
    "museum": ("poi_attractions.svg", "Museum", 1), "art-gallery": ("poi_attractions.svg", "Gallery", 3),
    "attraction": ("poi_attractions.svg", "Attraction", 1), "monument": ("poi_attractions.svg", "Monument", 2),
    "castle": ("poi_attractions.svg", "Castle", 2), "zoo": ("poi_attractions.svg", "Zoo", 1),
    "amusement-park": ("poi_attractions.svg", "Theme park", 1), "viewpoint": ("poi_attractions.svg", "Viewpoint", 2),
    "park": ("poi_park.svg", "Park", 2), "garden": ("poi_park.svg", "Garden", 3), "stadium": ("poi_stadium.svg", "Stadium", 1),
    "golf": ("poi_stadium.svg", "Golf", 3), "theatre": ("poi_theater.svg", "Theater", 2), "cinema": ("poi_theater.svg", "Cinema", 2),
    "school": ("poi_school.svg", "School", 3), "college": ("poi_school.svg", "University", 2),
    "place-of-worship": ("poi_church.svg", "Church", 3), "religious-christian": ("poi_church.svg", "Church", 3),
    "religious-muslim": ("poi_mosque.svg", "Mosque", 3), "bus": ("poi_bus.svg", "Bus stop", 4),
    "rail": ("poi_train.svg", "Station", 1), "rail-metro": ("poi_train.svg", "Subway", 2),
    "rail-light": ("poi_train.svg", "Tram", 3), "airport": ("poi_airport.svg", "Airport", 0),
    "car": ("poi_car_repair.svg", "Car", 3), "car-repair": ("poi_car_repair.svg", "Car repair", 3),
    "car-wash": ("poi_car_wash.svg", "Car wash", 3), "fitness-centre": ("poi_gym.svg", "Gym", 3),
    "beach": ("poi_beach.svg", "Beach", 2),
}

def _classify_vec(p):
    mk = (p.get("maki") or "").strip()
    if mk in _MAKI:
        return _MAKI[mk]
    # OpenMapTiles scheme ("poi": class/subclass): I reuse the OSM table
    sub = (p.get("subclass") or p.get("class") or "").strip()
    if sub:
        for k in ("amenity", "shop", "tourism", "leisure", "historic"):
            c = _classify_poi({k: sub})
            if c: return c
    return None

_POI_SRC = {"t": 0, "v": None}

def _poi_tile_source():
    """Finds (once an hour) from which vector source and layer the style's POIs come."""
    now = time.time()
    if _POI_SRC["v"] and now - _POI_SRC["t"] < 3600:
        return _POI_SRC["v"]
    found = None
    for sid in ("1", "2"):
        url = ("https://api.maptiler.com/maps/01a06434-8dc2-7506-b480-5e73d816c10d/style.json?key=" + MAPTILER_KEY) if sid == "1" else \
              ("https://api.maptiler.com/maps/01a059f2-13fd-7093-8f14-673805645690/style.json?key=" + MAPTILER_KEY)
        raw, _st = _fetch_cached('style', _md5(url), url, ttl=1800)
        if not raw: continue
        try: data = json.loads(raw)
        except ValueError: continue
        for layer in data.get('layers', []):
            sl = (layer.get('source-layer') or '').lower()
            if layer.get('type') == 'symbol' and 'poi' in sl and layer.get('source') in data.get('sources', {}):
                src = data['sources'][layer['source']]
                u = src.get('url') or ''
                if u.startswith('mapbox://'):
                    found = {"kind": "mapbox", "id": u.replace('mapbox://', ''), "layer": layer.get('source-layer')}
                elif src.get('tiles'):
                    found = {"kind": "tpl", "tpl": src['tiles'][0], "layer": layer.get('source-layer')}
                elif u.startswith('http'):
                    tj, _s2 = _fetch_cached('tj', _md5(u) + ".json", u, ttl=86400)
                    try:
                        tl = (json.loads(tj) or {}).get('tiles') if tj else None
                    except ValueError:
                        tl = None
                    if tl: found = {"kind": "tpl", "tpl": tl[0], "layer": layer.get('source-layer')}
                if found: break
        if found: break
    if not found:                        # fallback: Mapbox Streets v8 (the one used by the proxy)
        found = {"kind": "mapbox", "id": "mapbox.mapbox-streets-v8", "layer": "poi_label"}
    _POI_SRC.update(t=now, v=found)
    print(f"[map_proxy] POI: sorgente tile = {found}", flush=True)
    return found

def _vec_poi_tile(z, x, y):
    """POIs (already classified) of the z/x/y vector tile. None = not yet available."""
    src = _poi_tile_source()
    if src["kind"] == "mapbox":
        url = f"https://api.mapbox.com/v4/{src['id']}/{z}/{x}/{y}.vector.pbf?access_token={TOKEN}"
        name = f"{_md5(src['id'])[:8]}_{z}_{x}_{y}.pbf"          # same key as the /tile route: shared cache
    else:
        url = src["tpl"].replace("{z}", str(z)).replace("{x}", str(x)).replace("{y}", str(y))
        name = f"{_md5(src['tpl'])[:8]}_{z}_{x}_{y}.pbf"
    data, st = _fetch_cached('vec', name, url, ttl=30 * 86400, allow_empty=True)
    if data is None: return None
    out = []
    try:
        pts = _mvt_poi_points(data, z, x, y)
    except Exception as e:
        print(f"[map_proxy] POI: tile illeggibile {z}/{x}/{y}: {e!r}", flush=True)
        return []
    for pr in pts:
        c = _classify_vec(pr)
        if not c: continue
        icon, cat, lv = c
        name_ = (pr.get("name") or pr.get("name_it") or pr.get("name_en") or "").strip()
        if not name_:
            if lv > 1: continue
            name_ = cat
        out.append({"id": "v%s_%s_%s" % (z, x, len(out)) , "lat": round(pr["lat"], 6), "lon": round(pr["lon"], 6),
                    "name": name_, "icon": icon, "category": cat, "tier": lv, "named": True})
    return out

_vec_pool = ThreadPoolExecutor(max_workers=8)

@app.route('/map-pois')
def map_pois():
    try:
        s_ = float(request.args['s']); w_ = float(request.args['w'])
        n_ = float(request.args['n']); e_ = float(request.args['e'])
        z = float(request.args.get('z', 15))
    except (KeyError, ValueError):
        return jsonify({"pois": [], "pending": 0})
    full = request.args.get('full') == '1'
    maxtier = 4 if full else _tier_for_zoom(z)
    if maxtier < 0:
        return jsonify({"pois": [], "pending": 0})
    cx_, cy_ = (w_ + e_) / 2, (s_ + n_) / 2

    # 1) vector tiles (the same as the map) — main source
    tz = 16 if z >= 15.7 else (15 if z >= 14.8 else 14)
    if full: tz = 16
    # The MapTiler source only reaches zoom 15 (16 answers HTTP 400 and the icons stayed
    # "pending" forever from zoom 15.7 up); Mapbox reaches 16.
    tz = min(tz, 16 if _poi_tile_source()["kind"] == "mapbox" else 15)
    x0, y1 = _lonlat_tile(w_, s_, tz)
    x1, y0 = _lonlat_tile(e_, n_, tz)
    tiles = [(x, y) for x in range(x0, x1 + 1) for y in range(y0, y1 + 1)]
    ccx, ccy = _lonlat_tile(cx_, cy_, tz)
    tiles.sort(key=lambda t: abs(t[0] - ccx) + abs(t[1] - ccy))
    tiles = tiles[:30]
    futs = [_vec_pool.submit(_vec_poi_tile, tz, t[0], t[1]) for t in tiles]
    # A single deadline for all (before 10 s for the first + 2 s for each of the
    # others, one after the other: without network the answer arrived after ~70 s). Those not
    # ready stay "pending" and the UI asks again shortly.
    _cf_wait(futs, timeout=4)
    allp, pending, vec_n = [], 0, 0
    for f in futs:
        res = f.result() if f.done() and not f.exception() else None
        if res is None: pending += 1
        else:
            allp.extend(res); vec_n += len(res)

    # 2) OpenStreetMap: only what is already cached (the rest downloads in the background for next time)
    osm_n = 0
    if z >= 15.2 or full:
        ox0, oy1 = _lonlat_tile(w_, s_, _POI_TILE_Z)
        ox1, oy0 = _lonlat_tile(e_, n_, _POI_TILE_Z)
        otiles = [(x, y) for x in range(ox0, ox1 + 1) for y in range(oy0, oy1 + 1)][:12]
        for (x, y) in otiles:
            path = _cache_path('poi', f"{x}_{y}.json")
            got = None
            try:
                if time.time() - path.stat().st_mtime < _POI_TTL:
                    got = json.loads(path.read_text("utf-8"))
            except (OSError, ValueError):
                pass
            if got is None:
                if _poi_pool._work_queue.qsize() < 8:
                    _poi_pool.submit(_poi_tile, x, y, True)
            else:
                allp.extend(got); osm_n += len(got)

    sel = [p for p in allp if s_ <= p["lat"] <= n_ and w_ <= p["lon"] <= e_ and p["tier"] <= maxtier]
    if full:
        # A single response for all zooms: each POI is assigned the minimum zoom ("minz") from which it
        # appears. The thinning is done by bands (14, 15.2, 16.2, 17.2) with cells that halve,
        # so an icon visible at one band stays visible at the following ones. The map shows or
        # hides them by itself when changing zoom, without requesting anything.
        sel.sort(key=lambda p: (p["tier"], 0 if p["named"] else 1, p["name"]))
        uniq, names = [], set()
        for p in sel:
            nk = (p["name"].lower(), p["icon"], round(p["lat"], 3), round(p["lon"], 3))
            if nk in names: continue
            names.add(nk); uniq.append(p)
        minz = {}
        for zmin, mt, tzz in ((14.0, 1, 14), (15.2, 2, 15), (16.2, 3, 16), (17.2, 4, 17)):
            cell = 360.0 / (256 * (2 ** tzz)) * 54
            taken = set()
            for i, p in enumerate(uniq):
                if p["tier"] > mt: continue
                c = (int(p["lon"] / cell), int(p["lat"] / (cell * 0.7)))
                if c in taken: continue
                taken.add(c)
                minz.setdefault(i, zmin)
        out = []
        for i, p in enumerate(uniq):
            if i not in minz: continue
            o = {k: p[k] for k in ("id", "lat", "lon", "name", "icon", "category", "tier")}
            o["minz"] = minz[i]; out.append(o)
            if len(out) >= 500: break
        return jsonify({"pois": out, "pending": pending})
    # thinning: one icon per cell (~54px), the highest levels and named places win
    cell_deg = 360.0 / (256 * (2 ** z)) * 54
    sel.sort(key=lambda p: (p["tier"], 0 if p["named"] else 1, p["name"]))
    taken, names, out = set(), set(), []
    for p in sel:
        nk = (p["name"].lower(), p["icon"], round(p["lat"], 3), round(p["lon"], 3))
        if nk in names: continue
        c = (int(p["lon"] / cell_deg), int(p["lat"] / (cell_deg * 0.7)))
        if c in taken: continue
        taken.add(c); names.add(nk)
        out.append({k: p[k] for k in ("id", "lat", "lon", "name", "icon", "category", "tier")})
        if len(out) >= 90: break
    if _DEBUG:
        print(f"[map_proxy] POI z={z:.1f} tile z{tz} x{len(tiles)}: vettoriali={vec_n} osm={osm_n} -> {len(out)} (in attesa {pending})", flush=True)
    return jsonify({"pois": out, "pending": pending})

# ── TRAFFIC LIGHTS and STOP SIGNS (OpenStreetMap, z15 tiles cached on disk) ──
_SIG_Z = 15
_SIG_TTL = 30 * 86400
_SIG_QUERY = """[out:json][timeout:25];
node["highway"~"^(traffic_signals|stop)$"]({s},{w},{n},{e});
out tags 3000;"""

def _sig_tile(x, y, wait):
    """[{lat,lon,kind}] of the z15 tile; kind = signal|stop. wait=False: None if missing."""
    path = _cache_path('signals', f"{x}_{y}.json")
    try:
        if time.time() - path.stat().st_mtime < _SIG_TTL:
            return json.loads(path.read_text("utf-8"))
    except (OSError, ValueError):
        pass
    k = ('sig', x, y)
    with _inflight_lock:
        ev = _inflight.get(k)
        leader = ev is None
        if leader: ev = _inflight[k] = threading.Event()
    if not leader:
        if not wait: return None
        ev.wait(30)
        try: return json.loads(path.read_text("utf-8"))
        except (OSError, ValueError): return None
    try:
        s_, w_, n_, e_ = _tile_bounds(x, y, _SIG_Z)
        q = _SIG_QUERY.format(s=round(s_, 5), w=round(w_, 5), n=round(n_, 5), e=round(e_, 5))
        with _overpass_sem:
            for url in _OVERPASS:
                try:
                    r = _http.post(url, data={"data": q}, timeout=(4, 28))
                    if r.status_code == 200:
                        out = []
                        for el in r.json().get("elements", []):
                            tg = el.get("tags") or {}
                            hw = tg.get("highway")
                            if hw not in ("traffic_signals", "stop") or "lat" not in el: continue
                            # "pure" pedestrian traffic lights (crossing) stay: they are real traffic lights
                            out.append({"lat": round(el["lat"], 6), "lon": round(el["lon"], 6),
                                        "kind": "signal" if hw == "traffic_signals" else "stop"})
                        path.write_text(json.dumps(out), "utf-8")
                        return out
                except (requests.RequestException, ValueError):
                    continue
        try: return json.loads(path.read_text("utf-8"))     # rete assente: copia vecchia
        except (OSError, ValueError): return None
    finally:
        with _inflight_lock: _inflight.pop(k, None)
        ev.set()

@app.route('/map-signals')
def map_signals():
    try:
        s_ = float(request.args['s']); w_ = float(request.args['w'])
        n_ = float(request.args['n']); e_ = float(request.args['e'])
    except (KeyError, ValueError):
        return jsonify({"items": [], "pending": 0})
    x0, y1 = _lonlat_tile(w_, s_, _SIG_Z)
    x1, y0 = _lonlat_tile(e_, n_, _SIG_Z)
    tiles = [(x, y) for x in range(x0, x1 + 1) for y in range(y0, y1 + 1)][:9]
    items, pending = [], 0
    for (x, y) in tiles:
        path = _cache_path('signals', f"{x}_{y}.json")
        got = None
        try:
            if time.time() - path.stat().st_mtime < _SIG_TTL:
                got = json.loads(path.read_text("utf-8"))
        except (OSError, ValueError):
            pass
        if got is None:
            pending += 1
            if _poi_pool._work_queue.qsize() < 8:
                _poi_pool.submit(_sig_tile, x, y, True)
        else:
            items.extend(got)
    items = [i for i in items if s_ <= i["lat"] <= n_ and w_ <= i["lon"] <= e_][:400]
    return jsonify({"items": items, "pending": pending})

# ── SPEED CAMERAS (OpenStreetMap, z13 tiles cached on disk) ──
_CAM_Z = 13
_CAM_TTL = 14 * 86400
_CAM_QUERY = """[out:json][timeout:30];
(
  node["highway"="speed_camera"]({s},{w},{n},{e});
  node["enforcement"~"^(maxspeed|average_speed|traffic_signals)$"]({s},{w},{n},{e});
);
out tags 2000;"""
_CARD_DEG = {"N": 0, "NNE": 22, "NE": 45, "ENE": 67, "E": 90, "ESE": 112, "SE": 135, "SSE": 157,
             "S": 180, "SSW": 202, "SW": 225, "WSW": 247, "W": 270, "WNW": 292, "NW": 315, "NNW": 337}

def _cam_dir(v):
    """OSM direction -> degrees (0..359) or None (forward/backward/both are not known)."""
    if not v: return None
    v = str(v).strip().upper()
    if v in _CARD_DEG: return _CARD_DEG[v]
    try:
        d = float(v.replace(",", "."))
        return int(d) % 360
    except ValueError:
        return None

def _cam_limit(v):
    try:
        parts = str(v).split()
        n = int(parts[0])
        if len(parts) > 1 and parts[1].lower() == "mph": n = int(round(n * 1.609))
        return n if 10 <= n <= 200 else None
    except (ValueError, IndexError):
        return None

def _cam_from_elements(elements):
    out = []
    for el in elements:
        tg = el.get("tags") or {}
        if "lat" not in el or "lon" not in el: continue
        enf = tg.get("enforcement", "")
        if enf == "average_speed": kind = "tutor"
        elif enf == "traffic_signals": kind = "redlight"
        elif tg.get("highway") == "speed_camera" or enf == "maxspeed": kind = "speed"
        else: continue
        out.append({"lat": round(el["lat"], 6), "lon": round(el["lon"], 6), "kind": kind,
                    "limit": _cam_limit(tg.get("maxspeed")), "dir": _cam_dir(tg.get("direction") or tg.get("camera:direction"))})
    return out

def _cam_tile(x, y, wait):
    """Cameras of the z13 tile; None if the network is missing and there is no copy."""
    path = _cache_path('speedcams', f"{x}_{y}.json")
    try:
        if time.time() - path.stat().st_mtime < _CAM_TTL:
            return json.loads(path.read_text("utf-8"))
    except (OSError, ValueError):
        pass
    k = ('cam', x, y)
    with _inflight_lock:
        ev = _inflight.get(k)
        leader = ev is None
        if leader: ev = _inflight[k] = threading.Event()
    if not leader:
        if not wait: return None
        ev.wait(35)
        try: return json.loads(path.read_text("utf-8"))
        except (OSError, ValueError): return None
    try:
        s_, w_, n_, e_ = _tile_bounds(x, y, _CAM_Z)
        q = _CAM_QUERY.format(s=round(s_, 5), w=round(w_, 5), n=round(n_, 5), e=round(e_, 5))
        with _overpass_sem:
            for url in _OVERPASS:
                try:
                    r = _http.post(url, data={"data": q}, timeout=(4, 32))
                    if r.status_code == 200:
                        out = _cam_from_elements(r.json().get("elements", []))
                        path.write_text(json.dumps(out), "utf-8")
                        return out
                except (requests.RequestException, ValueError):
                    continue
        try: return json.loads(path.read_text("utf-8"))     # rete assente: copia vecchia
        except (OSError, ValueError): return None
    finally:
        with _inflight_lock: _inflight.pop(k, None)
        ev.set()

@app.route('/map-speedcams')
def map_speedcams():
    try:
        lat = float(request.args['lat']); lon = float(request.args['lon'])
        r_m = max(300.0, min(6000.0, float(request.args.get('r', 3500))))
    except (KeyError, ValueError):
        return jsonify({"items": [], "pending": 0})
    dlat = r_m / 111320.0
    dlon = r_m / (111320.0 * max(0.2, math.cos(math.radians(lat))))
    x0, y1 = _lonlat_tile(lon - dlon, lat - dlat, _CAM_Z)
    x1, y0 = _lonlat_tile(lon + dlon, lat + dlat, _CAM_Z)
    tiles = [(x, y) for x in range(x0, x1 + 1) for y in range(y0, y1 + 1)][:12]
    items, pending = [], 0
    for (x, y) in tiles:
        path = _cache_path('speedcams', f"{x}_{y}.json")
        got = None
        try:
            if time.time() - path.stat().st_mtime < _CAM_TTL:
                got = json.loads(path.read_text("utf-8"))
        except (OSError, ValueError):
            pass
        if got is None:
            try:                                         # even an expired copy is better than nothing
                got = json.loads(path.read_text("utf-8"))
            except (OSError, ValueError):
                got = None
            pending += 1
            if _poi_pool._work_queue.qsize() < 8:
                _poi_pool.submit(_cam_tile, x, y, True)
        if got: items.extend(got)
    out, seen = [], set()
    for c in items:
        key = (c["kind"], c["lat"], c["lon"])
        if key in seen: continue
        seen.add(key)
        if _haversine_km(lat, lon, c["lat"], c["lon"]) * 1000.0 <= r_m:
            out.append(c)
    return jsonify({"items": out[:300], "pending": pending})

@app.route('/poi-debug')
def poi_debug():
    """Diagnostics: http://127.0.0.1:9098/poi-debug?lat=..&lon=..&z=16"""
    try:
        lat = float(request.args.get('lat', 45.4642)); lon = float(request.args.get('lon', 9.19))
        z = int(request.args.get('z', 16))
    except ValueError:
        return jsonify({"error": "invalid parameters"}), 400
    src = _poi_tile_source()
    x, y = _lonlat_tile(lon, lat, z)
    res = _vec_poi_tile(z, x, y)
    from collections import Counter
    return jsonify({"source": src, "tile": [z, x, y], "pois": None if res is None else len(res),
                    "per_categoria": None if res is None else dict(Counter(p["category"] for p in res)),
                    "esempi": (res or [])[:8]})

# ── Ricerche unificate tramite TomTom API ──

# ── CARBURANTI (MIMIT) ──────────────────────────────────────────────────────────
# Prices of Italian gas stations (open data of the Ministry, updated every day), matched
# to the gas stations of the "Nearby gas stations" list. Downloaded in the background and kept in
# ~/.cache/volta/carburanti; if the download fails the list works as before (without prices).
import csv as _csv
_FUEL = {"ready": False, "loading": False, "grid": {}, "stations": 0, "priced": 0, "ts": 0, "err": "", "date": ""}
_FUEL_URLS = {"anag": "https://www.mimit.gov.it/images/exportCSV/anagrafica_impianti_attivi.csv",
              "prezzi": "https://www.mimit.gov.it/images/exportCSV/prezzo_alle_8.csv"}
_FUEL_DIR = os.path.join(os.path.expanduser("~"), ".cache", "volta", "carburanti")
_FUEL_MAX_AGE = 6 * 3600
_FUEL_MATCH_M = 150.0

def _fuel_text(name, stale_ok=False):
    path = os.path.join(_FUEL_DIR, name + ".csv")
    try:
        if stale_ok or time.time() - os.path.getmtime(path) < _FUEL_MAX_AGE:
            return open(path, "rb").read()
    except OSError:
        pass
    try:
        r = _http.get(_FUEL_URLS[name], timeout=90)
        r.raise_for_status()
        data = r.content
        if len(data) < 1000: raise ValueError("file troppo corto (%d byte)" % len(data))
        os.makedirs(_FUEL_DIR, exist_ok=True)
        tmp = path + ".tmp"
        with open(tmp, "wb") as f: f.write(data)
        os.replace(tmp, path)
        return data
    except Exception:
        if os.path.isfile(path):          # download failed: better old data than nothing
            return open(path, "rb").read()
        raise

def _fuel_rows(data):
    try: text = data.decode("utf-8-sig")
    except UnicodeDecodeError: text = data.decode("latin-1")
    lines = text.splitlines()
    hi = next((i for i, l in enumerate(lines[:10]) if "idimpianto" in l.lower()), -1)
    if hi < 0: raise ValueError("header not found (idImpianto column)")
    h = lines[hi]
    sep = max((";", "|", ","), key=lambda s: h.count(s))
    rdr = _csv.reader(lines[hi:], delimiter=sep)
    cols = [c.strip().lower() for c in next(rdr)]
    return cols, rdr, (lines[0] if hi > 0 else "")

def _fuel_kind(desc):
    d = (desc or "").strip().lower()
    if d.startswith("benzina"): return "benzina"
    if d.startswith("gasolio"): return "gasolio"
    if d == "gpl": return "gpl"
    if d.startswith("metano"): return "metano"
    return ""

def _fnum(s):
    try: return float(str(s).strip().replace(",", "."))
    except ValueError: return None

def _fuel_build(stale_ok=False):
    cols, rdr, head = _fuel_rows(_fuel_text("anag", stale_ok))
    ix = {c: i for i, c in enumerate(cols)}
    need = ("idimpianto", "latitudine", "longitudine")
    if any(n not in ix for n in need): raise ValueError("colonne anagrafica inattese: %s" % cols)
    st = {}
    for r in rdr:
        try:
            la, lo = _fnum(r[ix["latitudine"]]), _fnum(r[ix["longitudine"]])
            if la is None or lo is None or not (35 < la < 48 and 6 < lo < 19): continue
            st[r[ix["idimpianto"]].strip()] = (la, lo)
        except IndexError:
            continue
    cols, rdr, head = _fuel_rows(_fuel_text("prezzi", stale_ok))
    ix = {c: i for i, c in enumerate(cols)}
    need = ("idimpianto", "desccarburante", "prezzo")
    if any(n not in ix for n in need): raise ValueError("colonne prezzi inattese: %s" % cols)
    prices = {}
    for r in rdr:
        try:
            sid = r[ix["idimpianto"]].strip()
            if sid not in st: continue
            k = _fuel_kind(r[ix["desccarburante"]])
            p = _fnum(r[ix["prezzo"]])
            if not k or p is None or not (0.3 < p < 6): continue
            d = prices.setdefault(sid, {})
            if k not in d or p < d[k]: d[k] = round(p, 3)
        except IndexError:
            continue
    grid = {}
    for sid, pr in prices.items():
        la, lo = st[sid]
        grid.setdefault((int(la * 100), int(lo * 100)), []).append((la, lo, pr))
    _FUEL.update({"grid": grid, "stations": len(st), "priced": len(prices), "ts": time.time(), "err": "", "date": head.strip()[:40], "ready": True})
    print("[fuel] %d stations, %d with prices (%s)" % (len(st), len(prices), head.strip()[:40]), flush=True)

def _fuel_loop():
    # First round: if there is a copy on disk (even an old one) it is used right away, so that the
    # gas station list has the prices without waiting for the download; then it updates.
    try:
        _fuel_build(stale_ok=True)
    except Exception:
        pass
    while True:
        _FUEL["loading"] = True
        try:
            _fuel_build(); wait = _FUEL_MAX_AGE
        except Exception as e:
            _FUEL["err"] = repr(e)[:200]; wait = 600
            print("[fuel] not available:", _FUEL["err"], flush=True)
        _FUEL["loading"] = False
        time.sleep(wait)

_fuel_started = False
_fuel_start_lock = threading.Lock()

def _fuel_start():
    """Starts the fuel prices the first time they are needed (gas station list). Before
    it started at startup and downloaded two large CSVs every 6 hours even without ever using them."""
    global _fuel_started
    with _fuel_start_lock:
        if _fuel_started: return
        _fuel_started = True
    threading.Thread(target=_fuel_loop, daemon=True, name="carburanti").start()

def _fuel_wait(sec):
    t = time.time()
    while _FUEL["loading"] and not _FUEL["ready"] and time.time() - t < sec: time.sleep(0.1)

def _fuel_prices_near(lat, lon):
    """Prezzi {benzina, gasolio, gpl, metano} dell'impianto MIMIT piu' vicino (entro 150 m), o None."""
    try:
        if not _FUEL["ready"] or lat is None or lon is None: return None
        g = _FUEL["grid"]; cx, cy = int(lat * 100), int(lon * 100)
        k = math.cos(math.radians(lat)); best, bd = None, _FUEL_MATCH_M
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for la, lo, pr in g.get((cx + dx, cy + dy), ()):
                    d = math.hypot((la - lat) * 111320.0, (lo - lon) * 111320.0 * k)
                    if d < bd: best, bd = pr, d
        return dict(best) if best else None
    except Exception:
        return None

@app.route('/fuel-status')
def fuel_status():
    """Diagnostica: http://127.0.0.1:9098/fuel-status"""
    _fuel_start()
    return jsonify({k: _FUEL[k] for k in ("ready", "loading", "stations", "priced", "err", "date")} | {"eta_s": int(time.time() - _FUEL["ts"]) if _FUEL["ts"] else None})

@app.route('/nearby-places')
def nearby_places():
    """Cerca benzinai o ristoranti vicini tramite TomTom Nearby Search."""
    try:
        lat = float(request.args.get('lat'))
        lon = float(request.args.get('lon'))
    except (TypeError, ValueError):
        return jsonify([]), 400

    kind = request.args.get('type', 'fuel')
    category = "7311" if kind == 'fuel' else "7315"

    if kind == 'fuel':
        _fuel_start()
        _fuel_wait(6)      # first start: waits for the prices (6 s at most)
    url = "https://api.tomtom.com/search/2/nearbySearch/.json"
    params = {
        "key": TOMTOM_KEY,
        "lat": lat,
        "lon": lon,
        "radius": 15000,
        "limit": 20,
        "categorySet": category,
        "language": "it-IT"
    }

    try:
        resp = _http.get(url, params=params, timeout=5)
        resp.raise_for_status()
        results = []
        for p in resp.json().get("results", []):
            poi = p.get("poi", {})
            addr = p.get("address", {})
            pos = p.get("position", {})
            
            street = addr.get("streetName", "")
            if addr.get("streetNumber"):
                street += f" {addr.get('streetNumber')}"
            city = addr.get("municipality", "")
            address = f"{street}, {city}".strip(", ")

            results.append({
                "name": poi.get("name", ""),
                "address": address,
                "lat": pos.get("lat"),
                "lon": pos.get("lon"),
                "distanceKm": round(p.get("dist", 0) / 1000.0, 1),
                "prices": (_fuel_prices_near(pos.get("lat"), pos.get("lon")) if kind == "fuel" else None)
            })
        
        print(f"[map_proxy] TomTom nearby({kind}): {len(results)} risultati", flush=True)
        return jsonify(results)
    except Exception as e:
        print(f"[map_proxy] TomTom nearby fallito: {e}", flush=True)
        return jsonify([])

@app.route('/poi-search')
def poi_search():
    """Unified search (Addresses + Businesses) via TomTom."""
    q = (request.args.get('q') or '').strip()
    try:
        lat = float(request.args.get('lat'))
        lon = float(request.args.get('lon'))
    except (TypeError, ValueError):
        return jsonify([]), 400

    if len(q) < 2:
        return jsonify([])

    q_encoded = urllib.parse.quote(q)
    url = f"https://api.tomtom.com/search/2/search/{q_encoded}.json"
    # lat/lon only to give priority to nearby results, WITHOUT "radius": with
    # radius=20000 TomTom only searched within 20 km, so "roma" searched from Milan
    # gave only the streets with "roma" in the name and never the city.
    params = {
        "key": TOMTOM_KEY,
        "lat": lat,
        "lon": lon,
        "limit": 15,
        "language": "it-IT",
        "typeahead": "true"
    }

    try:
        resp = _http.get(url, params=params, timeout=5)
        resp.raise_for_status()
        results = []
        for p in resp.json().get("results", []):
            poi = p.get("poi", {})
            addr = p.get("address", {})
            pos = p.get("position", {})

            name = poi.get("name")
            if not name:
                if p.get("type") == "Street":
                    name = addr.get("streetName", "")
                elif p.get("type") in ("Geography", "Cross Street"):
                    name = addr.get("municipality", "") or addr.get("freeformAddress", "")
                elif p.get("type") == "Point Address":
                    name = f"{addr.get('streetName', '')} {addr.get('streetNumber', '')}"
                else:
                    name = addr.get("freeformAddress", "Indirizzo")

            address = addr.get("freeformAddress", "")

            category = ""
            cats = poi.get("classifications", [])
            if cats:
                category = cats[0].get("names", [{}])[0].get("name", "")

            is_business = bool(poi)

            results.append({
                "name": name.strip(", "),
                "address": address,
                "lat": pos.get("lat"),
                "lon": pos.get("lon"),
                "distanceKm": round(p.get("dist", 0) / 1000.0, 1),
                "category": category,
                "phone": poi.get("phone", ""),
                "website": poi.get("url", ""),
                "isBusiness": is_business
            })
        
        print(f"[map_proxy] TomTom Unificato('{q}'): {len(results)} risultati", flush=True)
        return jsonify(results)
    except Exception as e:
        print(f"[map_proxy] TomTom search fallito: {e}", flush=True)
        return jsonify([])

@app.route('/reverse-geocode')
def reverse_geocode():
    try:
        lat = float(request.args.get('lat'))
        lon = float(request.args.get('lon'))
    except (TypeError, ValueError):
        return jsonify({"street": "", "city": ""}), 400

    cache_key = (round(lat, 4), round(lon, 4))
    now = time.time()
    cached = _geocode_cache.get(cache_key)
    if cached and (now - cached[0]) < _GEOCODE_CACHE_TTL:
        return jsonify(cached[1])

    try:
        url = f"https://api.maptiler.com/geocoding/{lon},{lat}.json?key={MAPTILER_KEY}&limit=1"
        resp = _http.get(url, timeout=3)
        resp.raise_for_status()
        data = resp.json()
        features = data.get('features', [])

        street = ""
        city = ""
        if features:
            top = features[0]
            place_name = top.get('place_name', '') or top.get('text', '')
            parts = [p.strip() for p in place_name.split(',')]
            # Only the street name: the first piece of place_name includes the house number in
            # odd formats (e.g. "Piazza del Duomo 23n01"), hence the numbers at the bottom right.
            street = (top.get('text') or (parts[0] if parts else "")).strip()

            # City: the municipality first; "place" is often the neighborhood (e.g. "Duomo" for Milan)
            ctx = top.get('context', [])
            for want in ('municipality', 'place'):
                c = next((c for c in ctx if c.get('id', '').startswith(want)), None)
                if c:
                    city = c.get('text', '')
                    break
            if not city and len(parts) >= 2:
                city = parts[-2] if len(parts) >= 3 else parts[-1]

        result = {"street": street, "city": city}
        if len(_geocode_cache) > 300:          # before it grew for the whole trip
            for k in [k for k, v in _geocode_cache.items() if now - v[0] >= _GEOCODE_CACHE_TTL]:
                _geocode_cache.pop(k, None)
        _geocode_cache[cache_key] = (now, result)
        return jsonify(result)
    except Exception as e:
        print(f"[map_proxy] reverse geocoding fallito: {e}", flush=True)
        return jsonify({"street": "", "city": ""}), 502

# ═══════════════ ARCADE: games found in Bottles + covers chosen by the user ═══════════════
# Every Bottles bottle has a bottle.yml with the list of added programs
# (External_Programs). Any game added in Bottles shows up in the Arcade by
# itself. Cover and hero are chosen by the user (no copyrighted images in the project).
_BOTTLES_DIRS = [Path.home() / ".var/app/com.usebottles.bottles/data/bottles/bottles",
                 Path.home() / ".local/share/bottles/bottles"]

def _game_key(name):
    import re as _r
    return _r.sub(r'[^a-z0-9]+', '_', name.lower()).strip('_')[:60] or 'gioco'

def _bottles_games():
    import re as _r, yaml
    skip = _r.compile(r'^(game|game_x64|launcher|setup|install|unins|uninstall|update|updater|crash|config|redist)', _r.I)
    out, seen_exe = [], set()
    for root in _BOTTLES_DIRS:
        for yml in sorted(root.glob('*/bottle.yml')):
            try:
                d = yaml.safe_load(yml.read_text('utf-8')) or {}
            except Exception as e:
                print("[arcade] bottle.yml illeggibile:", yml, e, flush=True)
                continue
            bottle = d.get('Name') or yml.parent.name
            for prog in (d.get('External_Programs') or {}).values():
                if not isinstance(prog, dict) or prog.get('removed'):
                    continue
                name = (prog.get('name') or '').strip()
                exe = str(prog.get('path') or '').replace('\\', '/').lower()
                if not name or skip.match(name):
                    continue
                exe_id = (bottle, exe)
                if exe and exe_id in seen_exe:        # same executable twice (discovered automatically)
                    continue
                seen_exe.add(exe_id)
                out.append({"key": _game_key(name), "name": name, "bottle": bottle, "program": name})
    return out

@app.route('/games')
def games_list():
    try:
        games = _bottles_games()
    except Exception as e:
        print("[arcade] games search failed:", repr(e), flush=True)
        games = []
    for g in games:
        g["cover"] = "http://127.0.0.1:9098/games/art/%s/cover?n=%s" % (g["key"], urllib.parse.quote(g["name"]))
        g["hero"]  = "http://127.0.0.1:9098/games/art/%s/hero?n=%s" % (g["key"], urllib.parse.quote(g["name"]))
    return jsonify({"games": games})

# Covers chosen by the user: ~/.config/volta/arcade_art/<key>_<cover|hero>.jpg
_CUSTOM_ART_DIR = Path(os.path.expanduser("~/.config/volta/arcade_art"))

@app.route('/games/art/<key>/<kind>')
def game_art(key, kind):
    """Cover/hero chosen by the user. Without it, 404: the UI shows a card with the game's name."""
    if kind not in ("cover", "hero"):
        return Response(status=404)
    cf = _CUSTOM_ART_DIR / ("%s_%s.jpg" % (_game_key(key), kind))
    if cf.exists():
        return send_from_directory(_CUSTOM_ART_DIR, cf.name, max_age=0)
    return Response(status=404)

@app.route('/games/art/<key>/<kind>/custom', methods=['POST', 'DELETE'])
def game_art_custom(key, kind):
    """POST {"path": "/path/image.png"}: uses that image. DELETE: goes back to the automatic one."""
    if kind not in ("cover", "hero"):
        return Response(status=404)
    cf = _CUSTOM_ART_DIR / ("%s_%s.jpg" % (_game_key(key), kind))
    if request.method == 'DELETE':
        try:
            cf.unlink()
        except FileNotFoundError:
            pass
        return jsonify({"ok": True})
    path = os.path.expanduser(str((request.get_json(silent=True) or {}).get("path") or "").replace("file://", ""))
    try:
        im = Image.open(path).convert("RGB")
        im.thumbnail((1920, 1920))
        _CUSTOM_ART_DIR.mkdir(parents=True, exist_ok=True)
        im.save(cf, format="JPEG", quality=90)
    except Exception as e:
        return jsonify({"ok": False, "error": "invalid image: %s" % e}), 400
    return jsonify({"ok": True})

# ═══════════════ WEB APP ICONS (Browser › Add to apps) ═══════════════
# The site's "home screen" icon (apple-touch-icon, 180 px), otherwise Google's
# (128 px). Downloaded once and saved in data/webapp_icons: visible even offline.
_WEBAPP_ICONS = _PROJECT / 'data' / 'webapp_icons'

def _webapp_icon_image(host):
    """Site icon as RGBA image (cached in data/webapp_icons), None if not found."""
    name = host.replace(':', '_') + '.png'
    f = _WEBAPP_ICONS / name
    if f.exists():
        try:
            return Image.open(f).convert('RGBA')
        except Exception:
            pass
    base = 'https://' + host
    best = None                        # the largest icon among the sources is kept
    for src in (base + '/apple-touch-icon.png', base + '/apple-touch-icon-precomposed.png',
                'https://www.google.com/s2/favicons?domain=%s&sz=128' % host):
        try:
            r = _http.get(src, timeout=(3, 6))
        except requests.RequestException:
            continue
        ok_type = r.headers.get('content-type', '').startswith('image') or r.content[:8] == b'\x89PNG\r\n\x1a\n'
        if r.status_code == 200 and ok_type and len(r.content) > 300:
            try:                       # everything as PNG (some sites give ICO/JPEG)
                im = Image.open(io.BytesIO(r.content)).convert('RGBA')
            except Exception:
                continue
            if best is None or im.width > best.width:
                best = im
            if best.width >= 144:
                break
    if best is None:
        return None
    _WEBAPP_ICONS.mkdir(parents=True, exist_ok=True)
    best.save(f, format='PNG')
    return best

def _host_of(u):
    return (urllib.parse.urlparse(u if '://' in u else 'https://' + u).hostname or '').lower()

@app.route('/webapp-icon')
def webapp_icon():
    host = _host_of((request.args.get('url') or '').strip())
    if not host:
        return Response(status=400)
    if _webapp_icon_image(host) is None:
        return Response(status=404)
    return send_from_directory(_WEBAPP_ICONS, host.replace(':', '_') + '.png', max_age=86400)

def _dominant_color(im):
    """Most 'colourful' average colour of the logo (grey average if it has none)."""
    small = im.resize((32, 32))
    r = g = b = w = 0.0
    ar = ag = ab = an = 0
    for (pr, pg, pb, pa) in small.getdata():
        if pa < 128:
            continue
        mx, mn = max(pr, pg, pb), min(pr, pg, pb)
        sat = 0 if mx == 0 else (mx - mn) / mx
        wt = sat * sat
        r += pr * wt; g += pg * wt; b += pb * wt; w += wt
        ar += pr; ag += pg; ab += pb; an += 1
    if w > 1:
        return (int(r / w), int(g / w), int(b / w))
    if an:
        return (ar // an, ag // an, ab // an)
    return (40, 40, 40)

@app.route('/webapp-card')
def webapp_card():
    """Ready-made 'card' (16:9) for a site: its logo centred on a gradient made
    from the logo's own colour. Saved in data/webapp_icons/<host>.card.png."""
    host = _host_of((request.args.get('url') or '').strip())
    if not host:
        return Response(status=400)
    name = host.replace(':', '_') + '.card.png'
    if not (_WEBAPP_ICONS / name).exists():
        icon = _webapp_icon_image(host)
        if icon is None:
            return Response(status=404)
        W, H = 640, 352
        # Opaque square icon with a flat background (apple-touch-icon): the card
        # takes that very colour so the logo blends in; otherwise a darkened
        # version of the logo's dominant colour.
        corners = [icon.getpixel(pt) for pt in ((1, 1), (icon.width - 2, 1), (1, icon.height - 2), (icon.width - 2, icon.height - 2))]
        flat = all(c[3] > 240 and max(abs(c[i] - corners[0][i]) for i in range(3)) < 12 for c in corners)
        if flat:
            cr, cg, cb = corners[0][:3]
            top = bot = (cr, cg, cb)
        else:
            cr, cg, cb = _dominant_color(icon)
            top = (int(cr * 0.62), int(cg * 0.62), int(cb * 0.62))
            bot = (int(cr * 0.26), int(cg * 0.26), int(cb * 0.26))
        card = Image.new('RGB', (W, H))
        px = card.load()
        for y in range(H):
            t = y / (H - 1)
            row = tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(3))
            for x in range(W):
                px[x, y] = row
        side = int(H * 0.5)
        logo = icon.resize((side, side), Image.LANCZOS)
        card.paste(logo, ((W - side) // 2, (H - side) // 2), logo)
        _WEBAPP_ICONS.mkdir(parents=True, exist_ok=True)
        card.save(_WEBAPP_ICONS / name, format='PNG')
    return send_from_directory(_WEBAPP_ICONS, name, max_age=86400)

@app.route('/qr.svg')
def qr_svg():
    """QR of an https address (guided setup: "open on the phone")."""
    text = (request.args.get('text') or '')[:500]
    if not text.startswith('https://'):
        return Response("https addresses only", status=400, mimetype='text/plain')
    try:
        import segno
        buf = io.BytesIO()
        segno.make(text, error="m").save(buf, kind="svg", scale=8, border=2, xmldecl=False,
                                         dark="#000000", light="#FFFFFF")
        return Response(buf.getvalue(), mimetype='image/svg+xml', headers={"Cache-Control": "max-age=86400"})
    except Exception:
        return Response("QR not available: install segno", status=501, mimetype='text/plain')

@app.route('/ping')
def ping():
    # immediate answer, without network: the UI uses it to know the proxy is up
    return "ok"

@app.route('/static/<path:filename>')
def serve_static(filename):
    return send_from_directory(_PROJECT / 'assets' / 'static', filename)

@app.route('/tilejson/<source_id>')
def get_tilejson(source_id):
    url = f"https://api.mapbox.com/v4/{source_id}.json?secure&access_token={TOKEN}"
    raw, st = _fetch_cached('tj', _md5(source_id) + ".json", url, ttl=86400)
    if raw is None:
        return jsonify({"error": "tilejson unreachable"}), st or 502
    tj = json.loads(raw)
    if 'tiles' in tj:
        tj['tiles'] = [f"http://127.0.0.1:9098/tile/{source_id}/{{z}}/{{x}}/{{y}}"]
    return jsonify(tj)

@app.route('/tile/<source_id>/<z>/<x>/<y>')
def get_tile(source_id, z, x, y):
    url = f"https://api.mapbox.com/v4/{source_id}/{z}/{x}/{y}.vector.pbf?access_token={TOKEN}"
    data, st = _fetch_cached('vec', f"{_md5(source_id)[:8]}_{z}_{x}_{y}.pbf", url, ttl=30 * 86400, allow_empty=True)
    if data is None:
        return Response(status=st or 502)
    r = Response(data, mimetype='application/x-protobuf')
    r.headers['Cache-Control'] = _CACHE_HDR
    return r

@app.route('/sprite/<path:sprite_path>')
def get_sprite(sprite_path):
    url = f"https://api.mapbox.com/styles/v1/{sprite_path}?access_token={TOKEN}"
    ext = 'json' if sprite_path.endswith('.json') else 'png'
    data, st = _fetch_cached('sprite', _md5(sprite_path) + "." + ext, url, ttl=7 * 86400)
    if data is None:
        return Response(status=st or 502)
    r = Response(data, mimetype='application/json' if ext == 'json' else 'image/png')
    r.headers['Cache-Control'] = _CACHE_HDR
    return r

@app.route('/fonts/<path:font_path>')
def get_font(font_path):
    url = f"https://api.mapbox.com/fonts/v1/{font_path}?access_token={TOKEN}"
    data, st = _fetch_cached('fonts', _md5(font_path) + ".pbf", url)
    if data is None:
        return Response(status=st or 502)
    r = Response(data, mimetype='application/x-protobuf')
    r.headers['Cache-Control'] = _CACHE_HDR
    return r

# ═══════════════ DESTINAZIONE DAL TELEFONO (via ntfy.sh) ═══════════════
# The phone publishes the link/address on an ntfy.sh "channel" with a random secret name; the car stays
# listening on the same channel. The address is fixed and works with any network (hotspot included):
# no local IPs, no open port on the PC. The channel name is the secret: saved in
# ~/.config/volta_phone_topic (yours only), it must not be shared.
import re as _re, secrets as _secrets

_NTFY_SERVER = "https://ntfy.sh"
_PH_TOPIC_FILE = Path.home() / '.config' / 'volta_phone_topic'
_PH_STATE_FILE = Path.home() / '.config' / 'volta_phone_last'
_ph_queue = []                       # destinazioni in attesa: {id, lat, lon, name, address, ts, auto}
_ph_lock = threading.Lock()
_ph_pos = {"lat": None, "lon": None}  # last car position (to search for addresses near you)
_ph_status = {"online": False}

# ── speed: ntfy session with TCP keep-alive and "warm" connections ──
class _PhKeepAliveAdapter(requests.adapters.HTTPAdapter):
    def init_poolmanager(self, *a, **k):
        import socket
        opts = [(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1), (socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)]
        for name, val in (("TCP_KEEPIDLE", 15), ("TCP_KEEPINTVL", 5), ("TCP_KEEPCNT", 3)):
            if hasattr(socket, name): opts.append((socket.IPPROTO_TCP, getattr(socket, name), val))
        k["socket_options"] = opts
        super().init_poolmanager(*a, **k)

_PH_SESS = requests.Session()
_PH_SESS.mount("https://", _PhKeepAliveAdapter())
_PH_SESS.headers.update({"User-Agent": "volta-infotainment"})

_ph_warm_until = [0.0]

def _ph_prewarm():
    """Keeps open the connections to the sites used to resolve a shared link,
    but ONLY in the 10 minutes after opening the "destination from the phone" QR (before it
    ran forever: ~6500 requests per day even without ever using the feature)."""
    urls = ("https://maps.app.goo.gl/", "https://www.google.com/generate_204", "https://api.tomtom.com/")
    while True:
        if time.time() < _ph_warm_until[0]:
            for u in urls:
                try: _http.head(u, timeout=(4, 4), allow_redirects=False)
                except Exception: pass
        time.sleep(40)


def _ph_topic():
    try:
        t = _PH_TOPIC_FILE.read_text('utf-8').strip()
        if len(t) >= 16: return t
    except OSError:
        pass
    t = "volta-" + _secrets.token_urlsafe(18)
    try:
        _PH_TOPIC_FILE.parent.mkdir(parents=True, exist_ok=True)
        _PH_TOPIC_FILE.write_text(t, 'utf-8')
        os.chmod(_PH_TOPIC_FILE, 0o600)
    except OSError:
        pass
    return t

_FLOAT = r'(-?\d{1,3}\.\d{3,})'      # free text: at least 3 decimals
_FLU = r'(-?\d{1,3}\.\d+)'            # inside Google URLs
_URL_RE = _re.compile(r'https?://[^\s<>"\']+', _re.I)

def _ph_parse_url(u):
    """Looks for coordinates (and name) inside a Google Maps URL. -> (lat, lon, name) or None"""
    try: pu = urllib.parse.urlparse(u)
    except ValueError: return None
    host = (pu.netloc or '').lower()
    path = urllib.parse.unquote(pu.path or '')
    qs = urllib.parse.parse_qs(pu.query)
    name = ''
    m = _re.search(r'/maps/place/([^/@]+)', path)
    if m: name = m.group(1).replace('+', ' ').strip()
    full = urllib.parse.unquote(u)
    # 1) exact pin of the place: !3d<lat>!4d<lon>
    m = _re.search(r'!3d' + _FLU + r'!4d' + _FLU, full)
    if m: return float(m.group(1)), float(m.group(2)), name
    # 2) parametri espliciti
    for key in ('destination', 'q', 'll', 'query', 'daddr', 'center'):
        for v in qs.get(key, []):
            m = _re.fullmatch(r'\s*' + _FLU + r'\s*,\s*' + _FLU + r'\s*', v)
            if m: return float(m.group(1)), float(m.group(2)), name
    # 3) map center @lat,lon (less precise than the pin: only if nothing else)
    m = _re.search(r'@' + _FLU + r',' + _FLU, path) or _re.search(r'/' + _FLU + r',' + _FLU + r'(?:[/,?]|$)', path)
    if m: return float(m.group(1)), float(m.group(2)), name
    # Google consent: the real address is in the continue parameter
    if 'consent.' in host:
        for c in qs.get('continue', []):
            r = _ph_parse_url(c)
            if r: return r
    return None

def _ph_query_text(u):
    """Search text contained in a maps URL without coordinates (e.g. ?q=Via+Roma+1)."""
    try: qs = urllib.parse.parse_qs(urllib.parse.urlparse(u).query)
    except ValueError: return ''
    for key in ('q', 'query', 'destination', 'daddr'):
        if qs.get(key): return qs[key][0]
    return ''

def _ph_follow(u, hops=6):
    """Follows the redirects (maps.app.goo.gl etc.) by hand: tries to read the coordinates at every hop."""
    seen = []
    for _ in range(hops):
        r = _ph_parse_url(u)
        if r: return r, u
        seen.append(u)
        try:
            resp = _http.get(u, allow_redirects=False, timeout=(3, 5), stream=True)
            loc = resp.headers.get('Location'); resp.close()
        except requests.RequestException:
            break
        if not loc: break
        u = urllib.parse.urljoin(u, loc)
        if u in seen: break
    r = _ph_parse_url(u)
    return (r, u) if r else (None, u)

def _ph_geocode(q):
    """Address/name -> (lat, lon, name, address) with TomTom, near the car's position if known."""
    params = {"key": TOMTOM_KEY, "limit": 1, "language": "it-IT", "countrySet": "IT,CH,FR,AT,SI,SM,VA,DE,ES"}
    if _ph_pos["lat"] is not None:
        params.update({"lat": _ph_pos["lat"], "lon": _ph_pos["lon"]})
    try:
        r = _http.get("https://api.tomtom.com/search/2/search/%s.json" % urllib.parse.quote(q), params=params, timeout=6)
        r.raise_for_status()
        res = (r.json().get("results") or [])
        if not res: return None
        p = res[0]; pos = p.get("position") or {}; addr = p.get("address") or {}; poi = p.get("poi") or {}
        name = poi.get("name") or addr.get("freeformAddress") or q
        return pos["lat"], pos["lon"], name, addr.get("freeformAddress", "")
    except (requests.RequestException, ValueError, KeyError):
        return None

def _ph_resolve(text):
    """Text received from the phone -> destination dict or None. Accepts: Google Maps link (even short),
    shared text 'Name\\nlink', coordinates 'lat, lon', free address."""
    text = (text or '').strip()
    if not text: return None
    m = _re.fullmatch(r'\s*(?:geo:)?' + _FLOAT + r'\s*[,; ]\s*' + _FLOAT + r'.*', text)
    if m:
        return {"lat": float(m.group(1)), "lon": float(m.group(2)), "name": "Destinazione", "address": ""}
    um = _URL_RE.search(text)
    if um:
        url = um.group(0).rstrip('.,);')
        before = text[:um.start()].strip().strip('-–:').strip()
        res, final = _ph_follow(url)
        if res:
            lat, lon, name = res
            nm = name or before.split('\n')[0].strip() or "Destinazione"
            return {"lat": lat, "lon": lon, "name": nm[:80], "address": ""}
        q = _ph_query_text(final) or _ph_query_text(url)
        if not q and final != url:
            mm = _re.search(r'/maps/place/([^/@]+)', urllib.parse.unquote(final))
            if mm: q = mm.group(1).replace('+', ' ')
        q = q or before
        if q:
            g = _ph_geocode(q)
            if g: return {"lat": g[0], "lon": g[1], "name": g[2][:80], "address": g[3]}
        return None
    g = _ph_geocode(text)
    if g: return {"lat": g[0], "lon": g[1], "name": g[2][:80], "address": g[3]}
    return None

def _ph_push(text, auto=True):
    _t0 = time.time()
    res = _ph_resolve(text)
    print("[ph] risolta in %.1f s" % (time.time() - _t0), flush=True)
    if not res:
        print("[map_proxy] destination from the phone: not recognized", flush=True)
        return False
    item = dict(res, id=_secrets.token_hex(4), ts=time.time(), auto=auto)
    with _ph_lock:
        _ph_queue.append(item)
        del _ph_queue[:-5]
    print("[map_proxy] destination from the phone: %s" % item["name"], flush=True)
    return True

def _ph_load_state():
    try: return json.loads(_PH_STATE_FILE.read_text('utf-8'))
    except (OSError, ValueError): return {}

def _ph_save_state(st):
    try:
        _PH_STATE_FILE.parent.mkdir(parents=True, exist_ok=True)
        _PH_STATE_FILE.write_text(json.dumps(st), 'utf-8')
    except OSError:
        pass

def _ph_listen():
    """Keeps listening on the ntfy channel; reconnects by itself. Messages older than 15 minutes or already seen: ignored."""
    delay = 3
    while True:
        st = _ph_load_state()
        since = max(int(st.get("time", 0)), int(time.time()) - 900)
        url = "%s/%s/json?since=%d" % (_NTFY_SERVER, _ph_topic(), since)
        try:
            with _PH_SESS.get(url, stream=True, timeout=(8, 60)) as r:
                if r.status_code != 200:
                    raise requests.RequestException("HTTP %s" % r.status_code)
                _ph_status["online"] = True; delay = 3
                for line in r.iter_lines(chunk_size=1, decode_unicode=True):
                    if not line: continue
                    try: ev = json.loads(line)
                    except ValueError: continue
                    if ev.get("event") != "message": continue
                    mid, mt = ev.get("id"), int(ev.get("time", 0))
                    if mid and mid == st.get("id"): continue
                    st = {"id": mid, "time": mt}; _ph_save_state(st)
                    if time.time() - mt > 900: continue
                    print("[ph] consegna ntfy: %.1f s" % (time.time() - mt), flush=True)
                    _ph_push(str(ev.get("message", ""))[:1500], True)
        except (requests.RequestException, ValueError) as e:
            print("[map_proxy] ntfy: %s" % str(e)[:120], flush=True)
        _ph_status["online"] = False
        time.sleep(delay); delay = min(delay * 2, 60)

@app.route('/phone-dest/pending')
def phone_dest_pending():
    """Local (car only): returns and empties the destinations that arrived. Updates the position for the geocoding."""
    try:
        _ph_pos["lat"] = float(request.args.get('lat')); _ph_pos["lon"] = float(request.args.get('lon'))
    except (TypeError, ValueError):
        pass
    now = time.time()
    with _ph_lock:
        _ph_queue[:] = [i for i in _ph_queue if now - i["ts"] < 1800]
        out = list(_ph_queue); _ph_queue.clear()
    return jsonify({"items": out})

def _ph_url():
    return "%s/%s" % (_NTFY_SERVER, _ph_topic())

@app.route('/phone-dest/info')
def phone_dest_info():
    _ph_warm_until[0] = time.time() + 600     # QR open: connections ready for 10 minutes
    return jsonify({"ready": _ph_status["online"], "qr": "http://127.0.0.1:9098/phone-dest/qr.svg"})

@app.route('/phone-dest/qr.svg')
def phone_dest_qr():
    try:
        import segno
        buf = io.BytesIO()
        segno.make(_ph_url(), error="m").save(buf, kind="svg", scale=8, border=3, xmldecl=False)
        return Response(buf.getvalue(), mimetype='image/svg+xml', headers={"Cache-Control": "no-store"})
    except Exception:
        return Response("QR not available: install python3-segno", status=501, mimetype='text/plain')

def _ph_start():
    _ph_topic()
    threading.Thread(target=_ph_prewarm, daemon=True).start()
    threading.Thread(target=_ph_listen, daemon=True).start()


# ═══════════════ RADIO: COSA STA TRASMETTENDO ═══════════════
# Reads the ICY metadata (StreamTitle) of Icecast/Shoutcast streams, without listening: opens the stream,
# skips the first "icy-metaint" bytes and reads the text block. HLS streams (.m3u8) do not have them: empty.
import re as _re

_RN_TTL = 45
_RN_FAIL_TTL = 180
_rn_cache = {}
_rn_pool = ThreadPoolExecutor(max_workers=8)

def _rn_readn(raw, n):
    buf = b''
    while len(buf) < n:
        chunk = raw.read(n - len(buf))
        if not chunk: break
        buf += chunk
    return buf

def _rn_fetch(url):
    if '.m3u8' in url.lower() or not url.lower().startswith(('http://', 'https://')):
        return ""
    r = None
    try:
        r = _http.get(url, headers={"Icy-MetaData": "1"}, stream=True, timeout=(4, 6))
        mi = r.headers.get("icy-metaint")
        if r.status_code != 200 or not mi: return ""
        mi = int(mi)
        if mi <= 0 or mi > 262144: return ""
        if len(_rn_readn(r.raw, mi)) < mi: return ""
        lb = _rn_readn(r.raw, 1)
        if not lb: return ""
        n = lb[0] * 16
        if n == 0: return ""
        blk = _rn_readn(r.raw, n)
        try: txt = blk.decode('utf-8')
        except UnicodeDecodeError: txt = blk.decode('latin-1', 'replace')
        m = _re.search(r"StreamTitle='(.*?)'\s*(?:;|\x00|$)", txt.replace('\x00', ''), _re.S)
        t = (m.group(1) if m else "").strip()
        return t[:140]
    except (requests.RequestException, ValueError, OSError):
        return None
    finally:
        if r is not None:
            try: r.close()
            except Exception: pass

def _rn_get(url):
    now = time.time()
    c = _rn_cache.get(url)
    if c and now < c[0]: return c[1]
    t = _rn_fetch(url)
    if t is None: _rn_cache[url] = (now + _RN_FAIL_TTL, ""); return ""
    _rn_cache[url] = (now + _RN_TTL, t); return t

@app.route('/radio-now')
def radio_now():
    urls = [u for u in request.args.getlist('u') if u][:40]
    futs = {u: _rn_pool.submit(_rn_get, u) for u in urls}
    out = {}
    for u, f in futs.items():
        try: out[u] = f.result(timeout=12)
        except Exception: out[u] = ""
    return jsonify(out)


# Radio from all over the world (countries, stations, logos): see radio_api.py
from radio_api import register as _radio_register
_radio_register(app, _fetch_cached, _CACHE_ROOT, str(_PROJECT / 'assets' / 'fonts' / 'VoltaFont-Bold.ttf'))

if __name__ == '__main__':
    # _ph_start()  # destination from the phone (ntfy): feature not published
    app.run(port=9098, debug=False, threaded=True)
