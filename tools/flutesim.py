#!/usr/bin/env python3
"""flutesim.py — a model of flute.cpp, and the measurements behind its constants.

A LINE-BY-LINE port of flute.cpp. If that file changes, change this one — or
delete it rather than let it drift.

This model is the reason the shipped voice works. Three findings came out of it
that no amount of reading would have produced:

  1. The reflection must INVERT. With a non-inverting reflection the second
     harmonic measured ZERO at every breath level — a closed pipe, which cannot
     overblow to an octave however the jet is tuned.

  2. Overblowing is NOT emergent from the cubic. Driving the jet harder shifts
     the harmonic balance but not the preferred mode. Forcing it produced
     non-harmonic modes at 2.23x and 27x, which are chaos rather than registers.
     The card switches register explicitly instead.

  3. The loop period is 1.5 delay traversals, not 1. Measured f*delay is a
     constant 32000 = 48000/1.5. Assuming 1 puts the instrument a fourth sharp.

Run:  python tools/flutesim.py           (fast: the assertions)
      python tools/flutesim.py --sweep   (slow: the full characterisation)
"""

import math
import sys

SR = 48000
DELAY_SIZE = 1024
DELAY_MASK = DELAY_SIZE - 1

JET_RATIO_Q16   = 32768        # 0.5
JET_FEEDBACK_Q12 = 6144        # 1.5
BREATH_OFFSET_Q12 = 4096
LOOP_GAIN_Q15   = 32000
DAMP_DEFAULT    = 12000
DC_POLE_Q15     = 32735
NOISE_LP_Q15    = 9000

LO_NOTE, HI_NOTE = 36, 75
LOOP_FACTOR = 1.5

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


# ---------------------------------------------------------------------------
# The port
# ---------------------------------------------------------------------------

def clamp_s16(v):
    return max(-32768, min(32767, v))


def jet_cubic(x):
    if x > 6144:
        x = 6144
    if x < -6144:
        x = -6144
    x2 = (x * x) >> 12
    x3 = (x2 * x) >> 12
    return x - ((x3 * 21845) >> 16)


def midi_hz(n):
    return 440.0 * 2 ** ((n - 69) / 12.0)


def delay_q16_for(f):
    """Loop delay, Q16 samples. NOTE THE 1.5 — see the module docstring."""
    return int(round(SR / (LOOP_FACTOR * f) * 65536))


class Bore:
    def __init__(self, delay_q16, damp=DAMP_DEFAULT, gain=LOOP_GAIN_Q15,
                 noise=0, seed=0x1234567):
        self.buf = [0] * DELAY_SIZE
        self.w = 0
        self.delay = delay_q16
        self.jet_off = max(2 << 16, (delay_q16 * JET_RATIO_Q16) >> 16)
        self.damp = damp
        self.gain = gain
        self.noise = noise
        self.lp = 0
        self.dcx = 0
        self.dcy = 0
        self.nlp = 0
        self.rng = seed

    def _rand(self):
        s = self.rng
        s ^= (s << 13) & 0xFFFFFFFF
        s ^= s >> 17
        s ^= (s << 5) & 0xFFFFFFFF
        self.rng = s & 0xFFFFFFFF
        return (self.rng >> 17) - 16384

    def _read(self, off):
        pos = ((self.w << 16) - off) & ((DELAY_SIZE << 16) - 1)
        ip = (pos >> 16) & DELAY_MASK
        mu = pos & 0xFFFF
        a = self.buf[ip]
        b = self.buf[(ip + 1) & DELAY_MASK]
        return a + (((b - a) * mu) >> 16)

    def step(self, breath):
        bore = self._read(self.delay)
        jet_tap = self._read(self.jet_off)

        self.lp += ((bore - self.lp) * self.damp) >> 15
        refl = -((self.lp * self.gain) >> 15)

        white = self._rand()
        self.nlp += ((white - self.nlp) * NOISE_LP_Q15) >> 15
        noise = (((self.nlp * self.noise) >> 15) * breath) >> 12

        offset = (breath * BREATH_OFFSET_Q12) >> 12
        x = offset + noise - ((jet_tap * JET_FEEDBACK_Q12) >> 12)

        self.buf[self.w] = clamp_s16(jet_cubic(x) + refl)
        self.w = (self.w + 1) & DELAY_MASK

        # +16384 rounds the shift. Without it the blocker's own truncation bias
        # accumulates to a stable -497 offset. See flute.cpp.
        y = bore - self.dcx + ((self.dcy * DC_POLE_Q15 + 16384) >> 15)
        self.dcx = bore
        self.dcy = y
        return y


