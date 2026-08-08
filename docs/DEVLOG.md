# DEVLOG

What was got wrong, and how it was found. Written for whoever changes this next
— including me.

---

## v2.1.0 — second hardware session

Three reports, one of them a hard bug.

### "Gliss mode seems to hang up"

LEDs cycling 0→1→3→2 with the noise continuing is the TUNE animation, and the
cause was a design error rather than a coding one:

**Switch UP was both legato and a staged hold gesture** — 1s showed the gap bar,
3s entered tune. So any slur lasting three seconds dropped the card into tune
mode. Worse, tune exited on a *tap*, and a tap means switch DOWN, which is the
mute. Holding up to play legato therefore led somewhere with no obvious way
back.

The rule this cost, and it is general: **never hang a hold gesture on a switch
position that is also a continuous playing mode.** Momentary positions can carry
gestures; held ones cannot. Switch up now does exactly one thing.

### Tuning moved INTO calibration

The first fix was to put tune after a successful calibration. Better, but still
a phase to sit through — and the observation that settled it came from the user:
**calibration and tuning use disjoint controls.** Calibration reads CV In 1 and
the switch tap; tuning reads Y and Main. Nothing is shared.

So they now run concurrently. A quiet reference note drones for the whole
calibration and Y/Main tune it while the fifteen combinations are taught. That
removes a mode, removes a gesture, and removes the phase — three things gone for
no cost.

The drone sets its **own** timbre, and has to: `ApplyTimbre()` derives everything
from the breath knob, which during calibration is the fine tune. Left to it, the
reference note would change character every time the tuning was nudged.

### "Breath is basically too loud all the time, next to note"

The air was mixed level with the tone rather than under it. Fixed by attenuating
the noise itself (`kAirGainQ12 = half`) rather than by changing the mix ratio —
pushing the ratio far enough to fix the balance also flattened the X knob,
because the audible range of the mix is narrow.

That in turn moved the useful mix range UP (a quieter source needs a larger
share to be heard at all), so `kAirMax/kAirMin` were re-measured: 3450/1800
rather than 3700/2200. X now sweeps 0.90 → 0.995 tonal, still clearly breathy at
the CCW end with the pitch always leading.

### "Linear rather than the log it needs to be"

v2.0 SQUARED the level curve, which is the *opposite* of what was wanted: it
spends the whole sweep still getting louder and only arrives at the very end.
That is what read as an unresponsive knob.

The fix is **two curves from one knob**:

| | shape | drives |
|---|---|---|
| **level** | `1-(1-n)³` — fast, then flat | the VCA |
| **effort** | linear to the stop | brightness, drive, register |

Level is at 3453/4096 by half the travel and only gains 643 more over the entire
top half. Effort keeps climbing (+2210 over that same half), so once the note
stops getting louder it keeps getting brighter and richer — which is what the
request for "a quick up-to-volume then continuing should add overtones" actually
describes.

The register threshold moved onto effort for the same reason: on the level curve
it would sit in a region where a large physical movement barely changes the
number.

One off-by-one caught on the way: the cubic returns exactly 4096 at full travel,
one past the documented 0..4095. Harmless in today's arithmetic — but the
squared curve it replaced had the identical off-by-one and *that* one clipped
the DAC. Clamped rather than reasoned about again later.

---

## v2.0.0 — the voice was rebuilt, because v1's was broken

First hardware session. Two symptoms, reported by ear:

> That's super super metallic sounding — and the main knob CCW never gets even
> close to silent.

Both came from **one line**, and it is the most instructive bug in the project.

### The jet drove itself

The waveguide's jet excitation was:

```cpp
const int32_t x = offset + noise - ((jetTap * kJetFeedbackQ12) >> 12);
```

`offset` and `noise` both scaled with breath. **The feedback term did not.** So
with the knob at zero the bore's own returning pressure still drove the
nonlinearity — measured, the jet was still injecting 1919 into a bore that was
supposed to be silent. The instrument played itself.

Everything followed from that:

