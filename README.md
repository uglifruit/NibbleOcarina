# NIBBLE OCARINA

**Four buttons. One cable. A flute you bow with a switch.**

A program card for the [Music Thing Modular Workshop
System](https://www.musicthing.co.uk/workshopsystem/) Computer.

Patch one output of the **Four Voltages** module into CV In 1 and its four
buttons become the finger holes. The momentary switch is the bow, and Main is
how hard you play: quiet notes are short, loud ones ring on, and past halfway
it starts to sing with vibrato.

The tone is a pure sine and the expression is **vibrato**. Turn the Main knob up
and it comes quickly to full volume; keep going and vibrato grows on top of it —
so a phrase moves from quiet and steady, through loud and steady, into loud and
singing. **X** decides what kind of vibrato that is, morphing from fast-and-wide
through fast-and-tight to slow-and-wide.

In tune to under a cent across four and a half octaves.

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
| **CV In 2** | pitch offset, ±2 octaves in semitones |
| **Audio In 1** | offsets the Main knob *(used as CV)* |
| **Audio In 2** | offsets the X knob *(used as CV)* |
| **Pulse In 1** | the bow, same as the switch: gate high sounds a note |
| **Main** | held: note peak, then vibrato · released: trims the release live *(fine tune while calibrating)* |
| **X** | attack shape, vibrato character, wavefold *(octave while calibrating)* |
| **Y** | scale *(coarse tune while calibrating)* |
| **Switch ↑** | flick to toggle portamento — shown on LED 4 |
| **Switch —** | rest — the card is silent here |
| **Switch ↓** | **the bow** — tap to strike, hold to sustain |
| **Audio Out 1** | the tone, a sine |
| **Audio Out 2** | the same tone, wavefolded as X rises |
| **CV Out 1** | 1V/oct pitch — the root is 0V, and it carries the vibrato |
| **CV Out 2** | level — tracks what you hear |
| **Pulse Out 1** | gate: high for the whole note, release included |
| **Pulse Out 2** | the same tone as a square |

### Gestures

**There are none.** Calibration runs once at power-on; reset to get back to it.

Both switch positions are playing controls — down is the bow, up toggles
portamento — and a position you use while playing cannot also carry a timer
without firing mid-phrase. That mistake cost two separate bugs; see
`docs/DEVLOG.md`.

---

## Playing it

**Silence is the knob, not the fingering.** This is not a design preference. The
Four Voltages module has no rest voltage: let go of every button and its output
stays at whatever was last pressed. "No holes covered" is not a state it can
express, so it cannot mean silence. Here the card is simply silent until you
bow it, which is how a struck instrument works anyway.

**The switch is the bow.** The card is silent until you sound it: **tap** for a
struck note, **hold** to sustain. While you hold it, turning Main swells the
note and changing the fingering glides to the new pitch without re-attacking —
the way a finger moves on a bowed string. Pulse In 1 does exactly the same, so a
sequencer gate plays it the way your finger does.

**Releasing a finger is a note — but only while the bow is down.** Come off AB
onto A while holding and you hear A: that is what makes trilling work, hold one
hole, waggle another, get an alternation, up to about 16 waggles a second
clean. Once the bow comes up the fingering **locks**: the release tail keeps
ringing at whatever note it was given, however the fingers move underneath it,
and only picks up new fingering on the *next* strike. A dying note doesn't
re-finger the way a held one does — it's already committed.

**The Main knob does three things, and the last one changes meaning the moment
you let go.** While held it sets how loud the note is, how long it *will* ring,
and — past about a third of its travel — how much vibrato it gains:

| Main | note |
|---|---|
| just above silent | quiet, ~95ms, steady |
| a third up | most of full volume, a few hundred ms |
| two thirds | loud, ~1s, singing |
| full | loudest, over a second, wide vibrato |

Loud notes lasting longer is the coupling that makes one knob feel like
dynamics rather than a fader. Turn it while the bow is held and the note swells
or eases under your hand.

**The instant you release, Main's job changes.** It stops setting peak — the
note has already been given its shape — and starts **trimming the release**,
live, for as long as the tail is still sounding. Leave the knob where it was
(or turn it up) and the note rings out for the length its peak earned. Turn it
down during the tail and the release shortens as you turn it, all the way to a
near-instant cutoff — a way to choke a note off without touching the switch,
and it works on a tail that's already ringing, not just a position chosen in
advance. It can only shorten what the peak bought, never stretch a note out
past its own length.

**X chooses the attack and the vibrato together:**

| X | attack | vibrato |
|---|---|---|
| fully CCW | 3ms — a strike | fast and wide, 8Hz / 50 cents |
| centre | ~50ms | fast and tight, 8Hz / 10 cents |
| fully CW | ~790ms — a swell | slow and wide, 3Hz / 50 cents |

So the anticlockwise end is percussive and dramatic, the clockwise end slow and
gentle in both respects at once. X also opens the wavefolder on Audio Out 2 and
tilts the level up slightly.

**The three audio outputs are one oscillator.** Audio 1 is its sine, Audio 2 the
same wave folded, Pulse 2 the same wave squared — all at identical pitch and
phase, so they mix without comb filtering and the square always lines up.

---

## Ten combinations

Four buttons can express fifteen non-empty combinations (not sixteen — see
above), and the card used to walk all fifteen and work out at the end whether
the resistor network had separated them well enough to use them all.

**Ten is now the only mode**: four singles and six pairs. Fifteen turned out to
be simply too many to play — the triples are awkward fingerings whatever the
voltages happen to do, and it made every calibration five taps longer.

**Pressing three or four buttons is safe.** Those voltages land far from every
learned level, so the card ignores them and holds the note you were already
playing rather than jumping somewhere wrong. That rejection is what makes
dropping them safe rather than merely convenient.

If two learned levels come out too close to tell apart, both bottom LEDs flash
during the walk and **LED 5 stays dimly lit while you play**. Four Voltages has
four outputs and they behave differently, so trying another one costs a minute:
hold the switch down for two seconds to recalibrate from anywhere.

If the LEDs alternate fast in columns, calibration **failed** — almost always
nothing patched into CV In 1. The previous calibration is kept.

---

## Tuning and octave

**Both happen during calibration.** A quiet reference note drones for the whole
of it, and all three knobs shape that note while you teach the fingering:

- **Y** — coarse tune, ±12 semitones
- **Main** — fine tune, ±100 cents
- **X** — **octave**: C2, C3, C4 or C5

Calibration itself reads only CV In 1 and the switch, so the knobs are free —
neither job costs the other anything and there is no separate mode to enter.

The default is **C4**, a concert flute's lowest note. C2 is two octaves below
that and is genuinely sub-bass; it is still there if you want it, but it is no
longer where the card starts.

Both knobs pick up from wherever they already are, so starting a calibration
never jumps the tuning; a knob takes control only once you actually move it.

**Tuning is not saved**, deliberately: nothing on this card is written to flash.
A power cycle means calibrating again anyway, and that is when you retune.

---

<!-- BEGIN GENERATED -->
### Fingering

Ten combinations: four singles and six pairs. The LEDs mirror the
Four Voltages buttons, so the panel shows the fingering directly.

| Degree | Buttons | Holes |
|-------:|---------|-------|
| 0 | A | `●○ / ○○` |
| 1 | B | `○● / ○○` |
| 2 | C | `○○ / ●○` |
| 3 | D | `○○ / ○●` |
| 4 | AB | `●● / ○○` |
| 5 | AC | `●○ / ●○` |
| 6 | AD | `●○ / ○●` |
| 7 | BC | `○● / ●○` |
| 8 | BD | `○● / ○●` |
| 9 | CD | `○○ / ●●` |

Pressing three or four buttons does **nothing** — those voltages
land far from every learned level, so the card ignores them and holds
the note you were already playing rather than jumping somewhere wrong.

### Calibration order

Hold each combination and tap the switch. This is NIBBLE's own
order, which is the one part of the calibration with real hardware
history behind it.

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

### Scales

Ten combinations are ten DEGREES of the chosen scale, so the scale
also sets the range. Everything below is derived from `scales.h`
and `pitch.h` — see `tools/caltable.py`.

| Y | Scale | Notes/oct | Range from C4 |
|--:|-------|----------:|---------------|
| 0 | Phrygian | 7 | C4–D#5 |
| 1 | Hirajoshi | 5 | C4–G#5 |
| 2 | Harmonic Minor | 7 | C4–D#5 |
| 3 | Natural Minor | 7 | C4–D#5 |
| 4 | Minor Pentatonic | 5 | C4–A#5 |
| 5 | m7 Arpeggio | 4 | C4–D#6 |
| 6 | Dorian | 7 | C4–D#5 |
| 7 | Major Pentatonic | 5 | C4–A5 |
| 8 | Ionian (Major) | 7 | C4–E5 |
| 9 | Maj7 Arpeggio | 4 | C4–E6 |
| 10 | Whole Tone | 6 | C4–F#5 |
| 11 | Chromatic | 12 | C4–A4 |

### Octaves

Chosen with the X knob during calibration.

| X | Base | Transpose | Widest scale reaches |
|--:|------|----------:|----------------------|
| 0 | C2 | +12 | E5 |
| 1 | C3 | +12 | E6 |
| 2 | C4  *(default)* | +12 | E7 |
| 3 | C5 | +8 | C8 |

<!-- END GENERATED -->

Scales are ordered dark → bright, so the Y knob reads as one axis. Because the
combinations are degrees rather than pitches, the scale also sets the range: a
4-note arpeggio spreads fifteen fingerings over three and a half octaves, while
Chromatic packs them into just over one.

---

## LEDs

| | LEDs 0–3 | LED 4 | LED 5 |
|---|---|---|---|
| **Playing** | the fingering | level | dim if two levels collided |
| **Calibrating** | the combination to hold | ● during singles | ● during pairs |
| **Captured** | all six, briefly | | |
| **Collision** | dark | flashing | flashing |
| **Done** | fade out | | |
| **Failed** | columns alternating fast | | |
| **Aborted** | all six, twice | | |

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
python tools/flutesim.py      # the voice: pitch, vibrato, fold, silence
python tools/breathsim.py     # the level and vibrato curves, articulation
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
