# -*- coding: utf-8 -*-
"""FogMS weather textures: offline prototype for FogMS_Weather_Design.md (section 5). numpy + Pillow only, no third-party data.

Everything is tileable (periodic lattices) and small. Outputs (default D:/FogMS_ProbeFrames/texgen, never the repo):

  T_FogMS_WeatherPattern_<N>.png   RGBA8 N x N. Every channel is rank-equalised to a uniform histogram, so a weather state's
                                   coverage C (= oktas / 8) thresholded as  field > 1 - C  covers the area fraction C exactly.
      R  cumuliform field   = equalise(0.55 Perlin fBm + 0.63 inverted Worley fBm)  -> heaps / clusters (Cu, Cb)
      G  stratiform field   = equalise(0.70 stretched Perlin fBm + 0.30 low Perlin)   -> rolls, streets, sheets (St, Sc)
      B  storm seed field   = equalise(very low frequency Perlin)                     -> where storm cells prefer to form
      A  deck field         = equalise(domain-warped Perlin fBm)                      -> mid deck thickness (As, Ns)
  T_FogMS_Curl2D_<M>.png           RG8 M x M: divergence-free curl of a tileable Perlin fBm potential (0.5 = no motion).
  T_FogMS_CloudTypeLUT_<W>x<H>.png RGBA8. U = cloud type (0 stratus, .25 stratocumulus, .5 cumulus, .75 congestus,
                                   1 cumulonimbus), V = 1 - height in the layer (top row = cloud top).
      R  vertical profile without anvil (FogMS height-profile family, FogMS_DensityAuthoring_Design.md section 3)
      G  edge softness (width of the noise threshold band)
      B  detail type (0 wispy .. 1 billowy, Nubis 2022 noise composite)
      A  anvil spread (profile = R * (1 + A))
  weather_demo_<state>.png         RGBA8 weather maps composed like the runtime would (R coverage, G type, B storm, A deck)
  texgen_sheet.png                 contact sheet for the art lead
  texgen_report.json               the numbers printed below

Checks printed: exact periodicity of every generator (|f(x + 1 tile) - f(x)|), a seam metric (wrap-around step vs the
N - 1 interior steps) with a cropped non-periodic negative control, coverage -> area fraction on the 8-bit texture, the same
during a stratiform <-> cumuliform transition (corrected threshold vs the naive 1 - C), curl divergence before/after 8-bit
quantisation, LUT profile sanity, timings.

Usage: python gen_weather_textures.py [--out DIR] [--size 512] [--curl 128] [--seed 11]
"""
import argparse, json, os, time
import numpy as np
from PIL import Image, ImageDraw

# ----------------------------------------------------------------------------------------------------------------- noise
def _fade(t):
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0)

class Perlin2:
    """Periodic 2D gradient noise: lattice period (px, py) cells; coordinates in cell units, any real value (wraps)."""
    def __init__(self, rng, px, py):
        self.px, self.py = int(px), int(py)
        ang = rng.random((self.px, self.py)) * 2.0 * np.pi
        self.g = np.stack([np.cos(ang), np.sin(ang)], axis=-1).astype(np.float64)

    def __call__(self, u, v):
        i0 = np.floor(u).astype(np.int64); j0 = np.floor(v).astype(np.int64)
        fu = u - i0; fv = v - j0
        def corner(di, dj):
            g = self.g[(i0 + di) % self.px, (j0 + dj) % self.py]
            return g[..., 0] * (fu - di) + g[..., 1] * (fv - dj)
        su, sv = _fade(fu), _fade(fv)
        a = corner(0, 0) + su * (corner(1, 0) - corner(0, 0))
        b = corner(0, 1) + su * (corner(1, 1) - corner(0, 1))
        return a + sv * (b - a)  # about [-0.7, 0.7]