def run(delay_q16, breath, n=24000, skip=12000, **kw):
    b = Bore(delay_q16, **kw)
    out = []
    for i in range(n):
        v = b.step(breath)
        if i >= skip:
            out.append(v)
    return out


# ---------------------------------------------------------------------------
# Measurement
# ---------------------------------------------------------------------------

def goertzel(x, f):
    """Energy at exactly f. Unambiguous, unlike autocorrelation, which locks
    onto harmonics and sub-harmonics and produced three separate false
    conclusions while this voice was being built."""
    w = 2 * math.pi * f / SR
    c = 2 * math.cos(w)
    s1 = s2 = 0.0
    for v in x:
        s0 = v + c * s1 - s2
        s2 = s1
        s1 = s0
    return math.sqrt(abs(s1 * s1 + s2 * s2 - c * s1 * s2)) / len(x)


def rms(x):
    return math.sqrt(sum(v * v for v in x) / len(x)) if x else 0.0


def harmonics(out, f0, n=4):
    return [goertzel(out, f0 * k) for k in range(1, n + 1)]


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def test_tuning_law():
    """f * delay must be a constant, and that constant must be SR/1.5.

    This is the measurement that fixes the loop geometry. Getting it wrong is a
    uniform perfect-fourth error, which looks like a bad tuning constant and is
    actually a misunderstanding of the topology.
    """
    print("tuning law")
    prods = []
    for samples in (90, 120, 180, 240):
        out = run(samples << 16, 2500)
        # Period by peak-to-peak spacing over a long window: robust here because
        # we only need the product, not a precise pitch.
        f = dominant_freq(out, lo=40, hi=2000)
        prods.append(f * samples)
    spread = (max(prods) - min(prods)) / max(prods)
    print(f"        f*delay over 4 lengths: "
          f"{', '.join(f'{p:.0f}' for p in prods)}")
    check_true("f*delay is constant (within 5%)", spread < 0.05,
               f"spread {spread*100:.1f}%")
    mean = sum(prods) / len(prods)
    want = SR / LOOP_FACTOR
    check_true(f"...and equals SR/{LOOP_FACTOR} = {want:.0f}",
               abs(mean - want) / want < 0.05, f"got {mean:.0f}")


def dominant_freq(out, lo=40.0, hi=4000.0):
    """Coarse spectral peak by Goertzel over a log grid, then refined.

    Deliberately NOT autocorrelation: ACF locked onto the 4th harmonic and onto
    sub-harmonics at different points while this file was being written, and
    produced confident wrong answers both times.
    """
    if rms(out) < 20:
        return 0.0
    best_f, best_e = 0.0, -1.0
    f = lo
    while f < hi:
        e = goertzel(out, f)
        if e > best_e:
            best_e, best_f = e, f
        f *= 1.02
    # refine
    step = best_f * 0.02
    for _ in range(3):
        for cand in (best_f - step, best_f, best_f + step):
            if cand <= 0:
                continue
            e = goertzel(out, cand)
            if e > best_e:
                best_e, best_f = e, cand
        step /= 3
    return best_f


def test_even_harmonics_exist():
    """The reflection sign check, stated as a property rather than a constant.

    A non-inverting reflection makes this a closed pipe: h2 measures ZERO and
    the instrument can never overblow to an octave. This assertion is the
    tripwire on that whole class of mistake.
    """
    print("open-pipe character")
    f0 = 220.0
    out = run(delay_q16_for(f0), 2500)
    h = harmonics(out, f0)
    ratio = h[1] / max(h[0], 1e-9)
    print(f"        h1={h[0]:.1f} h2={h[1]:.1f} h3={h[2]:.1f} h4={h[3]:.1f}")
    check_true("second harmonic is present (open pipe, not closed)",
               ratio > 0.05, f"h2/h1 = {ratio:.2f}")


def test_range_fundamental():
    """Across the shipped range, the fundamental must lead its own octave.

    This is what kPitchHiNote is derived from. The collapse above it is abrupt:
    E(f0)/E(2f0) goes 0.29 -> 0.08 -> 0.01 within four semitones, and past that
    the bore plays an octave above what CV Out 1 reports.
    """
    print("fundamental across the range")
    worst = (99.0, None)
    for n in range(LO_NOTE, HI_NOTE + 1, 3):
        f0 = midi_hz(n)
        out = run(delay_q16_for(f0), 2500)
        h = harmonics(out, f0, 2)
        r = h[0] / max(h[1], 1e-9)
        if r < worst[0]:
            worst = (r, n)
        if n % 9 == 0:
            print(f"        MIDI {n:2d} {f0:7.1f}Hz  E0/E2 = {r:5.2f}")
    check_true("fundamental leads its octave across MIDI 36..75",
               worst[0] > 0.20, f"worst {worst[0]:.2f} at MIDI {worst[1]}")

    # And the cliff is where we say it is.
    f0 = midi_hz(81)
    out = run(delay_q16_for(f0), 2500)
    h = harmonics(out, f0, 2)
    r = h[0] / max(h[1], 1e-9)
    check_true("...and has collapsed by MIDI 81 (why kPitchHiNote is 75)",
               r < 0.10, f"E0/E2 = {r:.2f}")


