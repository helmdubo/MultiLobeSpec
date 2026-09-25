# -*- coding: utf-8 -*-
"""W37/W38 CPU proof, no editor. Usage: python fwd_lobe_check.py   (numpy >= 2; exit code 0 = PASS)

Forward lobe (material node FogMS_ForwardLobe). Evaluates the material HLSL itself: FORWARD_LOBE_CODE_V2 (W38, the node
matedit_density.py installs) and FORWARD_LOBE_CODE_V1 (W37) are read from matedit_density.py as text (ast, no
`import unreal`) and their statements are translated line by line to numpy (translate()); an independent float64 reference
(reference()) is written from the math. The lobe depends on the view direction only through mu = dot(L, -CameraVector), so
  mean over the sphere = 1/2 * integral_{-1}^{1} Lobe(mu) dmu,
integrated with composite 32-point Gauss-Legendre on panels refined toward mu = 1 (the peak at g 0.9 is ~0.006 wide in mu),
and cross-checked on real 3D vectors (Fibonacci sphere of view directions, random sun directions). V2 checks for
g (Phase G) in {0, 0.3, 0.6, 0.8, 0.9} x c (MS Eccentricity) in {0, 0.5, 1} x Strength x S x Depth (MS Occlusion, incl. 0)
x Floor:
  1. translation == reference (float64 1e-12 relative, the HLSL in float32 1e-4);
  2. |mean - 1| <= 1e-3 (float64 and float32 HLSL; the math gives 1 exactly for every c: each octave phase is
     F + (1 - F) * 4pi*HG(g c^(i-1)));
  3. min over mu >= 1 - Strength * (1 - Floor) (the bound; the true minimum is higher since 4pi*HG > 0);
  4. identity: Strength 0, InjectionMode 1 or 3 -> the branch is not taken, Result == Emissive (bit-exact); S = 0 -> Lobe 1
     (also at Depth 0: the node clamps b to 0.001, no 0^0), results finite everywhere;
  5. W37 equivalence: V2 with Ecc 0.5 == V1 bit for bit (float64 and float32, every W37 config, Depth in [0.05, 1]).
The float32 runs rely on NumPy >= 2 promotion (NEP 50: Python float literals stay float32 next to float32 arrays).
F2 sun cone (FogMS_Transport.usf FogMS_SunConeDirection): the permutation FogMSSunConeIndex is read from the .usf; it must be a
permutation of 0..7 whose DirectSamples-4 halves (even / odd bit count of K) are the best of the 35 splits of the 8-point
Vogel disk (smallest mean shift; smallest mean shift + largest second-moment difference); the tilted rays lie inside the
cone and the frame is orthonormal."""
import ast, math, os, re, sys, itertools
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
MATEDIT = os.path.join(HERE, "matedit_density.py")
USF = os.path.normpath(os.path.join(HERE, "..", "..", "..", "Shaders", "Private", "FogMS_Transport.usf"))
FAILS = []

def check(ok, msg):
    print(("PASS " if ok else "FAIL ") + msg)
    if not ok: FAILS.append(msg)

# ---------------------------------------------------------------- HLSL text -> numpy
def hlsl_code(name="FORWARD_LOBE_CODE_V2"):
    tree = ast.parse(open(MATEDIT, encoding="utf-8").read())
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(getattr(t, "id", "") == name for t in node.targets):
            return node.value.value
    raise SystemExit(name + " not found in " + MATEDIT)

def translate(code):
    """(condition, [statements]) as Python source. Accepts only the restricted form of FORWARD_LOBE_CODE_V1/V2: one statement
    per line, one 'if (...)' block, at most one ternary per statement; anything else aborts."""
    lines = [l.strip() for l in code.replace("\r\n", "\n").split("\n")]
    lines = [l for l in lines if l and not l.startswith("//")]
    head = ["float3 Result = Emissive;", "BRANCH"]
    if lines[:2] != head or lines[3] != "{" or lines[-2] != "}" or lines[-1] != "return Result;":
        raise SystemExit("FORWARD_LOBE_CODE layout changed; update translate(): %r" % lines)
    m = re.fullmatch(r"if \((.*)\)", lines[2])
    if not m: raise SystemExit("no if-line: " + lines[2])
    def expr(s):
        s = re.sub(r"(\d+(?:\.\d*)?(?:e[-+]?\d+)?)f\b", r"\1", s)
        s = s.replace("View.AtmosphereLightDirection[0].xyz", "SunDir")
        return s
    cond = expr(m.group(1)).replace("&&", " and ")
    body = []
    for l in lines[4:-2]:
        if not l.endswith(";"): raise SystemExit("statement without ';': " + l)
        l = re.sub(r"^(float3|float)\s+", "", l[:-1])
        t = re.fullmatch(r"(\w+) = (.+?) \? (.+?) : (.+)", l)
        if t: l = "%s = where(%s, %s, %s)" % t.groups()
        if "?" in l or "{" in l: raise SystemExit("unsupported statement: " + l)
        body.append(expr(l))
    return cond, body

