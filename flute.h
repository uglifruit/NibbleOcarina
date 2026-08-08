// flute.h — the bore: a jet-driven waveguide.
//
// Modelled and measured in tools/flutesim.py, which is a line-by-line port of
// flute.cpp. Every constant below came out of that model rather than out of a
// textbook, and the ones that matter carry their measurement.
//
// ---------------------------------------------------------------------------
// TOPOLOGY
// ---------------------------------------------------------------------------
//
//   breath (DC operating point)
//        |
//        v
//   [ jet cubic ] <--- jet tap (delay/2) ------+
//        |                                     |
//        +--> (+) --> [ delay line ] --> [ 1-pole LP ] --> INVERT --> +
//                          |
//                          +--> [ DC block ] --> out
//
// Three things about this are load-bearing and were each got wrong first:
//
// 1. THE REFLECTION INVERTS. An open pipe end is a pressure NODE, so the
//    pressure wave reflects with opposite sign. Two inversions per round trip
//    give all harmonics, which is what a flute has. A NON-inverting reflection
//    gives a closed pipe: odd harmonics only. That was measurable and stark —
//    energy at the second harmonic was ZERO at every breath level, and no
//    amount of jet tuning could produce an octave, because a closed pipe
//    physically cannot overblow to one.
//
// 2. BREATH IS AN OPERATING POINT, NOT A SIGNAL. It does not get added to the
//    loop; it positions the jet on the nonlinear curve, and the returning bore
//    pressure perturbs it around that point. Adding breath as a signal makes
//    the amplitude go DOWN as you blow harder, which is both wrong and funny.
//
// 3. THE LOOP PERIOD IS 1.5 TRAVERSALS, not 1. See kLoopFactorNum in pitch.h.
//
// ---------------------------------------------------------------------------
// ON OVERBLOWING — WHAT IS AND IS NOT CLAIMED
// ---------------------------------------------------------------------------
//
// The register change is EXPLICIT, not emergent. This is a deliberate
// correction to the original design, which assumed a cubic jet nonlinearity
// would overblow on its own as breath rose.
//
// It does not, and the model says why: driving the jet harder changes the
// harmonic BALANCE (the tone brightens, measurably) but not which mode the
// loop prefers. Attempts to force it — scaling jet gain with breath, shortening
// the jet delay with breath, moving the operating point further along the
// cubic — either changed nothing or threw the loop into non-harmonic modes at
// 2.23x and 27x the fundamental. Those are not registers, they are chaos.
//
// What IS emergent, and is used: above about MIDI 78 the bore abandons its
// fundamental by itself and plays the octave, because the jet tap gets too
// short to place accurately. That is a real physical effect and it is why
// kPitchHiNote is 75 — the card stays below it and switches registers on
// purpose instead, with hysteresis so the boundary cannot chatter.
//
// A player cannot tell the difference. The devlog can, and should.

#pragma once
#include <stdint.h>
#include "ocarina.h"

