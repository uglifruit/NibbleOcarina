// flute.cpp — the voice.
//
// A line-by-line counterpart of tools/flutesim.py. Keep them in step.
//
// Step() runs at 48kHz inside a DMA interrupt handler. Integer only, no
// division, no 64-bit anything.

#include "flute.h"
#include "fastmath.h"
#include "pico.h"

namespace nib {

void Flute::Init()
{
	phase_ = 0;
	inc_   = 0;
	fold_  = 0;
	muted_ = false;
	sine_ = folded_ = 0;
	square_ = false;
	dcX1_ = dcY1_ = dcX2_ = dcY2_ = 0;
}

void __not_in_flash_func(Flute::Step)(int32_t levelQ12)
{
	// Zero level is EXACTLY zero on every output, and it costs one branch.
	//
	// Silence is structural here: everything is feed-forward from an oscillator
	// that is simply not scaled. v1's waveguide could not make this promise
	// because its excitation did not depend on the knob — see flute.h.
	if (levelQ12 <= 0 || muted_)
	{
		sine_   = 0;
		folded_ = 0;
		square_ = false;
		return;
	}

	phase_ += inc_;

	// The raw oscillator, Q12 (+/-4096).
	const int32_t raw = fast_sin(phase_) >> 3;

	// --- Audio Out 1: the sine -------------------------------------------
	int32_t s = (raw * levelQ12) >> 12;
	{
		// The +16384 ROUNDS the shift. It sits inside the blocker's own
		// feedback path, so a half-LSB truncation bias accumulates to a stable
		// non-zero offset — measured at -497 on a +/-4300 signal in v1, on the
		// output of the thing whose entire job is removing DC.
		const int32_t y = s - dcX1_ + ((dcY1_ * kDcPoleQ15 + 16384) >> 15);
		dcX1_ = s;
		dcY1_ = y;
		sine_ = y;
	}

	// --- Audio Out 2: the wavefold ---------------------------------------
	//
	// Triangle reflection: drive the signal past the rails and mirror it back
	// inside. Each reflection folds a peak into a valley, which is what
	// generates the harmonics — and because the shape stays symmetric they are
	// ODD only, so it thickens into something reedy rather than into fuzz.
	//
	// The loop is bounded at kFoldPasses rather than run to convergence: this
	// is the audio path, and an unbounded loop would make the sample cost
	// depend on the signal. Four is well past audible difference.
	int32_t f = (raw * (4096 + fold_ * 3)) >> 12;
	for (int i = 0; i < kFoldPasses; i++)
	{
		if      (f >  4096) f =  8192 - f;
		else if (f < -4096) f = -8192 - f;
		else break;
	}
	f = (f * levelQ12) >> 12;
	{
		const int32_t y = f - dcX2_ + ((dcY2_ * kDcPoleQ15 + 16384) >> 15);
		dcX2_ = f;
		dcY2_ = y;
		folded_ = y;
	}

	// --- Pulse Out 2: the square -----------------------------------------
	//
	// Taken from the oscillator's sign, NOT from the level-scaled sine. A
	// comparator on a scaled signal flips at the same instants but chatters
	// around zero as the level approaches silence.
	square_ = (raw >= 0);
}

// ---------------------------------------------------------------------------
// Knob mappings
// ---------------------------------------------------------------------------

namespace {

/// Linear interpolate a..b by t, where t is Q12 0..4096.
inline int32_t Lerp(int32_t a, int32_t b, int32_t tQ12)
{
	return a + (((b - a) * tQ12) >> 12);
}

} // namespace

Vibrato VibratoFor(int32_t mainQ12, int32_t xKnob)
{
	int32_t x = xKnob;
	if (x < 0) x = 0;
	if (x > 4095) x = 4095;

	// X morphs through three anchors, in two segments — so the middle anchor
	// lands on a real position of the knob rather than somewhere the maths
	// glosses over.
	int32_t rate, depth;
	if (x < 2048)
	{
		const int32_t t = x << 1;                        // 0..4094 across the half
		rate  = kVibRateFastQ8;                          // fast throughout
		depth = Lerp(kVibDepthWide, kVibDepthTight, t);
	}
	else
	{
		const int32_t t = (x - 2048) << 1;
		rate  = Lerp(kVibRateFastQ8, kVibRateSlowQ8, t);
		depth = Lerp(kVibDepthTight, kVibDepthWide, t);
	}

	// MAIN scales the depth, from nothing to the full amount X asked for.
	//
	// Below kVibOnset there is NO vibrato — not a small amount, none. The lower
	// half of the knob is the volume stage and the upper half is the vibrato
	// stage; letting them overlap would mean a turn in the lower half both
	// raised the level and started a wobble, so neither would read as its own
	// gesture.
	int32_t m = mainQ12;
	if (m < kVibOnset) return Vibrato{rate, 0};
	if (m > 4095) m = 4095;

	// The divisor is a compile-time constant, so this is a multiply.
	constexpr int32_t kSpan = 4095 - kVibOnset;
	const int32_t grow = ((m - kVibOnset) << 12) / kSpan;
	return Vibrato{rate, (depth * grow) >> 12};
}

int32_t XVolumeBoost(int32_t xKnob)
{
	int32_t x = xKnob;
	if (x < 0) x = 0;
	if (x > 4095) x = 4095;
	return (kXVolumeTilt * x) >> 12;
}

int32_t FoldFor(int32_t xKnob)
{
	int32_t x = xKnob;
	if (x < 0) x = 0;
	if (x > 4095) x = 4095;
	return (kFoldMax * x) >> 12;
}

} // namespace nib
