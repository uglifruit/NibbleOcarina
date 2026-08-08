# Hardware testing

Nothing in this card has ever met a real Four Voltages module. Everything below
is what to check, in the order that finds problems fastest.

---

## The one number worth bringing back

**`minGap15`, for each of the four Four Voltages outputs.**

Whether fifteen combinations are separable on real hardware is the card's
central unknown. NIBBLE has been asking for this measurement across five
sessions and never captured it. This card puts it on the LEDs precisely so it
comes back from a session as a number rather than a yes/no.

Fill this in:

| Four Voltages output | FV knob position | Bar (of 4) | Mode chosen | Notes |
|---|---|---|---|---|
| 1 | | | | |
| 2 | | | | |
| 3 | | | | |
| 4 | | | | |

To read it: calibrate, and if it falls back to 10-mode the top four LEDs show
the bar for 2.5s. Hold switch-up for 1s any time to see it again.

Even if every output lands on 10-mode, **that is the answer** — and it is worth
knowing conclusively rather than by assumption.

---

## Running order

Work down. Each step assumes the ones above passed.

### 1. It boots

- [ ] Card powers up, LEDs do something, no smoke.
- [ ] It enters calibration by itself after about a second.
- [ ] **Cold power-up, not just a warm reset.** These fail differently — a
      missing `PICO_XOSC_STARTUP_DELAY_MULTIPLIER` works on a warm reset and
      hangs on a cold boot. Unplug the case, wait, plug in.
- [ ] Boot with the switch held **down**, and again held **up**. Neither should
      trigger a gesture on release. (The boot latch takes one reading after a
      settling window; the failure mode is the card acting as if you had held
      the switch on every single boot.)

### 2. Calibration

Patch one Four Voltages output into CV In 1.

- [ ] The target combination is readable off LEDs 0–3 and matches the chart in
      the README.
- [ ] LED 4/5 phase marker: one lit for singles, the other for pairs, both for
      triples, both blinking fast for all-four.
- [ ] Holding a combination and tapping captures it — all six LEDs flash.
- [ ] Tapping **while still moving** is rejected with the collision flash rather
      than silently recording a mid-slew voltage.
- [ ] All fifteen can be walked without the 30s timeout expiring.
- [ ] Holding the switch down 2s **during** the walk aborts it.
- [ ] **With nothing patched into CV In 1**, calibration must FAIL — columns
      alternating fast — and keep the previous calibration. It must not fall
      back to 10-mode; fifteen identical readings are not ten good ones.

### 3. Does it play

- [ ] Main knob at zero is **silent**. Not quiet: silent.
- [ ] Turning it up brings the instrument in smoothly.
- [ ] Each combination plays a distinct note, and the same combination plays the
      same note every time.
- [ ] The LEDs show the fingering you are holding.
- [ ] **Releasing a finger sounds the new note.** AB → A must sound A. If it is
      silent, the ghost rule has crept back in somehow.

### 4. Trilling — the gesture this card is built around

- [ ] Hold one hole, waggle another. Should be a clean alternation.
- [ ] Switch **up** (legato) for a fast trill: it should glide, not stutter.
- [ ] Switch **middle** (tongued): each note articulates. A very fast trill will
      thin its chiffs rather than machine-gunning.
- [ ] How fast can you trill before notes drop? The model says ~16Hz clean and a
      hard cliff around 20Hz. **If trilling goes silent rather than getting
      ragged, that is `kSettleTicks`** — the failure is a cliff, not a slope.

### 5. Breath and register

- [ ] The octave jump lands around 70% of the knob's travel.
- [ ] Sitting right at the boundary and wobbling: the octave must NOT flutter.
- [ ] It drops back when you ease off.
- [ ] Switch **down** cuts the sound *dead* — the bore should damp in ~50ms, not
      ring out. This is the difference between a stop and a gate close.
- [ ] Releasing it re-attacks cleanly.

### 6. Tuning

- [ ] Switch up 1s shows the gap bar; keep holding to 3s and it enters tune.
- [ ] The drone sounds and is steady.
- [ ] **Entering tune does not change the pitch.** The knobs are wherever they
      were from playing; nothing should move until you turn one.
- [ ] Y moves it in semitones, Main in cents.
- [ ] Leaving with a tap keeps the offsets.
- [ ] Power cycle: tuning is gone. That is intended.

### 7. Against the rack

- [ ] CV Out 1 into an oscillator: it should track 1V/oct, and the scale root
      should be 0V.
- [ ] **Tune the oscillator to the internal voice, then play the whole range.**
      They must not drift apart — the two are computed on separate paths and
      this is the check that they agree.
- [ ] Move the fine tune: the oscillator and the internal voice must move
      *together*.
- [ ] Coarse tune **flat** (below zero) as well as sharp — the negative
      direction was wrong once.
- [ ] During a legato slur, CV Out 1 should slide, not jump.
- [ ] CV Out 2 follows the breath.
- [ ] Pulse Out 1 is high while sounding; Pulse Out 2 blips per articulated
      note. (Pulse Out 2 silently never fired at one point — worth a scope or an
      LED.)
- [ ] Pulse In 1 re-articulates without changing pitch.
- [ ] CV In 2 adds to the breath when patched, and is ignored when not.

### 8. Listening

Not checkboxes — judgements, and the things a model cannot make.

- Does it sound like a wind instrument, or like a filtered oscillator?
- Is the chiff a tongue stroke, a click, or inaudible? Its constants are at the
  top of `breath.h` and are expected to need tuning by ear.
- Is the X knob useful across its whole travel, or interesting in one spot?
- Is the vibrato depth right? 15 cents at 5Hz is a guess.
- Does the low end have body, or is it thin? Does the top thin out too early?

---

## Known-unknown list

Things shipped without evidence, most likely to be wrong first:

1. **Fifteen levels separating at all.** Expect 10-mode.
2. **Every tolerance in `ocarina.h`.** Estimates, inherited from NIBBLE's
   estimates.
3. **`kSettleTicks` versus trill speed.** Raising it to reject hardware noise
   lowers the trill ceiling proportionally, and the failure is abrupt.
4. **The chiff.** Cannot be modelled.
5. **Vibrato depth and rate.**
6. **Whether the register jump feels musical** or like a switch being thrown.
7. **The X morph's endpoints** — whether fully CCW is usefully breathy or just
   noisy.

Record what you find here. A note saying "tried output 3, bar was 3/4, nudged
the knob and got 15-mode" is worth more than any amount of further modelling.
