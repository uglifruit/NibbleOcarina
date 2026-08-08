# NIBBLE OCARINA

**Four buttons. One cable. A wind instrument you blow with a knob.**

A program card for the [Music Thing Modular Workshop
System](https://www.musicthing.co.uk/workshopsystem/) Computer.

Patch one output of the **Four Voltages** module into CV In 1 and its four
buttons become the finger holes of a physically-modelled flute. The Main knob is
your breath: turn it up and the instrument speaks, turn it down and it stops.
Blow hard and it jumps the octave.

Blowing harder is louder, brighter and richer all at once, so the knob is a
real dynamics control rather than a volume fader — and X decides how airy the
whole range is, from a soft breathy whisper to a clear focused tone. It is in
tune to under a cent across three and a half octaves.

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
| **Main** | breath: loudness, then brightness and richness *(fine tune while calibrating)* |
| **X** | character: breathy and soft → pure and focused |
| **Y** | scale *(coarse tune while calibrating)* |
| **Switch ↑** | legato: glide between notes, plus vibrato — and nothing else |
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

That is the only hold gesture on the card. **Switch up carries none**: it is a
position you hold while playing, and a held playing position cannot also be a
timer without firing mid-phrase.

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

**The Main knob is the expression, and it does two things in sequence.** It
reaches nearly full volume by about half its travel — fast, like ears hear
loudness — and past that point it stops getting louder and starts getting
brighter and richer instead. So the bottom half is "how loud" and the top half
is "how hard", which is what blowing into a real instrument feels like.
**X** sets where that whole range sits — fully anticlockwise is airy and soft,
fully clockwise is pure and focused. The two multiply, so soft playing at CCW is
nearly all breath while hard playing at CW is a strong clear tone.

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

**Tuning happens during calibration.** A quiet reference note drones for the
whole of it, and you tune that note while you teach the fingering:

- **Y** — coarse, ±12 semitones
- **Main** — fine, ±100 cents

The two jobs use different controls — calibration reads CV In 1 and the switch,
tuning reads Y and Main — so neither costs the other anything and there is no
separate mode to enter or leave.

Both knobs pick up from wherever they already are, so starting a calibration
never jumps the tuning; a knob takes control only once you actually move it.

**Tuning is not saved**, deliberately: nothing on this card is written to flash.
A power cycle means calibrating again anyway, and that is when you retune.

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

The voice plays MIDI 36..91 (C2 to G6), so every scale reaches all
fifteen degrees and transposes a full octave. Everything below is
derived from `scales.h` and `pitch.h` — see `tools/caltable.py`.

| Y | Scale | Notes/oct | Degrees | Transpose | Range (deg 0..top) |
|--:|-------|----------:|--------:|----------:|--------------------|
| 0 | Phrygian | 7 | 15/15 | +12 | C2–C4 |
| 1 | Hirajoshi | 5 | 15/15 | +12 | C2–G#4 |
| 2 | Harmonic Minor | 7 | 15/15 | +12 | C2–C4 |
| 3 | Natural Minor | 7 | 15/15 | +12 | C2–C4 |
| 4 | Minor Pentatonic | 5 | 15/15 | +12 | C2–A#4 |
| 5 | m7 Arpeggio | 4 | 15/15 | +12 | C2–G5 |
| 6 | Dorian | 7 | 15/15 | +12 | C2–C4 |
| 7 | Major Pentatonic | 5 | 15/15 | +12 | C2–A4 |
| 8 | Ionian (Major) | 7 | 15/15 | +12 | C2–C4 |
| 9 | Maj7 Arpeggio | 4 | 15/15 | +12 | C2–G5 |
| 10 | Whole Tone | 6 | 15/15 | +12 | C2–E4 |
| 11 | Chromatic | 12 | 15/15 | +12 | C2–D3 |

<!-- END GENERATED -->

Scales are ordered dark → bright, so the Y knob reads as one axis. Because the
combinations are degrees rather than pitches, the scale also sets the range: a
4-note arpeggio spreads fifteen fingerings over three and a half octaves, while
Chromatic packs them into just over one.

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
between them they caught a detune that appeared only once the fine tune was
touched, a breath curve that overflowed by one count at full travel, and a DC
blocker whose own rounding left a permanent offset.

They also failed to catch the one that mattered — see `docs/DEVLOG.md`. The v1
voice ignored the breath knob entirely and no assertion noticed, because they
all tested internals rather than what a player would hear. `flutesim.py` is now
written the other way round: silence, dynamic range, brightness, character.

---

## Credits

- `ComputerCard.h` by Chris Johnson, MIT — the Workshop Computer HAL.
- Scale tables from Workshop System release 89, *Lockstep*.
- The Workshop System is by [Music Thing Modular](https://www.musicthing.co.uk/).

CC BY 4.0.