NS = {"saturate": lambda x: np.clip(x, 0.0, 1.0), "clamp": np.clip, "max": np.maximum, "pow": np.power,
      "rsqrt": lambda x: 1.0 / np.sqrt(x), "where": np.where,
      "normalize": lambda v: v / np.linalg.norm(v, axis=-1, keepdims=True),
      "dot": lambda a, b: np.sum(a * b, axis=-1)}

def run_hlsl(prog, Strength, G, Depth, BackFloor, FieldA, Mode, SunDir, CameraVector, dtype=np.float64, Ecc=0.5):
    """Result/Emissive of the translated HLSL (Emissive = 1). Scalars and vectors cast to dtype (float32 = GPU precision).
    Ecc is the v2 pin (MID FogMS_ForwardEcc); the v1 code never reads it."""
    cond, body = prog
    f = lambda x: np.asarray(x, dtype=dtype)
    env = dict(NS, Strength=f(Strength), G=f(G), Depth=f(Depth), BackFloor=f(BackFloor), FieldA=f(FieldA), Mode=f(Mode),
               SunDir=f(SunDir), CameraVector=f(CameraVector), Emissive=f(1.0), Ecc=f(Ecc))
    env["Result"] = env["Emissive"]
    if eval(cond, env):
        for stmt in body: exec(stmt, env)
    return np.broadcast_to(env["Result"], np.shape(CameraVector)[:-1]).astype(np.float64), bool(eval(cond, env))

def reference(mu, s, g, b, F, S, c=0.5):
    """The math, float64: 1 + (1 - F) * (f1 (p(g) - 1) + f2 (p(g c) - 1)), b clamped to [0.001, 1] as documented (W38;
    W37 had g/2, i.e. c = 0.5, and b >= 0.05)."""
    b = min(max(b, 0.001), 1.0)
    p = lambda gg: (1 - gg * gg) / (1 + gg * gg - 2 * gg * mu) ** 1.5
    f1 = s * 2 / 3 * S ** b if S > 0 else 0.0
    f2 = s * 1 / 3 * S ** (b * b) if S > 0 else 0.0
    return 1 + (1 - F) * (f1 * (p(g) - 1) + f2 * (p(g * c) - 1))

# Composite Gauss-Legendre in mu, panels halving toward mu = 1.
_x, _w = np.polynomial.legendre.leggauss(32)
_edges = np.unique(np.concatenate([[-1.0, 0.0], 1.0 - 2.0 ** -np.arange(0, 40)]))
_edges = np.append(_edges[_edges < 1.0], 1.0)
MU = np.concatenate([0.5 * (b - a) * _x + 0.5 * (a + b) for a, b in zip(_edges[:-1], _edges[1:])])
WMU = np.concatenate([0.5 * (b - a) * _w for a, b in zip(_edges[:-1], _edges[1:])])  # sum = 2

def cam_from_mu(mu):
    """Sun +Z; view direction v with dot(v, sun) = mu; CameraVector = -v."""
    v = np.stack([np.sqrt(np.clip(1 - mu * mu, 0, 1)), np.zeros_like(mu), mu], axis=-1)
    return -v

def fibonacci(n):
    i = np.arange(n) + 0.5
    z = 1 - 2 * i / n; r = np.sqrt(1 - z * z); phi = i * math.pi * (3 - math.sqrt(5))
    return np.stack([r * np.cos(phi), r * np.sin(phi), z], axis=-1)

