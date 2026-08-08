#!/usr/bin/env python3
"""pitchsim.py — verifies the pitch path against exact arithmetic.

Cheap to run, and it catches the class of bug that is nearly impossible to hear:
a single mistyped LUT entry is one note that is wrong and nothing else.

The important assertion is the CROSS-CHECK: the internal bore is tuned by a
delay length and CV Out 1 by a millivolt value, computed on two entirely
separate code paths. If they disagree, the card plays one note and tells the
rest of the rack a different one. Nothing else in the system would notice.

Run:  python tools/pitchsim.py
"""

import re
import sys
import os

SR = 48000.0
LO_NOTE, HI_NOTE = 36, 91
MV_PER_SEMI_Q8 = 21333
MAX_ROOT = 12
MAX_DEGREE = 15          # 15-mode uses degrees 0..14
# v1 held delay lengths here, for a waveguide. The voice is an oscillator
# now, so the table holds phase INCREMENTS and the octave shift goes the other
# way -- increments double where delays halved.

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

FAILURES = []


def check(name, got, want):
    if got == want:
        print(f"  ok    {name}")
    else:
        print(f"  FAIL  {name}\n          got  {got}\n          want {want}")
        FAILURES.append(name)


def check_within(name, got, want, tol, unit=""):
    if abs(got - want) <= tol:
        print(f"  ok    {name}  ({got:+.3f}{unit})")
    else:
        print(f"  FAIL  {name}\n          got {got}{unit}, want {want}+/-{tol}{unit}")
        FAILURES.append(name)


def midi_hz(n):
    return 440.0 * 2 ** ((n - 69) / 12.0)


def cents(f_got, f_want):
    import math
    return 1200.0 * math.log2(f_got / f_want)


# ---------------------------------------------------------------------------
# Parse the real table out of pitch.cpp, so this tests the shipped values
# rather than a copy that could drift.
# ---------------------------------------------------------------------------

def load_table():
    src = open(os.path.join(ROOT, "pitch.cpp"), encoding="utf-8").read()
    body = src[src.index("kIncQ32Base[12]"):]
    body = body[body.index("{"):body.index("};")]
    vals = [int(m) for m in re.findall(r"(\d+)u", body)]
    return vals


def load_scales():
    """Pull the scale tables out of scales.h, same reasoning as load_table."""
    src = open(os.path.join(ROOT, "scales.h"), encoding="utf-8").read()
    body = src[src.index("kScales[kNumScales]"):]
    body = body[body.index("{"):body.index("\n};")]
    out = []
    for m in re.finditer(r"\{\s*(\d+),\s*\{([0-9,\s]+)\}\s*\}", body):
        ln = int(m.group(1))
        steps = [int(x) for x in m.group(2).split(",") if x.strip()]
        out.append((ln, steps[:ln]))
    return out


# ---------------------------------------------------------------------------
# The C++ functions, mirrored
# ---------------------------------------------------------------------------

TABLE = load_table()


def semi_to_inc_q32(semi):
    semi = max(LO_NOTE, min(HI_NOTE, semi))
    n = semi - LO_NOTE
    oct_, st = n // 12, n % 12
    return TABLE[st] << oct_


CENT_Q24 = 9691


def apply_fine_cents(inc, c):
    # Note the SIGN: sharpening RAISES an increment, where it shortened a delay.
    x = c * CENT_Q24
    lin = (inc * x) >> 24
    quad = (((x * x) >> 24) * inc) >> 25
    return inc + lin + quad


def semis_to_mv(semi):
    return (semi * MV_PER_SEMI_Q8 + 128) >> 8


def pitch_mv(semi, fine_cents):
    # Referenced to the root, so the root leaves the card at 0V.
    return semis_to_mv(semi - LO_NOTE) + ((fine_cents * 853 + 512) >> 10)