def grid(n):
    c = (np.arange(n, dtype=np.float64) + 0.5) / n  # (0,1): one tile
    y, x = np.meshgrid(c, c, indexing="ij")          # arrays are [y, x] = image rows, columns
    return x, y

# Every field is a function of tile coordinates (x, y), period 1 in both, so periodicity can be tested exactly: f(x + 1, y) = f(x, y).
def fbm_fn(rng, px, py, octaves, gain=0.5, warp=None):
    """Periodic Perlin fBm; warp = (wx, wy) periodic functions returning offsets in tile units, or None."""
    layers = [(Perlin2(rng, px * 2 ** o, py * 2 ** o), 2 ** o) for o in range(octaves)]
    def f(x, y):
        if warp is not None:
            x, y = x + warp[0](x, y), y + warp[1](x, y)
        total, amp, norm = 0.0, 1.0, 0.0
        for noise, s in layers:
            total = total + amp * noise(x * px * s, y * py * s)
            norm += amp; amp *= gain
        return total / norm
    return f

def worley_fn(rng, cells):
    """Periodic inverted Worley F1 (1 = feature point), one point per cell, 3x3 search."""
    pts = rng.random((cells, cells, 2))
    def f(x, y):
        u, v = x * cells, y * cells
        ci, cj = np.floor(u).astype(np.int64), np.floor(v).astype(np.int64)
        best = np.full(np.shape(u), np.inf)
        for di in (-1, 0, 1):
            for dj in (-1, 0, 1):
                ii, jj = ci + di, cj + dj
                p = pts[ii % cells, jj % cells]
                np.minimum(best, (ii + p[..., 0] - u) ** 2 + (jj + p[..., 1] - v) ** 2, out=best)
        return np.clip(1.0 - np.sqrt(best), 0.0, 1.0)
    return f

def worley_fbm_fn(rng, cells):
    a, b, c = worley_fn(rng, cells), worley_fn(rng, cells * 2), worley_fn(rng, cells * 4)
    return lambda x, y: 0.625 * a(x, y) + 0.25 * b(x, y) + 0.125 * c(x, y)

def equalise(a):
    """Rank transform to a uniform histogram in [0,1] (area fraction of a > 1 - C is C)."""
    flat = a.ravel(); order = np.argsort(flat, kind="stable")
    ranks = np.empty(flat.size, dtype=np.float64); ranks[order] = np.arange(flat.size)
    return (ranks / (flat.size - 1)).reshape(a.shape)

