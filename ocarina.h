// ocarina.h — the card's vocabulary: rates, combos, tolerances, pitch helpers.
//
// NIBBLE OCARINA turns one Four Voltages output into the fingering of a
// physically-modelled wind instrument. This file is the shared language every
// other file speaks; the interesting logic lives in levels.cpp (detection),
// flute.cpp (the bore) and main.cpp (the UI).
//
// ---------------------------------------------------------------------------
// WHAT THE HARDWARE CAN AND CANNOT DO
// ---------------------------------------------------------------------------
//
// Four Voltages has four non-latching buttons feeding a resistor network. Each
// closed-switch COMBINATION produces one distinct voltage, so a single patch
// cable into CV In 1 carries the whole keyboard.
//
// Two properties of that module drive almost every decision in this card:
//
//   1. There is NO REST VOLTAGE. Release everything and the output sits at the
//      last-pressed level — and it stays there through a power cycle. "No holes
//      covered" is not a state the module can express, so there are FIFTEEN
//      possible readings (every non-empty subset of four buttons), never
//      sixteen, and SILENCE CANNOT COME FROM THE FINGERING. It comes from the
//      breath knob, which is where it comes from on a real ocarina.
//
//   2. The Four Voltages knob moves every level unpredictably. So the learned
//      calibration is RAM-ONLY and is re-taught whenever that knob is touched.
//      Nothing in this card is ever written to flash.
//
// ---------------------------------------------------------------------------
// 15-MODE AND 10-MODE
// ---------------------------------------------------------------------------
//
// Whether fifteen levels are actually distinguishable on real hardware is
// unknown — NIBBLE's own devlog calls even TEN "the card's central gamble" and
// it was never measured. Rather than bet, this card MEASURES: calibration walks
// all fifteen combinations, computes the true minimum gap, and either runs all
// fifteen or falls back to NIBBLE's proven ten (four singles + six pairs).
//
// The fallback is not a second implementation. Because combo indices 0..9 here
// are bit-for-bit NIBBLE's, 10-mode is this same code with activeCount_ = 10.

#pragma once
#include <stdint.h>

