# DEVLOG

What was got wrong, and how it was found. Written for whoever changes this next
— including me.

---

## v1.0.0 — first build

The card exists, builds clean at ~4.8% flash / ~8.4% RAM, and every model
passes. No hardware session yet, so nothing below has met a real Four Voltages
module.

---

## The design changed three times before a line was written

### Sixteen fingerings is impossible

The card was asked for with four buttons mapped to four holes — sixteen
patterns. It cannot work, and the reason is worth keeping.

Four Voltages has **no rest voltage**. Release every button and its output stays
at whatever was last pressed, through a power cycle. "No holes covered" is not a
state the module can express. That leaves fifteen (every non-empty subset), and
it means **silence cannot come from the fingering at all**.

Which turned out to be a better instrument anyway: silence moved to the breath
knob, where a wind instrument keeps it.

### Fifteen might not work either

No Four Voltages has ever been measured — not here, not in NIBBLE across five
hardware sessions. NIBBLE's own devlog calls ten-level separation "the card's
central gamble". Fifteen levels need ~5.9V of near-even spacing.

So the card measures instead of assuming: it walks all fifteen, computes the
tightest gap, and either runs fifteen or falls back to NIBBLE's proven ten. The
fallback is not a second implementation — combo indices 0..9 *are* NIBBLE's, so
10-mode is the same code with `activeCount_ = 10`.

Then the LEDs report **how close it got**, in quarters of the threshold, because
a bare pass/fail cannot steer anything: it does not distinguish "try another
output" from "nudge the knob".

### The ghost rule had to go

NIBBLE suppresses the level a release falls back to. Carried over unexamined, it
would have been the worst bug in the card.

That rule exists because NIBBLE's only way to be silent *is* the fingering, so a
release firing a note is an artefact. Here, a release **is a note** — lift a
finger from AB and A should sound. That is trilling, which is the gesture this
instrument is built around.

Deleting it removed the plan's subtlest logic (a sticky `ghostFrom_` through
release cascades) and replaced it with four lines. The trade is NIBBLE's
hold-and-tap bank-select gesture, which only worked because releases were
silent.

---

## The bore took the longest and the plan was wrong about it

The original design said overblowing would "fall out of the physics" from a
cubic jet nonlinearity. Three separate things were wrong.

### It was a closed pipe

Measured harmonic content: **h2 was exactly zero at every breath level.** Only
odd harmonics. That is a closed pipe, which physically cannot overblow to an
octave however the jet is tuned — no amount of parameter sweeping would ever
have found it.

The plan said to use a *non-inverting* reflection "for a flute". Backwards: an
open pipe end is a pressure node, so the pressure wave reflects inverted. Two
inversions per round trip give all harmonics. Fixing the sign brought h2/h1 to
about 0.5 immediately.

This is exactly the kind of sign someone flips to fix an apparent tuning problem
and silently changes the instrument, so it carries a comment.

### The loop was a fourth sharp

With the sign corrected the whole instrument was uniformly ~494 cents sharp,
which looks like a bad tuning constant and is a geometry error.

The jet tap is not a passive observer — it feeds the nonlinearity, so it forms a
second path of half the length. **One period is 1.5 delay traversals**, and
measured `f × delay` is a constant 32000 = 48000/1.5. The delay table was
regenerated against that.

### Overblowing still did not emerge

With a correct open pipe, driving the jet harder brightens the tone measurably
but does not change which mode the loop prefers. Every mechanism tried —
breath scaling jet gain, breath shortening the jet delay, breath moving the
operating point along the cubic — either changed nothing or threw the loop into
non-harmonic modes at 2.23× and 27×. Those are chaos, not registers.

So the register change is **explicit**: hard breath adds an octave, with a
hysteresis band. A player cannot distinguish it; the source can, and says so.

What *is* emergent, and is used to bound the range: above about MIDI 78 the bore
abandons its fundamental on its own, because the jet tap drops under ~20 samples
and the interpolator can no longer place its phase. That is why `kPitchHiNote`
is 75 — past it the card would play an octave above what CV Out 1 reports.

### Measurement lied twice before it told the truth

