# DEVLOG

What was got wrong, and how it was found. Written for whoever changes this next
— including me.

---

## v4.0.3 — the "easing" in v4.0.2 was invisible at the ear

> "still VERY AUDIBLY stopping. I'm not hearing that decay to silence."

v4.0.2 slowed the release's exponential shift further below two thresholds,
verified in the model by measuring the dB-per-200ms rate near the start and
end of the tail — and that measurement genuinely showed the rate falling.
The model was telling the truth about `env_`, the internal accumulator, and
lying about what a player would hear.

`LevelQ12()` — what the VCA actually multiplies by — is `env_ >> kEnvFrac`, a
12-bit integer. An exponential's per-tick step is a fraction of the
**remaining value**, so slowing the shift further only postpones the point
where that step rounds to less than one `LevelQ12()` count; it does not
prevent it. Traced sample-by-sample: below the second threshold the last
nineteen audible output levels each held **dead flat for 43–85ms**, and then
the very last one — level 1 — dropped straight to 0. A staircase with a long
flat tread and then a cliff is not a fade by any measure that matters; it is
closer to what was reported the first time, just with more silence smuggled
in front of it. `test_the_tail_eases_off()` passed because it only ever
looked at `env_` through 200ms dB windows wide enough to average the plateau
away — a test measuring the accumulator, not the loudspeaker.

### The fix is a change of shape, not a slower slope

Below `kEaseLevel1` (32, replacing the old two-threshold `kEaseLevel1`/
`kEaseLevel2` pair) the release stops being exponential in `env_` altogether
and switches to a **linear ramp**, sized in ticks
(`kEaseTailTicks = 96`, ≈32ms) rather than as a fraction of the remainder.
The step size is computed **once**, at the instant the ramp is entered, from
the level at that instant, and held fixed for the whole ramp — recomputing
it every tick against the shrinking value would just reconstruct the same
bug one level lower down.

That guarantees every one of the last audible counts gets equal, bounded
time and the final step to silence is never bigger than the ones before it.
Traced on a full-peak note: level 32 down to 0, one count every ~3 ticks
(1ms), perfectly even, no plateau.

Total release time for a full-peak note dropped to 859ms (from the un-eased
v4.0.1 baseline's ~1.5s, and from v4.0.2's inflated-but-inaudible ~3.06s) —
shorter than either prior version, because it is no longer paying for a tail
that sounded identical to a shorter one. `kReleaseShiftMax` was not touched;
if the overall release still feels too short once this is heard on hardware,
that is the knob to raise next, now that the ending itself is no longer the
problem.

**The lesson: a model result is only as honest as what it's allowed to
measure.** `test_the_tail_eases_off()` was rewritten to walk the tail
sample-by-sample and assert directly on the thing that matters — no held
level lasting more than ~40ms near the end, and the last five audible counts
each exactly one apart — rather than trusting a coarse dB-rate average to
notice a staircase hiding underneath it.

---

## v4.0.2 — a straight line in dB still sounds like it stops

> "I am still hearing notes finish. can the Main/X knob have some that decay to
> silence nicer!"

The floor fix in v4.0.1 let the last audible dB actually play, but the shape
was still wrong. A one-pole release is a **constant rate in decibels** — a
straight line on a dB-vs-time plot — and a straight line has no sense of
"ending" until it crosses the floor and is simply gone. Real instruments don't
decay like that: the rate itself slows as the sound dies, which is what a
"fade" actually sounds like as opposed to a "stop."

### Easing the tail

`Breath::Tick()`'s release branch now increases the release shift — i.e.
slows the decay rate — as the envelope crosses two lower thresholds, measured
in the same units as `LevelQ12()`:

- below `kEaseLevel1` (256 of 4095): one extra shift
- below `kEaseLevel2` (32 of 4095): two extra shifts total

So the note decays at its normal (peak-coupled) rate for most of its length,
then eases twice on the way out. Measured on a full-peak note in
`tools/breathsim.py`: the first 200ms drops 10.2dB, the last 200ms before
silence drops only 1.7dB — a six-fold slowdown — and the total release
stretched from ~1.5s to ~3.06s, all of it now audibly *tapering* rather than
running at one rate until it's gone.

`kEnvFloor` did not need to move again; it was already at the lowest
representable step (see v4.0.1). This is purely a shape change on the way down
to that floor.

A new model test, `test_the_tail_eases_off()`, plays a full-peak note and
asserts the late-stage dB/200ms rate is under half the early-stage rate, plus
that the whole tail clears 2000ms — so a future change to the release
constants can't silently flatten the taper back into a straight line without
tripping a test.

---

## v4.0.1 — the tail was being chopped, and every attack clicked

> "the Decay to silence is always too abrupt (I can hear it cut off)"

