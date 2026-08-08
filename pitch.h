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

/// The highest note the bore can sound: MIDI 96 (C7, 2093Hz).
///
/// At the top the loop is only 23 samples long and the jet tap sits at half of
/// that, so the model is running out of resolution rather than out of memory —
/// the tone thins out and the register balance gets unreliable. Notes above
/// this are clamped rather than allowed to degrade silently.
constexpr int kPitchHiNote = 96;

// ---------------------------------------------------------------------------
// Semitone -> delay length
// ---------------------------------------------------------------------------

/// Loop delay in SAMPLES for each semitone of the reference octave, Q16.
///
/// Generated, not typed: entry i = round(65536 * 48000 / f), where f is the
/// frequency of MIDI note (36 + i). A hand-written table here is one wrong
/// note and nothing else, which is why tools/pitchsim.py re-derives every
/// entry from exact arithmetic and asserts the error in cents.
extern const uint32_t kDelayQ16Base[12];

/// MIDI semitone -> loop delay in samples, Q16. No divide, no exp, no 64-bit.
///
/// The delay HALVES exactly per octave, so the octave is a right shift and only
/// the twelve within-octave ratios need a table. One lookup, one shift.
///
/// On `n / 12`: dividing a small non-negative int by a compile-time constant is
/// strength-reduced by gcc into a multiply-and-shift. It is NOT a libgcc call.
/// The thing that must be avoided on this chip is 64-bit division, which has no
/// hardware support and was the single biggest cause of NIBBLE overrunning its
/// sample budget — not all division everywhere. See fastmath.h's kHzToIncQ16.
static inline uint32_t SemiToDelayQ16(int semi)
{
	if (semi < kPitchLoNote) semi = kPitchLoNote;
	if (semi > kPitchHiNote) semi = kPitchHiNote;
	const int n   = semi - kPitchLoNote;
	const int oct = n / 12;
	const int st  = n - oct * 12;
	return kDelayQ16Base[st] >> oct;
}

/// ln(2)/1200 in Q24 — the per-cent exponent for the fine-tune ratio.
constexpr int32_t kCentQ24 = 9691;

/// Bend a Q16 delay by a fine-tune offset in cents.
///
/// The exact ratio is 2^(-c/1200) = exp(-x) where x = c * ln2/1200.
///
/// A FIRST-ORDER expansion (1 - x) is not good enough here, and it is worth
/// recording why rather than rediscovering it: over the +/-100 cent range this
/// card offers it is off by 3.1 cents at the extremes — small enough to look
/// plausible, large enough to hear against a tuned oscillator, and it would
/// have shown up as "the fine tune is slightly wrong at the ends" forever.
/// tools/pitchsim.py measured it.
///
/// The second-order term (1 - x + x^2/2) brings the worst case to 0.058 cents,
/// which is inaudible, for one extra multiply and shift.
///
/// The 64-bit multiplies here are fine (~10 cycles each). This runs at control
/// rate on a note change, never per sample. A 64-bit DIVIDE would not be.
static inline uint32_t ApplyFineCents(uint32_t dQ16, int32_t cents)
{
	const int32_t d = static_cast<int32_t>(dQ16);

	// x = cents * ln2/1200, in Q24.
	const int32_t xQ24 = cents * kCentQ24;

	// first order: d * x
	const int32_t lin = static_cast<int32_t>(
		(static_cast<int64_t>(d) * xQ24) >> 24);

	// second order: d * x^2 / 2
	const int32_t quad = static_cast<int32_t>(
		(((static_cast<int64_t>(xQ24) * xQ24) >> 24) * d) >> 25);

	return static_cast<uint32_t>(d - lin + quad);
}

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
/// At 1000mV/octave, one cent is 1000/1200 = 0.8333mV. In Q10 that is
/// round(0.8333 * 1024) = 853, giving 0.83301mV — within 0.04% of exact.
///
/// It is NOT 683. That value is 2/3, not 5/6, and it detunes the CV by 19.5
/// cents at full fine-tune travel while leaving it perfect at zero — so the
/// card tunes up correctly, then drifts sharp or flat as soon as anyone touches
/// the fine control, which reads as "the tuning knob is badly calibrated"
/// rather than as an arithmetic error. tools/pitchsim.py caught it by
/// cross-checking against the bore.
static inline int32_t PitchMillivolts(int32_t semi, int32_t fineCents)
{
	return SemisToMillivolts(semi - kBaseNote) + ((fineCents * 853 + 512) >> 10);
}

} // namespace nib
