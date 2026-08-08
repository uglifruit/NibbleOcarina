// flute.cpp — the bore.
//
// A line-by-line counterpart of tools/flutesim.py. Keep them in step.
//
// Step() runs at 48kHz inside a DMA interrupt handler, against a budget of
// 4000 cycles per sample. Integer only, no division, no 64-bit anything.

#include "flute.h"
#include "fastmath.h"
#include "pico.h"

namespace nib {

void Waveguide::Init(uint32_t seed)
{
	for (int i = 0; i < kDelaySize; i++) buf_[i] = 0;
	write_ = 0;
	lp_ = dcX1_ = dcY1_ = noiseLp_ = lastNoise_ = 0;
	// xorshift32 never recovers from a zero seed.
	rng_ = seed ? seed : 0x1234567u;
	SetDelayQ16(static_cast<uint32_t>(256) << 16);
}

void Waveguide::SetDelayQ16(uint32_t dQ16)
{
	// Guard both ends. Too long reads past the buffer into stale samples; too
	// short and the interpolator has nothing to work with.
	const uint32_t maxQ16 = static_cast<uint32_t>(kDelayMax) << 16;
	const uint32_t minQ16 = static_cast<uint32_t>(8) << 16;
	if (dQ16 > maxQ16) dQ16 = maxQ16;
	if (dQ16 < minQ16) dQ16 = minQ16;

	delayQ16_ = dQ16;

	// The jet tap. This is a Q16 multiply, which is why it lives here at
	// control rate and not in Step().
	jetOffQ16_ = static_cast<uint32_t>(
		(static_cast<uint64_t>(dQ16) * kJetRatioQ16) >> 16);
	if (jetOffQ16_ < (2u << 16)) jetOffQ16_ = 2u << 16;
}

void Waveguide::SetTimbre(int32_t dampQ15, int32_t noiseQ15)
{
	if (dampQ15 < kDampMinQ15) dampQ15 = kDampMinQ15;
	if (dampQ15 > kDampMaxQ15) dampQ15 = kDampMaxQ15;
	dampQ15_  = dampQ15;
	noiseQ15_ = noiseQ15;
	gainQ15_  = kLoopGainQ15;
}

void Waveguide::Mute()
{
	// Drop the loop gain so the bore damps out over ~50ms instead of ringing
	// on. This is a STOP, not a gate close: releasing it re-attacks cleanly
	// because the resonator has actually been emptied.
	gainQ15_ = 27000;
}

/// Linear-interpolated read. The write side stays integer — only the read is
/// fractional, which is the cheap way round.
///
/// Linear interpolation is mildly lowpass, and here that is a feature: it is
/// the same high-frequency loop damping a real bore has. The alternative
/// (allpass) has a transient-dependent tuning glitch on every note change,
/// which is far worse in an instrument that changes note constantly.
int32_t __not_in_flash_func(Waveguide::ReadFrac)(uint32_t offQ16) const
{
	const uint32_t pos = ((write_ << 16) - offQ16)
	                   & ((static_cast<uint32_t>(kDelaySize) << 16) - 1);
	const uint32_t ip = (pos >> 16) & kDelayMask;
	const uint32_t mu = pos & 0xFFFF;
	const int32_t a = buf_[ip];
	const int32_t b = buf_[(ip + 1) & kDelayMask];
	return a + (((b - a) * static_cast<int32_t>(mu)) >> 16);
}

int32_t __not_in_flash_func(Waveguide::Step)(int32_t breathQ12)
{
	// 1. The bore's returning wave, and the pressure at the jet.
	const int32_t bore   = ReadFrac(delayQ16_);
	const int32_t jetTap = ReadFrac(jetOffQ16_);

	// 2. Reflection: one-pole loss, then INVERT.
	//
	// The inversion is the whole character of the instrument — an open end is
	// a pressure node. Removing it turns this into a clarinet (odd harmonics,
	// overblows a twelfth) and it will still sound plausible, which is what
	// makes it a dangerous line to "fix".
	lp_ += ((bore - lp_) * dampQ15_) >> 15;
	const int32_t refl = -((lp_ * gainQ15_) >> 15);

	// 3. Breath noise. Scaled by breath, because a hard blow is noisier than a
	//    soft one — that is most of what "breathy" means.
	const int32_t white = rand_bipolar(rng_);
	noiseLp_ += ((white - noiseLp_) * kNoiseLpQ15) >> 15;
	const int32_t noise = (((noiseLp_ * noiseQ15_) >> 15) * breathQ12) >> 12;
	lastNoise_ = noise;

	// 4. The jet. Breath sets WHERE on the curve we sit; the bore pressure
	//    perturbs us around that point.
	const int32_t offset = (breathQ12 * kBreathOffsetQ12) >> 12;
	const int32_t x = offset + noise - ((jetTap * kJetFeedbackQ12) >> 12);

	// 5. Into the line: jet plus the reflected wave.
	buf_[write_] = clampS16(jet_cubic(x) + refl);
	write_ = (write_ + 1) & kDelayMask;

	// 6. DC blocker on the output tap. Mandatory — see flute.h.
	//
	// The +16384 ROUNDS the shift, and it is not cosmetic. An arithmetic shift
	// truncates toward negative infinity, and this shift sits inside the
	// blocker's own feedback path, so that half-LSB bias accumulates until it
	// reaches a stable non-zero fixed point: measured at -497 on a +/-4300
	// signal, i.e. a permanent 11% DC offset on the output of the thing whose
	// entire job is removing DC.
	//
	// Lowering the corner frequency makes it WORSE, not better, which is what
	// gives it away as a rounding artefact rather than leaked signal. Rounding
	// brings it to within a couple of LSB of zero.
	const int32_t y = bore - dcX1_ + ((dcY1_ * kDcPoleQ15 + 16384) >> 15);
	dcX1_ = bore;
	dcY1_ = y;
	return y;
}

} // namespace nib