Two separate faults at the same seam, and the second was not reported because
the first was masking it.

### The floor was 18dB too high

`kEnvFloor` snapped the envelope to zero at level 8 of 4095 — **-54dB**. That
reads as negligible and is plainly audible: it lopped the last 18dB off every
note, so the tail stopped rather than faded.

The right floor is one step below what the DAC can render. Output is 12-bit and
`LevelQ12()` shifts the accumulator down by `kEnvFrac`, so the last audible
level is `env == (1 << kEnvFrac)`, i.e. **1**. Cutting there means the envelope
runs exactly as long as it is audible and not one tick longer.

A loud note's release went from 1086ms to 1484ms, and the last few dB now take
about 300ms instead of being removed. As a bonus the ~85ms the old envelope
spent counting down BELOW audibility — with Pulse Out 1 still high — is gone.

### And every attack began with a step

Checking whether the *start* of a note was also at fault turned up a separate
bug: the oscillator's phase kept free-running while the voice was silent, so a
new note began wherever the sine happened to be. Measured, a **2671-count step**
on the first sample — against the ~200 a 220Hz sine moves per sample. That is a
click on every single note, and no envelope can hide it because even the fastest
attack is applied after the oscillator.

Parking the phase at zero while silent takes it to 87 counts. The DC blockers
are reset with it, since their stored history belongs to a note that has ended.

**The lesson: when a seam sounds wrong, check both sides of it.** The report was
about the decay; the attack was worse and nobody had mentioned it, because the
chopped tail was the louder problem.

---

## v4.0.0 — the instrument is bowed, not blown

The largest change since the voice was rebuilt, and it changes how the card is
played rather than how it sounds.

**The momentary switch is now the bow.** The card is silent until you sound it:

- **tap** → a struck note, attack then release
- **hold** → the note sustains for as long as you hold it
- **while held** → moving Main swells the note in real time, and changing the
  fingering GLIDES to the new pitch without re-attacking

Pulse In 1 is the same control, so a sequencer gate plays the card exactly as a
finger does. A short trigger gives a struck note; a long gate gives a held one.
One behaviour, two sources, no mode.

Before this, Main sounded a continuous drone and the switch was a MUTE. That is
backwards for something you play rather than leave running, and it meant every
note had the same shape.

### The envelope

Main sets the note's peak **and**, coupled to it, how long it takes to die: a
loud note rings for over a second, a quiet one is gone in 95ms. One gesture,
two musical consequences — that coupling is what makes the knob feel like
dynamics rather than like a fader.

X sets the attack shape alongside its vibrato duties, from a 3ms strike to a
790ms swell, so the anticlockwise end is percussive-and-wide-vibrato and the
clockwise end is slow-and-gentle in both respects at once.

**One measurement error worth recording.** The slow attack was first set from
the one-pole's time to −60dB, which is the wrong measure for an attack: a
one-pole reaches 90% in about 2.3 time constants, not 6.9. Shift 9 arrived in
196ms rather than the intended second, so the "swell" end of the knob was barely
slower than its middle. Shift 11 gives the 790ms it was supposed to.

### Portamento is a toggle now

Flick the switch **up and let go** — it toggles, and LED 4 shows the state.
Holding up does nothing at all.

This is the third arrangement of this control and the first that does not fight
the player. v2.0 made UP a held legato mode *and* hung a staged 1s/3s gesture on
it, so a three-second slur dropped the card into tune mode with no way out. The
rule that cost, applied properly this time: **a switch position you hold while
playing cannot also carry a gesture.** Up is now momentary in practice.

The glide itself went from shift 9 to 5 — about 70ms between adjacent notes
rather than over a second. On a struck instrument the old rate meant the glide
was still arriving after the note had decayed.

### Calibration lost its gesture

There is no 2-second hold any more. The switch is a playing control and cannot
carry one, which is the same rule as above. Calibration runs once at power-on;
reset to get back to it.

### LEDs

LED 4 became the portamento indicator — the one thing about the card's state
that cannot be heard until the next note. LED 5 became the level meter, so the
panel shows each note's shape as it rises and decays.

---

## v3.3.0 — an S-curve, because both ends matter

> "Doesn't seem to cleanly go to silence. Can we have an S shaped ramp from
> silence?"

The fifth-power curve from v3.1 fixed the slow ramp by being steepest **exactly
at the bottom** — measured, 6dB per ten counts of knob just above the threshold.
That is a switch, not a fade, and it is why the instrument did not so much fade
in as arrive.

Four curves have now been wrong here in four different ways, which is worth
listing because each looked right at the time:

