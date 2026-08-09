// pitch.h — combo index to pitch, for both the internal bore and the CV out.
//
// The chain is:
//
//   combo index -> scale degree -> semitone -> (a) delay length, for the bore
//                                           -> (b) millivolts,   for CV Out 1
//
// Both branches must agree, or the internal voice and anything patched to CV
// Out 1 play different notes. tools/pitchsim.py asserts they do, in cents.

#pragma once
#include <stdint.h>
#include "ocarina.h"

namespace nib {

// ---------------------------------------------------------------------------
// Range
// ---------------------------------------------------------------------------

/// The lowest note the bore can sound: MIDI 36 (C2, 65.4Hz).
///
/// This is not a musical choice, it is the delay line's length. 65.4Hz needs
/// 734 samples at 48kHz, and the buffer is 768 (see flute.h) so that the fine
/// tune has room to bend flat without running off the end.
constexpr int kPitchLoNote = 36;

/// The highest note the voice will play: MIDI 108 (C8, 4186Hz).
///
/// Set by the WAVEFOLDER, not by the oscillator. A pure sine would not alias
/// until its fundamental passed Nyquist, around MIDI 135 — but Audio Out 2
/// folds, and folding generates harmonics. At MIDI 108 the fifth harmonic is
/// 20.9kHz, just under the 24kHz Nyquist; at MIDI 112 it is 26.4kHz and folds
/// back into the audible band as an inharmonic whistle.
///
/// v1 capped this at 75 as an artefact of the waveguide (its jet tap ran out of
/// resolution and the bore abandoned its fundamental). v3 raised it to 91, which
/// then became the binding limit once the default octave moved up to C4 — the
/// top of a scale from C5 reaches MIDI 100 and would have clamped silently.
constexpr int kPitchHiNote = 108;

// ---------------------------------------------------------------------------
// Semitone -> delay length
// ---------------------------------------------------------------------------

/// Oscillator phase increment for each semitone of the reference octave, Q32.
///
/// Generated, not typed: entry i = round(f * 2^32 / 48000) for MIDI note
/// (36 + i). A hand-written table here is one wrong note and nothing else,
/// which is why tools/pitchsim.py re-derives every entry from exact arithmetic
/// and asserts the error in cents.
///
/// v1 held delay LENGTHS here instead, because the voice was a waveguide. Note
/// the direction flipped with it: increment DOUBLES per octave where delay
/// halved, so the octave is a left shift now rather than a right one.
extern const uint32_t kIncQ32Base[12];

/// MIDI semitone -> phase increment, Q32. No divide, no exp, no 64-bit.
///
/// The increment DOUBLES exactly per octave, so the octave is a left shift and
/// only the twelve within-octave ratios need a table. One lookup, one shift.
///
/// On `n / 12`: dividing a small non-negative int by a compile-time constant is
/// strength-reduced by gcc into a multiply-and-shift. It is NOT a libgcc call.
/// The thing that must be avoided on this chip is 64-bit division, which has no
/// hardware support and was the single biggest cause of NIBBLE overrunning its
/// sample budget — not all division everywhere. See fastmath.h's kHzToIncQ16.
static inline uint32_t SemiToIncQ32(int semi)
{
	if (semi < kPitchLoNote) semi = kPitchLoNote;
	if (semi > kPitchHiNote) semi = kPitchHiNote;
	const int n   = semi - kPitchLoNote;
	const int oct = n / 12;
	const int st  = n - oct * 12;
	return kIncQ32Base[st] << oct;
}

/// ln(2)/1200/16 in Q28 — the per-SIXTEENTH-of-a-cent exponent.
///
/// Cents are carried in Q4 (sixteenths) rather than whole numbers, and that is
/// not decoration: vibrato depth passes through here, and at small depths whole
/// cents quantise the entire modulation to ZERO. The result was a vibrato that
/// did nothing at all until its depth reached two cents and then appeared
/// abruptly — an audible step reported from hardware.
///
/// The constant is numerically identical to the old Q24 per-whole-cent value,
/// because dividing by 16 and shifting 4 bits further cancel exactly. Only the
/// shift changes.
constexpr int32_t kCentQ28 = 9691;

/// Bend a Q32 phase increment by a fine-tune offset in Q4 CENTS (sixteenths).
///
/// The exact ratio is 2^(c/1200) = exp(x) where x = c * ln2/1200. Note the
/// SIGN: sharpening raises the increment, where in v1 it shortened a delay.
///
/// A FIRST-ORDER expansion (1 + x) is not good enough here, and it is worth
/// recording why rather than rediscovering it: over the +/-100 cent range this
/// card offers it is off by 3.1 cents at the extremes — small enough to look
/// plausible, large enough to hear against a tuned oscillator, and it would
/// have shown up as "the fine tune is slightly wrong at the ends" forever.
///
/// The second-order term (1 + x + x^2/2) brings the worst case under 0.1 cents
/// for one extra multiply and shift.
///
/// The 64-bit multiplies here are fine (~10 cycles each). This runs at control
/// rate on a note change, never per sample. A 64-bit DIVIDE would not be.
static inline uint32_t ApplyFineCents(uint32_t incQ32, int32_t centsQ4)
{
	const uint32_t v = incQ32;

	// x = cents * ln2/1200, in Q28. Signed: negative cents flatten.
	const int32_t xQ28 = centsQ4 * kCentQ28;

	const int32_t lin = static_cast<int32_t>(
		(static_cast<int64_t>(v) * xQ28) >> 28);

	const int32_t quad = static_cast<int32_t>(
		(((static_cast<int64_t>(xQ28) * xQ28) >> 28) * static_cast<int64_t>(v)) >> 29);

	return static_cast<uint32_t>(static_cast<int64_t>(v) + lin + quad);
}

// ---------------------------------------------------------------------------
// Fitting the scales into the bore
// ---------------------------------------------------------------------------

/// How far the root may be transposed, per OCTAVE, in semitones.
///
/// The limit is per-octave rather than per-scale now, because the octave select
/// moves the whole instrument and the top octave is the one that runs out of
/// headroom: from C5, ten degrees of the widest scale plus a full octave of
/// transpose would reach MIDI 116, past the ceiling.
///
/// The failure this guards against is SILENT: past kPitchHiNote the pitch
/// clamps while CV Out 1 keeps climbing, so the card plays one note and tells
/// the rack another. tools/pitchsim.py asserts it every run.
constexpr uint8_t kMaxRootForOctave[4] = { 12, 12, 12, 8 };

// ---------------------------------------------------------------------------
// Semitone -> millivolts
// ---------------------------------------------------------------------------

/// Pitch CV in millivolts, from an ABSOLUTE MIDI note.
///
/// The CV is referenced to kBaseNote, so the scale root leaves the card at 0V
/// and an oscillator sitting at its own zero is already in tune — the same
/// convention NIBBLE uses.
///
/// This is not cosmetic. Emitting absolute MIDI would put the root at 3V, and
/// the top of a 4-note arpeggio (degree 14, +43 semitones) at 6.6V — past the
/// rail, silently clipped, before any transposition at all. Referencing to the
/// root buys back exactly the 3V that the bore's low-frequency limit cost.
///
/// Coarse tune is folded into `semi` by the caller (it shifts the root), so
/// only the fine offset is applied here.
///
/// The reference is FIXED at kBaseNote rather than following the octave select.
/// That is deliberate: it means the octave switch moves the CV output too, so
/// an oscillator patched to CV Out 1 changes octave with the card instead of
/// staying put while the internal voice moves. The alternative — re-referencing
/// so the root is always 0V — would make the octave control silently do nothing
/// outside the box, which is the sort of disagreement between the two pitch
/// paths this file exists to prevent.
///
/// At 1000mV/octave, one cent is 1000/1200 = 0.8333mV. In Q10 that is
/// round(0.8333 * 1024) = 853, giving 0.83301mV — within 0.04% of exact.
///
/// It is NOT 683. That value is 2/3, not 5/6, and it detunes the CV by 19.5
/// cents at full fine-tune travel while leaving it perfect at zero — so the
/// card tunes up correctly, then drifts sharp or flat as soon as anyone touches
/// the fine control, which reads as "the tuning knob is badly calibrated"
/// rather than as an arithmetic error. tools/pitchsim.py caught it by
/// cross-checking against the bore.
/// `fineCentsQ4` is in SIXTEENTHS of a cent, matching ApplyFineCents(), so the
/// two pitch paths cannot drift apart at small vibrato depths.
static inline int32_t PitchMillivolts(int32_t semi, int32_t fineCentsQ4)
{
	// 853/1024 mV per cent, then /16 for the Q4 -> one shift of 14.
	return SemisToMillivolts(semi - kBaseNote) + ((fineCentsQ4 * 853 + 8192) >> 14);
}

} // namespace nib
