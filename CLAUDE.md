# NIBBLE OCARINA — working notes

A wind instrument for the Music Thing Workshop Computer. Sibling of NIBBLE
(`../WoskshopButtons`, note the typo in that folder name) and WorkshopBio.

It was a physical model in v1 and is not one now — see "The voice" below. The
README still calls it a flute, which it sounds like; the source does not claim
physics it no longer does.

Read this before changing anything. Most of it is the record of something that
was got wrong first.

---

## Hard rules

**Fixed point only.** `ProcessSample()` runs inside a DMA interrupt with a
4000-cycle budget at 192MHz. The RP2040 has no FPU. `-Wdouble-promotion
-Wfloat-conversion` are on to enforce this; a warning from either means float
has leaked into a hot path.

**No 64-bit division, ever.** The M0+ has no hardware divider, so a 64-bit
divide is a libgcc call of several hundred cycles. It was the single biggest
cause of NIBBLE overrunning its sample budget. 64-bit *multiplies* are fine
(~10 cycles). Division by a compile-time constant is fine — gcc strength-reduces
it. It is the runtime 64-bit case that must not appear.

**Nothing is written to flash.** Not the learned levels (the Four Voltages knob
invalidates them, so a saved calibration would silently restore a *wrong* one),
not the tuning offsets (deliberately session-scoped). `hardware_flash` is linked
only because `ComputerCard.h` needs it to read the factory CV calibration from
EEPROM. Do not add a flash-write path; the XIP/interrupt dance it requires is a
hazard this card has no reason to take on.

**`CVOutMillivolts` and `CVOutMIDINote` are flash-resident.** Cache the last
value and only call on a change, or they put XIP reads in the control path.

**Hot functions get `__not_in_flash_func`, hot tables `__not_in_flash`.**

**Nothing touching hardware in the card's constructor.** It runs before the SDK
is ready and a peripheral access wedges the chip. Setup goes in `main()`.

**Keep `PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64`.** Without it the card fails cold
power-up but works from a warm reset, which is a maximally confusing bug.

---

## The hardware's two awkward facts

Both drive more of this design than anything else.

**1. Four Voltages has no rest voltage.** Release every button and the output
stays where it was — through a power cycle. So:

- There are **fifteen** readable states, never sixteen. "No holes covered"
  cannot be expressed.
- **Silence has to come from the breath knob.** That is not a stylistic choice.
- The first settle after power-on must be swallowed (`primed_`), or the card
  fires a note nobody asked for.

**2. Its knob moves every level unpredictably.** Hence RAM-only calibration, and
hence the card measuring rather than assuming.

---

## No ghost rule — this is the big divergence from NIBBLE

NIBBLE suppresses the level a release falls back to, because with no rest
voltage a naive detector fires a spurious note every time you let go, and its
only way to be silent *was* the fingering.

OCARINA has a breath knob, so a release is not an artefact — **it is a note**.
Lift a finger from AB and A should sound. That is what makes trilling work.
Carrying NIBBLE's rule over would actively break the instrument.

The trade, and it is deliberate: NIBBLE's hold-and-tap "bank select" gesture is
gone, because it worked only by virtue of releases being silent.

---

## Architecture

```
main.cpp     boot, control dispatch, calibration, tuning, LEDs, routing
levels.*     Four Voltages -> combo index. 15/10 adaptive. No ghost rule.
pitch.*      combo -> degree -> semitone -> phase increment AND millivolts
flute.*      the voice: oscillator, saturator, noise, bore filter, VCA
breath.*     the air, articulation, chiff, the register switch
ocarina.h    shared vocabulary and every tolerance
```

48kHz sample rate, 3kHz control rate (`kCtrlDiv = 16`). The two control jobs are
staggered onto different samples (`ctrlDiv_ == 0` and `== 8`) so no single
sample pays for both.

---

## The voice, and what it is not

**This is NOT a physical model, and the header says so.** v1 was a jet-driven
waveguide. It shipped with a bug that made every symptom the first hardware
session reported, and the bug could not be fixed without removing the jet.

The jet's feedback term had **no breath in it**, so at zero breath the bore's
own pressure kept driving the nonlinearity — measured, still injecting 1919 into
a bore that was meant to be silent. The instrument played itself. That single
line produced: never silent, a breath knob spanning 1.2:1 in loudness (and
getting *quieter* at the top), a spectrum with h2 louder than the fundamental,
and an octave that took over above MIDI 60.