| | shape | what it got wrong |
|---|---|---|
| v2.0 | squared | still getting louder at the very top |
| v2.1 | cubic `1-(1-n)³` | 84% of AMPLITUDE by half travel, but only -12dB at a quarter |
| v3.1 | fifth power | fast, but steepest at the bottom — no soft start |
| v3.3 | **smoothstep + lift** | flat at BOTH ends, steep through the middle |

What the first three missed is that **both ends matter**. Leaving silence gently
needs the curve flat near zero; not feeling sluggish needs it steep in the
middle. That is a smoothstep, `n²(3-2n)`. The `lift` term `s(1-s)` is then zero
at both extremes and largest in the middle, so it recovers the speed a plain
smoothstep gives away without touching the flat ends.

The deadband dropped 120 → 60 with it. It had been doing double duty as a mute
AND as a buffer against the sound slamming on; with an S-curve the second job is
gone, so the first audible sound now arrives at about 3% of travel rather than
4.5%.

### The multiply order is load-bearing

The obvious `n2 = (n*n)>>12; sm = (n2*(3-2n))>>12` throws away four bits before
the second multiply, and the rounding that costs made the curve
**NON-MONOTONIC**: 108 places where turning the knob UP made the level go DOWN.
Individually inaudible, but a volume control that sometimes goes backwards is
not a volume control.

Shifting once at the end is exact but overflows int32 — `n*n*(3-2n)` peaks at
2.06e11. Reordering so the small factor is reduced first keeps the peak
intermediate at 16.7M and is exactly monotonic across all 4096 inputs.
`breathsim.py` asserts zero backward steps.

---

## v3.2.0 — the "vibrato step" was an octave jump

> "when vibrato boundary on main knob is added it seems to be an octave higher
> - hence step"

A better diagnosis than mine, and it identified something I had walked past
twice. The step was never in the vibrato at all.

**The Main knob still had a register switch on it.** Past about 70% of travel it
added twelve semitones — `if (regLast_) semi += 12;` — instantly, in one control
tick. That is a hard octave jump landing right in the middle of the vibrato
stage, which is exactly what "the vibrato boundary sounds an octave higher"
describes.

It was a fossil. The register switch existed to fake the overblow that the v1
WAVEGUIDE could not produce on its own, and it survived two complete rewrites of
the voice because nothing ever forced a look at it. Its own comment still said
"overblowing does not emerge from the bore's physics (see flute.h)" — a bore
that had not existed for two versions.

Removed entirely. The Main knob is level, then vibrato, and **nothing in its
travel changes the pitch**. The octave is chosen deliberately with X during
calibration, which is where an octave control belongs.

`tools/breathsim.py` now asserts that `Breath` exposes no register at all, so it
cannot quietly come back.

### Why the Q4 fix in v3.1 was still worth doing

The whole-cent quantisation was real — vibrato genuinely did nothing below one
cent and then arrived at two. It just was not the thing being reported. Both
were steps in the same region of the same knob, which is how one masked the
other.

The lesson is about diagnosis rather than code: I had a plausible mechanism, it
measured true, and it was the wrong bug. "It's not there in portamento mode"
narrowed it correctly; "it seems to be an octave higher" identified it exactly.
Ask what the player is HEARING, not just where.

---

## v3.1.0 — the knob, the step, and ten is enough

Four reports from the third hardware session.

### "The ramp from silence to sound still seems very slow"

Two causes compounding, and the second is the interesting one.

The deadband was 7% of travel, which is a long way to turn before anything at
all happens. That part was easy.

The real cause was that **the level curve was being judged in the wrong units.**
v2.1 used a cubic, `1-(1-n)^3`, which reaches 84% of full AMPLITUDE by half the
travel — that looks fast, and it is what the previous devlog entry proudly
reported. But loudness is logarithmic: in dB the same curve was still at -17dB
at a tenth of the travel and only reached -12dB, roughly half as loud, at a
QUARTER. That is the slow ramp.

A fifth power gets to half perceived volume by about an eighth of the travel and
is essentially full by a third. Two extra multiplies.

**The lesson is the measurement, not the constant:** an amplitude curve says
almost nothing about how a knob feels. Judge it in dB.

### "The adding vibrato stage has an audible step"

Followed by the clue that solved it: *"It's NOT there in portamento mode."*

Vibrato depth was carried in WHOLE CENTS. At depths under one cent the
arithmetic `(fast_sin(phase) * depth) >> 15` rounds to zero for every phase, so
vibrato produced literally nothing until its depth crossed two cents — and then
appeared all at once.

Portamento hid it because a glide keeps `glideCents` continuously non-zero, so
`ApplyFineCents()` runs every tick either way. In tongued mode with the tuning
centred, the total cents term was exactly zero, and the `if (cents)` guard
skipped the pitch bend entirely. The pitch snapped between "unmodified" and
"modified".

