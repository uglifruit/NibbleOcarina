#!/usr/bin/env python3
"""flutesim.py — a model of flute.cpp, and the measurements behind its constants.

A LINE-BY-LINE port. If flute.cpp changes, change this — or delete it rather
than let it drift.

---------------------------------------------------------------------------
WHAT THIS FILE EXISTS TO PREVENT
---------------------------------------------------------------------------

v1 shipped a waveguide that drove itself: it ignored the breath knob entirely
and no assertion noticed, because every one of them tested internals. The rule
since then is that assertions are written against WHAT A PLAYER WOULD REPORT.

v2's air path then turned out to add nothing audible — which no assertion could
have caught, because "does this contribute to the sound" is a judgement. What
the model CAN do is prove the controls move the sound as far as they claim, so
that a knob which feels dead is a design decision rather than a bug.

    silence      zero level must be EXACTLY zero on every output
    dynamic      the knob must span a wide, monotonic level range
    vibrato      Main must add none at the bottom and a lot at the top
    character    X must genuinely change the vibrato's rate AND depth
    fold         X must genuinely change Audio Out 2's harmonics
    phase        the three outputs must stay locked to one oscillator

Run:  python tools/flutesim.py
      python tools/flutesim.py --grid    print the Main x X table
"""

import math
import sys

SR = 48000

FOLD_MAX = 2048
FOLD_PASSES = 4
DC_POLE_Q15 = 32735

VIB_RATE_FAST_Q8 = 8 * 256
VIB_RATE_SLOW_Q8 = 3 * 256
VIB_DEPTH_WIDE_Q4 = 50 * 16
VIB_DEPTH_TIGHT_Q4 = 10 * 16
VIB_ONSET = 1200
X_VOLUME_TILT = 700

FAILURES = []


def check(name, got, want):
    if got == want:
        print(f"  ok    {name}")
    else:
        print(f"  FAIL  {name}\n          got  {got}\n          want {want}")
        FAILURES.append(name)


def check_true(name, cond, detail=""):
    if cond:
        print(f"  ok    {name}{('  ' + detail) if detail else ''}")
    else:
        print(f"  FAIL  {name}  {detail}")
        FAILURES.append(name)


def clamp(v):
    return max(-32768, min(32767, v))


SIN = [int(round(32767 * math.sin(i / 256 * math.pi / 2))) for i in range(257)]


def fast_sin(ph):
    q = (ph >> 30) & 3
    frac = (ph >> 6) & 0xFFFFFF
    idx = frac >> 16
    mu = frac & 0xFFFF
    if q & 1:
        idx = 255 - idx
        mu = 65536 - mu
        if mu == 65536:
            mu = 0
            idx += 1
    a, b = SIN[idx], SIN[idx + 1]
    v = a + (((b - a) * mu) >> 16)
    return -v if q & 2 else v


def midi_hz(n):
    return 440.0 * 2 ** ((n - 69) / 12.0)


# ---------------------------------------------------------------------------
# The port
# ---------------------------------------------------------------------------

def lerp(a, b, t):
    return a + (((b - a) * t) >> 12)


def vibrato_for(main_q12, x):
    """flute.cpp VibratoFor(). Returns (rate_q8, cents_q4)."""
    x = max(0, min(4095, x))
    if x < 2048:
        t = x << 1
        rate = VIB_RATE_FAST_Q8
        depth = lerp(VIB_DEPTH_WIDE_Q4, VIB_DEPTH_TIGHT_Q4, t)
    else:
        t = (x - 2048) << 1
        rate = lerp(VIB_RATE_FAST_Q8, VIB_RATE_SLOW_Q8, t)
        depth = lerp(VIB_DEPTH_TIGHT_Q4, VIB_DEPTH_WIDE_Q4, t)

    m = main_q12
    if m < VIB_ONSET:
        return rate, 0
    m = min(4095, m)
    span = 4095 - VIB_ONSET
    grow = ((m - VIB_ONSET) << 12) // span
    return rate, (depth * grow) >> 12


def x_volume_boost(x):
    x = max(0, min(4095, x))
    return (X_VOLUME_TILT * x) >> 12


