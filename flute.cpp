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

void Flute::Init(uint32_t seed)
{
	phase_ = 0;
	inc_   = 0;
	rng_   = seed ? seed : 0x1234567u;   // xorshift never recovers from zero
	n1_ = n2_ = b1_ = b2_ = 0;
	dcX1_ = dcY1_ = lastNoise_ = 0;
	muted_ = false;
	airQ12_ = kAirMin; cutQ15_ = kCutMin;
	resQ15_ = kResMin; driveQ12_ = kDriveMin;
}

void Flute::SetTimbre(int32_t airQ12, int32_t cutQ15, int32_t resQ15,
                      int32_t driveQ12)
{
	if (airQ12 < 0) airQ12 = 0;
	if (airQ12 > 4096) airQ12 = 4096;
	if (cutQ15 < 512)     cutQ15 = 512;
	if (cutQ15 > kCutMax) cutQ15 = kCutMax;

	airQ12_   = airQ12;
	resQ15_   = resQ15;
	driveQ12_ = driveQ12;

	// Deliberately does NOT clear muted_. Mute state belongs to Mute()/Unmute()
	// alone — in v1 this setter reset the loop gain, which quietly cancelled
	// the chiff stop every time the X knob moved.
	cutQ15_ = muted_ ? kMuteCutQ15 : cutQ15;
}

void Flute::Mute()
{
	// Slam the filter shut. There is no feedback loop here, so unlike the old
	// waveguide there is nothing that can keep ringing — the tail is only the
	// filter's own, and closing it kills that in a few milliseconds.
	//
	// The cutoff has to go genuinely LOW, not merely low-ish: at 1200 the
	// filter still passed 69% of the level, because a 2-pole at that corner is
	// nowhere near shut for a signal an octave below it. Measured, not guessed.
	muted_  = true;
	cutQ15_ = kMuteCutQ15;
}

void Flute::Unmute()
{
	muted_ = false;
}

int32_t __not_in_flash_func(Flute::Step)(int32_t breathQ12)
{
	// Zero breath is EXACTLY zero out, and it costs one branch to guarantee.
	//
	// v1 could not make this promise: its excitation did not depend on breath,
	// so the bore drove itself and the card never went quiet. Everything here
	// is feed-forward, so silence is structural rather than something that has
	// to be arranged.
	if (breathQ12 <= 0)
	{
		lastNoise_ = 0;
		return 0;
	}

	// 1. The tone. Exact pitch, by construction.
	phase_ += inc_;
	int32_t tone = fast_sin(phase_) >> 4;          // ~+/-2048

	// 2. Soft saturation: y = x - x^3/3.
	//
	// The same curve the old jet used, but here nothing feeds back into it, so
	// it can only colour a pitch that is already fixed. It cannot run away and
	// it cannot change which frequency comes out.
	{
		int32_t d = (tone * driveQ12_) >> 12;
		if (d >  4096) d =  4096;
		if (d < -4096) d = -4096;
		const int32_t d2 = (d * d) >> 12;
		const int32_t d3 = (d2 * d) >> 12;
		tone = d - ((d3 * 21845) >> 16);           // d^3 / 3
	}

	// 3. The air. Two poles, so it is breath rather than hiss.
	const int32_t white = rand_bipolar(rng_) >> 2;
	n1_ += ((white - n1_) * kNoiseLpQ15) >> 15;
	n2_ += ((n1_   - n2_) * kNoiseLpQ15) >> 15;
	lastNoise_ = (n2_ * breathQ12) >> 12;

	// 4. Mix tone and air.
	const int32_t src = (tone * (4096 - airQ12_) + n2_ * airQ12_) >> 12;

	// 5. The bore: a 2-pole resonant lowpass, standing in for the body the
	//    delay line used to provide.
	b1_ += (((src - b1_) + (((b1_ - b2_) * resQ15_) >> 15)) * cutQ15_) >> 15;
	b2_ += ((b1_ - b2_) * cutQ15_) >> 15;
	int32_t out = b2_;

	// 6. Breath as a VCA. Together with the filter and drive above, this is
	//    what makes blowing harder louder AND brighter AND richer.
	out = (out * breathQ12) >> 12;

	// 7. DC blocker. The +16384 ROUNDS the shift: it sits inside the blocker's
	//    own feedback path, so a half-LSB truncation bias accumulates to a
	//    stable non-zero offset — measured at -497 on a +/-4300 signal in v1,
	//    on the output of the thing whose entire job is removing DC.
	const int32_t y = out - dcX1_ + ((dcY1_ * kDcPoleQ15 + 16384) >> 15);
	dcX1_ = out;
	dcY1_ = y;
	return y;
}

// ---------------------------------------------------------------------------

Timbre TimbreFor(int32_t breathQ12, int32_t xKnob)
{
	int32_t b = breathQ12;
	if (b < 0) b = 0;
	if (b > 4095) b = 4095;
	int32_t x = xKnob;
	if (x < 0) x = 0;
	if (x > 4095) x = 4095;

	Timbre t;

	// X: breathy -> pure, over the range where it is actually audible.
	int32_t air = kAirMax - (((kAirMax - kAirMin) * x) >> 12);
	// Blowing harder thins the air out, so soft playing is mostly breath.
	air -= (kAirBreathTilt * b) >> 12;
	if (air < 0) air = 0;
	t.air = air;

	// Brightness rises with breath, and X shifts the whole range.
	int32_t cut = kCutMin + ((7000 * b) >> 12) + ((3000 * x) >> 12);
	if (cut > kCutMax) cut = kCutMax;
	t.cut = cut;

	t.res   = kResMin + (((kResMax - kResMin) * x) >> 12);
	t.drive = kDriveMin + ((kDriveSpan * b) >> 12);
	return t;
}

} // namespace nib
