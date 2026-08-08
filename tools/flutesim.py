#!/usr/bin/env python3
"""flutesim.py — a model of flute.cpp, and the measurements behind its constants.

A LINE-BY-LINE port. If flute.cpp changes, change this — or delete it rather
than let it drift.

---------------------------------------------------------------------------
WHAT THIS FILE EXISTS TO PREVENT
---------------------------------------------------------------------------

v1 shipped a jet-driven waveguide whose feedback term contained no breath, so
the bore drove itself forever. Every symptom from the first hardware session
came from that one line: "super metallic", and the breath knob never reaching
silence.

The v1 model PASSED. Its assertions checked tuning, stability, DC and harmonic
presence — none of which noticed that the instrument was ignoring the breath
knob. Worse, one assertion was actively too lenient: `E(f0) > 0.20 * E(2f0)`
passed a note whose octave was FIVE TIMES louder than its fundamental.

So the assertions below are written against the symptoms a player would report,
not against the internals:

    silence          zero breath must be EXACTLY zero out
    dynamic range    the knob must span at least 30:1 in level
    monotonic        more breath must always mean more level
    brightness       more breath must also mean brighter
    character        X must move air content across a wide, audible range
    fundamental      the note played must be the note asked for, loudest

Any one of those would have caught v1 before it was ever flashed.

Run:  python tools/flutesim.py
      python tools/flutesim.py --grid    print the breath x X table
"""

import math
import sys

SR = 48000

NOISE_LP_Q15 = 9000
CUT_MIN, CUT_MAX = 2600, 13000
RES_MIN, RES_MAX = 9000, 13000
AIR_MAX, AIR_MIN = 3450, 1800
AIR_GAIN = 2048
AIR_BREATH_TILT = 700
DRIVE_MIN, DRIVE_SPAN = 4000, 20000
DC_POLE_Q15 = 32735
MUTE_CUT = 200

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


def inc_for(hz):
    return int(round(hz * 4294967296.0 / SR)) & 0xFFFFFFFF


# ---------------------------------------------------------------------------
# The port
# ---------------------------------------------------------------------------

def timbre_for(breath, x):
    """flute.cpp TimbreFor()."""
    b = max(0, min(4095, breath))
    x = max(0, min(4095, x))
    air = AIR_MAX - (((AIR_MAX - AIR_MIN) * x) >> 12)
    air -= (AIR_BREATH_TILT * b) >> 12
    if air < 0:
        air = 0
    cut = CUT_MIN + ((7000 * b) >> 12) + ((3000 * x) >> 12)
    if cut > CUT_MAX:
        cut = CUT_MAX
    res = RES_MIN + (((RES_MAX - RES_MIN) * x) >> 12)
    drive = DRIVE_MIN + ((DRIVE_SPAN * b) >> 12)
    return air, cut, res, drive


class Flute:
    def __init__(self, seed=0x1234567):
        self.ph = 0
        self.inc = 0
        self.rng = seed
        self.n1 = self.n2 = 0
        self.b1 = self.b2 = 0
        self.dcx = self.dcy = 0
        self.last_noise = 0
        self.muted = False
        self.air, self.cut, self.res, self.drive = AIR_MIN, CUT_MIN, RES_MIN, DRIVE_MIN

    def set_pitch(self, hz):
        self.inc = inc_for(hz)

    def set_timbre(self, air, cut, res, drive):
        self.air = max(0, min(4096, air))
        cut = max(512, min(CUT_MAX, cut))
        self.res, self.drive = res, drive
        self.cut = MUTE_CUT if self.muted else cut

    def mute(self):
        self.muted = True
        self.cut = MUTE_CUT

    def unmute(self):
        self.muted = False

    def step(self, breath):
        if breath <= 0:
            self.last_noise = 0
            return 0

        self.ph = (self.ph + self.inc) & 0xFFFFFFFF
        tone = fast_sin(self.ph) >> 4

        d = (tone * self.drive) >> 12
        d = max(-4096, min(4096, d))
        d2 = (d * d) >> 12
        d3 = (d2 * d) >> 12
        tone = d - ((d3 * 21845) >> 16)

        self.rng ^= (self.rng << 13) & 0xFFFFFFFF
        self.rng ^= self.rng >> 17
        self.rng ^= (self.rng << 5) & 0xFFFFFFFF
        white = ((self.rng >> 17) - 16384) >> 2
        self.n1 += ((white - self.n1) * NOISE_LP_Q15) >> 15
        self.n2 += ((self.n1 - self.n2) * NOISE_LP_Q15) >> 15
        air = (self.n2 * AIR_GAIN) >> 12
        self.last_noise = (air * breath) >> 12

        src = (tone * (4096 - self.air) + air * self.air) >> 12

        self.b1 += (((src - self.b1) + (((self.b1 - self.b2) * self.res) >> 15))
                    * self.cut) >> 15
        self.b2 += ((self.b1 - self.b2) * self.cut) >> 15
        out = self.b2

        out = (out * breath) >> 12

        y = out - self.dcx + ((self.dcy * DC_POLE_Q15 + 16384) >> 15)
        self.dcx = out
        self.dcy = y
        return clamp(y)