def fold_for(x):
    x = max(0, min(4095, x))
    return (FOLD_MAX * x) >> 12


class Flute:
    def __init__(self):
        self.ph = 0
        self.inc = 0
        self.fold = 0
        self.muted = False
        self.sine = 0
        self.folded = 0
        self.square = False
        self.dcx1 = self.dcy1 = 0
        self.dcx2 = self.dcy2 = 0

    def set_pitch(self, hz):
        self.inc = int(round(hz * 4294967296.0 / SR)) & 0xFFFFFFFF

    def step(self, level):
        if level <= 0 or self.muted:
            self.sine = 0
            self.folded = 0
            self.square = False
            # Park the phase so the next note starts at zero rather than
            # mid-cycle -- otherwise every attack begins with a step. See
            # flute.cpp.
            self.ph = 0
            self.dcx1 = self.dcy1 = 0
            self.dcx2 = self.dcy2 = 0
            return

        self.ph = (self.ph + self.inc) & 0xFFFFFFFF
        raw = fast_sin(self.ph) >> 3

        s = (raw * level) >> 12
        y = s - self.dcx1 + ((self.dcy1 * DC_POLE_Q15 + 16384) >> 15)
        self.dcx1, self.dcy1 = s, y
        self.sine = y

        f = (raw * (4096 + self.fold * 3)) >> 12
        for _ in range(FOLD_PASSES):
            if f > 4096:
                f = 8192 - f
            elif f < -4096:
                f = -8192 - f
            else:
                break
        f = (f * level) >> 12
        y = f - self.dcx2 + ((self.dcy2 * DC_POLE_Q15 + 16384) >> 15)
        self.dcx2, self.dcy2 = f, y
        self.folded = y

        self.square = raw >= 0


def run(hz, level, x=0, n=20000, skip=10000, mute=False):
    f = Flute()
    f.set_pitch(hz)
    f.fold = fold_for(x)
    if mute:
        f.muted = True
    sines, folds, squares = [], [], []
    for i in range(n):
        f.step(level)
        if i >= skip:
            sines.append(f.sine)
            folds.append(f.folded)
            squares.append(f.square)
    return sines, folds, squares


# ---------------------------------------------------------------------------
# Measurement
# ---------------------------------------------------------------------------

def rms(x):
    return math.sqrt(sum(v * v for v in x) / len(x)) if x else 0.0


def goertzel(x, f):
    w = 2 * math.pi * f / SR
    c = 2 * math.cos(w)
    s1 = s2 = 0.0
    for v in x:
        s0 = v + c * s1 - s2
        s2, s1 = s1, s0
    return math.sqrt(abs(s1 * s1 + s2 * s2 - c * s1 * s2)) / len(x)


def harmonics(out, f0, n=8):
    return [goertzel(out, f0 * k) for k in range(1, n + 1)]


def brightness(out, f0, n=8):
    """Spectral centroid in harmonic numbers. 1.0 = a pure sine."""
    h = harmonics(out, f0, n)
    return sum((i + 1) * v for i, v in enumerate(h)) / max(sum(h), 1e-9)


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def test_silence():
    print("silence")
    s, f, q = run(220.0, 0, n=4000, skip=0)
    check("zero level: sine is silent", max(abs(v) for v in s), 0)
    check("zero level: fold is silent", max(abs(v) for v in f), 0)
    check("zero level: square stays low", any(q), False)

    s, f, _ = run(220.0, 3000, x=2048, mute=True, n=4000, skip=0)
    check("muted: sine is silent", max(abs(v) for v in s), 0)
    check("muted: fold is silent", max(abs(v) for v in f), 0)


def test_dynamic_range():
    print("dynamic range")
    pts = [(lv, rms(run(220.0, lv)[0])) for lv in (100, 400, 1000, 2000, 3000, 4095)]
    for lv, r in pts:
        print(f"        level {lv:4d} -> rms {r:8.1f}")
    check_true("spans at least 30:1", pts[-1][1] / max(pts[0][1], 1e-9) >= 30,
               f"{pts[-1][1]/max(pts[0][1],1e-9):.0f}:1")
    check("monotonic", all(pts[i][1] > pts[i-1][1] for i in range(1, len(pts))),
          True)


