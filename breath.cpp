// breath.cpp — the air supply, articulation and the register switch.
//
// All control rate (3kHz). Integer only.

#include "breath.h"
#include "fastmath.h"
#include "pico.h"

namespace nib {

void Breath::Init()
{
	curved_ = breath_ = effort_ = 0;
	chiffTicks_ = 0;
	sinceChiff_ = kChiffMinGapTicks;
	chiffFired_ = false;
	stopped_ = false;
	vibCents_ = vibRateQ8_ = vibCentsMax_ = 0;
	vibPhase_ = 0;
	artic_ = Articulation::Tongued;
}

void Breath::SetKnob(int32_t knob, int32_t cvAdd)
{
	int32_t v = knob + cvAdd;
	if (v < 0) v = 0;
	if (v > 4095) v = 4095;

	if (v < kBreathThresh)
	{
		// SILENT. This is the card's only way to stop, because the fingering
		// cannot express "no holes covered" — see ocarina.h.
		curved_ = 0;
		effort_ = 0;
	}
	else
	{
		// ONE knob, TWO curves, and they diverge on purpose.
		//
		// The divisor is a compile-time constant, so gcc strength-reduces this
		// into a multiply — it is not a runtime division.
		//
		// The clamp on `n` is not belt-and-braces: at v == 4095 the division
		// yields exactly 4096, one PAST full scale in Q12, which is enough to
		// clip the DAC on the loudest note the card can play.
		int32_t n = ((v - kBreathThresh) << 12) / (4095 - kBreathThresh);
		if (n > 4095) n = 4095;

		// EFFORT is linear — it is simply how far the knob has been turned.
		effort_ = n;

		// LEVEL is an S-CURVE: flat at both ends, steep through the middle.
		//
		// Three curves have been wrong here in three different ways, so the
		// history is worth keeping.
		//
		//   v2.0  SQUARED. The opposite of what is wanted — it spends the whole
		//         sweep still getting louder and only arrives at the very end.
		//
		//   v2.1  cubic 1-(1-n)^3. Looks fast (84% of full AMPLITUDE by half
		//         travel) but amplitude is not loudness: in dB it was still
		//         only -12dB at a quarter of the travel. "Very slow ramp".
		//
		//   v3.1  fifth power. Fixed the slowness by being steepest exactly at
		//         the bottom — 6dB per ten counts of knob just above the
		//         threshold, which is a switch rather than a fade. Reported as
		//         not going cleanly to silence.
		//
		// What all three missed is that BOTH ends matter. A curve that leaves
		// silence gently has to be flat near zero, and one that does not feel
		// sluggish has to be steep in the middle — that is a smoothstep,
		// n^2*(3-2n), which is flat at zero AND at full.
		//
		// The `lift` term then pulls the whole thing up without touching those
		// two flat ends: s*(1-s) is zero at both extremes and largest in the
		// middle, so adding it steepens the middle only. That recovers the
		// speed the plain smoothstep gives away, at the cost of two multiplies.
		// The multiply order is NOT free to rearrange.
		//
		// The obvious `n2 = (n*n)>>12; sm = (n2 * (3-2n))>>12` throws away four
		// bits before the second multiply, and the rounding that costs makes
		// the curve NON-MONOTONIC: 108 places where turning the knob up made
		// the level go DOWN by a count or two. Inaudible individually, but it
		// means the control does not always do what it says.
		//
		// Shifting once at the end would be exact, but n*n*(3-2n) peaks at
		// 2.06e11 and overflows int32. Reordering so the small factor is
		// reduced first keeps the peak intermediate at 16.7M and is exactly
		// monotonic across all 4096 inputs. Verified in tools/breathsim.py.
		const int32_t inner = (n * (3 * 4096 - 2 * n)) >> 12;
		int32_t sm = (n * inner) >> 12;
		if (sm > 4095) sm = 4095;
		curved_ = sm + ((sm * (4096 - sm)) >> 12);
		// The curve reaches exactly 4096 at full travel, one past the 0..4095
		// this is documented to return. Harmless in today's arithmetic, but an
		// earlier curve had the identical off-by-one and that one did clip the
		// DAC. Clamp rather than re-derive the reasoning later.
		if (curved_ > 4095) curved_ = 4095;
	}

}

void Breath::SetStopped(bool s)
{
	stopped_ = s;
}

void Breath::NoteOn()
{
	// Legato slurs: no chiff, the pitch glides instead.
	if (artic_ != Articulation::Tongued) return;

	// Too soon since the last one. This is what keeps a fast trill from
	// becoming a stutter — see kChiffMinGapTicks.
	if (sinceChiff_ < kChiffMinGapTicks) return;

	chiffTicks_ = kChiffTicks;
	sinceChiff_ = 0;
	chiffFired_ = true;
}

void Breath::Tick()
{
	static_assert(kChiffTicks > 0, "chiff must last at least one tick");

	// chiffFired_ is an EDGE, not a level: it must be true for exactly the one
	// tick on which a chiff began. NoteOn() sets it; the tick that follows the
	// one where it was read clears it.
	//
	// The ordering matters. Tick() runs after NoteOn() within a control tick,
	// so clearing at the TOP would erase the flag before main.cpp ever sees
	// it, and Pulse Out 2 would never fire. Clearing at the BOTTOM leaves it
	// set for exactly one read. This is the same level-vs-edge distinction
	// NIBBLE's looper needed for OnBeat() vs BeatEdge(), and it got it wrong
	// first too.
	const bool firedThisTick = chiffFired_;

	if (sinceChiff_ < kChiffMinGapTicks) sinceChiff_++;

	if (chiffTicks_ > 0) chiffTicks_--;

	// Vibrato. Always available now — its depth is set by the knobs, and a
	// depth of zero is how "no vibrato" is expressed, rather than by an
	// articulation mode gating it on and off.
	if (vibCentsMax_ > 0)
	{
		// Q8 Hz -> Q32 phase increment per control tick, via a precomputed
		// reciprocal. A runtime divide here would be a libgcc call on this chip.
		const uint32_t inc = static_cast<uint32_t>(
			(static_cast<uint64_t>(static_cast<uint32_t>(vibRateQ8_))
			 * kVibHzToIncQ16) >> 16);
		vibPhase_ += inc;
		vibCents_ = (fast_sin(vibPhase_) * vibCentsMax_) >> 15;
	}
	else
	{
		// Reset the phase so vibrato always starts from the centre of its
		// sweep rather than wherever it happened to stop. Coming in mid-swing
		// puts the note briefly off-pitch the moment the depth opens up.
		vibPhase_ = 0;
		vibCents_ = 0;
	}

	// What the bore actually gets.
	if (stopped_)
	{
		// The chiff stop. Not merely a gate close: Flute::Mute() also slams the
		// bore filter shut, so the tail dies in a few milliseconds rather than
		// decaying. That is what makes it a STOP, and what makes the release
		// re-attack cleanly rather than continuing whatever was still sounding.
		breath_ = 0;
	}
	else
	{
		int32_t b = curved_;
		// The chiff's dip in the air. Applied AFTER the curve so it is a real
		// interruption of the flow rather than a change in how hard you blow.
		if (chiffTicks_ > 0)
		{
			const int32_t dip = (kChiffDipQ12 * chiffTicks_) / kChiffTicks;
			b -= dip;
			if (b < 0) b = 0;
		}
		breath_ = b;
	}

	// Consume the edge. See the note at the top of this function.
	if (firedThisTick) chiffFired_ = false;
}

} // namespace nib
