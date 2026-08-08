#!/usr/bin/env python3
"""levelsim.py — a model of levels.cpp, and the tests that pin its behaviour.

This is a LINE-BY-LINE port of levels.cpp. If that file changes, change this
one — or delete it rather than let it drift into telling you a comfortable lie.
NIBBLE's equivalent model caught four real bugs before they reached hardware.

It is named levelsim rather than ghostsim (NIBBLE's name) because the thing
NIBBLE's model existed to verify — the ghost rule — is deliberately gone here.
See the note in levels.h: OCARINA gets silence from the breath knob, so a
release IS a note, and suppressing it would break trilling.

Run:  python tools/levelsim.py
"""

import sys

# ---------------------------------------------------------------------------
# Vocabulary — mirrors ocarina.h
# ---------------------------------------------------------------------------

MAX_LEVELS = 15
LEVELS_10  = 10
NUM_SINGLES = 4

(A, B, C, D,
 AB, AC, AD, BC, BD, CD,
 ABC, ABD, ACD, BCD,
 ABCD) = range(15)
NONE_COMBO = -1

NAMES = ["A", "B", "C", "D",
         "AB", "AC", "AD", "BC", "BD", "CD",
         "ABC", "ABD", "ACD", "BCD",
         "ABCD"]

COMBO_MASK = [0x1, 0x2, 0x4, 0x8,
              0x3, 0x5, 0x9, 0x6, 0xA, 0xC,
              0x7, 0xB, 0xD, 0xE,
              0xF]

LEARN_ORDER = [A, B, C, D,
               AB, CD, AC, BD, AD, BC,
               ABC, ABD, ACD, BCD,
               ABCD]

# NIBBLE's learn order, for the "the fallback really is NIBBLE" assertion.
NIBBLE_LEARN_ORDER = [A, B, C, D, AB, CD, AC, BD, AD, BC]

CTRL_RATE = 3000

SETTLE_TOL_10, DEADBAND_10, MATCH_WINDOW_10, COLLISION_MIN_10 = 24, 16, 96, 64
SETTLE_TOL_15, DEADBAND_15, MATCH_WINDOW_15, COLLISION_MIN_15 = 16, 10, 56, 40

SETTLE_TICKS   = CTRL_RATE // 80        # 37
CV_SMOOTH_SHIFT = 3
DEFAULT_LO, DEFAULT_HI = -1500, 1500
MIN_LEARN_SPAN = 400
GAP_NEEDED_15  = 144

# Events
EV_NONE, EV_TRIGGER = 0, 1

MODE_15, MODE_10 = 15, 10
RESULT_OK, RESULT_FAILED = "ok", "failed"


# ---------------------------------------------------------------------------
# fastmath.h equivalents
# ---------------------------------------------------------------------------

def asr(v, s):
    """Arithmetic shift right, matching C's >> on a negative int32_t.

    Python's >> already floors toward -inf for ints, which is what an
    arithmetic shift does. Spelled out so the intent is not accidental.
    """
    return v >> s


def slew_exact(v, target, shift):
    """One-pole slew that always REACHES its target.

    The plain shift stalls asymmetrically: arithmetic shift-right floors toward
    -inf, so approaching from above lands exactly while approaching from below
    stops ~2^shift short. Measured at 17 units of direction-dependent error on
    this very input chain in NIBBLE, which broke its learn round-trip.
    """
    d = target - v
    if d == 0:
        return v
    step = asr(d, shift)
    if step == 0:
        step = 1 if d > 0 else -1
    return v + step


def iabs(v):
    return -v if v < 0 else v


# ---------------------------------------------------------------------------
# LevelTracker — the port
# ---------------------------------------------------------------------------

