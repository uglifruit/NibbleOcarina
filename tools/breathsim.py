#!/usr/bin/env python3
"""breathsim.py — a model of breath.cpp: the envelope and the level curve.

A LINE-BY-LINE port. If breath.cpp changes, change this — or delete it rather
than let it drift.

The card is BOWED, not blown: silent until the switch is held or Pulse In 1
goes high. A tap is a struck note, a hold sustains. So the things worth pinning
down are the ones a player would notice:

    silence      no gate must mean no sound, and the tail must actually end
    struck       a tap must give a complete note, not a click
    sustain      a hold must hold, indefinitely, without drifting
    swell        moving the knob mid-note must move the note
    coupling     loud notes must last longer than quiet ones
    curve        the level curve must be an S, and MONOTONIC

Run:  python tools/breathsim.py
"""

import math
import sys

CTRL_RATE = 3000

BREATH_THRESH = 60

ATTACK_SHIFT_FAST = 2
ATTACK_SHIFT_SLOW = 11
RELEASE_SHIFT_MIN = 6
RELEASE_SHIFT_MAX = 10
ENV_FLOOR = 1
EASE_LEVEL_1 = 256
EASE_LEVEL_2 = 32
ENV_FRAC = 8

VIB_HZ_TO_INC_Q16 = 366503876
GLIDE_SHIFT = 5

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
    return int(32767 * math.sin(2 * math.pi * (phase / 4294967296.0)))


# ---------------------------------------------------------------------------
# The port
# ---------------------------------------------------------------------------