def lobe_checks():
    prog = translate(hlsl_code("FORWARD_LOBE_CODE_V2"))
    prog1 = translate(hlsl_code("FORWARD_LOBE_CODE_V1"))
    print("HLSL v2 condition:", prog[0]); print("HLSL v2 body:\n  " + "\n  ".join(prog[1]))
    diff = [(a, b) for a, b in zip(prog1[1], prog[1]) if a != b]
    print("v1 -> v2 changed statements:\n  " + "\n  ".join("%s  ->  %s" % d for d in diff))
    check(prog1[0] == prog[0] and len(prog1[1]) == len(prog[1]) and [a for a, _ in diff] == ["g2 = 0.5 * g1", "b = clamp(Depth, 0.05, 1.0)"],
          "v2 differs from v1 only in g2 (G * Ecc) and the Depth clamp (0.001)")
    CAM = cam_from_mu(MU); SUN = np.array([0.0, 0.0, 1.0])
    GRID = np.concatenate([np.linspace(-1, 1, 200001), [-1.0, 1.0]]); CAMG = cam_from_mu(GRID)
    worst = {"tr64": 0.0, "tr32": 0.0, "mean64": 0.0, "mean32": 0.0, "minmargin": 1e9, "n": 0, "nonfinite": 0}
    eq = {"n": 0, "diff64": 0, "diff32": 0}
    for g in (0.0, 0.3, 0.6, 0.8, 0.9):
        for c in (0.0, 0.5, 1.0):
            for s in (0.25, 0.5, 0.8, 1.0):
                for S in (0.0, 0.05, 0.5, 1.0):
                    for b in (0.0, 0.05, 0.5, 1.0):
                        for F in (0.0, 0.25, 0.5, 1.0):
                            A = 0.5 + 0.5 * S
                            ref = reference(MU, s, g, b, F, S, c)
                            h64, taken = run_hlsl(prog, s, g, b, F, A, 2.0, SUN, CAM, Ecc=c)
                            h32, _ = run_hlsl(prog, s, g, b, F, A, 2.0, SUN, CAM, np.float32, Ecc=c)
                            if not taken: check(False, "branch not taken for Strength %g mode 2" % s)
                            worst["nonfinite"] += int(not (np.all(np.isfinite(h64)) and np.all(np.isfinite(h32))))
                            worst["tr64"] = max(worst["tr64"], float(np.max(np.abs(h64 - ref) / ref)))
                            worst["tr32"] = max(worst["tr32"], float(np.max(np.abs(h32 - ref) / ref)))
                            worst["mean64"] = max(worst["mean64"], abs(float(np.dot(WMU, h64)) / 2 - 1))
                            worst["mean32"] = max(worst["mean32"], abs(float(np.dot(WMU, h32)) / 2 - 1))
                            lo, _ = run_hlsl(prog, s, g, b, F, A, 2.0, SUN, CAMG, np.float32, Ecc=c)
                            worst["minmargin"] = min(worst["minmargin"], float(lo.min()) - (1 - s * (1 - F)))
                            if S == 0.0 and not np.all(h32 == 1.0):
                                check(False, "S = 0 must give Lobe 1 exactly (g %g c %g s %g b %g)" % (g, c, s, b))
                            worst["n"] += 1
                            # W37 equivalence: every W37 config (Depth >= 0.05) at Ecc 0.5, bit for bit on MU and the grid.
                            if c == 0.5 and b >= 0.05:
                                for dtype, key in ((np.float64, "diff64"), (np.float32, "diff32")):
                                    for cams in (CAM, CAMG):
                                        r2, _ = run_hlsl(prog, s, g, b, F, A, 2.0, SUN, cams, dtype, Ecc=0.5)
                                        r1, _ = run_hlsl(prog1, s, g, b, F, A, 2.0, SUN, cams, dtype)
                                        eq[key] += int(not np.array_equal(r1, r2))
                                eq["n"] += 1
    check(worst["nonfinite"] == 0, "all results finite (incl. MS Occlusion 0 and S = 0): %d non-finite configs" % worst["nonfinite"])
    check(worst["tr64"] <= 1e-12, "translated HLSL v2 == reference, float64 max rel %.2e (%d configs, c in {0, 0.5, 1})" % (worst["tr64"], worst["n"]))
    # float32: h = 1 + g^2 - 2 g mu cancels to (1 - g)^2 = 0.01 at g 0.9, mu 1: ~1e-7 * 2 / 0.01 relative, x1.5 in h^-1.5.
    check(worst["tr32"] <= 1e-4, "translated HLSL v2 in float32 vs reference max rel %.2e (cancellation in 1+g^2-2g mu)" % worst["tr32"])
    check(worst["mean64"] <= 1e-3, "mean over the sphere = 1 for every c: max |mean-1| %.2e (float64)" % worst["mean64"])
    check(worst["mean32"] <= 1e-3, "mean over the sphere = 1 for every c: max |mean-1| %.2e (HLSL in float32)" % worst["mean32"])
    check(worst["minmargin"] >= -1e-6, "min over mu >= 1 - Strength*(1-Floor): worst margin %+.3e" % worst["minmargin"])
    check(eq["n"] > 0 and eq["diff64"] == 0 and eq["diff32"] == 0,
          "v2 at Ecc 0.5 == v1 bit for bit: %d W37 configs x (1312 Gauss + 200003 grid mu) x float64/float32, %d/%d differ"
          % (eq["n"], eq["diff64"], eq["diff32"]))

    # 3D: real vectors, arbitrary sun directions, the FieldA/mode path as the material sees it.
    V = fibonacci(400000); rng = np.random.default_rng(37); worst3 = 0.0; eq3 = 0
    for trial in range(3):
        L = rng.normal(size=3); L /= np.linalg.norm(L)
        for g in (0.0, 0.3, 0.6, 0.8, 0.9):
            for c in (0.0, 0.5, 1.0):
                r, _ = run_hlsl(prog, 0.8, g, 0.5, 0.25, 1.0, 2.0, L * 0.999, -V, np.float32, Ecc=c)  # unnormalized sun on purpose
                worst3 = max(worst3, abs(float(r.mean()) - 1))
                if c == 0.5:
                    r1, _ = run_hlsl(prog1, 0.8, g, 0.5, 0.25, 1.0, 2.0, L * 0.999, -V, np.float32)
                    eq3 += int(not np.array_equal(r, r1))
    check(worst3 <= 1e-3, "3D Fibonacci sphere (400k view dirs, 3 random suns, s 0.8, F 0.25, c 0/0.5/1): max |mean-1| %.2e" % worst3)
    check(eq3 == 0, "3D Fibonacci sphere: v2 at Ecc 0.5 == v1 bit for bit (float32), %d of 15 differ" % eq3)

    # Identity paths: Strength 0 (material default) and modes 1 / 3 (full field, debug) skip the branch.
    for s, mode in ((0.0, 2.0), (0.8, 1.0), (0.8, 3.0), (0.8, 0.0)):
        r, taken = run_hlsl(prog, s, 0.6, 0.5, 0.25, 1.0, mode, SUN, CAM, np.float32, Ecc=0.5)
        check(not taken and np.all(r == 1.0), "identity: Strength %g InjectionMode %g -> Result == Emissive" % (s, mode))

    # Intuition table for the guide: lobe at mu = 1 (looking at the sun), cos 15 deg, 0 (sideways), -1 (sun behind).
    print("\nLobe (x isotropic) at S = T_sun*k, MS Occlusion 0.5, MS Back Floor 0.25:")
    print("  %-20s %6s | %8s %8s %8s %8s" % ("config", "S", "at sun", "15 deg", "side", "behind"))
    mus = np.array([1.0, math.cos(math.radians(15)), 0.0, -1.0])
    for s, g, c in ((0.5, 0.6, 0.5), (0.8, 0.6, 0.5), (0.8, 0.8, 0.5), (0.5, 0.6, 0.0), (0.5, 0.6, 1.0)):
        for S in ((1.0, 0.5, 0.1) if c == 0.5 else (1.0,)):
            v = reference(mus, s, g, 0.5, 0.25, S, c)
            print("  s %.1f g %.1f c %.1f    %6.2f | %8.2f %8.2f %8.2f %8.2f" % (s, g, c, S, *v))

