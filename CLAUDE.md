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