def quantize_note(root, scale, degree):
    ln, steps = scale
    octave = degree // ln
    note = root + octave * 12 + steps[degree % ln]
    return max(0, min(127, note))


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def inc_to_hz(inc):
    return inc * SR / 4294967296.0


def test_table_exact():
    print("increment table")
    check("table has 12 entries", len(TABLE), 12)
    worst, worst_i = 0.0, -1
    for i in range(12):
        want = round(midi_hz(LO_NOTE + i) * 4294967296.0 / SR)
        if TABLE[i] != want:
            check(f"entry {i} exact", TABLE[i], want)
        e = abs(cents(inc_to_hz(TABLE[i]), midi_hz(LO_NOTE + i)))
        if e > worst:
            worst, worst_i = e, i
    check("every entry matches exact arithmetic",
          all(TABLE[i] == round(midi_hz(LO_NOTE + i) * 4294967296.0 / SR)
              for i in range(12)), True)
    print(f"        worst rounding error: {worst:.4f} cents (entry {worst_i})")


def test_octave_shift():
    """The shift-per-octave trick must hold across the whole range."""
    print("octave shifting")
    worst, worst_n = 0.0, -1
    for n in range(LO_NOTE, HI_NOTE + 1):
        e = abs(cents(inc_to_hz(semi_to_inc_q32(n)), midi_hz(n)))
        if e > worst:
            worst, worst_n = e, n
    print(f"        worst error over MIDI {LO_NOTE}..{HI_NOTE}: "
          f"{worst:.4f} cents (note {worst_n})")
    check_within("worst octave-shift error under 1 cent", worst, 0.0, 1.0, "c")


def test_increment_fits():
    """A phase increment must not overflow uint32_t, or the note wraps to a
    wrong pitch instead of simply being too high."""
    print("increment bounds")
    top = semi_to_inc_q32(HI_NOTE)
    top = apply_fine_cents(top, 100)          # sharpest fine tune too
    print(f"        highest increment: {top} ({inc_to_hz(top):.1f}Hz)")
    check("top note fits in uint32", top < 2**32, True)
    # And it must stay under Nyquist, or the oscillator aliases.
    check(f"top note is under Nyquist ({SR/2:.0f}Hz)",
          inc_to_hz(top) < SR / 2, True)


def test_fine_cents():
    print("fine tune")
    import math
    worst = 0.0
    for n in (36, 60, 91):
        d0 = semi_to_inc_q32(n)
        for c in range(-100, 101, 5):
            d = apply_fine_cents(d0, c)
            got = cents(inc_to_hz(d), inc_to_hz(d0))
            worst = max(worst, abs(got - c))
    print(f"        worst deviation from ideal: {worst:.3f} cents")
    check_within("linear approximation good to 0.5 cents", worst, 0.0, 0.5, "c")

    check("zero cents is a no-op",
          apply_fine_cents(semi_to_inc_q32(60), 0), semi_to_inc_q32(60))


def test_cv_and_bore_agree():
    """THE cross-check: the two pitch paths must name the same note.

    The voice is tuned by a phase increment; CV Out 1 by a millivolt value. They
    share no arithmetic. If they drift apart the card plays one pitch and tells
    the rack another, and nothing in the system would flag it.
    """
    print("voice vs CV agreement")
    worst, worst_at = 0.0, None
    for n in range(LO_NOTE, HI_NOTE + 1):
        for c in (-100, -37, 0, 37, 100):
            f_bore = inc_to_hz(apply_fine_cents(semi_to_inc_q32(n), c))
            # CV is 1V/oct referenced to the scale root at kBaseNote = 36 = 3V.
            mv = pitch_mv(n, c)
            f_cv = midi_hz(LO_NOTE) * 2 ** (mv / 1000.0)
            e = abs(cents(f_bore, f_cv))
            if e > worst:
                worst, worst_at = e, (n, c)
        # end fine loop
    print(f"        worst disagreement: {worst:.3f} cents at "
          f"note {worst_at[0]}, {worst_at[1]:+d} cents")
    check_within("bore and CV agree within 5 cents", worst, 0.0, 5.0, "c")