def test_tuning_accuracy():
    print("tuning accuracy")
    worst, worst_n = 0.0, None
    for n in range(LO_NOTE, HI_NOTE + 1, 6):
        f0 = midi_hz(n)
        out = run(delay_q16_for(f0), 2500)
        f = dominant_freq(out, lo=f0 * 0.7, hi=f0 * 1.4)
        c = abs(1200 * math.log2(f / f0)) if f > 0 else 999
        if c > worst:
            worst, worst_n = c, n
    print(f"        worst error {worst:.1f} cents at MIDI {worst_n}")
    check_true("in tune to 25 cents across the range", worst < 25.0,
               f"{worst:.1f}c")


def test_stability():
    """No corner of the parameter space may run away or fall silent."""
    print("stability")
    bad = []
    for n in (36, 48, 60, 72):
        for br in (600, 1500, 2500, 3500, 4095):
            for damp in (8000, 12000, 22000):
                out = run(delay_q16_for(midi_hz(n)), br, n=8000, skip=4000,
                          damp=damp)
                pk = max(abs(v) for v in out)
                if pk > 30000 or pk < 100:
                    bad.append((n, br, damp, pk))
    check("no corner clips or dies", bad, [])


def test_silence():
    print("silence")
    out = run(delay_q16_for(220.0), 0, n=8000, skip=4000)
    check_true("zero breath is silent", max(abs(v) for v in out) < 5,
               f"peak {max(abs(v) for v in out)}")


def test_dc():
    """The jet is asymmetric about the operating point, so without the blocker
    the loop accumulates DC until the delay line saturates and the voice dies."""
    print("DC blocker")
    out = run(delay_q16_for(220.0), 4095)
    mean = sum(out) / len(out)
    check_true("output is DC-free at full breath", abs(mean) < 30,
               f"mean {mean:+.1f}")


def sweep():
    """The full characterisation. Slow; run when a constant changes."""
    print("\n--- breath sweep at MIDI 57 (A3) ---")
    f0 = midi_hz(57)
    print(f"{'breath':>7} {'freq':>9} {'peak':>7} {'h2/h1':>7} {'h3/h1':>7}")
    for br in (500, 1000, 1500, 2000, 2500, 3000, 3500, 4095):
        out = run(delay_q16_for(f0), br)
        h = harmonics(out, f0, 3)
        f = dominant_freq(out, lo=f0 * 0.4, hi=f0 * 4)
        pk = max(abs(v) for v in out)
        print(f"{br:7d} {f:9.1f} {pk:7d} {h[1]/max(h[0],1e-9):7.2f} "
              f"{h[2]/max(h[0],1e-9):7.2f}")

    print("\n--- full range ---")
    print(f"{'MIDI':>5} {'target':>9} {'got':>9} {'cents':>8} {'E0/E2':>7}")
    for n in range(LO_NOTE, 85):
        f0 = midi_hz(n)
        out = run(delay_q16_for(f0), 2500)
        h = harmonics(out, f0, 2)
        f = dominant_freq(out, lo=f0 * 0.4, hi=f0 * 3)
        c = 1200 * math.log2(f / f0) if f > 0 else 0
        flag = "" if n <= HI_NOTE else "  (above kPitchHiNote)"
        print(f"{n:5d} {f0:9.2f} {f:9.2f} {c:+8.1f} "
              f"{h[0]/max(h[1],1e-9):7.2f}{flag}")


def main():
    if "--sweep" in sys.argv:
        sweep()
        return 0

    print("flutesim — model of flute.cpp\n")
    test_tuning_law()
    test_even_harmonics_exist()
    test_range_fundamental()
    test_tuning_accuracy()
    test_stability()
    test_silence()
    test_dc()

    print()
    if FAILURES:
        print(f"{len(FAILURES)} FAILED: {', '.join(FAILURES)}")
        return 1
    print("all green")
    return 0


if __name__ == "__main__":
    sys.exit(main())