namespace nib {

// ---------------------------------------------------------------------------
// Rates
// ---------------------------------------------------------------------------

constexpr int32_t kSampleRate = 48000;

/// Control-rate divider. ProcessSample() runs the cheap audio path every
/// sample and the expensive control path every kCtrlDiv-th sample.
constexpr int32_t kCtrlDiv  = 16;
constexpr int32_t kCtrlRate = kSampleRate / kCtrlDiv;   // 3000Hz

// ---------------------------------------------------------------------------
// Combo vocabulary
// ---------------------------------------------------------------------------

constexpr int kNumButtons = 4;
constexpr int kNumSingles = 4;
constexpr int kNumPairs   = 6;
constexpr int kNumTriples = 4;

/// The playable combinations: four singles and six pairs.
///
/// Four buttons can express fifteen non-empty subsets (not sixteen — see the
/// no-rest-voltage note above), and the card used to walk all fifteen and
/// decide at the end whether the voltages separated well enough to use them.
///
/// TEN is now the only mode. Fifteen was reported from hardware as simply too
/// many to play: the triples are awkward fingerings whether or not the resistor
/// network resolves them, and the extra five taps made every calibration
/// longer. The triples and the all-four combo are still SAFE — they land far
/// from any learned level and are rejected by the match window, so pressing one
/// leaves the current note alone rather than jumping somewhere wrong.
constexpr int kNumLevels = 10;

/// Combo indices, ordered by POPCOUNT then lexicographically. The ordering is
/// load-bearing in three ways:
///
///   idx <  4   -> a single
///   idx < 10   -> a single or a pair, i.e. THE ENTIRE 10-MODE SET
///   idx == 14  -> all four
///
/// Because the first ten entries are identical to NIBBLE's, falling back to
/// 10-mode needs no separate table, no re-walk of the calibration, and no
/// second code path that could drift away from the proven one.
enum Combo : int8_t {
	kA = 0, kB = 1, kC = 2, kD = 3,
	kAB = 4, kAC = 5, kAD = 6, kBC = 7, kBD = 8, kCD = 9,
	kABC = 10, kABD = 11, kACD = 12, kBCD = 13,
	kABCD = 14,
	kComboNone = -1
};

/// Button bitmask per combo. Bit i == button i (A=0, B=1, C=2, D=3).
constexpr uint8_t kComboMask[15] = {
	0x1, 0x2, 0x4, 0x8,                    // A    B    C    D
	0x3, 0x5, 0x9, 0x6, 0xA, 0xC,          // AB   AC   AD   BC   BD   CD
	0x7, 0xB, 0xD, 0xE,                    // ABC  ABD  ACD  BCD
	0xF                                    // ABCD
};

/// How many buttons each combo holds down. Used for the calibration phase
/// marker on LEDs 4/5 ("more light = more fingers").
constexpr uint8_t kComboPop[15] = {
	1, 1, 1, 1,  2, 2, 2, 2, 2, 2,  3, 3, 3, 3,  4
};

/// The LED mask IS the button mask: LEDs 0-3 sit in the same 2x2 arrangement
/// as the Four Voltages buttons, so the panel mirrors the fingering directly.
static inline uint8_t ComboLedMask(int8_t combo)
{
	if (combo < 0 || combo >= 15) return 0;
	return kComboMask[combo];
}

/// Is `sub` a strict subset of `sup`? One AND, no loop.
///
/// Detection does NOT use this (see levels.h: there is no ghost rule). It is
/// kept because tools/caltable.py and the README fingering chart reason about
/// subsets and popcounts, and having one definition beats two.
static inline bool IsStrictSubsetOf(int8_t sub, int8_t sup)
{
	if (sub < 0 || sub >= 15) return false;
	if (sup < 0 || sup >= 15) return false;
	const uint8_t a = kComboMask[sub], b = kComboMask[sup];
	return (a != b) && ((a & b) == a);
}

/// The order calibration walks the combos in.
///
/// Two properties matter, and both are deliberate:
///
///   1. It is NIBBLE's kLearnOrder verbatim, which is the one part of this
///      card's calibration with hardware history behind it.
///
///   2. It is geometric on the 2x2 LED block, so the player can read the next
///      target off the panel: singles, then rows, then columns, then diagonals.
constexpr uint8_t kLearnOrder[kNumLevels] = {
	kA, kB, kC, kD,
	kAB, kCD, kAC, kBD, kAD, kBC
};

// ---------------------------------------------------------------------------
// Detection tolerances
// ---------------------------------------------------------------------------
//
// All in raw CV-in units: signed 12-bit, -2048..2047 over roughly +/-6V, so
// about 2.9mV per unit.
//
// These are NIBBLE's values, unchanged. They are themselves estimates rather
// than measurements — nobody has ever recorded a real Four Voltages spread —
// but they are estimates that have survived several hardware sessions, which is
// more than can be said for anything new. DO NOT "IMPROVE" THEM.

constexpr int32_t kSettleTol    = 24;
constexpr int32_t kDeadband     = 16;
constexpr int32_t kMatchWindow  = 96;
constexpr int32_t kCollisionMin = 64;

/// How long a plateau must hold before it counts. 37 ticks at 3kHz = 12.3ms.
///
/// This constant also sets the fastest playable TRILL, because with no ghost
/// rule every settled change is a note: a finger waggling faster than one
/// transition per 12.3ms outruns the detector and drops notes. Shorter and ADC
/// noise starts firing spurious ones. Expect to tune this on hardware — it is
/// the constant most likely to need it. See tools/levelsim.py, which sweeps
/// trill rates against it.
constexpr int32_t kSettleTicks = kCtrlRate / 80;

/// Input smoothing on the raw CV. Kills ADC dither without smearing an edge.
constexpr uint8_t kCvSmoothShift = 3;

/// Uncalibrated default spread, used before any learn has been done. The card
/// still plays; the LED indicator says it is guessing.
constexpr int32_t kDefaultLo = -1500;
constexpr int32_t kDefaultHi =  1500;

/// A learn whose whole span is under this is degenerate — almost always nothing
/// patched into CV In 1. Refuse it and keep the previous calibration rather
/// than installing a card that looks calibrated and plays one note forever.
/// 400 units is about 1.2V; ten levels genuinely spread cover several.
constexpr int32_t kMinLearnSpan = 400;

// ---------------------------------------------------------------------------
// Pitch
// ---------------------------------------------------------------------------

/// Millivolts per semitone, Q8. round(1000/12 * 256).
constexpr int32_t kMvPerSemiQ8 = 21333;

/// Semitone count -> millivolts, ROUNDED.
///
/// The +128 is not decoration. Truncating biases every note flat by up to a
/// millivolt (1.2 cents), which is audible against a tuned oscillator;
/// rounding brings the worst case over the useful range to 0.33mV.
static inline int32_t SemisToMillivolts(int32_t semis)
{
	return (semis * kMvPerSemiQ8 + 128) >> 8;
}

/// The scale root, as a MIDI note — the DEFAULT octave.
///
/// C4 (262Hz), which is a concert flute's lowest note. The card used to sit at
/// C2 and it was reported as being "right in the low end of notes" when what
/// was wanted was a flute; two octaves down is sub-bass, not a wind instrument.
///
/// The octave is selectable during calibration (X knob), from C2 up to C5, so
/// the low registers are still reachable — they are just no longer the default.
///
/// CV Out 1 does NOT emit this as an absolute MIDI pitch. It emits semitones
/// ABOVE THE ROOT, so the root leaves the card at 0V exactly as NIBBLE's does.
/// See PitchMillivolts() in pitch.h.
constexpr int kBaseNote = 60;

/// The octaves the X knob offers during calibration, as MIDI base notes.
///
/// C2 is the old default and is kept because a sub-bass drone has its uses; C4
/// is a concert flute; C5 is piccolo/soprano-ocarina territory. A 7-note scale
/// from C5 runs to C7, which is inside the voice's range.
constexpr int kNumOctaves = 4;
constexpr int kOctaveBase[kNumOctaves] = { 36, 48, 60, 72 };

/// Which entry of kOctaveBase the card starts on — C4.
constexpr int kDefaultOctave = 2;

/// The most the root may ever be transposed, in semitones — one octave.
///
/// This is only the CEILING on the coarse-tune control. The binding limit is
/// PER SCALE and lives in pitch.h as kMaxRootFor[], because a 7-note mode has
/// an octave of headroom inside the bore while a 4-note arpeggio has none.
///
/// The failure this guards against is silent: past the bore's top note the
/// delay clamps while CV Out 1 keeps climbing, so the card plays one pitch and
/// reports another, and nothing in the system notices.
constexpr int kMaxRoot = 12;

// ---------------------------------------------------------------------------
// Gestures
// ---------------------------------------------------------------------------

/// Hold threshold, in control ticks. There is only one hold gesture on the
/// card: DOWN for two seconds calibrates.
///
/// Switch UP deliberately carries NO gesture. It is legato, a position you hold
/// while playing, and v2.0 also hung a staged 1s/3s hold on it — so a slur
/// lasting three seconds dropped the card into tune mode with no way back.
/// A held playing position cannot also be a gesture.
constexpr int32_t kHoldCalTicks  = 2 * kCtrlRate;

/// How far a knob must move before it takes control, of 4095.
///
/// Wider than ADC dither, narrower than a deliberate nudge. Reused from NIBBLE,
/// where a fixed reference without this threshold handed control back roughly
/// 17,000 times in 200k ticks with nobody touching the knob.
constexpr int32_t kKnobMoveThresh = 64;

/// Boot: burn this many samples before reading the switch even once.
///
/// The switch is derived from a mux channel through a ~60Hz filter that starts
/// at zero, and zero decodes as Down. Take ONE reading after the filter has
/// settled. Latching on "Down seen at any point in the window" latches on every
/// single boot — two sibling cards shipped exactly that bug.
constexpr int32_t kBootWindowSamples = kSampleRate / 2;
constexpr int32_t kSplashSamples     = kSampleRate;

// ---------------------------------------------------------------------------
// LEDs
// ---------------------------------------------------------------------------

constexpr int kNumLeds = 6;

constexpr uint16_t kLedFull = 4095;
constexpr uint16_t kLedDim  = 700;
constexpr uint16_t kLedGlow = 180;

} // namespace nib