def test_vibrato_grows_with_main():
    """THE expression. Main must add none at the bottom and a lot at the top,
    with a clean handover from the level stage."""
    print("vibrato vs Main")
    for m in (0, 1000, 2000, 2500, 3200, 4095):
        rate, cents = vibrato_for(m, 0)
        print(f"        main {m:4d} -> {cents/16:5.2f} cents at {rate/256:.1f}Hz")
    # Derived from VIB_ONSET rather than hard-coded, so moving the onset does
    # not silently leave these testing the wrong place.
    check("no vibrato below the onset", vibrato_for(VIB_ONSET - 1, 0)[1], 0)
    check("still none exactly at the onset", vibrato_for(VIB_ONSET, 0)[1], 0)
    check_true("and it is genuinely on by a quarter of the way past",
               vibrato_for(VIB_ONSET + (4095 - VIB_ONSET) // 4, 0)[1] > 16,
               f"{vibrato_for(VIB_ONSET + (4095-VIB_ONSET)//4, 0)[1]/16:.1f} cents")
    check_true("full depth at the top", vibrato_for(4095, 0)[1] >= VIB_DEPTH_WIDE_Q4 - 16,
               f"{vibrato_for(4095,0)[1]/16:.1f} cents")
    prev = -1
    bad = []
    for m in range(4096):
        c = vibrato_for(m, 0)[1]
        if c < prev:
            bad.append(m)
        prev = c
    check("depth never goes backwards", bad, [])


def test_x_morphs_character():
    """X must genuinely move BOTH rate and depth, through three anchors."""
    print("vibrato vs X")
    pts = []
    for x in (0, 1024, 2048, 3072, 4095):
        rate, cents = vibrato_for(4095, x)
        pts.append((x, rate / 256.0, cents))
        print(f"        X {x:4d} -> {cents/16:5.2f} cents at {rate/256:.1f}Hz")

    check_true("CCW is fast and wide", pts[0][1] > 7.5 and pts[0][2] >= 45 * 16,
               f"{pts[0][1]:.1f}Hz {pts[0][2]/16:.1f}c")
    check_true("middle is fast and tight", pts[2][1] > 7.5 and pts[2][2] <= 12 * 16,
               f"{pts[2][1]:.1f}Hz {pts[2][2]/16:.1f}c")
    check_true("CW is slow and wide", pts[-1][1] < 3.5 and pts[-1][2] >= 45 * 16,
               f"{pts[-1][1]:.1f}Hz {pts[-1][2]/16:.1f}c")


def test_x_tilts_volume():
    print("level tilt vs X")
    lo, hi = x_volume_boost(0), x_volume_boost(4095)
    print(f"        X 0 -> +{lo},  X 4095 -> +{hi}")
    check("no boost at CCW", lo, 0)
    check_true("a small boost at CW", 400 < hi < 1000, f"+{hi}")


def test_fold_adds_harmonics():
    """Audio Out 2 must go somewhere audible as X rises."""
    print("wavefold vs X")
    f0 = 220.0
    pts = []
    for x in (0, 1024, 2048, 3072, 4095):
        _, folded, _ = run(f0, 3000, x)
        b = brightness(folded, f0)
        pts.append((x, b))
        h = harmonics(folded, f0, 6)
        m = max(h)
        print(f"        X {x:4d} -> centroid {b:5.2f}   "
              + " ".join(f"{v/m:4.2f}" for v in h))
    check_true("X=0 is a pure sine", pts[0][1] < 1.05, f"centroid {pts[0][1]:.2f}")
    check_true("X=max is much richer", pts[-1][1] > 2.0,
               f"centroid {pts[-1][1]:.2f}")
    # Monotonic, and this is why kFoldMax stops at 2048: past there the
    # centroid dips (3.60 -> 3.00) as a completing fold returns the
    # fundamental, and a knob that brightens then dulls reads as broken.
    check("richness rises with X, without dipping back",
          all(pts[i][1] >= pts[i-1][1] - 0.02 for i in range(1, len(pts))), True)


def test_outputs_agree():
    """The three shapes come from one oscillator, so they must stay locked.

    If they drifted, mixing Audio 1 and Audio 2 would comb-filter and the square
    on Pulse Out 2 would not line up with either.
    """
    print("output phase")
    f0 = 220.0
    sines, folds, squares = run(f0, 3000, x=1024)
    flips = [i for i in range(1, len(squares)) if squares[i] and not squares[i-1]]
    period = SR / f0
    gaps = [flips[i] - flips[i-1] for i in range(1, len(flips))]
    check_true("square runs at the oscillator's pitch",
               abs(sum(gaps)/len(gaps) - period) < 2,
               f"{sum(gaps)/len(gaps):.1f} vs {period:.1f} samples")
    check_true("fold shares the pitch",
               goertzel(folds, f0) > goertzel(folds, f0 * 1.5), "")


def test_no_dc():
    print("DC")
    for x in (0, 2048, 4095):
        s, f, _ = run(220.0, 4095, x)
        ms, mf = sum(s) / len(s), sum(f) / len(f)
        check_true(f"X={x}: sine and fold are DC-free",
                   abs(ms) < 30 and abs(mf) < 30, f"{ms:+.1f} / {mf:+.1f}")


def test_notes_start_without_a_click():
    """A new note must begin at zero, not wherever the oscillator was.

    A free-running phase puts a step of up to full scale on the first sample of
    every attack -- measured at 2671 counts against the ~200 a 220Hz sine moves
    per sample. The envelope cannot hide it, because even the fastest attack is
    applied after the oscillator.
    """
    print("clean attacks")
    f = Flute()
    f.set_pitch(220.0)
    f.fold = fold_for(2048)
    out = []
    for _ in range(2000):
        f.step(3000)
        out.append(f.sine)
    for _ in range(500):
        f.step(0)
        out.append(f.sine)
    for _ in range(200):
        f.step(3000)
        out.append(f.sine)

    seam = out[2495:2520]
    jump = max(abs(seam[i] - seam[i - 1]) for i in range(1, len(seam)))
    print(f"        largest jump at the seam: {jump}")
    # One sample of a 220Hz sine at this level moves ~200 counts, so anything
    # in that region is the waveform itself rather than a discontinuity.
    check_true("a new note starts without a step", jump < 400, f"{jump} counts")


def test_no_clipping():
    print("headroom")
    bad = []
    for x in (0, 2048, 4095):
        for lv in (1000, 3000, 4095):
            s, f, _ = run(220.0, lv, x, n=8000, skip=4000)
            pk = max(max(abs(v) for v in s), max(abs(v) for v in f))
            if pk > 8200:
                bad.append((x, lv, pk))
    check("nothing exceeds the output range", bad, [])


def grid():
    print("\nMain (down) x X (across):  rms / vibrato cents @ Hz\n")
    print(f"{'main':>6} " + "".join(f"{'X=' + str(x):>22}" for x in (0, 2048, 4095)))
    for m in (500, 1500, 2500, 3300, 4095):
        row = f"{m:6d} "
        for x in (0, 2048, 4095):
            lvl = min(4095, m + x_volume_boost(x))
            r = rms(run(220.0, lvl, x)[0])
            rate, cents = vibrato_for(m, x)
            row += f"{r:8.0f} /{cents/16:5.1f}c@{rate/256:.1f}Hz "
        print(row)


def main():
    if "--grid" in sys.argv:
        grid()
        return 0

    print("flutesim — model of flute.cpp\n")
    test_silence()
    test_dynamic_range()
    test_vibrato_grows_with_main()
    test_x_morphs_character()
    test_x_tilts_volume()
    test_fold_adds_harmonics()
    test_outputs_agree()
    test_no_dc()
    test_notes_start_without_a_click()
    test_no_clipping()

    print()
    if FAILURES:
        print(f"{len(FAILURES)} FAILED: {', '.join(FAILURES)}")
        return 1
    print("all green")
    return 0


if __name__ == "__main__":
    sys.exit(main())
