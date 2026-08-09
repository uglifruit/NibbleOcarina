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

NUM_LEVELS = 10
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

LEARN_ORDER = [A, B, C, D, AB, CD, AC, BD, AD, BC]

# NIBBLE's learn order. Ours IS NIBBLE's now -- the card walked fifteen and
# picked a mode until hardware reported fifteen as simply too many to play.
NIBBLE_LEARN_ORDER = [A, B, C, D, AB, CD, AC, BD, AD, BC]

CTRL_RATE = 3000

SETTLE_TOL, DEADBAND, MATCH_WINDOW, COLLISION_MIN = 24, 16, 96, 64

SETTLE_TICKS   = CTRL_RATE // 80        # 37
CV_SMOOTH_SHIFT = 3
DEFAULT_LO, DEFAULT_HI = -1500, 1500
MIN_LEARN_SPAN = 400

# Events
EV_NONE, EV_TRIGGER = 0, 1

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
        self.level = [0] * NUM_LEVELS
        self.sorted_ = []
        self.slot_of = {}
        self.thresh = []
        self.learned = False
        self.collisions = 0
        self.min_gap = 0

        self.smooth = 0
        self.cand_mean = 0
        self.cand_ticks = 0
        self.current = NONE_COMBO
        self.primed = False

    # -- table construction --------------------------------------------------

    def _rebuild(self):
        n = NUM_LEVELS
        self.sorted_ = sorted(range(n), key=lambda i: self.level[i])
        self.thresh = [(self.level[self.sorted_[k]] +
                        self.level[self.sorted_[k + 1]]) >> 1
                       for k in range(n - 1)]
        self.slot_of = {c: k for k, c in enumerate(self.sorted_)}

    def init_default(self):
        for i in range(NUM_LEVELS):
            self.level[i] = (DEFAULT_LO +
                             (DEFAULT_HI - DEFAULT_LO) * i // (NUM_LEVELS - 1))
        self.learned = False
        self.collisions = 0
        self._rebuild()

    def _count_collisions(self):
        c = 0
        for i in range(NUM_LEVELS):
            for j in range(i):
                if iabs(self.level[i] - self.level[j]) < COLLISION_MIN:
                    c += 1
        return c

    def analyse(self, cap10):
        """Install a completed 10-capture set.

        Returns RESULT_OK or RESULT_FAILED. On FAILED nothing is installed and
        the previous calibration survives.
        """
        srt = sorted(cap10)
        if srt[-1] - srt[0] < MIN_LEARN_SPAN:
            # Nothing patched into CV In 1. A FAILURE, not a degraded mode --
            # ten identical readings do not become a usable calibration.
            return RESULT_FAILED

        self.min_gap = min(srt[i] - srt[i - 1] for i in range(1, NUM_LEVELS))
        self.level = list(cap10)
        self.collisions = self._count_collisions()
        self.learned = True
        self._rebuild()
        return RESULT_OK

    def reset_held(self):
        self.current = NONE_COMBO

    # -- matching ------------------------------------------------------------

    def match(self, v, cur):
        n = NUM_LEVELS
        k = 0
        while k < n - 1 and v > self.thresh[k]:
            k += 1
        cand = self.sorted_[k]

        # Schmitt hysteresis, biased toward the level we are already on, and
        # applied to ADJACENT slots only: a two-slot jump is unambiguous and
        # making it wait would add latency for nothing.
        if cur >= 0 and cand != cur and cur in self.slot_of:
            cur_slot = self.slot_of[cur]
            if cur_slot == k + 1 and v > self.thresh[k] - DEADBAND:
                cand = cur
            elif cur_slot == k - 1 and k >= 1 and v < self.thresh[k - 1] + DEADBAND:
                cand = cur

        # In range of the slot, but is it NEAR the level that owns the slot?
        # In 10-mode this is what makes triples and the quad safely ignorable.
        if iabs(v - self.level[cand]) > MATCH_WINDOW:
            return NONE_COMBO
        return cand

    # -- the detect step -----------------------------------------------------

    def step(self, cv_in):
        """One control tick. Returns (event, combo_index)."""
        self.smooth = slew_exact(self.smooth, cv_in, CV_SMOOTH_SHIFT)

        # Settle detector. The mean RESTARTS on every excursion rather than a
        # counter merely resetting, so slow drift can never accumulate into a
        # false settle.
        if self.cand_ticks > 0 and iabs(self.smooth - self.cand_mean) <= SETTLE_TOL:
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
        """Move to a combo's voltage.

        Combos past the learned ten are still physically producible by the
        module — that is the whole point of the match window — so the model has
        to be able to emit them. They sit above the top learned level.
        """
        if combo < len(self.levels):
            self.target = float(self.levels[combo])
        else:
            # Somewhere plausible but far from any learned centre.
            self.target = float(self.levels[-1] + 220 * (combo - len(self.levels) + 1))

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


def spaced10(lo=-1500, hi=1500):
    """Ten evenly spaced levels. Gap is (hi-lo)/9 = 333 units, comfortable."""
    return [lo + (hi - lo) * i // (NUM_LEVELS - 1) for i in range(NUM_LEVELS)]


def squashed10():
    """Two levels almost on top of each other, so the collision warning fires
    and the player is told rather than left guessing."""
    lv = spaced10()
    lv[AC] = lv[AB] + 20
    return lv


def degenerate10():
    """Nothing patched in: every reading the same."""
    return [7] * NUM_LEVELS


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


def build(levels):
    """A tracker calibrated on `levels`, primed and ready to play."""
    t = LevelTracker()
    res = t.analyse(levels)
    assert res == RESULT_OK, "test fixture failed to calibrate"
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
    check("learn order covers 0..9", sorted(LEARN_ORDER), list(range(10)))
    check("the learn order IS NIBBLE's", LEARN_ORDER, NIBBLE_LEARN_ORDER)
    check("10-set is singles+pairs",
          [bin(COMBO_MASK[i]).count("1") for i in range(10)],
          [1, 1, 1, 1, 2, 2, 2, 2, 2, 2])
    check("the mask table still describes all 15 subsets",
          sorted(COMBO_MASK), list(range(1, 16)))
    # Only the first ten are PLAYED. The rest stay in the table because the
    # match window has to reject them by distance, and because caltable.py
    # documents which fingerings do nothing.
    check("the ten played combos are singles and pairs",
          [bin(COMBO_MASK[i]).count("1") for i in range(NUM_LEVELS)],
          [1, 1, 1, 1, 2, 2, 2, 2, 2, 2])


def test_releases_are_notes():
    print("releases are notes (no ghost rule)")
    lv = spaced10()

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
    prime(t, hw, AB)
    check("AB -> CD -> AB", play(t, hw, [AB, CD, AB]), ["CD", "AB"])


def test_trill():
    print("the trill — the defining gesture")
    lv = spaced10()
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
    lv = spaced10()
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


def test_triples_are_safely_ignored():
    """Pressing three fingers must do NOTHING, not something wrong.

    Only ten combinations are learned, but the player can still physically
    press a triple. Its voltage lands far from every learned centre, so the
    match window rejects it and the current note simply stays. That rejection
    is the whole reason the extra five combos can be dropped safely.
    """
    print("triples are ignored, not misread")
    lv = spaced10()
    t = build(lv)
    hw = FourVoltages(lv)
    prime(t, hw, A)
    check("A -> ABC is silent", play(t, hw, [A, ABC]), [])
    check("...and the note is still A", NAMES[t.current], "A")
    check("A -> ABCD is silent", play(t, hw, [ABCD]), [])
    check("...then B still fires normally", play(t, hw, [B]), ["B"])


def test_boot_blip():
    print("boot blip")
    lv = spaced10()
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


def test_calibration_accepts_or_refuses():
    print("calibration")
    t = LevelTracker()
    check("a clean spread installs", t.analyse(spaced10()), RESULT_OK)
    check("...and reports its tightest gap", t.min_gap > 300, True)

    t = LevelTracker()
    check("a squashed spread still installs", t.analyse(squashed10()), RESULT_OK)
    check("...but warns about the collision", t.collisions > 0, True)

    t = LevelTracker()
    check("nothing patched -> FAILED", t.analyse(degenerate10()), RESULT_FAILED)
    check("...and nothing was installed", t.learned, False)


def test_round_trip():
    print("learn round-trip")
    lv = spaced10()

    t = build(lv)
    bad = [NAMES[i] for i in range(NUM_LEVELS) if t.match(lv[i], NONE_COMBO) != i]
    check("all 10 captures re-classify to themselves", bad, [])


def test_matches_nibble():
    """The detector must behave exactly as NIBBLE's proven one does.

    Settle, Schmitt, match-window rejection, primed. These are the one part of
    this card's detection with real hardware history behind them, so a drift
    here loses the only thing that was ever proven.
    """
    print("detector fidelity")
    lv = spaced10()
    t = build(lv)
    check("every learned level maps to itself",
          all(t.match(lv[i], NONE_COMBO) == i for i in range(NUM_LEVELS)), True)

    # Schmitt hysteresis needs a fixture where two levels are close enough that
    # a single reading falls inside BOTH match windows — otherwise the window
    # rejects the midpoint first and hysteresis never gets a say.
    #
    # That is not a contrivance, it is the case hysteresis exists FOR: two
    # combos the resistor network happened to place near each other, where
    # dither would otherwise flicker between them. Levels 150 apart (~440mV)
    # are comfortably legal and give overlapping windows.
    tight = [-1500 + 150 * i for i in range(NUM_LEVELS)]
    tt = LevelTracker()
    tt.level = list(tight)
    tt._rebuild()

    k = 4
    thr = tt.thresh[k]
    lo_combo, hi_combo = tt.sorted_[k], tt.sorted_[k + 1]
    probe = thr + 1                      # just inside the HIGHER slot
    assert iabs(probe - tt.level[lo_combo]) <= MATCH_WINDOW, \
        "fixture: probe outside the match window, test would be vacuous"
    check("hysteresis holds the level we came from",
          tt.match(probe, lo_combo), lo_combo)
    check("...but a neutral reading takes the new slot",
          tt.match(probe, NONE_COMBO), hi_combo)
    check("...and a big jump ignores hysteresis entirely",
          tt.match(tight[hi_combo] + 1, lo_combo) != lo_combo, True)

    # Match window: a value far from every learned level is rejected rather
    # than snapped to the nearest. This is what makes a triple harmless.
    far = lv[-1] + 400
    check("a far-from-anything value is rejected",
          t.match(far, NONE_COMBO), NONE_COMBO)


def test_noise_immunity():
    """A still finger must not produce notes, however long you wait."""
    print("noise immunity")
    lv = spaced10()
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
    test_triples_are_safely_ignored()
    test_boot_blip()
    test_calibration_accepts_or_refuses()
    test_round_trip()
    test_matches_nibble()
    test_noise_immunity()

    print()
    if FAILURES:
        print(f"{len(FAILURES)} FAILED: {', '.join(FAILURES)}")
        return 1
    print("all green")
    return 0


if __name__ == "__main__":
    sys.exit(main())
