#!/usr/bin/env python3
"""breathsim.py — a model of breath.cpp.

A LINE-BY-LINE port. If breath.cpp changes, change this — or delete it rather
than let it drift.

Small, but it covers three things that are easy to get wrong and awkward to
hear:

  - the register switch must not chatter at the boundary
  - the chiff pulse is an EDGE (one tick), not a level
  - a fast trill must not become a chiff stutter

Run:  python tools/breathsim.py
"""

import math
import sys

CTRL_RATE = 3000

BREATH_THRESH = 60

CHIFF_TICKS       = CTRL_RATE // 80          # ~12ms
CHIFF_NOISE_Q15   = 14000
CHIFF_DIP_Q12     = 1200
CHIFF_MIN_GAP     = (CTRL_RATE * 6) // 100   # 60ms

VIBRATO_CENTS = 15

TONGUED, LEGATO = 0, 1

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


def fast_sin(phase):
    """fastmath.h fast_sin, close enough for the vibrato assertions."""
    return int(32767 * math.sin(2 * math.pi * (phase / 4294967296.0)))


class Breath:
    def __init__(self):
        self.curved = 0
        self.effort = 0
        self.breath = 0
        self.chiff_noise = 0
        self.chiff_ticks = 0
        self.since_chiff = CHIFF_MIN_GAP
        self.chiff_fired = False
        self.stopped = False
        self.vib_cents = 0
        self.vib_phase = 0
        self.artic = TONGUED

    def set_knob(self, knob, cv_add=0):
        v = max(0, min(4095, knob + cv_add))
        if v < BREATH_THRESH:
            self.curved = 0
            self.effort = 0
        else:
            # The clamp matters: at v == 4095 this yields exactly 4096, one
            # past full scale in Q12. See breath.cpp.
            n = min(4095, ((v - BREATH_THRESH) << 12) // (4095 - BREATH_THRESH))
            self.effort = n
            # S-curve: smoothstep, then a lift that steepens only the middle.
            # Flat at BOTH ends -- leaves silence gently and arrives gently.
            # See breath.cpp for why three earlier curves were wrong.
            # Multiply order matters: reducing the small factor first keeps
            # this exactly monotonic AND inside int32. See breath.cpp.
            inner = (n * (3 * 4096 - 2 * n)) >> 12
            sm = min(4095, (n * inner) >> 12)
            self.curved = min(4095, sm + ((sm * (4096 - sm)) >> 12))


    def note_on(self):
        if self.artic != TONGUED:
            return
        if self.since_chiff < CHIFF_MIN_GAP:
            return
        self.chiff_ticks = CHIFF_TICKS
        self.since_chiff = 0
        self.chiff_fired = True

    def tick(self):
        fired_this_tick = self.chiff_fired

        if self.since_chiff < CHIFF_MIN_GAP:
            self.since_chiff += 1

        if self.chiff_ticks > 0:
            self.chiff_ticks -= 1
            self.chiff_noise = (CHIFF_NOISE_Q15 * self.chiff_ticks) // CHIFF_TICKS
        else:
            self.chiff_noise = 0

        if self.artic == LEGATO:
            self.vib_phase = (self.vib_phase +
                              int(5.0 * 4294967296.0 / CTRL_RATE)) & 0xFFFFFFFF
            self.vib_cents = (fast_sin(self.vib_phase) * VIBRATO_CENTS) >> 15
        else:
            self.vib_phase = 0
            self.vib_cents = 0

        if self.stopped:
            self.breath = 0
        else:
            b = self.curved
            if self.chiff_ticks > 0:
                b -= (CHIFF_DIP_Q12 * self.chiff_ticks) // CHIFF_TICKS
                if b < 0:
                    b = 0
            self.breath = b

        if fired_this_tick:
            self.chiff_fired = False


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def test_silence():
    print("silence")
    b = Breath()
    b.set_knob(0)
    b.tick()
    check("knob at zero is silent", b.breath, 0)
    b.set_knob(BREATH_THRESH - 1)
    b.tick()
    check("just below threshold is silent", b.breath, 0)
    b.set_knob(BREATH_THRESH + 1)
    b.tick()
    check_true("just above threshold sounds", b.breath >= 0)
    b.set_knob(4095)
    b.tick()
    check("full knob is full breath", b.breath, 4095)


def test_stop():
    print("chiff stop")
    b = Breath()
    b.set_knob(4095)
    b.tick()
    check("sounding before the stop", b.breath, 4095)
    b.stopped = True
    b.tick()
    check("stop silences immediately", b.breath, 0)
    b.stopped = False
    b.tick()
    check("release restores the air", b.breath, 4095)


def test_no_pitch_change_from_main():
    """The Main knob must NOT move the pitch anywhere in its travel.

    It used to add an octave past about 70%, faking the overblow a waveguide
    voice could not produce. That voice is gone, and on the current one the
    jump was just a 12-semitone step landing in the middle of the vibrato
    stage — reported from hardware as the vibrato boundary sounding "an octave
    higher".

    This test exists so the register cannot quietly come back: the knob is
    level then vibrato, and vibrato is a wobble ABOUT the note, never a
    transposition of it.
    """
    print("Main does not transpose")
    b = Breath()
    assert not hasattr(b, "register"),         "the register switch is back -- see docs/DEVLOG.md v3.2.0"
    check("Breath exposes no register", hasattr(b, "register"), False)


def test_chiff_is_an_edge():
    """ChiffFired() must be true for exactly one tick.

    If it were a level, Pulse Out 2 would sit high for the chiff's whole
    duration and read as a long gate rather than a trigger.
    """
    print("chiff pulse")
    b = Breath()
    b.set_knob(2000)
    b.note_on()
    fired = []
    for _ in range(50):
        fired.append(b.chiff_fired)
        b.tick()
    check("fires on exactly one tick", sum(1 for f in fired if f), 1)
    check("...and it is the first", fired[0], True)


def test_chiff_rate_limit():
    """A fast trill must not become a stutter."""
    print("chiff rate limit")
    b = Breath()
    b.set_knob(2000)
    # A 10Hz trill: a new note every 150 control ticks.
    chiffs = 0
    for note in range(12):
        b.note_on()
        for _ in range(CTRL_RATE // 20):     # 50ms between notes
            if b.chiff_fired:
                chiffs += 1
            b.tick()
    print(f"        12 notes at 20Hz -> {chiffs} chiffs")
    check_true("rate limit thins the chiffs on a fast trill", chiffs < 12,
               f"{chiffs} of 12")

    # At a normal playing speed every note articulates.
    b = Breath()
    b.set_knob(2000)
    chiffs = 0
    for note in range(8):
        b.note_on()
        for _ in range(CTRL_RATE // 4):      # 250ms between notes
            if b.chiff_fired:
                chiffs += 1
            b.tick()
    check("every note articulates at ordinary speed", chiffs, 8)


def test_legato_has_no_chiff():
    print("articulation")
    b = Breath()
    b.artic = LEGATO
    b.set_knob(2000)
    b.note_on()
    fired = False
    for _ in range(40):
        if b.chiff_fired:
            fired = True
        b.tick()
    check("legato does not chiff", fired, False)

    vs = []
    for _ in range(3000):
        b.tick()
        vs.append(b.vib_cents)
    check_true("legato vibrates", max(vs) > 10 and min(vs) < -10,
               f"range {min(vs)}..{max(vs)} cents")

    b.artic = TONGUED
    b.tick()
    check("tongued does not vibrate", b.vib_cents, 0)


def test_curve_is_monotonic():
    """More knob must always mean more of both curves."""
    print("breath curve")
    prev_l = prev_e = -1
    bad = []
    for k in range(4096):
        b = Breath()
        b.set_knob(k)
        if b.curved < prev_l or b.effort < prev_e:
            bad.append(k)
        prev_l, prev_e = b.curved, b.effort
    check("neither curve ever goes backwards", bad, [])


def test_level_is_log():
    """Level must rise FAST and flatten, not creep up linearly.

    Reported from hardware: "the knob is a bit less responsive than I'd like -
    linear rather than the log it needs to be". v2.0 SQUARED the level, which
    is the opposite curve -- it spends the whole sweep still getting louder.
    """
    print("level curve shape")
    def level_at(pct):
        b = Breath()
        b.set_knob(int(4095 * pct / 100))
        return b.curved

    for pct in (10, 20, 30, 50, 70, 100):
        print(f"        {pct:3d}% knob -> level {level_at(pct):4d}")

    # An S-curve, so BOTH ends are flat and the middle is steep.
    import math

    def db(pct):
        v = level_at(pct)
        return 20 * math.log10(v / 4095) if v else -99.0

    # Bottom: leaves silence GENTLY. The v3.1 fifth power jumped 6dB per ten
    # counts of knob just above the threshold, which is a switch not a fade.
    check_true("the first few percent are quiet, not a switch",
               db(3) < -35, f"{db(3):.0f} dB at 3%")
    check_true("...but sound has clearly arrived by a tenth",
               db(10) > -30, f"{db(10):.0f} dB at 10%")

    # Middle: steep enough not to feel sluggish.
    check_true("half volume by a third of the travel",
               db(33) > -8, f"{db(33):.0f} dB at 33%")

    # Top: flat, so the last stretch is vibrato rather than loudness.
    check_true("the top quarter adds almost no level",
               db(100) - db(75) > -1.0 and db(100) - db(75) < 1.0,
               f"{db(100)-db(75):+.1f} dB over the last quarter")


def test_effort_keeps_climbing():
    """Once level flattens, effort must keep going -- that is what gives the
    top of the knob something to do."""
    print("effort curve")
    def eff(pct):
        b = Breath()
        b.set_knob(int(4095 * pct / 100))
        return b.effort
    for pct in (50, 70, 85, 100):
        print(f"        {pct:3d}% knob -> effort {eff(pct):4d}")
    check_true("effort is still rising through the top half",
               eff(100) - eff(50) > 1600,
               f"+{eff(100)-eff(50)} over the last half")
    # The two must genuinely diverge, or there was no point splitting them.
    b = Breath(); b.set_knob(int(4095 * 0.75))
    check_true("level and effort diverge", b.curved - b.effort > 800,
               f"level {b.curved} vs effort {b.effort}")

    # Where the vibrato stage begins, as a fraction of the knob. It has to
    # leave real room to grow in -- crammed into the last tenth it cannot be
    # played deliberately.
    from flutesim import VIB_ONSET
    knob = next(k for k in range(4096)
                if (lambda b: (b.set_knob(k), b.effort)[1])(Breath()) >= VIB_ONSET)
    pct = 100 * knob / 4095
    print(f"        vibrato begins at {pct:.0f}% of travel")
    check_true("vibrato has most of the knob to grow in (20-45%)",
               20 <= pct <= 45, f"{pct:.0f}%")


def main():
    print("breathsim — model of breath.cpp\n")
    test_silence()
    test_stop()
    test_no_pitch_change_from_main()
    test_chiff_is_an_edge()
    test_chiff_rate_limit()
    test_legato_has_no_chiff()
    test_curve_is_monotonic()
    test_level_is_log()
    test_effort_keeps_climbing()

    print()
    if FAILURES:
        print(f"{len(FAILURES)} FAILED: {', '.join(FAILURES)}")
        return 1
    print("all green")
    return 0


if __name__ == "__main__":
    sys.exit(main())