def to8(a):
    return (np.clip(a, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)

def seam_check(a):
    """Tileability of a 2D field. For every row boundary (and column boundary) take the mean |step| across it; the wrap-around
    boundary (last -> first) is compared with the N - 1 interior boundaries. Returns (seam / median interior, percentile rank of
    the seam among all boundaries in %). A tileable field: ratio ~1 and a rank anywhere in 0..100; a seam: ratio >> 1, rank 100."""
    a = a.astype(np.float64)
    out = []
    for ax in (0, 1):
        b = np.moveaxis(a, ax, 0)
        steps = np.abs(np.diff(b, axis=0)).mean(axis=1)                 # N - 1 interior boundaries
        seam = np.abs(b[0] - b[-1]).mean()
        out.append((seam / np.median(steps), 100.0 * (steps < seam).mean()))
    return round(float(max(o[0] for o in out)), 3), round(float(max(o[1] for o in out)), 1)

def threshold_for_mix(c, w):
    """Threshold z with P((1 - w) X + w Y > z) = c for independent uniform X, Y (trapezoid CDF), so a blend of two equalised
    fields keeps the asked coverage c during a cloud-type transition (the shader can run the same three branches)."""
    a, b = min(w, 1.0 - w), max(w, 1.0 - w)
    q = 1.0 - c                                                         # CDF value at the threshold
    if a < 1e-6:
        return q
    if q <= a / (2.0 * b):
        return float(np.sqrt(2.0 * a * b * q))
    if q <= 1.0 - a / (2.0 * b):
        return float(0.5 * a + b * q)
    return float(1.0 - np.sqrt(2.0 * a * b * (1.0 - q)))

# ----------------------------------------------------------------------------------------------------------- products
def weather_pattern(rng, n):
    perlin = fbm_fn(rng, 4, 4, 5)                                   # 4..64 cells per tile
    cellular = worley_fbm_fn(rng, 6)
    stretched = fbm_fn(rng, 2, 8, 4)                                # features long along x (rolls / streets)
    low = fbm_fn(rng, 2, 2, 3)
    storm_seed = fbm_fn(rng, 1, 1, 2)                               # one or two blobs per tile
    wx, wy = fbm_fn(rng, 3, 3, 3), fbm_fn(rng, 3, 3, 3)
    deck = fbm_fn(rng, 3, 3, 5, warp=(lambda x, y: 0.08 * wx(x, y), lambda x, y: 0.08 * wy(x, y)))
    raw = {"R_cumuliform": lambda x, y: 0.55 * perlin(x, y) + 0.63 * cellular(x, y),  # offsets do not matter: equalised
           "G_stratiform": lambda x, y: 0.70 * stretched(x, y) + 0.30 * low(x, y),
           "B_storm_seed": storm_seed,
           "A_deck": deck}
    x, y = grid(n)
    ch = {k: equalise(f(x, y)) for k, f in raw.items()}
    # exact periodicity of each generator: the same field one tile to the right and one tile up
    period = {k: float(max(np.abs(f(x + 1.0, y) - f(x, y)).max(), np.abs(f(x, y + 1.0) - f(x, y)).max())) for k, f in raw.items()}
    return ch, period

def curl2d(rng, m):
    x, y = grid(m)
    psi = fbm_fn(rng, 4, 4, 4)(x, y)
    dpsi_dx = (np.roll(psi, -1, axis=1) - np.roll(psi, 1, axis=1)) * 0.5   # periodic central differences, [y, x]
    dpsi_dy = (np.roll(psi, -1, axis=0) - np.roll(psi, 1, axis=0)) * 0.5
    vx, vy = dpsi_dy, -dpsi_dx
    s = np.abs(np.stack([vx, vy])).max()                            # no clipping: clipping would break div = 0
    vx, vy = vx / s, vy / s
    def div(ax, ay):
        return ((np.roll(ax, -1, axis=1) - np.roll(ax, 1, axis=1)) + (np.roll(ay, -1, axis=0) - np.roll(ay, 1, axis=0))) * 0.5
    mag = np.sqrt(vx ** 2 + vy ** 2)
    d_float = float(np.sqrt((div(vx, vy) ** 2).mean()) / np.sqrt((mag ** 2).mean()))
    q = np.stack([to8(vx * 0.5 + 0.5), to8(vy * 0.5 + 0.5)], axis=-1)
    qx = q[..., 0].astype(np.float64) / 255.0 * 2 - 1; qy = q[..., 1].astype(np.float64) / 255.0 * 2 - 1
    d_8bit = float(np.sqrt((div(qx, qy) ** 2).mean()) / np.sqrt((qx ** 2 + qy ** 2).mean()))
    return q, vx, vy, d_float, d_8bit

def _ss(a, w, x):
    t = np.clip((x - a) / np.maximum(w, 1e-4), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)

# Anchors along the type axis: (type, B, T, SB, ST, Anvil, softness bottom, softness top, billowy). Heights are fractions of
# the LOW weather layer (its base .. its top), not of a Box: stratus is a thin sheet at the base, cumulonimbus fills the
# column and spreads an anvil under the top (FogMS round-33 Cumulonimbus: anvil 1.0, top softness 0.15).
TYPE_ANCHORS = [
    (0.00, 0.00, 0.15, 0.03, 0.08, 0.0, 0.30, 0.30, 0.15),   # stratus
    (0.25, 0.00, 0.25, 0.03, 0.12, 0.0, 0.25, 0.20, 0.45),   # stratocumulus
    (0.50, 0.00, 0.45, 0.02, 0.25, 0.0, 0.20, 0.10, 0.90),   # cumulus (flat base, dome)
    (0.75, 0.00, 0.75, 0.02, 0.30, 0.0, 0.15, 0.10, 1.00),   # cumulus congestus (tower)
    (1.00, 0.00, 1.00, 0.02, 0.15, 1.0, 0.15, 0.30, 0.80),   # cumulonimbus (tower + anvil, wispy anvil top)
]

def type_lut(w, h):
    anchors = np.array(TYPE_ANCHORS)
    t = (np.arange(w) + 0.5) / w
    p = np.stack([np.interp(t, anchors[:, 0], anchors[:, k]) for k in range(1, anchors.shape[1])], axis=-1)  # (w, 8)
    hh = (np.arange(h) + 0.5) / h
    H = hh[:, None]; B, T, SB, ST, A = (p[None, :, k] for k in range(5))
    body = _ss(B, SB, H) * (1.0 - _ss(T - ST, ST, H))
    mid = B + 0.5 * (T - B)
    anvil = A * _ss(mid, np.maximum(T - ST - mid, 1e-4), H) * (1.0 - _ss(T, 0.02, H))
    soft = p[None, :, 5] + (p[None, :, 6] - p[None, :, 5]) * H
    billowy = p[None, :, 7] * _ss(0.0, 0.25, H)                     # wispy right at the base (HZD: invert Worley at base)
    lut = np.stack([body, soft, billowy, anvil], axis=-1)           # rows: h = 0 (base) .. 1 (top)
    return lut[::-1], {"type": t, "params": p}                       # image rows: top of the cloud first

def compose_weather(ch, state, storms, n):
    """Runtime composition in numpy (the same math a small M_FogMS_WeatherCompose material would run into a render target)."""
    x, y = grid(n)
    t = state["type"]
    mix = float(_ss(0.2, 0.4, t))                                   # 0 stratiform field .. 1 cumuliform field
    f = ch["G_stratiform"] + (ch["R_cumuliform"] - ch["G_stratiform"]) * mix
    w = state.get("edge", 0.04)
    z = threshold_for_mix(state["coverage"], mix)                   # keeps area = coverage while the field is a blend
    cov = _ss(z - w, 2 * w, f)
    storm = np.zeros((n, n))
    for (sx, sy, r, intensity) in storms:
        dx = (x - sx + 0.5) % 1.0 - 0.5; dy = (y - sy + 0.5) % 1.0 - 0.5  # wrap: the tile is periodic
        storm = np.maximum(storm, intensity * np.exp(-(dx * dx + dy * dy) / (r * r)) * (0.6 + 0.4 * ch["B_storm_seed"]))
    storm = np.clip(storm, 0.0, 1.0)
    R = np.maximum(cov, _ss(0.15, 0.3, storm))
    G = t + (1.0 - t) * _ss(0.1, 0.5, storm)
    Bc = storm * state.get("precip", 1.0)
    deck = state.get("deck", 0.0) * _ss(1.0 - state.get("deck_coverage", 0.0) - w, 2 * w, ch["A_deck"])
    A = np.clip(np.maximum(deck, 0.8 * _ss(0.3, 0.5, storm) * state.get("anvil_shield", 0.0)), 0.0, 1.0)
    return np.stack([R, G, Bc, A], axis=-1), cov

# ----------------------------------------------------------------------------------------------------------- output
def viz_weather(m):
    cov, typ, storm, deck = m[..., 0], m[..., 1], m[..., 2], m[..., 3]
    r = np.clip(cov * (0.75 + 0.25 * typ) + 0.6 * storm, 0, 1)
    g = np.clip(cov * (0.75 + 0.25 * typ) - 0.3 * storm, 0, 1)
    b = np.clip(cov * (0.85 - 0.25 * typ) + 0.5 * deck, 0, 1)
    return np.stack([to8(r), to8(g), to8(b)], axis=-1)

def label(img, text):
    d = ImageDraw.Draw(img); d.rectangle([0, 0, img.width, 13], fill=(0, 0, 0)); d.text((3, 1), text, fill=(255, 255, 255))
    return img

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="D:/FogMS_ProbeFrames/texgen")
    ap.add_argument("--size", type=int, default=512)
    ap.add_argument("--curl", type=int, default=128)
    ap.add_argument("--lut", default="64x128")
    ap.add_argument("--seed", type=int, default=11)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    rng = np.random.default_rng(a.seed)
    rep = {"seed": a.seed, "size": a.size}

    t0 = time.perf_counter()
    ch, period = weather_pattern(rng, a.size)
    rep["t_pattern_s"] = round(time.perf_counter() - t0, 3)
    pat = np.stack([to8(ch[k]) for k in ("R_cumuliform", "G_stratiform", "B_storm_seed", "A_deck")], axis=-1)
    Image.fromarray(pat, "RGBA").save(os.path.join(a.out, "T_FogMS_WeatherPattern_%d.png" % a.size))
    rep["pattern_period_error_max"] = period                       # |f(x + 1 tile) - f(x)|: 0 up to float rounding
    rep["seam_metric_tile_R"] = seam_check(ch["R_cumuliform"])       # (step ratio, rank %) of the wrap line: ~1, any rank
    cut = ch["R_cumuliform"][: a.size - 37, : a.size - 37]           # negative control: a crop of the tile is not periodic
    rep["seam_metric_crop_control"] = seam_check(cut)                # a real seam: ratio >> 1, rank 100
    # coverage C -> area fraction, on the 8-bit texture exactly as a shader would read it
    q = pat[..., 0].astype(np.float64) / 255.0
    rep["coverage_to_area_R_8bit"] = {str(c): round(float((q > 1.0 - c).mean()), 4) for c in (0.125, 0.25, 0.375, 0.5, 0.625, 0.75, 0.875)}
    # type transition: blend of the two 8-bit fields with the corrected threshold keeps the covered area at C
    g8 = pat[..., 1].astype(np.float64) / 255.0
    rep["transition_area_at_C0.4"] = {}
    for m in (0.0, 0.25, 0.5, 0.75, 1.0):
        f = g8 + (q - g8) * m
        rep["transition_area_at_C0.4"][str(m)] = {"corrected": round(float((f > threshold_for_mix(0.4, m)).mean()), 4),
                                                  "naive_1-C": round(float((f > 0.6).mean()), 4)}

    t0 = time.perf_counter()
    curl, vx, vy, d_float, d_8bit = curl2d(rng, a.curl)
    rep["t_curl_s"] = round(time.perf_counter() - t0, 3)
    Image.fromarray(np.concatenate([curl, np.full(curl.shape[:2] + (1,), 128, np.uint8)], axis=-1), "RGB").save(
        os.path.join(a.out, "T_FogMS_Curl2D_%d.png" % a.curl))
    rep["curl_rel_divergence_float"] = d_float
    rep["curl_rel_divergence_8bit"] = round(d_8bit, 4)

    lw, lh = (int(v) for v in a.lut.lower().split("x"))
    lut, meta = type_lut(lw, lh)
    Image.fromarray(np.stack([to8(lut[..., k]) for k in range(4)], axis=-1), "RGBA").save(
        os.path.join(a.out, "T_FogMS_CloudTypeLUT_%dx%d.png" % (lw, lh)))
    prof = lut[::-1, :, 0] * (1.0 + lut[::-1, :, 3])                 # rows back to base..top
    hh = (np.arange(lh) + 0.5) / lh
    rep["lut_profile"] = {}
    for tt in (0.0, 0.25, 0.5, 0.75, 1.0):
        col = min(int(tt * lw), lw - 1); pcol = prof[:, col]
        filled = hh[pcol > 0.5]
        rep["lut_profile"][str(tt)] = {"top_of_density_gt_0.5": round(float(filled.max()), 3) if filled.size else 0.0,
                                       "column_mean": round(float(pcol.mean()), 3), "max": round(float(pcol.max()), 3)}

    states = {
        "scattered": ({"coverage": 0.40, "type": 0.50}, []),
        "overcast": ({"coverage": 1.00, "type": 0.10, "deck": 0.70, "deck_coverage": 0.90}, []),
        "thunderstorm": ({"coverage": 0.55, "type": 0.65, "deck": 0.40, "deck_coverage": 0.50, "anvil_shield": 1.0},
                         [(0.30, 0.35, 0.11, 1.0), (0.72, 0.62, 0.07, 0.8)]),
    }
    rep["demo_coverage_area"] = {}
    demos = {}
    for name, (st, storms) in states.items():
        m, cov = compose_weather(ch, st, storms, a.size)
        demos[name] = m
        Image.fromarray(np.stack([to8(m[..., k]) for k in range(4)], axis=-1), "RGBA").save(
            os.path.join(a.out, "weather_demo_%s.png" % name))
        rep["demo_coverage_area"][name] = {"asked": st["coverage"], "area_cov_gt_0.5": round(float((cov > 0.5).mean()), 4),
                                           "area_R_gt_0.5_with_storms": round(float((m[..., 0] > 0.5).mean()), 4)}

    # contact sheet
    tile = 192
    def gray(a01): return Image.fromarray(to8(a01), "L").convert("RGB").resize((tile, tile), Image.BILINEAR)
    row1 = [label(gray(ch[k]), k) for k in ("R_cumuliform", "G_stratiform", "B_storm_seed", "A_deck")]
    cviz = Image.fromarray(np.stack([curl[..., 0], curl[..., 1], np.full(curl.shape[:2], 128, np.uint8)], axis=-1), "RGB")
    lviz = Image.fromarray(to8(np.clip(lut[..., 0] * (1 + lut[..., 3]) * 0.5, 0, 1)), "L").convert("RGB")
    row2 = [label(cviz.resize((tile, tile), Image.NEAREST), "curl RG"),
            label(lviz.resize((tile, tile), Image.NEAREST), "LUT profile (x type, y height)"),
            label(Image.fromarray(to8(lut[..., 2]), "L").convert("RGB").resize((tile, tile), Image.NEAREST), "LUT billowy"),
            label(Image.fromarray(to8(lut[..., 3]), "L").convert("RGB").resize((tile, tile), Image.NEAREST), "LUT anvil")]
    row3 = [label(Image.fromarray(viz_weather(demos[k]), "RGB").resize((tile, tile), Image.BILINEAR), "demo " + k) for k in states]
    row3.append(label(Image.fromarray(viz_weather(np.tile(demos["scattered"], (2, 2, 1))), "RGB")
                      .resize((tile, tile), Image.BILINEAR), "2x2 repeat: no seams"))
    sheet = Image.new("RGB", (4 * tile, 3 * tile), (0, 0, 0))
    for r, row in enumerate((row1, row2, row3)):
        for c, im in enumerate(row):
            sheet.paste(im, (c * tile, r * tile))
    sheet.save(os.path.join(a.out, "texgen_sheet.png"))

    rep["files"] = sorted(f for f in os.listdir(a.out) if f.endswith(".png"))
    rep["bytes"] = int(sum(os.path.getsize(os.path.join(a.out, f)) for f in rep["files"]))
    with open(os.path.join(a.out, "texgen_report.json"), "w", encoding="utf-8") as fh:
        json.dump(rep, fh, indent=1)
    print(json.dumps(rep, indent=1))

if __name__ == "__main__":
    main()