def load_uint_array(fname, decl):
    """Pull a constexpr uint8_t array out of a header, so this tests the
    shipped values rather than a copy that could drift."""
    src = open(os.path.join(ROOT, fname), encoding="utf-8").read()
    body = src[src.index(decl):]
    body = body[body.index("{"):body.index("};")]
    # Strip // comments first: the entries carry trailing notes that contain
    # numbers ("span 24"), which would otherwise be parsed as array elements.
    body = re.sub(r"//[^\n]*", "", body)
    return [int(x) for x in re.findall(r"\d+", body)]


def test_no_cv_clipping():
    """kMaxRoot must not let any scale run the CV output past 6V.

    This is the bug NIBBLE's own keys.h warns about, one mode later: its
    kMaxRoot of 36 was derived for TEN degrees and would silently clip the top
    of a fifteen-degree arpeggio.
    """
    print("CV headroom and bore fit")
    scales = load_scales()
    check("parsed 12 scales", len(scales), 12)

    max_root = load_uint_array("pitch.h", "kMaxRootFor[12]")
    usable = load_uint_array("pitch.h", "kUsableDegrees[12]")
    check("kMaxRootFor has 12 entries", len(max_root), 12)
    check("kUsableDegrees has 12 entries", len(usable), 12)

    # 1. Nothing may exceed the 6V rail.
    worst, worst_at = -99999, None
    for si, sc in enumerate(scales):
        for root in range(0, max_root[si] + 1):
            for deg in range(usable[si]):
                n = quantize_note(LO_NOTE + root, sc, deg)
                mv = pitch_mv(n, 100)          # sharpest fine tune too
                if mv > worst:
                    worst, worst_at = mv, (si, root, deg, n)
    print(f"        highest CV: {worst}mV (scale {worst_at[0]}, root +{worst_at[1]}, "
          f"degree {worst_at[2]}, MIDI {worst_at[3]})")
    check("top of every scale stays under 6000mV", worst <= 6000, True)

    # 2. THE SILENT ONE. Past the bore's top note the delay clamps while the CV
    #    keeps climbing, so the card plays one pitch and tells the rack another.
    #    Nothing in the system would flag it, so it is asserted here.
    over = []
    under = []
    for si, sc in enumerate(scales):
        for root in range(0, max_root[si] + 1):
            for deg in range(usable[si]):
                n = quantize_note(LO_NOTE + root, sc, deg)
                if n > HI_NOTE:
                    over.append((si, root, deg, n))
                if n < LO_NOTE:
                    under.append((si, root, deg, n))
    check("no reachable note is above the bore's top", over, [])
    check("no reachable note is below the bore's floor", under, [])

    # 3. kMaxRootFor must be as generous as it can be: a scale with headroom
    #    should not be denied transposition it could have had.
    tight = []
    for si, sc in enumerate(scales):
        r = max_root[si] + 1
        if r > 12:
            continue
        fits = all(quantize_note(LO_NOTE + r, sc, d) <= HI_NOTE
                   for d in range(usable[si]))
        if fits:
            tight.append((si, max_root[si]))
    check("no scale is transposed less than it could be", tight, [])

    print(f"        usable degrees per scale: {usable}")
    print(f"        max transpose per scale:  {max_root}")


def main():
    print("pitchsim — pitch path verification\n")
    test_table_exact()
    test_octave_shift()
    test_increment_fits()
    test_fine_cents()
    test_cv_and_bore_agree()
    test_no_cv_clipping()

    print()
    if FAILURES:
        print(f"{len(FAILURES)} FAILED: {', '.join(FAILURES)}")
        return 1
    print("all green")
    return 0


if __name__ == "__main__":
    sys.exit(main())