class LevelTracker:
    def __init__(self):
        self.level = [0] * MAX_LEVELS
        self.sorted_ = []
        self.slot_of = {}
        self.thresh = []
        self.learned = False
        self.collisions = 0
        self.active = LEVELS_10
        self.mode = MODE_10
        self.min_gap = 0
        self.min_gap_15 = 0

        self.settle_tol = SETTLE_TOL_10
        self.deadband = DEADBAND_10
        self.match_window = MATCH_WINDOW_10

        self.smooth = 0
        self.cand_mean = 0
        self.cand_ticks = 0
        self.current = NONE_COMBO
        self.primed = False

    # -- table construction --------------------------------------------------

    def _rebuild(self):
        n = self.active
        self.sorted_ = sorted(range(n), key=lambda i: self.level[i])
        self.thresh = [(self.level[self.sorted_[k]] +
                        self.level[self.sorted_[k + 1]]) >> 1
                       for k in range(n - 1)]
        self.slot_of = {c: k for k, c in enumerate(self.sorted_)}

    def _set_tolerances(self):
        if self.active == MAX_LEVELS:
            self.settle_tol, self.deadband, self.match_window = (
                SETTLE_TOL_15, DEADBAND_15, MATCH_WINDOW_15)
        else:
            self.settle_tol, self.deadband, self.match_window = (
                SETTLE_TOL_10, DEADBAND_10, MATCH_WINDOW_10)

    def init_default(self):
        self.active = LEVELS_10
        self.mode = MODE_10
        for i in range(LEVELS_10):
            self.level[i] = (DEFAULT_LO +
                             (DEFAULT_HI - DEFAULT_LO) * i // (LEVELS_10 - 1))
        self.learned = False
        self.collisions = 0
        self._set_tolerances()
        self._rebuild()

    def _count_collisions(self, n, floor):
        c = 0
        for i in range(n):
            for j in range(i):
                if iabs(self.level[i] - self.level[j]) < floor:
                    c += 1
        return c

    def analyse(self, cap15):
        """Install a 15-capture set, measure it, and choose the mode.

        Returns RESULT_OK or RESULT_FAILED. On FAILED nothing is installed and
        the previous calibration survives.
        """
        s = sorted(cap15)
        span = s[-1] - s[0]
        if span < MIN_LEARN_SPAN:
            # Nothing patched into CV In 1. That is a FAILURE, not "use ten" —
            # fifteen identical readings do not become ten good ones.
            return RESULT_FAILED

        min_gap_15 = min(s[i] - s[i - 1] for i in range(1, MAX_LEVELS))

        t = sorted(cap15[:LEVELS_10])
        min_gap_10 = min(t[i] - t[i - 1] for i in range(1, LEVELS_10))

        self.level = list(cap15)
        self.min_gap_15 = min_gap_15

        if min_gap_15 >= GAP_NEEDED_15:
            self.mode, self.active = MODE_15, MAX_LEVELS
            self.min_gap = min_gap_15
            floor = COLLISION_MIN_15
        else:
            self.mode, self.active = MODE_10, LEVELS_10
            self.min_gap = min_gap_10
            floor = COLLISION_MIN_10

        self.collisions = self._count_collisions(self.active, floor)
        self.learned = True
        self._set_tolerances()
        self._rebuild()
        return RESULT_OK

    def reset_held(self):
        self.current = NONE_COMBO

    # -- matching ------------------------------------------------------------

    def match(self, v, cur):
        n = self.active
        k = 0
        while k < n - 1 and v > self.thresh[k]:
            k += 1
        cand = self.sorted_[k]

        # Schmitt hysteresis, biased toward the level we are already on, and
        # applied to ADJACENT slots only: a two-slot jump is unambiguous and
        # making it wait would add latency for nothing.
        if cur >= 0 and cand != cur and cur in self.slot_of:
            cur_slot = self.slot_of[cur]
            if cur_slot == k + 1 and v > self.thresh[k] - self.deadband:
                cand = cur
            elif cur_slot == k - 1 and k >= 1 and v < self.thresh[k - 1] + self.deadband:
                cand = cur

        # In range of the slot, but is it NEAR the level that owns the slot?
        # In 10-mode this is what makes triples and the quad safely ignorable.
        if iabs(v - self.level[cand]) > self.match_window:
            return NONE_COMBO
        return cand

    # -- the detect step -----------------------------------------------------

    def step(self, cv_in):
        """One control tick. Returns (event, combo_index)."""
        self.smooth = slew_exact(self.smooth, cv_in, CV_SMOOTH_SHIFT)

        # Settle detector. The mean RESTARTS on every excursion rather than a
        # counter merely resetting, so slow drift can never accumulate into a
        # false settle.
        if self.cand_ticks > 0 and iabs(self.smooth - self.cand_mean) <= self.settle_tol:
            self.cand_mean = slew_exact(self.cand_mean, self.smooth, 4)
            if self.cand_ticks < SETTLE_TICKS:
                self.cand_ticks += 1
        else:
            self.cand_mean = self.smooth
            self.cand_ticks = 1
            return (EV_NONE, -1)

        if self.cand_ticks < SETTLE_TICKS:
            return (EV_NONE, -1)

        m = self.match(self.cand_mean, self.current)
        if m == NONE_COMBO:
            return (EV_NONE, -1)      # unrecognised: stay latched
        if m == self.current:
            return (EV_NONE, -1)      # nothing changed

        # NO GHOST RULE. Every settled change is a genuine note, in both
        # directions — releasing a finger from AB lands on A and SOUNDS as A.
        # That is how a trill works.
        self.current = m

        if not self.primed:
            self.primed = True
            return (EV_NONE, -1)

        return (EV_TRIGGER, m)

    def settled(self):
        return self.cand_ticks >= SETTLE_TICKS

    def settled_value(self):
        return self.cand_mean


# ---------------------------------------------------------------------------
# Simulated hardware
# ---------------------------------------------------------------------------

class FourVoltages:
    """The Four Voltages output as the card sees it.

    Models the two behaviours that matter: finite slew between levels (so the
    settle detector has something real to reject) and no rest state (releasing
    everything leaves the output wherever it last was).
    """

    def __init__(self, levels, slew_ticks=6, noise=0, seed=12345):
        self.levels = levels
        self.slew_ticks = slew_ticks
        self.noise = noise
        self.value = float(levels[0])
        self.target = float(levels[0])
        self._rng = seed

    def _rand(self):
        s = self._rng
        s ^= (s << 13) & 0xFFFFFFFF
        s ^= s >> 17
        s ^= (s << 5) & 0xFFFFFFFF
        self._rng = s & 0xFFFFFFFF
        return self._rng

    def set_combo(self, combo):
        self.target = float(self.levels[combo])

    def tick(self):
        d = self.target - self.value
        if abs(d) < 0.5:
            self.value = self.target
        else:
            self.value += d / self.slew_ticks
        v = self.value
        if self.noise:
            v += (self._rand() % (2 * self.noise + 1)) - self.noise
        return int(round(v))


def spaced15(lo=-1500, hi=1500):
    """Fifteen evenly spaced levels — the case where 15-mode is achievable.

    Gap is (hi-lo)/14 = 214 units, comfortably over GAP_NEEDED_15.
    """
    return [lo + (hi - lo) * i // (MAX_LEVELS - 1) for i in range(MAX_LEVELS)]


def squashed15():
    """A realistic bad case: the triples bunch up near the top, as they would on
    a resistor network whose upper combinations crowd together. 15-mode must be
    refused and 10-mode must still be clean."""
    lv = spaced15()
    lv[ABC] = lv[ABCD] - 30
    lv[ABD] = lv[ABCD] - 60
    return lv


def degenerate15():
    """Nothing patched in: every reading the same."""
    return [7] * MAX_LEVELS


def play(tracker, hw, gesture, hold_ticks=80):
    out = []
    for combo in gesture:
        hw.set_combo(combo)
        for _ in range(hold_ticks):
            ev, idx = tracker.step(hw.tick())
            if ev != EV_NONE:
                out.append(NAMES[idx])
    return out


def prime(tracker, hw, combo, ticks=120):
    """Settle onto a starting combo and swallow the power-on blip."""
    hw.set_combo(combo)
    for _ in range(ticks):
        tracker.step(hw.tick())


def build(levels, force10=False):
    """A tracker calibrated on `levels`, primed and ready to play."""
    t = LevelTracker()
    res = t.analyse(levels)
    assert res == RESULT_OK, "test fixture failed to calibrate"
    if force10:
        t.mode, t.active = MODE_10, LEVELS_10
        t._set_tolerances()
        t._rebuild()
    return t


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

FAILURES = []


def check(name, got, want):
    if got == want:
        print(f"  ok    {name}")
    else:
        print(f"  FAIL  {name}\n          got  {got}\n          want {want}")
        FAILURES.append(name)


def test_vocabulary():
    print("vocabulary")
    check("learn order covers 0..14", sorted(LEARN_ORDER), list(range(15)))
    # The whole fallback story rests on this: the first ten indices, and the
    # order they are learned in, ARE NIBBLE's.
    check("first ten of learn order == NIBBLE's",
          LEARN_ORDER[:10], NIBBLE_LEARN_ORDER)
    check("10-set is singles+pairs",
          [bin(COMBO_MASK[i]).count("1") for i in range(10)],
          [1, 1, 1, 1, 2, 2, 2, 2, 2, 2])
    check("masks are the 15 non-empty subsets",
          sorted(COMBO_MASK), list(range(1, 16)))


def test_releases_are_notes():
    print("releases are notes (no ghost rule)")
    lv = spaced15()

    t = build(lv)
    hw = FourVoltages(lv)
    prime(t, hw, A)
    # The defining difference from NIBBLE: the release fires.
    check("A -> AB -> A", play(t, hw, [A, AB, A]), ["AB", "A"])

    t = build(lv)
    hw = FourVoltages(lv)
    prime(t, hw, A)
    check("A -> AB -> B", play(t, hw, [A, AB, B]), ["AB", "B"])

    t = build(lv)
    hw = FourVoltages(lv)
    prime(t, hw, ABC)
    check("ABC -> AB -> A", play(t, hw, [ABC, AB, A]), ["AB", "A"])

    t = build(lv)
    hw = FourVoltages(lv)
    prime(t, hw, ABCD)
    check("ABCD -> ABC -> AB -> A",
          play(t, hw, [ABCD, ABC, AB, A]), ["ABC", "AB", "A"])

    t = build(lv)
    hw = FourVoltages(lv)
    prime(t, hw, A)
    check("A -> ABC (skips AB)", play(t, hw, [A, ABC]), ["ABC"])


def test_trill():
    print("the trill — the defining gesture")
    lv = spaced15()
    t = build(lv)
    hw = FourVoltages(lv)
    prime(t, hw, C)
    # Hold C, waggle A. Every transition is a note, in both directions.
    check("C -> AC -> C -> AC -> C",
          play(t, hw, [C, AC, C, AC, C]), ["AC", "C", "AC", "C"])


def test_trill_rate():
    """How fast can a finger waggle before the detector drops notes?

    With no ghost rule, trill speed is bounded by SETTLE_TICKS: each note needs
    a plateau of 12.3ms plus the slew between levels. This test documents where
    the edge actually is rather than asserting a number we would like.
    """
    print("trill rate sweep")
    lv = spaced15()
    results = {}
    for hz in (4, 8, 12, 16, 20, 25, 30):
        half = max(1, int(CTRL_RATE / (2 * hz)))    # ticks per half-cycle
        t = build(lv)
        hw = FourVoltages(lv)
        prime(t, hw, C)
        n = 0
        for i in range(12):
            hw.set_combo(AC if i % 2 == 0 else C)
            for _ in range(half):
                ev, _idx = t.step(hw.tick())
                if ev != EV_NONE:
                    n += 1
        results[hz] = n
        print(f"        {hz:2d}Hz  ({half:3d} ticks/half)  -> {n:2d}/12 notes")

    # A musically useful trill is 8-12Hz; a very fast one is 16Hz. Those must be
    # perfect.
    check("4Hz trill is clean", results[4], 12)
    check("8Hz trill is clean", results[8], 12)
    check("12Hz trill is clean", results[12], 12)
    check("16Hz trill is clean", results[16], 12)

    # THE FAILURE IS A CLIFF, NOT A SLOPE, and that is worth knowing before
    # anyone edits kSettleTicks.
    #
    # A note needs SETTLE_TICKS (37) of plateau plus the slew between levels
    # (~6), so ~43 ticks per half-cycle. Above that everything fires; below it
    # the plateau never completes and NOTHING fires — not "some notes get
    # dropped", but total silence while the finger keeps moving.
    #
    # 20Hz passes with 75 ticks; 25Hz gives 60 and still passes; the floor sits
    # just under that. Raising kSettleTicks to reject hardware noise lowers this
    # ceiling proportionally, so if trilling ever goes silent on real hardware,
    # THIS is the constant that did it.
    check("the cliff is where the arithmetic says it is",
          results[30], 0)


def test_unrecognised_stays_latched():
    print("unrecognised values stay latched (10-mode)")
    lv = spaced15()
    t = build(lv, force10=True)
    hw = FourVoltages(lv)
    prime(t, hw, A)
    # ABC is not in the 10-set and sits far from any learned centre, so it is
    # rejected and A keeps sounding. This is what makes 10-mode safe.
    check("10-mode: A -> ABC is silent", play(t, hw, [A, ABC]), [])
    check("10-mode: A -> ABC -> B still fires B",
          play(t, hw, [ABC, B]), ["B"])


def test_boot_blip():
    print("boot blip")
    lv = spaced15()
    t = build(lv)
    hw = FourVoltages(lv)
    # Four Voltages powers up sitting at whatever was last pressed. The first
    # settle must be swallowed.
    hw.set_combo(D)
    fired = []
    for _ in range(200):
        ev, idx = t.step(hw.tick())
        if ev != EV_NONE:
            fired.append(NAMES[idx])
    check("first settle after power-on is silent", fired, [])
    check("but the next press fires", play(t, hw, [A]), ["A"])


def test_mode_decision():
    print("mode decision")
    t = LevelTracker()
    check("even 15-spread -> 15-mode",
          (t.analyse(spaced15()), t.mode), (RESULT_OK, MODE_15))

    t = LevelTracker()
    r = t.analyse(squashed15())
    check("squashed triples -> falls back to 10", (r, t.mode), (RESULT_OK, MODE_10))
    check("...and reports how close 15 got", t.min_gap_15 < GAP_NEEDED_15, True)

    t = LevelTracker()
    check("nothing patched -> FAILED (not 10-mode)",
          t.analyse(degenerate15()), RESULT_FAILED)
    check("...and nothing was installed", t.learned, False)


def test_round_trip():
    print("learn round-trip")
    lv = spaced15()

    t = build(lv)
    bad = [NAMES[i] for i in range(MAX_LEVELS) if t.match(lv[i], NONE_COMBO) != i]
    check("all 15 captures re-classify to themselves", bad, [])

    t = build(lv, force10=True)
    bad = [NAMES[i] for i in range(LEVELS_10) if t.match(lv[i], NONE_COMBO) != i]
    check("all 10 captures re-classify to themselves", bad, [])


def test_fallback_is_nibble():
    """10-mode must behave exactly as NIBBLE's proven detector does.

    Not a re-implementation to compare against — the point is that the SAME
    code with active=10 reproduces NIBBLE's level-detection behaviour: settle,
    Schmitt, match-window rejection, primed. If this drifts, the proven
    fallback is gone and there is nothing left to trust.
    """
    print("10-mode fidelity")
    lv = spaced15()
    t = build(lv, force10=True)

    check("only the ten are reachable",
          all(t.match(lv[i], NONE_COMBO) < LEVELS_10 or
              t.match(lv[i], NONE_COMBO) == NONE_COMBO
              for i in range(MAX_LEVELS)), True)

    # Schmitt hysteresis needs a fixture where two levels are close enough that
    # a single reading falls inside BOTH match windows — otherwise the window
    # rejects the midpoint first and hysteresis never gets a say.
    #
    # That is not a contrivance, it is the case hysteresis exists FOR: two
    # combos the resistor network happened to place near each other, where
    # dither would otherwise flicker between them. Levels 150 apart (~440mV)
    # are comfortably legal and give overlapping windows.
    tight = [-1500 + 150 * i for i in range(LEVELS_10)] + [0] * 5
    tt = LevelTracker()
    tt.level = list(tight)
    tt.active, tt.mode = LEVELS_10, MODE_10
    tt._set_tolerances()
    tt._rebuild()

    k = 4
    thr = tt.thresh[k]
    lo_combo, hi_combo = tt.sorted_[k], tt.sorted_[k + 1]
    probe = thr + 1                      # just inside the HIGHER slot
    assert iabs(probe - tt.level[lo_combo]) <= MATCH_WINDOW_10, \
        "fixture: probe outside the match window, test would be vacuous"
    check("hysteresis holds the level we came from",
          tt.match(probe, lo_combo), lo_combo)
    check("...but a neutral reading takes the new slot",
          tt.match(probe, NONE_COMBO), hi_combo)
    check("...and a big jump ignores hysteresis entirely",
          tt.match(tight[hi_combo] + 1, lo_combo) != lo_combo, True)

    # Match window: a value between two learned levels but far from both is
    # rejected rather than snapped.
    mid = (lv[ABC] + lv[ABCD]) // 2
    if min(iabs(mid - lv[i]) for i in range(LEVELS_10)) > MATCH_WINDOW_10:
        check("a far-from-anything value is rejected",
              t.match(mid, NONE_COMBO), NONE_COMBO)


def test_noise_immunity():
    """A still finger must not produce notes, however long you wait."""
    print("noise immunity")
    lv = spaced15()
    t = build(lv)
    hw = FourVoltages(lv, noise=6)
    prime(t, hw, BC)
    fired = []
    for _ in range(20000):
        ev, idx = t.step(hw.tick())
        if ev != EV_NONE:
            fired.append(NAMES[idx])
    check("20000 ticks on a held combo with dither", fired, [])


def main():
    print("levelsim — model of levels.cpp\n")
    test_vocabulary()
    test_releases_are_notes()
    test_trill()
    test_trill_rate()
    test_unrecognised_stays_latched()
    test_boot_blip()
    test_mode_decision()
    test_round_trip()
    test_fallback_is_nibble()
    test_noise_immunity()

    print()
    if FAILURES:
        print(f"{len(FAILURES)} FAILED: {', '.join(FAILURES)}")
        return 1
    print("all green")
    return 0


if __name__ == "__main__":
    sys.exit(main())
