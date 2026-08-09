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
	register_ = 0;
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

		// LEVEL rises very fast, because LOUDNESS IS LOGARITHMIC and this
		// curve has to fight that.
		//
		// The history is worth keeping, because two plausible curves were both
		// wrong. v2.0 SQUARED the value: that is the opposite of what is
		// wanted, spending the whole sweep still getting louder. v2.1 used a
		// cubic 1-(1-n)^3, which looks fast on paper — 84% of full amplitude by
		// half travel — but AMPLITUDE is not loudness: in dB it was still only
		// -12dB (about half as loud) at a quarter of the travel, which is what
		// "the ramp seems very slow" was describing.
		//
		// A fifth power of the same shape gets to roughly half perceived volume
		// by an eighth of the travel and full by about a third, leaving the
		// remaining two thirds for vibrato. Two extra multiplies.
		const int32_t inv = 4096 - n;
		const int32_t i2 = (inv * inv) >> 12;
		const int32_t i4 = (i2 * i2) >> 12;
		curved_ = 4096 - ((i4 * inv) >> 12);
		// The curve reaches exactly 4096 at full travel, one past the 0..4095
		// this is documented to return. Harmless in today's arithmetic, but an
		// earlier curve had the identical off-by-one and that one did clip the
		// DAC. Clamp rather than re-derive the reasoning later.
		if (curved_ > 4095) curved_ = 4095;
	}

	// The register switch, with hysteresis.
	//
	// Overblowing does not emerge from the bore's physics (see flute.h), so it
	// is decided here. The band between the two thresholds is what stops
	// breath noise at the boundary from flipping the octave several times a
	// second — which would be the most unmusical thing this card could do.
	// Against EFFORT, not level: level has flattened long before the top of
	// the knob, so a threshold on it would sit in a region where a large
	// physical movement barely changes the number.
	if (register_ == 0)
	{
		if (effort_ >= kRegisterUp) register_ = 1;
	}
	else
	{
		if (effort_ <= kRegisterDown) register_ = 0;
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