Worse, `kLoopFactor = 1.5` had been *measured on that broken system*, so the
tuning constant absorbed the bug and hid it. On the linear resonator alone the
factor is exactly 2.0.

Four separate repairs were tried — gating the jet output, gating the coupling,
shortening the jet tap, re-damping — and each left the loop oscillating at
0.27x, 2.1x, 5x or 8x the intended pitch. The nonlinear path and the bore
compete to set the frequency and the nonlinear path wins. See docs/DEVLOG.md.

What is there now is feed-forward: oscillator, noise, resonant lowpass, VCA.
Nothing feeds back into the nonlinearity, so it can only colour a pitch that is
already exact. **If you are tempted to reintroduce a feedback path for
authenticity, read the devlog first.**

**Envelope floors belong at the DAC's resolution, not at a round number.**
`kEnvFloor` was 8 of 4095 — -54dB — which chopped the last 18dB off every note
and was clearly audible as the decay "cutting off". The last audible level is
`1 << kEnvFrac`; anything above that removes tail the hardware could still
render, anything below just holds the gate high in silence.

**A one-pole release is a straight line in dB, and that reads as "stopping"
not "fading."** Fixing the floor (above) was not enough — the decay still
sounded like it finished rather than eased away.

**Slowing an exponential's shift near zero does not make it audible — it
makes it invisible.** The first attempt at the above added extra shift below
two low thresholds, verified by a model that measured `env_`'s dB-per-200ms
rate and saw it fall. But `LevelQ12()` is `env_ >> kEnvFrac`, a 12-bit
integer, and an exponential's step is a fraction of the *remaining* value —
so slowing the shift only postpones the point where that step rounds to under
one output count, it does not prevent it. Traced sample-by-sample: the last
nineteen audible levels each held dead flat for 43–85ms and then the last one
dropped straight to zero. A held plateau then a cliff is not a fade, and the
model's 200ms-wide dB windows were coarse enough to average the plateau away
without ever seeing it — a lesson in measuring the accumulator instead of the
loudspeaker. **Fix:** below `kEaseLevel1` (32) the release stops being
exponential and becomes a **linear** ramp, `kEaseTailTicks` (96, ≈32ms) long,
with the step size fixed once on entry from the level at that instant — never
recomputed against the shrinking remainder, or it reconstructs the same bug
one level down. `breathsim.py`'s `test_the_tail_eases_off()` walks the tail
sample-by-sample and asserts no held level lasts more than ~40ms near the end
and the last few counts step down by exactly one each — not a dB-rate average,
because that is what let the plateau through the first time.

**The oscillator's phase is parked at zero while silent.** Letting it free-run
puts a step of up to full scale on the first sample of every attack — measured
at 2671 counts against the ~200 a 220Hz sine moves per sample — and no envelope
can hide it, because the attack is applied after the oscillator. `flutesim.py`
asserts the seam stays under 400 counts.

**The instrument is BOWED.** Silent until the switch is held or Pulse In 1 goes
high. Tap = struck note, hold = sustain, and while held the fingering glides
without re-attacking. Main sets the note's peak AND its release length (louder
lasts longer); X sets the attack shape. Do not reintroduce a continuous drone —
it was there until v4.0 and it meant every note had the same shape.

**Main has two jobs on one position, in sequence, not two knobs.** While
gated it sets peak; the instant the gate falls it stops doing that (the peak
already shaped the note) and starts trimming the release LIVE, every tick,
for as long as the tail sounds — CCW shortens toward a truncate, CW leaves
the peak-coupled length alone. It can only shorten, never lengthen past
`kReleaseShiftMax`, or "louder rings on longer" stops meaning anything.
Requested directly: "If I want to truncate the note, I can main knob CCW to
speed up the release phase - so have it dynamically calculated" — dynamic
was explicit, so `Tick()` reads `mainRaw_` fresh every release tick rather
than latching a value at the moment the gate falls. `breathsim.py`'s
`test_release_trim()` asserts a knob change applied AFTER release is
honoured within milliseconds, not just a pre-release choice.

**A released note does not re-finger.** `NoteOn()` is only called for a
settled fingering change while the gate is up — during the release tail the
level tracker keeps running (so the next strike is never stale) but the
result isn't acted on, and the tail rings out at whatever note it was given.
Requested directly: "don't allow note changes during this release - only
during held." The one thing that must still happen is a fresh STRIKE mid-tail
picking up the CURRENT fingering rather than the frozen one, even if it
changed silently (no further settled event) during the tail — handled by
tracking the gate's own rising edge and calling `NoteOn(levels_.Current())`
on it directly, since `Current()` stays live regardless of the freeze.