def run(hz, breath, x=2048, n=20000, skip=10000, mute=False):
    f = Flute()
    f.set_pitch(hz)
    if mute:
        f.mute()
    f.set_timbre(*timbre_for(breath, x))
    out = []
    for i in range(n):
        v = f.step(breath)
        if i >= skip:
            out.append(v)
    return out


# ---------------------------------------------------------------------------
# Measurement
# ---------------------------------------------------------------------------

def rms(x):
    return math.sqrt(sum(v * v for v in x) / len(x)) if x else 0.0


def goertzel(x, f):
    """Energy at exactly f. Unambiguous, unlike autocorrelation, which locked
    onto harmonics AND sub-harmonics while v1 was being built and produced
    confident wrong answers both times."""
    w = 2 * math.pi * f / SR
    c = 2 * math.cos(w)
    s1 = s2 = 0.0
    for v in x:
        s0 = v + c * s1 - s2
        s2, s1 = s1, s0
    return math.sqrt(abs(s1 * s1 + s2 * s2 - c * s1 * s2)) / len(x)


def harmonics(out, f0, n=8):
    return [goertzel(out, f0 * k) for k in range(1, n + 1)]


def centroid(out, f0, n=9):
    h = harmonics(out, f0, n)
    return sum((i + 1) * v for i, v in enumerate(h)) / max(sum(h), 1e-9)


def tonal_fraction(out, f0):
    """1.0 = pure tone, 0 = pure noise."""
    tot = sum(v * v for v in out) / len(out)
    harm = sum(goertzel(out, f0 * k) ** 2 for k in range(1, 25)) * 2
    return harm / max(tot, 1e-9)


# ---------------------------------------------------------------------------
# Tests — written against what a player would notice
# ---------------------------------------------------------------------------

def test_silence():
    """THE assertion v1 needed and did not have.

    v1's excitation did not depend on breath, so the bore drove itself and the
    card never went quiet however far the knob came down. Nothing in its model
    ever checked.
    """
    print("silence")
    out = run(220.0, 0, n=8000, skip=0)
    check("zero breath is EXACTLY zero", max(abs(v) for v in out), 0)

    # And it must stay silent — no tail, no ring, no creep.
    f = Flute()
    f.set_pitch(220.0)
    f.set_timbre(*timbre_for(3000, 2048))
    for _ in range(8000):
        f.step(3000)
    tail = [f.step(0) for _ in range(8000)]
    check("and it stops instantly when breath is removed",
          max(abs(v) for v in tail), 0)


def test_dynamic_range():
    """v1 spanned 1.2:1 across the whole knob and got QUIETER at the top."""
    print("dynamic range")
    levels = [(br, rms(run(220.0, br))) for br in
              (100, 300, 700, 1400, 2400, 3400, 4095)]
    for br, r in levels:
        print(f"        breath {br:4d} -> rms {r:8.1f}")
    lo = levels[0][1]
    hi = levels[-1][1]
    check_true("the knob spans at least 30:1", hi / max(lo, 1e-9) >= 30,
               f"{hi/max(lo,1e-9):.0f}:1")

    rising = all(levels[i][1] > levels[i - 1][1] for i in range(1, len(levels)))
    check("more breath is always louder", rising, True)


def test_brightness_tracks_breath():
    """Blowing harder must sound harder, not just louder — otherwise the knob
    is a volume control wearing an instrument's name."""
    print("breath -> brightness")
    pts = [(br, centroid(run(220.0, br), 220.0)) for br in (400, 1400, 2600, 4095)]
    for br, c in pts:
        print(f"        breath {br:4d} -> centroid {c:5.2f}")
    check_true("brightness rises with breath", pts[-1][1] > pts[0][1] * 1.3,
               f"{pts[0][1]:.2f} -> {pts[-1][1]:.2f}")


def test_x_is_a_character_axis():
    """X must move the tone between genuinely airy and genuinely pure.

    The first attempt at this mapping swept air_mix 220..1900, which measured
    0.97 tonal at BOTH ends — three quarters of the knob doing nothing audible.
    The useful range turned out to be narrow and high.
    """
    print("X -> character")
    pts = [(x, tonal_fraction(run(220.0, 2200, x), 220.0))
           for x in (0, 1365, 2730, 4095)]
    for x, t in pts:
        print(f"        X {x:4d} -> tonal {t:5.3f}")
    # Reported from hardware: "breath is basically too loud all the time, next
    # to note". The air must be a texture UNDER the pitch, not level with it —
    # so the CCW end is breathy but still clearly pitched.
    check_true("fully CCW is airy but the note still leads",
               0.70 < pts[0][1] < 0.95, f"tonal {pts[0][1]:.3f}")
    check_true("fully CW is nearly pure", pts[-1][1] > 0.92,
               f"tonal {pts[-1][1]:.3f}")
    check_true("and the sweep is monotonic",
               all(pts[i][1] > pts[i - 1][1] for i in range(1, len(pts))), "")