| Symptom | Measurement |
|---|---|
| never silent | jet output 1919 at breath = 0 |
| knob does nothing | 2205 → 2697 → 1742 rms across the whole travel — 1.2:1, and *quieter* at the top |
| metallic | h2 = 1.14× the fundamental, h5 = 0.64, h8 = 0.41 |
| wrong octave above MIDI 60 | at MIDI 72 the second harmonic was 2.4× the first |

### The tuning constant had absorbed the bug

`kLoopFactorNum/Den = 1.5` was derived by measuring `f × delay` on the running
system — but that system's pitch was being set by the runaway nonlinearity, not
by the bore. Measured on the **linear resonator alone**, with the jet
disconnected, the factor is exactly **2.0**, which is textbook for an inverting
reflection.

So a constant that looked like careful empiricism was actually a fitted
artefact of a broken loop, and it had been documented at length as if it were
physics. That is the trap: v1's DEVLOG entry for it is confident, detailed, and
wrong.

### The jet could not be saved

Every repair attempt is recorded because each one looks reasonable and none
worked:

- gate the jet output with breath → pitch collapsed to 0.27×, 2.15×, 8.02×
- gate the *coupling* term instead → same, plus instability
- shorten the jet tap so it perturbs rather than resonates → loop factor fell
  toward 0.1, i.e. oscillating on a high harmonic
- re-damp to kill the competing modes → fundamental correct only at some delay
  lengths, chaotic at others

The reason is structural: the nonlinear path and the bore **compete** to set the
frequency, and the nonlinear path keeps winning. A jet model needs the loop gain
around the nonlinearity to sit in a narrow window that depends on pitch, damping
and drive simultaneously — and nothing in the design was holding it there.

### What replaced it

A tuned oscillator, a noise generator, a resonant lowpass standing in for the
body, and a VCA — all feed-forward. Nothing feeds back into the nonlinearity, so
it can only *colour* a pitch that is already exact.

| | v1 waveguide | v2 voice |
|---|---|---|
| tuning | 30–170 cents (variants) | **0.92 cents** |
| silence at zero breath | never | **exactly 0** |
| dynamic range | 1.2:1 | **98:1** |
| h2 / h1 | 1.14 | **0.016** |
| loudest partial vs octave | 0.41× at MIDI 72 | **19× everywhere** |
| range | MIDI 36–75 | **MIDI 36–91** |
| RAM | 8.35% | 7.48% |

The old MIDI 75 ceiling was itself an artefact — above it the waveguide lost its
fundamental because the jet tap got too short. Nothing has to pick its own mode
now, so the range extends nearly two octaves higher, and **every scale gets all
fifteen degrees and a full octave of transpose**. The two arpeggios previously
lost their top degrees and could not transpose at all.

The honest cost: **this is no longer a physical model.** It is a synthesiser
shaped to sound like one. `info.yaml` and `flute.h` both say so.

### The models failed, and that is the real lesson

Every v1 assertion passed. They tested tuning, stability, DC, harmonic presence
— internals. None of them noticed that the instrument was **ignoring the breath
knob**, because none of them ever varied breath and compared the result.

One was worse than useless: `E(f0) > 0.20 * E(2f0)` passed a note whose octave
was five times louder than its fundamental. It was written to allow the
brightening a real flute has toward the top, and it allowed the note being
wrong.

`flutesim.py` is now written against **what a player would report**:

```
silence          zero breath must be EXACTLY zero out
dynamic range    the knob must span at least 30:1
monotonic        more breath must always mean more level
brightness       more breath must also mean brighter
character        X must move air content across an audible range
fundamental      the note played must be the note asked for, loudest
```

Any one of those would have caught v1 before it was flashed. When a model and a
pair of ears disagree, the ears are the specification.

---

## v1.0.0 — first build

The card exists, builds clean at ~4.8% flash / ~8.4% RAM, and every model
passes. No hardware session yet, so nothing below has met a real Four Voltages
module.

**Everything in this section about the bore is superseded — see v2.0.0.** It is
kept because the reasoning was careful and still wrong, which is worth being
able to read.

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
