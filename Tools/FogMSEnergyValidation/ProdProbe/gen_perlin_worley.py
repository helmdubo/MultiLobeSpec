# -*- coding: utf-8 -*-
"""Tileable 3D Perlin-Worley noise (Nubis style) as a 2D pseudo-volume atlas for UE Volume Texture.
Output: RGBA8 PNG, tiles of N x N laid out in a grid (16 columns x 8 rows for N=128), slice index = row*cols + col.
R = Perlin-Worley (base cloud shape), G/B/A = Worley FBM at increasing frequency.
"""
import sys, numpy as np
from PIL import Image

N = int(sys.argv[1]) if len(sys.argv) > 1 else 128
OUT = sys.argv[2] if len(sys.argv) > 2 else "PerlinWorley_%d.png" % N
SEED = int(sys.argv[3]) if len(sys.argv) > 3 else 7
rng = np.random.default_rng(SEED)

def fade(t):
    return t * t * t * (t * (t * 6 - 15) + 10)

def perlin3d(period):
    """Tileable 3D gradient noise in [0,1], lattice period divides N."""
    g = rng.normal(size=(period, period, period, 3)).astype(np.float32)
    g /= np.linalg.norm(g, axis=-1, keepdims=True) + 1e-8
    coords = (np.arange(N, dtype=np.float32) + 0.5) * (period / N)
    x, y, z = np.meshgrid(coords, coords, coords, indexing="ij")
    x0, y0, z0 = np.floor(x).astype(int), np.floor(y).astype(int), np.floor(z).astype(int)
    fx, fy, fz = x - x0, y - y0, z - z0
    u, v, w = fade(fx), fade(fy), fade(fz)
    def dot(ix, iy, iz, dx, dy, dz):
        gg = g[ix % period, iy % period, iz % period]
        return gg[..., 0] * dx + gg[..., 1] * dy + gg[..., 2] * dz
    n000 = dot(x0, y0, z0, fx, fy, fz);         n100 = dot(x0 + 1, y0, z0, fx - 1, fy, fz)
    n010 = dot(x0, y0 + 1, z0, fx, fy - 1, fz); n110 = dot(x0 + 1, y0 + 1, z0, fx - 1, fy - 1, fz)
    n001 = dot(x0, y0, z0 + 1, fx, fy, fz - 1); n101 = dot(x0 + 1, y0, z0 + 1, fx - 1, fy, fz - 1)
    n011 = dot(x0, y0 + 1, z0 + 1, fx, fy - 1, fz - 1); n111 = dot(x0 + 1, y0 + 1, z0 + 1, fx - 1, fy - 1, fz - 1)
    nx00 = n000 + u * (n100 - n000); nx10 = n010 + u * (n110 - n010)
    nx01 = n001 + u * (n101 - n001); nx11 = n011 + u * (n111 - n011)
    nxy0 = nx00 + v * (nx10 - nx00); nxy1 = nx01 + v * (nx11 - nx01)
    n = nxy0 + w * (nxy1 - nxy0)
    return np.clip(n * 0.5 / 0.7 + 0.5, 0, 1).astype(np.float32)  # ~[-0.7,0.7] -> [0,1]

def worley3d(cells):
    """Tileable inverted Worley in [0,1]: 1 = feature point, 0 = far. One point per cell, 27-neighbour search."""
    pts = rng.random((cells, cells, cells, 3)).astype(np.float32)
    coords = (np.arange(N, dtype=np.float32) + 0.5) * (cells / N)
    x, y, z = np.meshgrid(coords, coords, coords, indexing="ij")
    cx, cy, cz = np.floor(x).astype(int), np.floor(y).astype(int), np.floor(z).astype(int)
    best = np.full((N, N, N), np.inf, dtype=np.float32)
    for ox in (-1, 0, 1):
        for oy in (-1, 0, 1):
            for oz in (-1, 0, 1):
                ix, iy, iz = cx + ox, cy + oy, cz + oz
                p = pts[ix % cells, iy % cells, iz % cells]
                dx = ix + p[..., 0] - x; dy = iy + p[..., 1] - y; dz = iz + p[..., 2] - z
                d = dx * dx + dy * dy + dz * dz
                np.minimum(best, d, out=best)
    d = np.sqrt(best)
    return np.clip(1.0 - d, 0, 1).astype(np.float32)

def perlin_fbm(base, octaves):
    total, amp, norm, p = 0, 1.0, 0.0, base
    for _ in range(octaves):
        total = total + amp * perlin3d(p); norm += amp; amp *= 0.5; p *= 2
    return total / norm

def worley_fbm(c):
    return worley3d(c) * 0.625 + worley3d(c * 2) * 0.25 + worley3d(c * 4) * 0.125

def remap(x, a, b, c, d):
    return c + (x - a) / (b - a) * (d - c)

print("perlin fbm ..."); pfbm = perlin_fbm(4, 5)              # periods 4..64
print("worley fbm ..."); w4 = worley_fbm(4); w8 = worley_fbm(8); w16 = worley_fbm(16)
pw = np.clip(remap(pfbm, 0.0, 1.0, w4, 1.0), 0, 1)          # Nubis: perlin pushed by inverted worley -> round billows

def to8(a):
    lo, hi = np.percentile(a, 0.5), np.percentile(a, 99.5)     # mild stretch for 8-bit use
    return (np.clip((a - lo) / (hi - lo), 0, 1) * 255 + 0.5).astype(np.uint8)

vol = np.stack([to8(pw), to8(w4), to8(w8), to8(w16)], axis=-1)  # (x,y,z,4)

cols = 16 if N >= 128 else 8
rows = N // cols
atlas = np.zeros((rows * N, cols * N, 4), dtype=np.uint8)      # (H, W, 4), H rows of tiles
for s in range(N):
    r, c = divmod(s, cols)
    tile = vol[:, :, s]           # (x, y) -> image expects (row=y, col=x)
    atlas[r * N:(r + 1) * N, c * N:(c + 1) * N] = np.transpose(tile, (1, 0, 2))
Image.fromarray(atlas, "RGBA").save(OUT)
prev = OUT.replace(".png", "_preview.png")
Image.fromarray(np.transpose(vol[:, :, N // 2], (1, 0, 2))[..., 0]).resize((512, 512), Image.NEAREST).save(prev)
print("saved", OUT, atlas.shape, "preview", prev)