# ---------------------------------------------------------------- F2 sun cone
def cone_checks():
    text = open(USF, encoding="utf-8").read()
    m = re.search(r"static const uint FogMSSunConeIndex\[8\]=\{([^}]*)\}", text)
    check(m is not None, "FogMSSunConeIndex found in " + USF)
    if not m: return
    perm = [int(t.strip().rstrip("u")) for t in m.group(1).split(",")]
    check(sorted(perm) == list(range(8)), "FogMSSunConeIndex %s is a permutation of 0..7" % perm)
    pts = np.array([[math.sqrt((i + .5) / 8) * math.cos(i * 2.39996), math.sqrt((i + .5) / 8) * math.sin(i * 2.39996)] for i in range(8)])
    def stat(idx):
        P = pts[list(idx)]; return P.mean(axis=0), (P[:, :, None] * P[:, None, :]).mean(axis=0)
    def split_cost(A, B):
        """(mean shift + largest second-moment difference over tangent directions, mean shift, |mean A|, |mean B|): how
        differently the two alternating DirectSamples-4 solves see the sun cone."""
        (ma, ca), (mb, cb) = stat(A), stat(B)
        dm = float(np.linalg.norm(ma - mb))
        return dm + float(np.linalg.norm(ca - cb, 2)), dm, float(np.linalg.norm(ma)), float(np.linalg.norm(mb))
    even = [perm[k] for k in range(8) if bin(k).count("1") % 2 == 0]
    odd = [perm[k] for k in range(8) if bin(k).count("1") % 2 == 1]
    ours = split_cost(even, odd)
    splits = [split_cost(A, [i for i in range(8) if i not in A]) for A in itertools.combinations(range(8), 4) if 0 in A]
    check(abs(ours[0] - min(c[0] for c in splits)) < 1e-12 and abs(ours[1] - min(c[1] for c in splits)) < 1e-12,
          "tetrahedron halves even K -> i %s, odd K -> i %s: mean shift %.3f tan(theta) (smallest of 35), mean shift + "
          "second-moment difference %.3f (smallest of 35); half-disk mean offsets %.3f / %.3f, full disk %.3f" % (
              sorted(even), sorted(odd), ours[1], ours[0], ours[2], ours[3], float(np.linalg.norm(pts.mean(axis=0)))))
    # The shader's frame and tilt (FogMS_SunConeDirection), float64 mirror.
    rng = np.random.default_rng(29); worst_ortho = 0.0; worst_angle = 0.0
    dirs = [np.array(v, float) for v in ((0, 0, 1), (0, 0, -1), (1, 0, 0), (0, 1, 1e-7), (0, 1, -1e-7))] + list(rng.normal(size=(200, 3)))
    for D in dirs:
        D = D / np.linalg.norm(D)
        sg = 1.0 if D[2] >= 0 else -1.0; a = -1.0 / (sg + D[2]); b = D[0] * D[1] * a
        T1 = np.array([1 + sg * D[0] * D[0] * a, sg * b, -sg * D[0]]); T2 = np.array([b, sg + D[1] * D[1] * a, -D[1]])
        M = np.stack([T1, T2, D]); worst_ortho = max(worst_ortho, float(np.abs(M @ M.T - np.eye(3)).max()))
        for deg in (1.0, 3.0, 5.0, 15.0):
            t = math.tan(math.radians(deg))
            for k in range(8):
                i = perm[k]; r = t * math.sqrt((i + .5) / 8); phi = i * 2.39996
                Dp = D + r * (math.cos(phi) * T1 + math.sin(phi) * T2); Dp /= np.linalg.norm(Dp)
                ang = math.degrees(math.acos(min(1.0, float(D @ Dp))))
                worst_angle = max(worst_angle, ang / deg)
    check(worst_ortho < 1e-12, "sun-cone frame (T1, T2, D) orthonormal, max error %.1e (incl. D.z = +-1, +-0)" % worst_ortho)
    check(worst_angle <= 1.0 + 1e-9, "tilted rays inside the cone: max tilt / theta %.4f (outermost point r = sqrt(7.5/8) tan)" % worst_angle)

if __name__ == "__main__":
    lobe_checks()
    print()
    cone_checks()
    print("\n%s (%d failed)" % ("ALL PASS" if not FAILS else "FAILED", len(FAILS)))
    sys.exit(1 if FAILS else 0)