Zero-crossing counting reported "overblowing" that was added harmonics on an
unchanged fundamental. Then autocorrelation locked onto the 4th harmonic in one
sweep and onto sub-harmonics in another, each time with confident wrong numbers.

Both produced hours of chasing. Goertzel at known frequencies — asking "how much
energy is at exactly f0, and at exactly 2·f0" — is unambiguous, and is what the
shipped model uses.

---

## Bugs the models caught

None of these would have been obvious on hardware; several would have been
mistaken for something else entirely.

| Bug | How it would have presented |
|---|---|
| cents→mV constant was 683 (2/3) not 853 (5/6) | perfectly in tune until you touch the fine control, then 19.5 cents out at full travel — reads as a badly calibrated knob |
| fine-tune approximation linear only | 3.1 cents off at the extremes; plausible, wrong |
| DC blocker's own shift truncation | permanent −497 offset on a ±4300 signal. Lowering the corner made it *worse*, which is what identified it as rounding rather than leaked signal |
| breath curve returned 4096 at full knob | one count past full scale, clipping the DAC on the loudest note |
| `kMaxRoot` derived from the CV rail | the *bore* clamps first, silently: delay stops while CV Out 1 keeps climbing |
| register thresholds set against the knob, applied to the curve | octave jump at 88% of travel instead of 70% — working, but unplayable |
| hand-typed delay LUT | several entries wrong; regenerated from exact arithmetic |

---

## Bugs only reading caught

Every module tested correct in isolation. `main.cpp` sequenced them wrongly, and
the models could not see the seam.

- **Pulse Out 2 could never fire.** `ChiffFired()` is an edge that
  `Breath::Tick()` consumes, and it was read *after* `Tick()`. Always false. The
  chiff's extra noise was never applied either. Both silent.
- **The chiff stop did not stop.** `Waveguide::Mute()` was written, documented,
  and never called — so switch-down zeroed the air and left the bore ringing,
  which is the one thing a stop exists to prevent. Worse, `SetTimbre()` reset
  the loop gain, so the mute would have been cancelled by the next X movement.
  Loop gain now belongs to `Mute()`/`Unmute()` alone.
- **The level detector froze during tuning.** Its smoothing and settle plateau
  are continuous state; leaving them unfed meant the first note after tuning was
  matched against a plateau from before it started.
- **Negative coarse tune was discarded** for the bore but applied to the CV, so
  tuning flat moved CV Out 1 and left the voice — disagreeing by up to an
  octave, in one direction only.
- **The glide never arrived.** `UpdatePitch()` early-returned on "nothing
  changed", which is the normal state *during* a glide.

That last one forced a better design. Gliding the delay length is wrong twice
over: delay is proportional to 1/f, so a linear slew sweeps pitch unevenly, and
it forces the CV — which is linear in pitch — to be recovered from a ratio via a
log, an approximation 500 cents out over an octave, or a runtime 64-bit divide
of exactly the kind that blew NIBBLE's budget. **Gliding the semitone in Q8**
makes the sweep musically even and lets both outputs fall out of the same two
numbers.

---

## Ideas not taken

- **A second bore on Audio Out 2**, octave-doubled, making it a double ocarina.
  Costs ~111 cycles/sample and fits comfortably. Audio Out 2 currently carries
  the breath-noise component alone, which is nearly free and more useful patched.
- **Saving the tuning to flash.** Deliberately not: it would be the only
  flash-write path on the card, and the XIP/interrupt dance it needs is a
  hazard with no matching benefit for a value that takes ten seconds to set.
- **Allpass interpolation** for the fractional delay. More accurate at the top,
  but it glitches tuning on every note change — bad in an instrument that
  changes note constantly. Linear interpolation's mild lowpass is a feature: it
  is the high-frequency loop damping a real bore has.

---

## Open questions for the first hardware session

1. `minGap15` per Four Voltages output. **The** number. See HARDWARE-TESTING.md.
2. Is `kSettleTicks` right? It trades noise rejection against trill ceiling, and
   the failure is a cliff — trilling goes silent rather than ragged.
3. Is the chiff audible and does it read as a tongue stroke?
4. Does the register jump feel musical or mechanical?
5. Is the low end thin? The bore is short at 65Hz.
