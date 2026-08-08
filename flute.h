// flute.h — the voice: a pure tone, three ways.
//
// Modelled and measured in tools/flutesim.py.
//
// ---------------------------------------------------------------------------
// WHAT THIS IS
// ---------------------------------------------------------------------------
//
//                          ┌──▶ (sine)        ──▶ Audio Out 1
//   osc (exact pitch) ─────┼──▶ (wavefolder)  ──▶ Audio Out 2
//                          └──▶ (comparator)  ──▶ Pulse Out 2
//                    ▲
//                 vibrato
//
// One oscillator, three simultaneous shapes of it, all at the same pitch and
// phase. There is no filter and no noise: the expression is VIBRATO, not
// breath, and the tone stays pure so the vibrato is what you hear.
//
// ---------------------------------------------------------------------------
// WHY THE AIR IS GONE
// ---------------------------------------------------------------------------
//
// v2.1 mixed filtered noise in as "breath". Reported from hardware: "the air is
// adding nothing". That was right — noise under a tone is either inaudible or
// it muddies the pitch, and the middle ground that would have sounded like an
// actual player breathing turned out not to exist at these levels.
//
// So the whole air path is deleted: no noise generator, no air/tone mix, no
// resonant body filter, no chiff noise burst. What replaced it as the source of
// expression is vibrato that GROWS with the knob — quiet and steady when you
// barely turn it, singing when you push. That is something a player does, and
// it is audible in a way the noise never was.
//
// A note on lineage, because this file has now been rewritten twice: v1 was a
// jet-driven waveguide that self-oscillated (see docs/DEVLOG.md), v2 was an
// oscillator with noise and a body filter, and this is v3. It has not been a
// physical model since v1 and it is not pretending to be one.

#pragma once
#include <stdint.h>
#include "ocarina.h"

namespace nib {

// ---------------------------------------------------------------------------
// Wavefolding — Audio Out 2
// ---------------------------------------------------------------------------

/// How hard X can drive the folder, Q12.
///
/// The fold is a triangle reflection: scale the signal up and mirror anything
/// past the rails back inside, which adds a pair of odd harmonics per fold.
/// ODD harmonics only, because the fold is symmetric — which is why it stays
/// reedy rather than turning to fuzz.
///
/// CAPPED AT 2048, and that is measured rather than cautious. Sweeping the
/// folder further is not monotonic: the spectral centroid climbs 1.04 -> 3.60
/// up to 2048, then DIPS back to 3.00 before rising again, because completing a
/// fold brings the fundamental back before the next pair of harmonics arrives.
///
/// The dip is a real property of wavefolding, not a bug — but a knob that gets
/// brighter, then duller, then brighter again reads as broken under the hand.
/// Stopping at the top of the monotonic region gives up some available richness
/// to keep the control honest.
constexpr int32_t kFoldMax = 2048;

/// How many reflections to allow. Four is well past the point where more makes
/// an audible difference, and it bounds the loop in the audio path.
constexpr int kFoldPasses = 4;

// ---------------------------------------------------------------------------
// DC
// ---------------------------------------------------------------------------

/// DC blocker pole, Q15. ~8Hz corner at 48kHz.
///
/// The folder is symmetric and produces no DC of its own, but the level VCA
/// multiplies whatever offset is present, and a stepped offset on a DC-coupled
/// output is a thump. Cheap insurance.
constexpr int32_t kDcPoleQ15 = 32735;

// ---------------------------------------------------------------------------

/// One voice: three shapes of a single oscillator.
class Flute
{
public:
	void Init();

	/// Set the pitch. Control rate only.
	void SetIncQ32(uint32_t inc) { inc_ = inc; }

	/// Wavefold depth for Audio Out 2, Q12. Control rate only.
	void SetFold(int32_t foldQ12) { fold_ = foldQ12; }

	/// Slam the level for the chiff stop.
	void Mute()   { muted_ = true; }
	void Unmute() { muted_ = false; }

	/// One sample. `levelQ12` is 0..4095; zero is exactly silent.
	void Step(int32_t levelQ12);

	/// The three shapes, valid after Step(). All at the same pitch and phase.
	int32_t Sine()   const { return sine_; }
	int32_t Folded() const { return folded_; }
	bool    Square() const { return square_; }

private:
	uint32_t phase_ = 0;
	uint32_t inc_   = 0;
	int32_t  fold_  = 0;
	bool     muted_ = false;

	int32_t sine_   = 0;
	int32_t folded_ = 0;
	bool    square_ = false;

	int32_t dcX1_ = 0, dcY1_ = 0;
	int32_t dcX2_ = 0, dcY2_ = 0;
};

// ---------------------------------------------------------------------------
// Vibrato
// ---------------------------------------------------------------------------
//
// The instrument's expression, and the reason the tone is pure.
//
// MAIN decides HOW MUCH: none at the bottom of the knob, growing once the level
// curve has flattened. So the knob reads as "quiet and steady" through "loud and
// steady" into "loud and singing", which is the shape of a phrase.
//
// X decides WHAT KIND, morphing through three anchors:
//
//     CCW    fast and wide    8Hz, 50 cents   dramatic
//     mid    fast and tight   8Hz, 10 cents   nervous, close
//     CW     slow and wide    3Hz, 50 cents   operatic
//
// X also tilts the overall level up a little across its sweep and opens the
// wavefolder on Audio Out 2, so the CW end is slower-vibrato, slightly louder
// and more harmonically complex all at once.

/// Vibrato anchors. Rate in Q8 Hz, depth in cents.
constexpr int32_t kVibRateFastQ8 = 8 * 256;
constexpr int32_t kVibRateSlowQ8 = 3 * 256;
constexpr int32_t kVibDepthWide  = 50;
constexpr int32_t kVibDepthTight = 10;

/// Where in Main's travel vibrato starts appearing, Q12.
///
/// Deliberately above where the level curve has flattened: the two stages should
/// not overlap, or turning the knob up in the lower half would both raise the
/// volume and start a wobble, and neither would read clearly.
constexpr int32_t kVibOnset = 2000;

/// How much extra level X adds across its sweep, Q12. Small — a tilt, not a
/// second volume control.
constexpr int32_t kXVolumeTilt = 700;

/// Vibrato rate and depth for a given Main/X position.
struct Vibrato { int32_t rateQ8, cents; };
Vibrato VibratoFor(int32_t mainQ12, int32_t xKnob);

/// The level X adds on top of Main's own curve, Q12.
int32_t XVolumeBoost(int32_t xKnob);

/// Wavefold depth for a given X position, Q12.
int32_t FoldFor(int32_t xKnob);

} // namespace nib
