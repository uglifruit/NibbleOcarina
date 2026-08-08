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

/// Every non-empty subset of four buttons. FIFTEEN, not sixteen — see the
/// no-rest-voltage note above.
constexpr int kMaxLevels = 15;

/// The fallback set: singles + pairs, exactly NIBBLE's ten.
constexpr int kLevels10 = 10;

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
constexpr uint8_t kComboMask[kMaxLevels] = {
	0x1, 0x2, 0x4, 0x8,                    // A    B    C    D
	0x3, 0x5, 0x9, 0x6, 0xA, 0xC,          // AB   AC   AD   BC   BD   CD
	0x7, 0xB, 0xD, 0xE,                    // ABC  ABD  ACD  BCD
	0xF                                    // ABCD
};

/// How many buttons each combo holds down. Used for the calibration phase
/// marker on LEDs 4/5 ("more light = more fingers").
constexpr uint8_t kComboPop[kMaxLevels] = {
	1, 1, 1, 1,  2, 2, 2, 2, 2, 2,  3, 3, 3, 3,  4
};

/// The LED mask IS the button mask: LEDs 0-3 sit in the same 2x2 arrangement
/// as the Four Voltages buttons, so the panel mirrors the fingering directly.
static inline uint8_t ComboLedMask(int8_t combo)
{
	if (combo < 0 || combo >= kMaxLevels) return 0;
	return kComboMask[combo];
}

/// Is `sub` a strict subset of `sup`? One AND, no loop.
///
/// Detection does NOT use this (see levels.h: there is no ghost rule). It is
/// kept because tools/caltable.py and the README fingering chart reason about
/// subsets and popcounts, and having one definition beats two.
static inline bool IsStrictSubsetOf(int8_t sub, int8_t sup)
{
	if (sub < 0 || sub >= kMaxLevels) return false;
	if (sup < 0 || sup >= kMaxLevels) return false;
	const uint8_t a = kComboMask[sub], b = kComboMask[sup];
	return (a != b) && ((a & b) == a);
}

/// The order calibration walks the combos in.
///
/// Two properties matter, and both are deliberate:
///
///   1. The first TEN entries are NIBBLE's kLearnOrder verbatim. If the mode
///      decision falls back to ten, the captures already taken ARE the right
///      ten in the right order — nothing is re-walked or discarded. That is
///      why the learn walks all fifteen unconditionally rather than probing.
///
///   2. It is geometric on the 2x2 LED block, so the player can read the next
///      target off the panel: singles, then rows, columns, diagonals, then the
///      triples with the DARK led walking D -> C -> B -> A, then all four.
constexpr uint8_t kLearnOrder[kMaxLevels] = {
	kA, kB, kC, kD,
	kAB, kCD, kAC, kBD, kAD, kBC,
	kABC, kABD, kACD, kBCD,
	kABCD
};

// ---------------------------------------------------------------------------
// Detection tolerances
// ---------------------------------------------------------------------------
//
// All in raw CV-in units: signed 12-bit, -2048..2047 over roughly +/-6V, so
// about 2.9mV per unit.
//
// The 10-mode values are NIBBLE's, unchanged. They are themselves estimates
// rather than measurements — nobody has ever recorded a real Four Voltages
// spread — but they are estimates that have survived five hardware sessions,
// which is more than can be said for anything new. DO NOT "IMPROVE" THEM.

constexpr int32_t kSettleTol10    = 24;
constexpr int32_t kDeadband10     = 16;
constexpr int32_t kMatchWindow10  = 96;
constexpr int32_t kCollisionMin10 = 64;

/// 15-mode tolerances, shrunk by roughly two thirds: fifteen levels across the
/// same span leave gaps about 10/15 as wide.
///
/// kSettleTol is deliberately NOT scaled all the way down. It is set by the ADC
/// noise floor, which does not shrink because we asked for more levels. 16
/// units (~46mV) is the practical floor — below that the plateau detector stops
/// settling at all and every capture reads "still moving", which presents as a
/// calibration that cannot be completed rather than as a tolerance being wrong.
constexpr int32_t kSettleTol15    = 16;
constexpr int32_t kDeadband15     = 10;
constexpr int32_t kMatchWindow15  = 56;
constexpr int32_t kCollisionMin15 = 40;

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

/// A 15-level calibration is accepted only if its tightest adjacent gap is at
/// least this wide. This is the whole gamble in one number.
///
/// Derivation:
///   kMatchWindow15 must be under HALF the smallest gap, or two neighbouring
///   levels' accept-windows overlap and the "reject anything far from a learned
///   centre" rule silently stops rejecting.        2 * 56  = 112
///   + kSettleTol15, for where the plateau may actually land       -> 128
///   + kDeadband15,  for the Schmitt pull                          -> 138
///   round up for drift while the card warms up                    -> 144
///
/// Sanity check, and it is sobering: fifteen levels each needing 144 units of
/// clear gap require 14 * 144 = 2016 units of span, which is about 5.9V across
/// a +/-6V input — nearly the entire range, near-perfectly evenly spaced.
/// Resistor networks are rarely that even. TEN levels need 9 * 144 = 1296
/// units, about 3.8V, which is comfortable. Expect 10-mode on most hardware;
/// 15-mode is a bonus the module grants or withholds, and §5.4's LED readout
/// exists so the player can tell which and why.
constexpr int32_t kGapNeeded15 = 144;

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

/// The scale root, as a MIDI note.
///
/// The BORE decides this, not musical taste: the delay line has to hold a whole
/// wavelength of the lowest note, so the instrument physically cannot play far
/// below C2. That is where the root sits.
///
/// CV Out 1 does NOT emit this as an absolute MIDI pitch. It emits semitones
/// ABOVE THE ROOT, so the root leaves the card at 0V exactly as NIBBLE's does.
/// See PitchMillivolts() in pitch.h for why: an absolute mapping would start
/// every patch 3V up and put the top of an arpeggio scale past the 6V rail.
constexpr int kBaseNote = 36;

/// Highest the root may be transposed, in semitones. One octave.
///
/// TWO independent limits apply and the tighter one wins, which is worth
/// recording because the obvious derivation only finds the looser:
///
///   CV:   at +100 cents of fine tune, a root of 28 puts the top of a 4-note
///         arpeggio (degree 14 = +43 semitones) at 5984mV, just inside the
///         6V rail. So the CV allows 28.
///
///   BORE: the same degree lands on MIDI 36 + root + 43, and the bore clamps
///         at kPitchHiNote = 96 (see pitch.h). Anything above root 17 is
///         SILENTLY CLAMPED — the CV keeps rising while the internal voice
///         stops, so the card plays one note and tells the rack another.
///
/// 12 sits comfortably under both, and an octave is a better musical unit for
/// a transpose control than an arbitrary 17. tools/pitchsim.py asserts both
/// limits, so raising this will fail loudly rather than detune quietly.
constexpr int kMaxRoot = 12;

// ---------------------------------------------------------------------------
// Gestures
// ---------------------------------------------------------------------------

/// Hold thresholds, in control ticks.
constexpr int32_t kHoldCalTicks  = 2 * kCtrlRate;        ///< DOWN 2s -> calibrate
constexpr int32_t kHoldGapTicks  = 1 * kCtrlRate;        ///< UP   1s -> minGap bar
constexpr int32_t kHoldTuneTicks = 3 * kCtrlRate;        ///< UP   3s -> tune

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