class Breath:
    def __init__(self):
        self.peak = 0
        self.effort = 0
        self.env = 0
        self.gate = False
        self.struck = False
        self.attack = ATTACK_SHIFT_FAST
        self.vib_cents = 0
        self.vib_rate_q8 = 0
        self.vib_cents_max = 0
        self.vib_phase = 0

    # -- inputs --------------------------------------------------------------

    def set_knob(self, knob, cv_add=0):
        v = max(0, min(4095, knob + cv_add))
        if v < BREATH_THRESH:
            self.peak = 0
            self.effort = 0
            return

        n = min(4095, ((v - BREATH_THRESH) << 12) // (4095 - BREATH_THRESH))
        self.effort = n

        # S-curve: smoothstep then a lift that steepens only the middle.
        # The multiply order matters -- reducing the small factor first keeps
        # this exactly monotonic AND inside int32. See breath.cpp.
        inner = (n * (3 * 4096 - 2 * n)) >> 12
        sm = min(4095, (n * inner) >> 12)
        self.peak = min(4095, sm + ((sm * (4096 - sm)) >> 12))

    def set_attack(self, x):
        x = max(0, min(4095, x))
        self.attack = ATTACK_SHIFT_FAST + (
            ((ATTACK_SHIFT_SLOW - ATTACK_SHIFT_FAST) * x) >> 12)

    def set_gate(self, on):
        self.struck = on and not self.gate
        self.gate = on

    def set_vibrato(self, rate_q8, cents_q4):
        self.vib_rate_q8 = rate_q8
        self.vib_cents_max = cents_q4

    # -- outputs -------------------------------------------------------------

    def level(self):
        return self.env >> ENV_FRAC

    def sounding(self):
        return self.env > 0

    # -- the tick ------------------------------------------------------------

    def tick(self):
        target = self.peak << ENV_FRAC

        if self.gate:
            if self.env < target:
                d = target - self.env
                step = d >> self.attack
                if step == 0:
                    step = 1
                self.env = min(target, self.env + step)
            elif self.env > target:
                d = self.env - target
                step = d >> self.attack
                if step == 0:
                    step = 1
                self.env = max(target, self.env - step)
        elif self.env > 0:
            rel = RELEASE_SHIFT_MIN + (
                ((RELEASE_SHIFT_MAX - RELEASE_SHIFT_MIN) * self.peak) >> 12)
            # The release EASES OFF as the note dies, so the tail lingers
            # instead of marching to zero at a constant rate. See breath.cpp.
            lvl = self.env >> ENV_FRAC
            if lvl <= EASE_LEVEL_2:
                rel += 2
            elif lvl <= EASE_LEVEL_1:
                rel += 1
            step = self.env >> rel
            if step == 0:
                step = 1
            self.env -= step
            if self.env < (ENV_FLOOR << ENV_FRAC):
                self.env = 0

        if self.vib_cents_max > 0 and self.env > 0:
            inc = (self.vib_rate_q8 * VIB_HZ_TO_INC_Q16) >> 16
            self.vib_phase = (self.vib_phase + inc) & 0xFFFFFFFF
            self.vib_cents = (fast_sin(self.vib_phase) * self.vib_cents_max) >> 15
        else:
            self.vib_phase = 0
            self.vib_cents = 0


def play(b, gate_ticks, total_ticks):
    """Gate high for `gate_ticks`, then low. Returns the level each tick."""
    out = []
    for i in range(total_ticks):
        b.set_gate(i < gate_ticks)
        b.tick()
        out.append(b.level())
    return out


def ms(ticks):
    return 1000.0 * ticks / CTRL_RATE


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def test_silence():
    print("silence")
    b = Breath()
    b.set_knob(3000)
    for _ in range(2000):
        b.set_gate(False)
        b.tick()
    check("no gate, no sound", b.level(), 0)
    check("...and nothing is sounding", b.sounding(), False)

    # And the tail must genuinely END rather than approach zero forever.
    b = Breath()
    b.set_knob(4095)
    out = play(b, 300, 60000)
    tail = next((i for i in range(300, len(out)) if out[i] == 0), None)
    check_true("the release reaches exact silence", tail is not None,
               f"at {ms(tail):.0f}ms" if tail else "never")

    # THE TAIL MUST FADE, NOT STOP.
    #
    # Reported from hardware: "the decay to silence is always too abrupt - I
    # can hear it cut off". The floor was 8 of 4095, which is -54dB: negligible
    # written down, plainly audible in the room, and it lopped the last 18dB
    # off every note. The last level before silence must be 1, the smallest
    # step the 12-bit output can render.
    last = out[tail - 1]
    check("the last audible level before silence is 1", last, 1)

    # And it must LINGER there rather than leaping from loud to nothing: the
    # final few dB should take a sensible number of ticks.
    quiet_from = next(i for i in range(300, tail) if out[i] <= 4)
    check_true("the last few dB take real time",
               ms(tail - quiet_from) > 5,
               f"{ms(tail-quiet_from):.0f}ms below level 4")


def test_the_tail_eases_off():
    """The decay must SLOW as the note dies.

    A constant-rate exponential is a straight line in dB and has no ending --
    it just gets quieter at the same speed until the arithmetic runs out. That
    is heard as a note that FINISHES rather than fades, and it was reported
    from hardware twice: "the decay to silence is always too abrupt", then "I
    am still hearing notes finish".

    Real instruments slow down as they die. This asserts the rate at the end of
    the tail is well under the rate at the start.
    """
    print("the tail eases off")
    b = Breath()
    b.set_knob(4095)
    b.set_attack(0)
    out = play(b, 600, 200000)
    end = next(i for i in range(600, len(out)) if out[i] == 0)

    def db_at(t_ms):
        i = 600 + int(t_ms * CTRL_RATE / 1000)
        v = out[i] if i < end else 0
        return 20 * math.log10(v / 4095) if v else -99.0

    total = ms(end - 600)
    early = db_at(0) - db_at(200)
    late = db_at(total * 0.7) - db_at(total * 0.7 + 200)
    print(f"        total {total:.0f}ms, "
          f"first 200ms {early:.1f}dB, late 200ms {late:.1f}dB")
    check_true("the decay slows toward the end", late < early / 2,
               f"{early:.1f}dB -> {late:.1f}dB per 200ms")
    check_true("and the whole tail is musically long", total > 2000,
               f"{total:.0f}ms")


def test_a_tap_is_a_note():
    print("a tap is a whole note")
    b = Breath()
    b.set_knob(3000)
    b.set_attack(0)
    out = play(b, 30, 30000)          # 10ms tap
    peak = max(out)
    check_true("a 10ms tap still reaches full level", peak > 2900,
               f"peak {peak}")
    end = next(i for i in range(len(out)) if out[i] == 0 and i > 30)
    check_true("...and rings on after the tap", ms(end) > 100,
               f"{ms(end):.0f}ms total")


def test_hold_sustains():
    print("a hold sustains")
    b = Breath()
    b.set_knob(3000)
    b.set_attack(0)
    out = play(b, 30000, 32000)       # 10s hold
    held = out[9000:29000]
    check_true("level is steady for the whole hold",
               max(held) - min(held) <= 1, f"drift {max(held)-min(held)}")
    check_true("...at the knob's peak", abs(held[-1] - b.peak) <= 1,
               f"{held[-1]} vs peak {b.peak}")


def test_swell():
    """Moving the knob mid-note must move the note."""
    print("swelling mid-note")
    b = Breath()
    b.set_knob(1200)
    b.set_attack(0)
    for _ in range(3000):
        b.set_gate(True)
        b.tick()
    quiet = b.level()

    b.set_knob(4095)
    for _ in range(3000):
        b.set_gate(True)
        b.tick()
    loud = b.level()
    print(f"        knob 1200 -> {quiet}, then knob 4095 -> {loud}")
    check_true("turning up swells the held note", loud > quiet * 2,
               f"{quiet} -> {loud}")

    # And back down again, so a swell is reversible.
    b.set_knob(1200)
    for _ in range(3000):
        b.set_gate(True)
        b.tick()
    check_true("...and turning down eases it back",
               abs(b.level() - quiet) < 100, f"back to {b.level()}")


def test_louder_lasts_longer():
    """The coupling that makes one knob feel like dynamics."""
    print("louder lasts longer")
    times = []
    for knob in (800, 1600, 2600, 4095):
        b = Breath()
        b.set_knob(knob)
        b.set_attack(0)
        out = play(b, 600, 90000)
        end = next(i for i in range(600, len(out)) if out[i] == 0)
        times.append((knob, b.peak, ms(end - 600)))
        print(f"        knob {knob:4d} (peak {b.peak:4d}) -> "
              f"release {ms(end-600):6.0f}ms")
    check_true("release grows with level",
               all(times[i][2] > times[i - 1][2] for i in range(1, len(times))),
               "")
    check_true("and the range is musically wide",
               times[-1][2] > times[0][2] * 3,
               f"{times[0][2]:.0f}ms -> {times[-1][2]:.0f}ms")


def test_attack_shape():
    print("attack shape from X")
    times = []
    for x in (0, 2048, 4095):
        b = Breath()
        b.set_knob(4095)
        b.set_attack(x)
        out = play(b, 30000, 30000)
        t90 = next(i for i in range(len(out)) if out[i] > b.peak * 9 // 10)
        times.append((x, ms(t90)))
        print(f"        X {x:4d} -> 90% at {ms(t90):7.1f}ms")
    check_true("X CCW is effectively a strike", times[0][1] < 30,
               f"{times[0][1]:.0f}ms")
    check_true("X CW is an audible swell", times[-1][1] > 300,
               f"{times[-1][1]:.0f}ms")


def test_level_curve():
    print("level curve shape")

    def level_at(pct):
        b = Breath()
        b.set_knob(int(4095 * pct / 100))
        return b.peak

    def db(pct):
        v = level_at(pct)
        return 20 * math.log10(v / 4095) if v else -99.0

    for pct in (3, 10, 33, 50, 75, 100):
        print(f"        {pct:3d}% knob -> peak {level_at(pct):4d} "
              f"({db(pct):6.1f} dB)")

    # An S: both ends flat, middle steep.
    check_true("the first few percent are quiet, not a switch",
               db(3) < -35, f"{db(3):.0f} dB at 3%")
    check_true("...but sound has clearly arrived by a tenth",
               db(10) > -30, f"{db(10):.0f} dB at 10%")
    check_true("half volume by a third of the travel",
               db(33) > -8, f"{db(33):.0f} dB at 33%")
    check_true("the top quarter adds almost no level",
               abs(db(100) - db(75)) < 1.0,
               f"{db(100)-db(75):+.1f} dB over the last quarter")


def test_curve_is_monotonic():
    """A volume control that sometimes goes backwards is not a volume control.

    The obvious way to write this curve loses four bits before the second
    multiply, which made it go DOWN in 108 places. See breath.cpp.
    """
    print("monotonicity")
    prev = -1
    bad = []
    for k in range(4096):
        b = Breath()
        b.set_knob(k)
        if b.peak < prev:
            bad.append(k)
        prev = b.peak
    check("the level curve never goes backwards", bad, [])

    prev = -1
    bad = []
    for k in range(4096):
        b = Breath()
        b.set_knob(k)
        if b.effort < prev:
            bad.append(k)
        prev = b.effort
    check("nor does effort", bad, [])


def test_struck_is_an_edge():
    """Struck() must be true for exactly the tick a note begins."""
    print("strike edge")
    b = Breath()
    b.set_knob(3000)
    fired = []
    for i in range(200):
        b.set_gate(10 <= i < 100)
        b.tick()
        fired.append(b.struck)
    check("fires on exactly one tick", sum(1 for f in fired if f), 1)
    check("...and it is the gate's rising edge", fired.index(True), 10)


def test_no_pitch_change_from_main():
    """The Main knob must never transpose. It sets level and vibrato depth.

    A register switch lived on it until v3.2 -- a fossil of the v1 waveguide,
    faking an overblow for a bore that had not existed for two rewrites -- and
    it read as an octave jump in the middle of the vibrato stage.
    """
    print("Main does not transpose")
    b = Breath()
    check("Breath exposes no register", hasattr(b, "register"), False)


def main():
    print("breathsim — model of breath.cpp\n")
    test_silence()
    test_the_tail_eases_off()
    test_a_tap_is_a_note()
    test_hold_sustains()
    test_swell()
    test_louder_lasts_longer()
    test_attack_shape()
    test_level_curve()
    test_curve_is_monotonic()
    test_struck_is_an_edge()
    test_no_pitch_change_from_main()

    print()
    if FAILURES:
        print(f"{len(FAILURES)} FAILED: {', '.join(FAILURES)}")
        return 1
    print("all green")
    return 0


if __name__ == "__main__":
    sys.exit(main())