namespace nib {

// ---------------------------------------------------------------------------
// The delay line
// ---------------------------------------------------------------------------

/// Power-of-two buffer so the read pointer wraps with an AND.
///
/// The longest delay the card ever asks for is the lowest note bent as flat as
/// the fine tune allows: MIDI 36 at -100 cents = 518 samples. 1024 covers that
/// with room to spare and costs 2KB of RAM.
constexpr int kDelaySize = 1024;
constexpr int kDelayMask = kDelaySize - 1;

/// The longest usable delay, as a guard on the pitch path.
constexpr int kDelayMax = 768;

// ---------------------------------------------------------------------------
// Voice constants — all measured in tools/flutesim.py
// ---------------------------------------------------------------------------

/// Where the jet reads the bore, as a fraction of the loop delay, Q16.
///
/// 0.5. This is half of the mechanism that sets kLoopFactorNum — change one and
/// the instrument's tuning moves, so change both and regenerate the table.
constexpr int32_t kJetRatioQ16 = 32768;

/// How hard the returning bore pressure perturbs the jet, Q12.
///
/// 1.5 (6144). Below about 1.0 the loop does not sustain; far above it the
/// perturbation swamps the operating point and the tone turns to noise.
constexpr int32_t kJetFeedbackQ12 = 6144;

/// How far full breath pushes the jet along the cubic, Q12.
constexpr int32_t kBreathOffsetQ12 = 4096;

/// Loop gain, Q15. 32000 of 32768 is 0.977.
///
/// Close to unity because the bore must sustain between breath fluctuations;
/// under it because a gain of exactly 1.0 is an oscillator that never stops,
/// and over it is one that grows until it clips and stays there.
///
/// NOT exposed on a knob, deliberately: this is the one parameter that can make
/// the resonator either dead or self-oscillating, and neither is musical.
constexpr int32_t kLoopGainQ15 = 32000;

/// Loop gain while the chiff stop is held. Low enough that the bore empties in
/// ~50ms rather than ringing through its natural decay.
constexpr int32_t kMuteGainQ15 = 27000;

/// Reflection-filter cutoff, Q15. Larger is brighter.
///
/// 12000 rather than the more obvious 20000, and the reason is specific: at
/// 20000 the LOWEST notes lock onto a spurious high mode instead of their
/// fundamental, because a bright loop gives that mode enough gain to win.
/// Darkening the reflection kills it. Measured across MIDI 36..48.
constexpr int32_t kDampDefaultQ15 = 12000;

/// Range the X knob sweeps damping over. The floor stays dark enough to keep
/// the low notes stable; see kDampDefaultQ15.
constexpr int32_t kDampMinQ15 = 8000;
constexpr int32_t kDampMaxQ15 = 22000;

/// DC blocker pole, Q15. 32735/32768 puts the corner near 8Hz at 48kHz.
///
/// Mandatory, not optional: the jet cubic is asymmetric about the breath
/// operating point, so the loop accumulates a DC offset. Without this the
/// delay line saturates and the voice dies within a second or two.
constexpr int32_t kDcPoleQ15 = 32735;

/// Breath-noise lowpass, Q15. Gives the noise a spectrum rather than a hiss.
constexpr int32_t kNoiseLpQ15 = 9000;

// ---------------------------------------------------------------------------

/// One bore. Holds its own delay line, so it is 2KB — keep it a member of the
/// card object (which lives in .bss) and never on the stack.
class Waveguide
{
public:
	void Init(uint32_t seed);

	/// Set the loop delay, Q16 samples. Control rate only.
	void SetDelayQ16(uint32_t dQ16);

	/// Timbre: damping and noise amount, both Q15. Control rate only.
	void SetTimbre(int32_t dampQ15, int32_t noiseQ15);

	/// Damp the bore hard, for the chiff stop. Explicitly paired with Unmute():
	/// no other setter may touch the loop gain, or the stop becomes
	/// intermittent depending on what else happened to be called.
	void Mute();
	void Unmute();

	/// One sample. `breathQ12` is 0..4095; zero is silence.
	int32_t Step(int32_t breathQ12);

	/// The breath-noise component alone, for Audio Out 2. Valid after Step().
	int32_t LastNoise() const { return lastNoise_; }

private:
	int32_t ReadFrac(uint32_t offQ16) const;

	int16_t  buf_[kDelaySize] = {};
	uint32_t write_    = 0;
	uint32_t delayQ16_ = 0;
	uint32_t jetOffQ16_ = 0;

	int32_t dampQ15_  = kDampDefaultQ15;
	int32_t noiseQ15_ = 0;
	int32_t gainQ15_  = kLoopGainQ15;

	int32_t lp_      = 0;
	int32_t dcX1_    = 0;
	int32_t dcY1_    = 0;
	int32_t noiseLp_ = 0;
	int32_t lastNoise_ = 0;
	uint32_t rng_    = 0x1234567u;
};

} // namespace nib