**No switch position that is held while playing may carry a gesture.** This has
now cost two bugs: switch-up as a held legato mode with a staged timer on it
(v2.0, dropped the card into tune mode mid-slur), and switch-down as both mute
and a 2-second calibration hold. Up is a TOGGLE you flick; down is the bow;
calibration has no gesture at all and runs only at boot.

**Time constants: a one-pole reaches 90% in ~2.3 tau, not 6.9.** 6.9 is the time
to -60dB, which is the right measure for a RELEASE and the wrong one for an
ATTACK. Using it made the slow attack arrive in 196ms instead of a second.

**The Main knob must never change the pitch.** It is level, then vibrato depth.
A register switch lived on it until v3.2 — a fossil of the v1 waveguide, faking
an overblow for a bore that had not existed for two rewrites — and it read as an
octave jump in the middle of the vibrato stage. `breathsim.py` asserts `Breath`
has no register field so it cannot return. The octave is X, during calibration.

**Ten combinations, not fifteen.** The card used to walk all fifteen and decide
at the end whether the voltages separated well enough to use them all. Hardware
said fifteen was simply too many to play. The triples and the quad are still
SAFE — they land far from any learned level and the match window rejects them,
so pressing one leaves the current note alone. `tools/levelsim.py` asserts that
directly, because it is what makes dropping them safe rather than merely
convenient.

**Cents are carried in Q4 (sixteenths) everywhere.** Whole cents quantise small
vibrato depths to ZERO, so vibrato did nothing until it reached two cents and
then arrived abruptly — reported from hardware as an audible step, and notably
absent in portamento mode because a glide keeps the cents term non-zero. If you
add another modulation source, it is Q4 too.

**The level curve is an S, and its multiply order is load-bearing.** Reducing
the small factor first (`n * ((n*(3-2n))>>12) >> 12`) is both exactly monotonic
and inside int32; the obvious `(n*n)>>12` form loses four bits and goes
BACKWARDS in 108 places, and a single end shift overflows. `breathsim.py`
asserts zero backward steps.

**Loudness is logarithmic and the level curve has to fight that.** A curve that
looks fast in amplitude is not fast to the ear: the cubic that preceded the
current fifth power reached 84% of full amplitude by half travel and was still
only -12dB — half as loud — at a quarter. Judge this curve in dB, never in
linear level.

**There is no air path.** v2 mixed filtered noise under the tone as "breath";
hardware said it added nothing, and it was right. The tone is a pure sine and
the expression is VIBRATO. Do not reintroduce noise without listening first —
this is the second time a plausible-on-paper timbre idea has been inaudible in
the room.

**Vibrato comes from BOTH knobs.** Main sets how much (none below `kVibOnset`,
full at the top), X sets what kind (fast/wide → fast/tight → slow/wide). The two
stages of Main must not overlap, or a turn in the lower half both raises the
level and starts a wobble and neither reads as its own gesture.

**The wavefolder is capped at half its range**, and that is measured: past fold
2048 the spectral centroid DIPS before rising again, because completing a fold
returns the fundamental. A knob that brightens then dulls reads as broken.

**The CV cache no longer hits most of the time, deliberately.** Vibrato changes
the pitch every tick, so the flash-resident `CVOutMillivolts` genuinely has to
be called — a pitch CV that updated once per note would not carry the vibrato.
~80 cycles of 4000. Do not "fix" it by throttling the update.

**The breath knob produces TWO curves, and they are not interchangeable.**
`BreathQ12()` is LEVEL — log-shaped, nearly full by half the travel, and it
drives the VCA. `EffortQ12()` is linear to the stop and drives brightness,
harmonic drive and the register threshold. Using level where effort belongs
makes the top half of the knob dead, because level has already flattened there.
Using effort where level belongs makes the knob feel unresponsive, which is
exactly what hardware reported of v2.0's squared curve.

**Switch UP carries no gesture, and must not.** It is legato — a position held
while playing. v2.0 hung a staged 1s/3s hold on it, so a three-second slur
dropped the card into tune mode with no way out (tune exited on a tap, and a tap
is switch DOWN). Momentary positions can carry gestures; held ones cannot.

**Tuning runs concurrently with calibration**, because the two use disjoint
controls: calibration reads CV In 1 and the switch, tuning reads Y and Main. The
drone sets its own timbre explicitly — `ApplyTimbre()` derives from the breath
knob, which during calibration is the fine tune, so leaving it to that would
make the reference note change character as you tuned it.

