// breath.cpp — the air supply, articulation and the register switch.
//
// All control rate (3kHz). Integer only.

#include "breath.h"
#include "fastmath.h"
#include "pico.h"

namespace nib {

void Breath::Init()
{
	curved_ = breath_ = 0;
	chiffNoise_ = chiffTicks_ = 0;
	sinceChiff_ = kChiffMinGapTicks;
	chiffFired_ = false;
	stopped_ = false;
	register_ = 0;
	vibCents_ = 0;
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
	}
	else
	{
		// Square the normalised value above threshold.
		//
		// A linear map wastes the interesting region: onset and the edge of
		// the register boundary both land in the first third of the travel,
		// leaving the top half doing almost nothing. Squaring spreads them out
		// and puts the octave jump around 70% of the knob rather than 40%.
		//
		// The divisor is a compile-time constant, so gcc strength-reduces this
		// into a multiply — it is not a runtime division.
		//
		// The clamp on `n` is not belt-and-braces: at v == 4095 the division
		// yields exactly 4096, one PAST full scale in Q12, and squaring that
		// gives 4096 rather than 4095. One count over the 12-bit range is
		// enough to clip the DAC on the loudest note the card can play.
		int32_t n = ((v - kBreathThresh) << 12) / (4095 - kBreathThresh);
		if (n > 4095) n = 4095;
		curved_ = (n * n) >> 12;
	}

	// The register switch, with hysteresis.
	//
	// Overblowing does not emerge from the bore's physics (see flute.h), so it
	// is decided here. The band between the two thresholds is what stops
	// breath noise at the boundary from flipping the octave several times a
	// second — which would be the most unmusical thing this card could do.
	if (register_ == 0)
	{
		if (curved_ >= kRegisterUp) register_ = 1;
	}
	else
	{
		if (curved_ <= kRegisterDown) register_ = 0;
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

	if (chiffTicks_ > 0)
	{
		chiffTicks_--;
		// Linear decay over the burst. An exponential would be more physical
		// but the whole event is 12ms — nobody can hear the difference, and
		// linear costs one subtract.
		chiffNoise_ = (kChiffNoiseQ15 * chiffTicks_) / kChiffTicks;
	}
	else
	{
		chiffNoise_ = 0;
	}

	// Vibrato, legato only.
	if (artic_ == Articulation::Legato)
	{
		vibPhase_ += kVibratoIncQ32;
		vibCents_ = (fast_sin(vibPhase_) * kVibratoCents) >> 15;
	}
	else
	{
		vibPhase_ = 0;
		vibCents_ = 0;
	}

	// What the bore actually gets.
	if (stopped_)
	{
		// The chiff stop. Not merely a gate close: Waveguide::Mute() also drops
		// the loop gain so the bore damps out over ~50ms instead of ringing on.
		// That is what makes it a STOP, and what makes the release re-attack
		// cleanly rather than continuing whatever was still sounding.
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
