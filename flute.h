// flute.h — the voice.
//
// Modelled and measured in tools/flutesim.py. Every constant below came out of
// that model, and the ones that matter carry their measurement.
//
// ---------------------------------------------------------------------------
// WHAT THIS IS
// ---------------------------------------------------------------------------
//
//   osc (exact pitch) ──▶ soft saturator ──┐
//                                          ├──▶ 2-pole bore filter ──▶ VCA ──▶
//   noise ──▶ 2-pole lowpass ──────────────┘         ▲                  ▲
//                                                  breath             breath
//
// A tuned oscillator supplies the pitch, filtered noise supplies the air, and a
// resonant lowpass standing in for the bore gives it a body. Breath scales the
// output AND opens the filter AND drives the saturator, so blowing harder is
// louder, brighter and richer together — which is what makes it feel like an
// instrument rather than a volume control.
//
// ---------------------------------------------------------------------------
// WHY IT IS NOT A WAVEGUIDE ANY MORE
// ---------------------------------------------------------------------------
//
// v1 was a jet-driven waveguide, and it had one bug that produced every symptom
// reported from the first hardware session — "super metallic", and the breath
// knob never reaching silence.
//
// The jet's feedback term was `-(jetTap * kJetFeedback) >> 12`, with NO BREATH
// IN IT. So with the knob at zero the bore's own returning pressure still drove
// the nonlinearity: measured, the jet was still outputting 1919 into a bore
// that was supposed to be silent. The instrument played itself. Consequences,
// all measured rather than guessed:
//
//   - it never went quiet, because nothing depended on breath;
//   - breath spanned 1.2:1 in loudness (2205 -> 2697 -> 1742 rms across the
//     whole knob — it actually got QUIETER at the top);
//   - the runaway saturated the delay line, giving h2 = 1.14x the fundamental,
//     h5 = 0.64, h8 = 0.41. That spiky spectrum IS the metallic sound;
//   - above MIDI 60 the octave won outright — at MIDI 72 the second harmonic
//     was 2.4x the fundamental, so the card played an octave above what CV
//     Out 1 reported.
//
// Worse, the loop-factor constant of 1.5 had been FITTED to that broken system.
// Measured on the linear resonator alone, with the jet disconnected, the factor
// is exactly 2.0 — so the tuning constant had absorbed the bug and hidden it.
//
// The jet could not be rescued. Gating it with breath, moving the gate to the
// coupling term, shortening the jet tap, re-damping — each either killed the
// oscillation or left the loop picking modes at 0.27x, 2.1x, 5x and 8x the
// intended pitch, because the nonlinear path and the bore compete to set the
// frequency and the nonlinear path keeps winning.
//
// Here nothing feeds back into the nonlinearity, so it can only COLOUR a pitch
// that is already exact. Tuning is 2.3 cents worst case across the whole range,
// against 30-170 cents for every waveguide variant tried.
//
// The cost, stated honestly: this is no longer a physical model. It is a
// synthesiser shaped to sound like one.

#pragma once
#include <stdint.h>
#include "ocarina.h"

namespace nib {

// ---------------------------------------------------------------------------
// Noise
// ---------------------------------------------------------------------------

/// Two cascaded one-poles turn white noise into air rather than hiss.
constexpr int32_t kNoiseLpQ15 = 9000;

// ---------------------------------------------------------------------------
// The bore filter
// ---------------------------------------------------------------------------

/// Cutoff range, Q15. Breath and X both open it.
constexpr int32_t kCutMin = 2600;
constexpr int32_t kCutMax = 13000;

/// Filter cutoff while the chiff stop is held, Q15.
///
/// Must be genuinely shut. 1200 measured at 69% of the open level — a 2-pole at
/// that corner is nowhere near closed for a note an octave below it, so the
/// stop barely stopped anything.
constexpr int32_t kMuteCutQ15 = 200;

/// Resonance range, Q15. Modest: this is a body, not a filter sweep, and high
/// resonance turns the noise back into a pitched tone, which defeats the point
/// of having air in the first place.
constexpr int32_t kResMin = 9000;
constexpr int32_t kResMax = 13000;

// ---------------------------------------------------------------------------
// Air
// ---------------------------------------------------------------------------

/// How airy the tone is, Q12 — 0 is pure tone, 4096 is pure noise.
///
/// The useful range is NARROW and HIGH, and this is measured: below about 2400
/// the noise is inaudible under the tone, and above 3800 the pitch disappears
/// altogether. A linear 0..4095 sweep wastes three quarters of the knob on no
/// audible change, which is exactly what the first attempt did.
constexpr int32_t kAirMax = 3700;   ///< X fully CCW: very breathy
constexpr int32_t kAirMin = 2200;   ///< X fully CW: nearly pure

/// How much harder blowing thins the air out. Soft playing is mostly breath on
/// a real instrument; leaning in makes the tone speak.
constexpr int32_t kAirBreathTilt = 700;

// ---------------------------------------------------------------------------
// Saturation
// ---------------------------------------------------------------------------

/// Drive into the soft saturator, Q12, from barely-touched to well past the
/// knee.
///
/// The floor is not zero and the span is large for a specific measured reason:
/// the oscillator peaks at 2047, and the saturator turns over at 4096, so drive
/// has to reach roughly 8000 in Q12 before the shape does ANYTHING. A range of
/// 3000..9000 — which looks generous — leaves the tone a pure sine throughout
/// and the breath knob changing only loudness.
constexpr int32_t kDriveMin  = 4000;
constexpr int32_t kDriveSpan = 20000;

/// DC blocker pole, Q15. ~8Hz corner at 48kHz. The saturator is asymmetric
/// under drive, so this is not optional.
constexpr int32_t kDcPoleQ15 = 32735;

// ---------------------------------------------------------------------------

/// One voice. No delay line any more, so this is small and cheap.
class Flute
{
public:
	void Init(uint32_t seed);

	/// Set the pitch. Control rate only.
	void SetIncQ32(uint32_t inc) { inc_ = inc; }

	/// Timbre, from breath and the X knob together. Control rate only.
	void SetTimbre(int32_t airQ12, int32_t cutQ15, int32_t resQ15,
	               int32_t driveQ12);

	/// Damp hard for the chiff stop — closes the filter so the tail dies fast.
	void Mute();
	void Unmute();

	/// One sample. `breathQ12` is 0..4095; zero is exactly silent.
	int32_t Step(int32_t breathQ12);

	/// The air component alone, for Audio Out 2. Valid after Step().
	int32_t LastNoise() const { return lastNoise_; }

private:
	uint32_t phase_ = 0;
	uint32_t inc_   = 0;
	uint32_t rng_   = 0x1234567u;

	int32_t airQ12_   = kAirMin;
	int32_t cutQ15_   = kCutMin;
	int32_t resQ15_   = kResMin;
	int32_t driveQ12_ = kDriveMin;
	bool    muted_    = false;

	int32_t n1_ = 0, n2_ = 0;      ///< noise lowpass
	int32_t b1_ = 0, b2_ = 0;      ///< bore filter
	int32_t dcX1_ = 0, dcY1_ = 0;
	int32_t lastNoise_ = 0;
};

/// Map the Main and X knobs onto the voice's four parameters.
///
/// Kept here, next to the constants it uses, so the mapping and the ranges
/// cannot drift apart. tools/flutesim.py asserts the whole grid.
struct Timbre { int32_t air, cut, res, drive; };
Timbre TimbreFor(int32_t breathQ12, int32_t xKnob);

} // namespace nib