**Breath and X both feed all four voice parameters.** That is deliberate and it
is what makes them interact: breath sets loudness AND brightness AND harmonic
richness, X sets how airy the whole range is. Two knobs in separate lanes felt
like a volume control and a tone control; multiplied, they feel like an
instrument.

**The air range is narrow and high.** Measured: below air_mix 2400 the noise is
inaudible under the tone, above 3800 the pitch disappears. A linear 0..4095
sweep wastes three quarters of the knob, which is exactly what the first attempt
at this mapping did.

**The saturator needs a lot of drive to do anything.** The oscillator peaks at
2047 and the curve turns over at 4096, so drive has to reach ~8000 in Q12 before
the shape has any effect at all. A range of 3000..9000 looks generous and leaves
the tone a pure sine throughout.

## The models are not optional

`tools/*.py` are line-by-line ports of the C++. **If the C++ changes, change
them — or delete them rather than let them drift into telling you a comfortable
lie.**

They caught, before any hardware: a cents→millivolts constant that was 2/3
instead of 5/6 (a 19.5-cent detune that appears only once the fine tune is
touched); a fine-tune approximation 3 cents out at full travel; a breath curve
returning 4096 at full knob, one count past full scale; a DC blocker whose own
truncation accumulated to a permanent −497 offset; and `kMaxRoot` derived from
the wrong constraint.

**And they missed the one that mattered.** Every v1 assertion passed while the
voice ignored the breath knob entirely, because they all tested internals —
tuning, stability, DC, harmonic presence — and none ever varied breath and
compared the result. One was actively harmful: `E(f0) > 0.20 * E(2f0)` passed a
note whose octave was five times louder than its fundamental.

`flutesim.py` is now written against what a PLAYER would report: silence,
dynamic range, monotonic loudness, brightness tracking breath, X moving
character, and the played note being the note asked for. Write assertions that
way. When a model and a pair of ears disagree, the ears are the specification.

**What they cannot catch is the wiring.** Every module tested correct in
isolation while `main.cpp` sequenced them wrongly: Pulse Out 2 could never fire
(an edge was read after the call that consumes it), the chiff stop never damped
the bore (`Mute()` was written, documented, and never called), the level
detector froze during tuning, negative coarse tune moved the CV but not the
voice, and the glide never arrived. Those came out of reading the file. Read it.

---

## Verifying

```sh
sh tools/syntax.sh                  # ~1s, catches type errors, does not link
python tools/levelsim.py            # detection, trilling, the mode decision
python tools/pitchsim.py            # pitch tables, both pitch paths agree
python tools/flutesim.py            # bore: tuning, stability, harmonics
python tools/breathsim.py           # breath, articulation, register switch
python tools/caltable.py --check    # README tables match the source
```

There is **no host C++ compiler on this machine**, which is why `syntax.sh`
cross-compiles with `-fsyntax-only` against the real SDK headers. It does not
link, so it cannot catch a missing symbol — run a real build before believing
anything.

```powershell
$env:PICO_SDK_PATH = "$env:USERPROFILE\.pico-sdk\sdk\2.2.0"
$env:PATH = "$env:USERPROFILE\.pico-sdk\cmake\v3.31.5\bin;$env:USERPROFILE\.pico-sdk\ninja\v1.12.1;$env:USERPROFILE\.pico-sdk\toolchain\14_2_Rel1\bin;$env:PATH"
cmake --build build
```

Watch `--print-memory-usage` every link. Currently ~4.8% flash, ~7.5% RAM; a
jump means something large landed in the wrong section. (v1 sat at 8.4% — the
delay line went with the waveguide.)

---

## What is still unverified

**Everything about the actual hardware.** No Four Voltages module has ever been
measured — not by this card and not by NIBBLE across five sessions. Every
tolerance in `ocarina.h` is an estimate, `kGapNeeded15` is a derivation rather
than a measurement, and whether fifteen levels separate at all is unknown.

The card is built to find out: it measures its own spread, falls back to
NIBBLE's proven ten, and reports how close it came on the LEDs. See
`docs/HARDWARE-TESTING.md` — the first session's most valuable output is a table
of `minGap15` values per Four Voltages output.

**The chiff cannot be modelled.** Whether a 12ms noise burst reads as a tongue
stroke or vanishes, and whether the breath dip is articulation or a click, is a
judgement by ear. Its constants are grouped at the top of `breath.h` and flagged
as expected to need tuning.