Cents are now Q4 (sixteenths) through the whole pitch path. The constant did not
even change — dividing by 16 and shifting four more bits cancel exactly — only
the shift and the callers.

### "Make the vib start much earlier in the Main knob"

`kVibOnset` 2000 → 1200, so vibrato begins about a quarter of the way up and has
three quarters of the travel to grow in. It still sits above where the level
curve has arrived, so the two stages stay legible: the bottom of the knob is
loudness, the rest is expression.

### "Let's just use 10 key combos. 15 is too many"

The adaptive 15/10 machinery is gone: no mode decision, no second tolerance set,
no four-bar gap meter, no mode-announcement LED vocabulary. Calibration is ten
taps instead of fifteen.

The triples and the all-four combo remain SAFE rather than merely unused — they
land far from every learned level, so the match window rejects them and pressing
one leaves the current note alone. `levelsim.py` asserts exactly that, because
it is the property that makes dropping them safe.

### The octave, and a ceiling that had to move

X now picks the octave during calibration — C2, C3, C4 or C5, defaulting to C4,
a concert flute's lowest note. The card had been sitting at C2, which is
sub-bass and nothing like a flute.

That immediately exposed a latent clamp. `kPitchHiNote` was 91, and the top of a
scale from C5 reaches MIDI 100 — the pitch would have clamped while CV Out 1
kept climbing, which is the silent disagreement `pitchsim.py` exists to catch.
It caught it.

The ceiling is now 108, and that limit comes from the WAVEFOLDER rather than the
oscillator: a pure sine would not alias until its fundamental passed Nyquist
around MIDI 135, but folding generates harmonics, and at MIDI 108 the fifth
harmonic is 20.9kHz — just inside. At MIDI 112 it is 26.4kHz and folds back as
an inharmonic whistle.

Transposition is capped per-octave rather than per-scale now (`{12, 12, 12, 8}`),
because the top octave is the one that runs out of room.

---

## v3.0.0 — the air went, vibrato arrived

> "No - not enjoying it. The air is adding nothing."

Which was right, and is a judgement no model could have made. The v2 voice mixed
filtered noise under a tone as "breath"; on hardware it was either inaudible or
it muddied the pitch, and the middle ground that would have sounded like a
player breathing does not exist at these levels.

So the whole air path is gone — noise generator, air/tone mix, resonant body
filter, chiff noise burst. What replaces it as the source of expression is
**vibrato that grows with the knob**, which is a thing a player actually does
and is audible in a way the noise never was.

### The shape of the instrument now

| | |
|---|---|
| **Main** | level (fast, log) → then **vibrato depth** |
| **X** | vibrato character → fast/wide, fast/tight, slow/wide, + level tilt + fold |

The two stages of Main do not overlap: vibrato starts at `kVibOnset = 2000`,
above where the level curve has flattened. Letting them overlap would mean a
turn in the lower half both raised the level and started a wobble, so neither
would read as its own gesture.

### One oscillator, three outputs

Audio 1 is a sine, Audio 2 the same wave wavefolded, Pulse 2 the same wave
squared — all from one phase accumulator, so they cannot drift apart. Mixing
Audio 1 and 2 cannot comb-filter, and the square always lines up with both.

The square is taken from the oscillator's SIGN rather than from the level-scaled
sine: a comparator on a scaled signal flips at the same instants but chatters
around zero as the level approaches silence.

### The wavefolder is capped at half its range, and that is measured

Sweeping the fold further is **not monotonic**. The spectral centroid climbs
1.04 → 3.60 up to fold 2048, then DIPS back to 3.00 before rising again, because
completing a fold returns the fundamental before the next harmonic pair arrives.

That dip is a real property of wavefolding rather than a bug — but a knob that
gets brighter, then duller, then brighter reads as broken under the hand. So
`kFoldMax` stops at the top of the monotonic region, giving up available
richness to keep the control honest.

### The CV cache stopped hitting, on purpose

`CVOutMillivolts` reaches a flash-resident helper, so the card caches its last
value to keep XIP reads out of the control path. With vibrato running, the pitch
changes every tick and the cache almost never hits.

That is correct rather than a regression: a pitch CV that only updated once per
note would not carry the vibrato, which is the entire expression. Measured, the
two calls are ~80 cycles of a 4000-cycle budget and the XIP line stays hot at a
3kHz call rate. The cache still earns its place across the whole lower half of
the knob, where vibrato is at zero.

### New I/O

CV In 2 became a pitch offset (±2 octaves, quantised to semitones so an
imprecise voltage moves a musical interval rather than leaving everything
slightly sharp). The two audio inputs became CV offsets for Main and X — doubled
on the way in, because `AudioIn` is ±2048 where the knobs are 0..4095, so a
full-scale CV can sweep a knob end to end from either extreme.

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