def test_plays_the_right_note():
    """v1's octave silently took over above MIDI 60 — at MIDI 72 the second
    harmonic was 2.4x the fundamental, so the card played an octave above what
    CV Out 1 reported. Its own assertion allowed a 5:1 octave and passed."""
    print("pitch")
    worst = (99.0, None)
    for n in range(36, 92, 5):
        f0 = midi_hz(n)
        out = run(f0, 2500)
        h = harmonics(out, f0, 2)
        r = h[0] / max(h[1], 1e-9)
        if r < worst[0]:
            worst = (r, n)
    check_true("the fundamental is always the loudest partial",
               worst[0] > 2.0, f"worst {worst[0]:.1f}x at MIDI {worst[1]}")

    # And it is the note that was asked for.
    worst_c = 0.0
    for n in (36, 48, 60, 72, 84, 91):
        f0 = midi_hz(n)
        out = run(f0, 2500)
        best = max(((goertzel(out, f0 * r), r) for r in
                    (0.25, 0.5, 1.0, 2.0, 4.0)))
        if best[1] != 1.0:
            check(f"MIDI {n} plays its own pitch", best[1], 1.0)
        # fine pitch, by peak search near the target
        f, e = f0, -1
        ff = f0 * 0.97
        while ff < f0 * 1.03:
            g = goertzel(out, ff)
            if g > e:
                e, f = g, ff
            ff *= 1.0005
        worst_c = max(worst_c, abs(1200 * math.log2(f / f0)))
    check_true("in tune to 5 cents across the range", worst_c < 5.0,
               f"{worst_c:.2f}c")


def test_not_metallic():
    """The reported symptom, as a number.

    v1 measured h2 = 1.14x the fundamental, h5 = 0.64, h8 = 0.41 — a spiky
    spectrum, which is what "metallic" sounds like. A flute is nearly a sine
    with a little h2/h3.
    """
    print("tone")
    out = run(220.0, 2500)
    h = harmonics(out, 220.0, 8)
    for i, v in enumerate(h):
        print(f"        h{i+1} {v/h[0]:6.3f} " + "#" * int(30 * v / h[0]))
    check_true("the fundamental dominates every harmonic",
               all(v < h[0] for v in h[1:]),
               f"loudest partial is {max(h[1:])/h[0]:.2f}x h1")
    check_true("no harsh upper spectrum",
               all(v < 0.5 * h[0] for v in h[4:]),
               f"max h5+ is {max(h[4:])/h[0]:.2f}x h1")


def test_stability():
    print("stability")
    bad = []
    for n in (36, 60, 91):
        for br in (200, 1200, 2500, 4095):
            for x in (0, 2048, 4095):
                out = run(midi_hz(n), br, x, n=8000, skip=4000)
                pk = max(abs(v) for v in out)
                if pk > 30000:
                    bad.append((n, br, x, pk))
    check("nothing clips at any corner", bad, [])


def test_dc():
    print("DC blocker")
    out = run(220.0, 4095)
    mean = sum(out) / len(out)
    check_true("output is DC-free at full breath", abs(mean) < 30,
               f"mean {mean:+.1f}")


def test_mute():
    print("chiff stop")
    quiet = rms(run(220.0, 3000, mute=True))
    loud = rms(run(220.0, 3000))
    print(f"        open {loud:.0f} -> muted {quiet:.0f}")
    check_true("muting drops the level hard", quiet < loud * 0.25,
               f"{100*quiet/max(loud,1e-9):.0f}% of open")


def grid():
    print("\nbreath (down) x X (across):  rms / tonal-fraction\n")
    print(f"{'breath':>7} " + "".join(f"{'X=' + str(x):>16}"
                                      for x in (0, 2048, 4095)))
    for br in (300, 1000, 2000, 3000, 4095):
        row = f"{br:7d} "
        for x in (0, 2048, 4095):
            out = run(220.0, br, x)
            row += f"{rms(out):8.0f}/{tonal_fraction(out, 220.0):5.2f} "
        print(row)


def main():
    if "--grid" in sys.argv:
        grid()
        return 0

    print("flutesim — model of flute.cpp\n")
    test_silence()
    test_dynamic_range()
    test_brightness_tracks_breath()
    test_x_is_a_character_axis()
    test_plays_the_right_note()
    test_not_metallic()
    test_stability()
    test_dc()
    test_mute()

    print()
    if FAILURES:
        print(f"{len(FAILURES)} FAILED: {', '.join(FAILURES)}")
        return 1
    print("all green")
    return 0


if __name__ == "__main__":
    sys.exit(main())
