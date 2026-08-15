// breath.cpp — the envelope.
//
// All control rate (3kHz). Integer only.

#include "breath.h"
#include "fastmath.h"
#include "pico.h"

namespace nib {

void Breath::Init()
{
	peak_ = effort_ = env_ = mainRaw_ = 0;
	gate_ = struck_ = false;
	attack_ = kAttackShiftFast;
	vibCents_ = vibRateQ8_ = vibCentsMax_ = 0;
	vibPhase_ = 0;
}

void Breath::SetKnob(int32_t knob, int32_t cvAdd)
{
	int32_t v = knob + cvAdd;
	if (v < 0) v = 0;
	if (v > 4095) v = 4095;
	mainRaw_ = v;   // Tick() reads this live during release — see kReleaseTrimShift

	if (v < kBreathThresh)
	{
		peak_   = 0;
		effort_ = 0;
		return;
	}

	// The divisor is a compile-time constant, so gcc strength-reduces this
	// into a multiply — it is not a runtime division.
	int32_t n = ((v - kBreathThresh) << 12) / (4095 - kBreathThresh);
	if (n > 4095) n = 4095;

	// EFFORT is linear — simply how far the knob has been turned. Timbre
	// follows this rather than the envelope, so brightness tracks how hard you
	// are playing instead of flickering with every note's decay.
	effort_ = n;

	// PEAK is an S-CURVE: flat at both ends, steep through the middle.
	//
	// Four curves have been wrong here in four different ways, so the history
	// is worth keeping.
	//
	//   v2.0  SQUARED. The opposite of what is wanted — it spends the whole
	//         sweep still getting louder and only arrives at the very end.
	//
	//   v2.1  cubic 1-(1-n)^3. Looks fast (84% of full AMPLITUDE by half
	//         travel) but amplitude is not loudness: in dB it was still only
	//         -12dB at a quarter of the travel. "Very slow ramp".
	//
	//   v3.1  fifth power. Fixed the slowness by being steepest exactly at the
	//         bottom — 6dB per ten counts of knob just above the threshold,
	//         which is a switch rather than a fade.
	//
	//   v3.3  smoothstep + lift. Flat at BOTH ends, steep in the middle.
	//
	// What the first three missed is that both ends matter. Leaving silence
	// gently needs the curve flat near zero; not feeling sluggish needs it
	// steep in the middle — that is a smoothstep, n^2*(3-2n). The lift term
	// s*(1-s) is zero at both extremes and largest in the middle, so it
	// recovers the speed a plain smoothstep gives away without touching the
	// flat ends.
	//
	// The multiply order is NOT free to rearrange. The obvious
	// `n2 = (n*n)>>12; sm = (n2*(3-2n))>>12` throws away four bits before the
	// second multiply, and the rounding that costs makes the curve
	// NON-MONOTONIC: 108 places where turning the knob up made the level go
	// DOWN. Shifting once at the end would be exact but n*n*(3-2n) peaks at
	// 2.06e11 and overflows int32. Reducing the small factor first keeps the
	// peak intermediate at 16.7M and is exactly monotonic across all 4096
	// inputs. Verified in tools/breathsim.py.
	const int32_t inner = (n * (3 * 4096 - 2 * n)) >> 12;
	int32_t sm = (n * inner) >> 12;
	if (sm > 4095) sm = 4095;
	peak_ = sm + ((sm * (4096 - sm)) >> 12);
	if (peak_ > 4095) peak_ = 4095;
}

void Breath::SetAttack(int32_t xKnob)
{
	if (xKnob < 0) xKnob = 0;
	if (xKnob > 4095) xKnob = 4095;
	// X sweeps the attack from a strike to a slow swell.
	attack_ = static_cast<uint8_t>(
		kAttackShiftFast +
		(((kAttackShiftSlow - kAttackShiftFast) * xKnob) >> 12));
}

void Breath::SetGate(bool on)
{
	// A note starts on the RISING edge only. Holding the gate does not
	// re-strike, which is what lets a held note be swelled and re-fingered
	// without ever re-attacking.
	struck_ = (on && !gate_);
	gate_ = on;
}

void Breath::Tick()
{
	// --- the envelope ----------------------------------------------------
	//
	// Attack and release are both one-pole, which gives the exponential shape
	// an ear expects and costs a shift and a subtract.
	//
	// The accumulator carries kEnvFrac extra bits. Without ANY, `v -= (v >>
	// shift)` STALLS: once `v >> shift` rounds to zero the decay stops dead,
	// capping the release length regardless of the shift. NIBBLE hit exactly
	// this and every setting past shift 11 decayed in the same 43ms. But too
	// MANY reintroduces a different fault: see kEnvFrac's comment in breath.h
	// for the two prior attempts at shaping the release ending that made it
	// worse rather than better, and why the actual fix needed neither.
	const int32_t target = peak_ << kEnvFrac;

	if (gate_)
	{
		// Climb toward the peak. Because the target is read every tick, moving
		// the knob mid-note swells or eases the note in real time.
		if (env_ < target)
		{
			const int32_t d = target - env_;
			int32_t step = d >> attack_;
			if (step == 0) step = 1;      // never stall short of the target
			env_ += step;
			if (env_ > target) env_ = target;
		}
		else if (env_ > target)
		{
			// The knob came DOWN mid-note. Ease off at the attack rate rather
			// than jumping, so a swell is reversible.
			const int32_t d = env_ - target;
			int32_t step = d >> attack_;
			if (step == 0) step = 1;
			env_ -= step;
			if (env_ < target) env_ = target;
		}
	}
	else if (env_ > 0)
	{
		// RELEASE. ONE shape, exponential, start to finish: constant
		// percentage loss per tick is constant dB per unit time, which is
		// what a fade actually is. No separate "ease" and no final ramp — see
		// kEnvFrac in breath.h for why both of those were tried and both made
		// the ending worse rather than better.
		//
		// BASE rate comes from the note's own peak — loud notes ring on, quiet
		// ones are short, which is what makes Main feel like dynamics while it
		// is setting peak (kReleaseShiftMin/Max). MAIN'S SECOND JOB: once the
		// gate has fallen, that peak-setting job is done, so the same physical
		// position now TRIMS this rate live, every tick, on top — fully CW
		// leaves it untouched, CCW shortens it toward a truncate. See
		// kReleaseTrimShift.
		const uint8_t trim = static_cast<uint8_t>(
			(kReleaseTrimShift * (4096 - mainRaw_)) >> 12);
		uint8_t rel = static_cast<uint8_t>(
			kReleaseShiftMin +
			(((kReleaseShiftMax - kReleaseShiftMin) * peak_) >> 12));
		rel = (rel > trim + kReleaseShiftFloor)
		          ? static_cast<uint8_t>(rel - trim)
		          : kReleaseShiftFloor;
		int32_t step = env_ >> rel;
		if (step == 0) step = 1;
		env_ -= step;
		if (env_ < (kEnvFloor << kEnvFrac)) env_ = 0;
	}

	// --- vibrato ---------------------------------------------------------
	if (vibCentsMax_ > 0 && env_ > 0)
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
}

} // namespace nib
