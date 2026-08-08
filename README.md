# NIBBLE OCARINA

**Four buttons. One cable. A wind instrument you blow with a knob.**

A program card for the [Music Thing Modular Workshop
System](https://www.musicthing.co.uk/workshopsystem/) Computer.

Patch one output of the **Four Voltages** module into CV In 1 and its four
buttons become the finger holes of a physically-modelled flute. The Main knob is
your breath: turn it up and the instrument speaks, turn it down and it stops.
Blow hard and it jumps the octave.

The bore is a real waveguide — a delay line, a nonlinear jet, a reflection
filter — not a sample and not a filtered oscillator. It is in tune to about four
cents across its range, and it gets brighter and thinner toward the top the way
a real flute does, because the model does that on its own.

Sibling of [NIBBLE](https://github.com/uglifruit/WorkshopNibble), which turns the
same four buttons into a keyboard and a drum machine.

---

## Quick start

1. Patch **one Four Voltages output → CV In 1**. That is the whole required
   patch.
2. Power up. The card goes straight into calibration.
3. Hold each combination it asks for and **tap the switch down** to capture it.
   The LEDs show which one to hold — they are laid out like the buttons.
4. When it finishes, turn the **Main knob** up and play.

Calibration takes fifteen taps. It has to: the voltages a resistor network
produces are arbitrary, and the Four Voltages knob moves all of them, so the
card measures rather than guesses. **Re-calibrate whenever you touch that
knob** — hold the switch down for two seconds.

---

## Panel

| | |
|---|---|
| **CV In 1** | Four Voltages output — the fingering |
| **CV In 2** | breath CV, adds to the knob |
| **Pulse In 1** | tongue: re-articulates without changing the note |
| **Main** | breath *(fine tune in TUNE)* |
| **X** | timbre: breathy and dark → focused and bright |
| **Y** | scale *(coarse tune in TUNE)* |
| **Switch ↑** | legato: glide between notes, plus vibrato |
| **Switch —** | tongued: a chiff on every note |
| **Switch ↓** | mute / chiff stop *(momentary)* |
| **Audio Out 1** | the flute |
| **Audio Out 2** | its breath noise alone |
| **CV Out 1** | 1V/oct pitch — the root is 0V |
| **CV Out 2** | breath envelope |
| **Pulse Out 1** | gate: high while sounding |
| **Pulse Out 2** | a blip per articulated note |

### Holds

| Gesture | Does |
|---|---|
| **Switch ↓ 2s** | calibrate — works from anywhere, including mid-flash |
| **Switch ↑ 1s** | show how close the last calibration came to 15-mode |
| **Switch ↑ 3s** | tune (keep holding past the first stage) |

---

## Playing it

**Silence is the knob, not the fingering.** This is not a design preference. The
Four Voltages module has no rest voltage: let go of every button and its output
stays at whatever was last pressed. "No holes covered" is not a state it can
express, so it cannot mean silence. On a real ocarina you stop by not blowing,
which is exactly what the Main knob does here.

**Releasing a finger is a note.** Come off AB onto A and you hear A. That is
what makes trilling work: hold one hole, waggle another, and you get an
alternation. Up to about 16 waggles a second is clean.

**Switch up is the trill mode.** Under tongued articulation every note gets a
chiff, and a fast trill becomes a stutter. Legato glides between the two pitches
instead, which is what a trill sounds like on a wind instrument.

**Blowing hard jumps the octave**, at about 70% of the knob's travel. There is a
hysteresis band, so it will not flutter at the boundary.

**The chiff stop is a real stop.** Holding the switch down does not just close a
gate — it damps the bore, so the sound stops instead of ringing out. Use it to
cut a phrase or play hard staccato.

---

## Fifteen combinations, or ten

Four buttons make fifteen usable combinations (not sixteen — see above). Whether
a real Four Voltages can space fifteen voltages far enough apart to tell them
reliably is **unknown**, and depends on the module, the output you patch, and
where its knob is.

So the card measures. Calibration walks all fifteen, computes the tightest gap
between any two, and decides:

- **15-mode** — all fifteen play. LEDs ramp down and both bottom LEDs blink
  three times. LED 5 glows dimly while you play.
- **10-mode** — four singles and six pairs, which is NIBBLE's proven set.
  Triples and all-four are ignored rather than played wrongly. LED 5 stays dark.

**Expect 10-mode.** Fifteen levels need nearly six volts of near-perfectly even
spacing, and resistor networks are rarely that even. 10-mode is a complete
instrument; 15-mode is a bonus the hardware grants or withholds.

### Chasing 15-mode

When it falls back, the top four LEDs show **how close it got**, in quarters:

| LEDs | Meaning |
|---|---|
| `●○○○` | badly collided — two combinations nearly identical. Try another output. |
| `●●○○` | far off |
| `●●●○` | close-ish — worth nudging the Four Voltages knob |
| `●●●●` | **very close** — a small nudge may do it |

Four Voltages has four outputs and they behave differently. Try each; nudge the
knob; recalibrate. Switch-down-2s re-enters calibration from anywhere, so it is
a fast loop. Switch-up-1s shows the bar again any time.

If both bottom LEDs flash **during** the walk, two captures have collided
already — that patch will not make 15, so you can abort and try another rather
than finishing all fifteen.

If the LEDs alternate fast in columns, calibration **failed**: almost always
nothing patched into CV In 1. The previous calibration is kept.

---

## Tuning

Hold **switch up for three seconds**. The card drones the root of the current
scale on both Audio Out 1 and CV Out 1.

- **Y** — coarse, ±12 semitones
- **Main** — fine, ±100 cents

Both start from wherever the knobs already are, so entering tune does not jump
the tuning; a knob takes control only once you actually move it. A single LED
circles the block so tune mode is unmistakable, and the bottom LEDs show how far
each offset is from zero — dark means no offset.

Tap the switch to leave. **Tuning is not saved**, deliberately: nothing on this
card is written to flash.

---

<!-- BEGIN GENERATED -->
### Fingering

Ten combinations in 10-mode, fifteen in 15-mode. The LEDs mirror the
Four Voltages buttons, so the panel shows the fingering directly.

| Degree | Buttons | Holes | Mode |
|-------:|---------|-------|------|
| 0 | A | `●○ / ○○` | both |
| 1 | B | `○● / ○○` | both |
| 2 | C | `○○ / ●○` | both |
| 3 | D | `○○ / ○●` | both |
| 4 | AB | `●● / ○○` | both |
| 5 | AC | `●○ / ●○` | both |
| 6 | AD | `●○ / ○●` | both |
| 7 | BC | `○● / ●○` | both |
| 8 | BD | `○● / ○●` | both |
| 9 | CD | `○○ / ●●` | both |
| 10 | ABC | `●● / ●○` | 15 only |
| 11 | ABD | `●● / ○●` | 15 only |
| 12 | ACD | `●○ / ●●` | 15 only |
| 13 | BCD | `○● / ●●` | 15 only |
| 14 | ABCD | `●● / ●●` | 15 only |

### Calibration order

Hold each combination and tap the switch. The first ten are
NIBBLE's own order, so a fall back to 10-mode keeps the captures
already taken.

| Step | Hold | Holes |
|-----:|------|-------|
| 1 | A | `●○ / ○○` |
| 2 | B | `○● / ○○` |
| 3 | C | `○○ / ●○` |
| 4 | D | `○○ / ○●` |
| 5 | AB | `●● / ○○` |
| 6 | CD | `○○ / ●●` |
| 7 | AC | `●○ / ●○` |
| 8 | BD | `○● / ○●` |
| 9 | AD | `●○ / ○●` |
| 10 | BC | `○● / ●○` |
| 11 | ABC | `●● / ●○` |
| 12 | ABD | `●● / ○●` |
| 13 | ACD | `●○ / ●●` |
| 14 | BCD | `○● / ●●` |
| 15 | ABCD | `●● / ●●` |

### Scales

The bore plays MIDI 36..75 (C2 to D#5), so the widest scales lose a
degree or two at the top and transpose less far. Everything below is
derived from `scales.h` and `pitch.h` — see `tools/caltable.py`.

| Y | Scale | Notes/oct | Degrees | Transpose | Range (deg 0..top) |
|--:|-------|----------:|--------:|----------:|--------------------|
| 0 | Phrygian | 7 | 15/15 | +12 | C2–C4 |
| 1 | Hirajoshi | 5 | 15/15 | +7 | C2–G#4 |
| 2 | Harmonic Minor | 7 | 15/15 | +12 | C2–C4 |
| 3 | Natural Minor | 7 | 15/15 | +12 | C2–C4 |
| 4 | Minor Pentatonic | 5 | 15/15 | +5 | C2–A#4 |
| 5 | m7 Arpeggio | 4 | 14/15 | +0 | C2–D#5 |
| 6 | Dorian | 7 | 15/15 | +12 | C2–C4 |
| 7 | Major Pentatonic | 5 | 15/15 | +6 | C2–A4 |
| 8 | Ionian (Major) | 7 | 15/15 | +12 | C2–C4 |
| 9 | Maj7 Arpeggio | 4 | 13/15 | +3 | C2–C5 |
| 10 | Whole Tone | 6 | 15/15 | +11 | C2–E4 |
| 11 | Chromatic | 12 | 15/15 | +12 | C2–D3 |

<!-- END GENERATED -->

Scales are ordered dark → bright, so the Y knob reads as one axis. The two
arpeggios span more than the bore can reach, so their top degree or two repeat
the highest note rather than going out of tune.

---

## LEDs

| | LEDs 0–3 | LED 4 | LED 5 |
|---|---|---|---|
| **Playing** | the fingering | breath | dim in 15-mode, dark in 10 |
| **Calibrating** | the combination to hold | ● singles, ●● triples | ● pairs, ●● triples |
| **Captured** | all six, briefly | | |
| **Collision** | dark | flashing | flashing |
| **Done, 15-mode** | fade out | 3 blinks | 3 blinks |
| **Done, 10-mode** | how close it got | one long blink | dark |
| **Failed** | columns alternating fast | | |
| **Aborted** | all six, twice | | |
| **Tuning** | one LED circling | coarse offset | fine offset |

---

## Building

Needs the Pico VS Code extension's toolchain (pico-sdk 2.2.0, GCC 14.2, CMake,
Ninja) at `~/.pico-sdk`.

```powershell
$env:PICO_SDK_PATH = "$env:USERPROFILE\.pico-sdk\sdk\2.2.0"
$env:PATH = "$env:USERPROFILE\.pico-sdk\cmake\v3.31.5\bin;$env:USERPROFILE\.pico-sdk\ninja\v1.12.1;$env:USERPROFILE\.pico-sdk\toolchain\14_2_Rel1\bin;$env:PATH"
cmake -B build -G Ninja
cmake --build build
```

`build/ocarina.uf2` is the card. Hold BOOTSEL, plug in, copy it across.

### Checks

```sh
sh tools/syntax.sh            # type-check every source in ~1s
python tools/levelsim.py      # the detector, trilling, the mode decision
python tools/pitchsim.py      # the pitch tables and the two pitch paths
python tools/flutesim.py      # the bore: tuning, stability, harmonics
python tools/breathsim.py     # breath, articulation, the register switch
python tools/caltable.py --check   # this README's tables match the source
```

The Python models are line-by-line ports of the C++, and they earned their keep:
between them they caught a wrong loop geometry that put the whole instrument a
fourth sharp, a reflection sign that made the bore a closed pipe, a detune that
only appeared once the fine tune was touched, and a breath curve that overflowed
by one count at full travel.

---

## Credits

- `ComputerCard.h` by Chris Johnson, MIT — the Workshop Computer HAL.
- Scale tables from Workshop System release 89, *Lockstep*.
- The Workshop System is by [Music Thing Modular](https://www.musicthing.co.uk/).

CC BY 4.0.
