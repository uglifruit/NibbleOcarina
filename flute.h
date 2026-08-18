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

/// Extra fold BIAS Audio Out 2 carries over Audio Out 1.
///
/// Both outputs take the fold now — Out 1 used to be a bare sine, so all the
/// harmonic content sat on an output many patches never use. Something still
/// has to keep the two worth patching separately, and this is it.
///
/// It is extra BIAS and deliberately not extra DEPTH. Depth was tried first
/// and broke the monotonic-brightness guarantee that kFoldMax exists to
/// protect: the fold is already at its ceiling, so any offset above it puts
/// Out 2 back in the region where the spectral centroid DIPS as X rises
/// (measured 3.46 -> 2.98 -> 3.08, i.e. brighter, then duller, then brighter).
/// Bias changes the harmonic CHARACTER instead of the fold count — measured,
/// it drives the second harmonic from 8.8 to 835 with zero dips anywhere in
/// X's sweep. Out 2 is the reedier output, not the more-folded one.
constexpr int32_t kFoldBiasExtra = 700;

/// Fold BIAS range, Q12, applied before folding to break its symmetry and
/// bring in EVEN harmonics. Kept well under the fold's own ±4096 rails: past
/// about a quarter the waveform spends so long clamped that the fundamental
/// starts to disappear rather than the tone simply filling out.
constexpr int32_t kFoldBiasMax = 1024;

/// How much fold the BD session parameter can add on its own, Q12.
///
/// X sweeps the fold as a PERFORMANCE control; this sets the baseline it
/// sweeps up FROM, so the voice can be reedy even with X at zero. The two sum
/// and the result is clamped to kFoldMax, which is what keeps the combined
/// control inside the monotonic region.
///
/// A one-pole tone filter was tried in this slot first and removed: measured,
/// it cost six times the level while moving the spectral centroid only
/// 3.28 -> 3.60, because 6dB/octave barely reshapes a spectrum whose
/// harmonics are already clustered. It behaved as a volume control that
/// slightly dulled. The folder is the timbre engine on this card — it moves
/// the centroid 1.04 -> 3.60 monotonically — so the parameter uses that
/// instead of adding DSP that does not earn its cycles.
constexpr int32_t kFoldBaseMax = 2048;

// ---------------------------------------------------------------------------
// DC
// ---------------------------------------------------------------------------

/// DC blocker pole, Q15. ~16Hz corner at 48kHz.
///
/// The level VCA multiplies whatever offset is present, and a stepped offset
/// on a DC-coupled output is a thump.
///
/// This is no longer cheap insurance against a nearly-symmetric signal — the
/// fold BIAS deliberately pushes the waveform off centre, and because folding
/// is nonlinear the DC that comes out is not the bias that went in. Measured
/// pre-blocker across the fold's range it swings from +534 to -825, so there
/// is nothing constant to subtract analytically; the blocker has to track it.
///
/// At the old ~8Hz corner it could not: ~5% of that offset survived, leaving
/// a permanent +43 counts on Audio Out 2 that was still there after six
/// seconds of audio. 16Hz brings the worst case to 19 counts. It also
/// slightly INCREASES the amplitude of the lowest notes rather than eating
/// them, because the offset it removes was itself consuming headroom (C2 peak
/// 3159 -> 3264, measured). The corner stays well under C2's 65Hz.
constexpr int32_t kDcPoleQ15 = 32700;

// ---------------------------------------------------------------------------

/// One voice: three shapes of a single oscillator.
class Flute
{
public:
	void Init();

	/// Set the pitch. Control rate only.
	void SetIncQ32(uint32_t inc) { inc_ = inc; }

	/// Wavefold depth, Q12. Control rate only.
	///
	/// This now drives BOTH outputs. Audio Out 1 used to be a bare sine with
	/// every bit of harmonic interest confined to Out 2, which a patch using
	/// only the main output never heard — the card had a timbre control that
	/// most of the time was inaudible. Both outputs fold at THIS depth; what
	/// separates them is kFoldBiasExtra, not more folding.
	void SetFold(int32_t foldQ12) { fold_ = foldQ12; }

	/// Fold BIAS, Q12, signed. Offsets the signal before folding.
	///
	/// A symmetric fold produces odd harmonics only — hollow, clarinet-ish.
	/// Pushing the waveform off centre first makes the positive and negative
	/// folds differ, which is what puts EVEN harmonics in, and that is the
	/// difference between hollow and full/reedy. The DC blocker downstream
	/// removes the offset itself, so this changes the timbre without moving
	/// the signal off zero.
	void SetFoldBias(int32_t biasQ12) { foldBias_ = biasQ12; }

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
	uint32_t phase_    = 0;
	uint32_t inc_      = 0;
	int32_t  fold_     = 0;
	int32_t  foldBias_ = 0;
	bool     muted_    = false;

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

/// Vibrato anchors. Rate in Q8 Hz, depth in Q4 CENTS (sixteenths).
///
/// Depth is Q4 for the same reason the pitch path is: at small depths, whole
/// cents round the entire modulation to zero, so vibrato appeared as a step
/// rather than a fade. 50 cents is 800 in Q4.
constexpr int32_t kVibRateFastQ8   = 8 * 256;
constexpr int32_t kVibRateSlowQ8   = 3 * 256;
constexpr int32_t kVibDepthWideQ4  = 50 * 16;
constexpr int32_t kVibDepthTightQ4 = 10 * 16;

/// Where in Main's travel vibrato starts appearing, Q12.
///
/// 1200 of 4095, so vibrato begins around a quarter of the way up and has three
/// quarters of the travel to grow in. It was 2000 (halfway), which left the
/// expression crammed into the top half — reported from hardware as wanting the
/// vibrato "much earlier in the Main knob".
///
/// It still sits above where the level curve has essentially arrived, so the
/// two stages remain legible: turn the bottom of the knob and it gets louder,
/// turn the rest and it sings.
constexpr int32_t kVibOnset = 1200;

/// How much extra level X adds across its sweep, Q12. Small — a tilt, not a
/// second volume control.
constexpr int32_t kXVolumeTilt = 700;

/// Vibrato rate (Q8 Hz) and depth (Q4 cents) for a given Main/X position.
struct Vibrato { int32_t rateQ8, centsQ4; };
Vibrato VibratoFor(int32_t mainQ12, int32_t xKnob);

/// The level X adds on top of Main's own curve, Q12.
int32_t XVolumeBoost(int32_t xKnob);

/// Wavefold depth for a given X position, Q12.
int32_t FoldFor(int32_t xKnob);

} // namespace nib
