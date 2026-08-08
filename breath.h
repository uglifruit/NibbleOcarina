// breath.h — how the knob becomes a wind instrument's air supply.
//
// Four jobs, all at control rate except Step():
//
//   1. The breath curve, and the threshold below which the card is SILENT.
//      This is what replaces the fingering's inability to express "no holes
//      covered" — Four Voltages has no rest voltage, so silence has to come
//      from somewhere else, and on a wind instrument that somewhere is the
//      air.
//
//   2. Articulation: tongued (a chiff on every new note) or legato (glide, no
//      chiff, plus vibrato).
//
//   3. The chiff stop: switch-down forces silence AND damps the bore.
//
//   4. The register switch. See flute.h — overblowing does not emerge from the
//      physics, so hard blowing jumps the octave explicitly, with hysteresis
//      so the boundary cannot chatter.

#pragma once
#include <stdint.h>
#include "ocarina.h"

namespace nib {

// ---------------------------------------------------------------------------
// Breath
// ---------------------------------------------------------------------------

/// Below this knob position the instrument is silent.
///
/// ~7% of travel. Wide enough that the knob has a definite "off" the player
/// can find without looking, narrow enough not to waste useful travel.
constexpr int32_t kBreathThresh = 300;

/// Where the octave jump happens, as an EFFORT value (0..4095).
///
/// Effort, not level: level is log-shaped and has flattened long before the top
/// of the knob, so a threshold on it would sit where a large physical movement
/// barely changes the number. Effort stays linear, so 2870 of 4095 really is
/// 70% of the travel.
///
/// The band between the two is hysteresis. Once the register has flipped up it
/// takes a drop back below kRegisterDown to return. Without a band, breath
/// noise at the boundary flips the octave several times a second, which is the
/// single most unmusical thing this card could do — tools/breathsim.py dithers
/// the knob across the threshold and asserts it stays put.
constexpr int32_t kRegisterUp   = 2870;   // ~70% of knob travel
constexpr int32_t kRegisterDown = 2460;   // ~60%, a comfortable band below

// ---------------------------------------------------------------------------
// Articulation
// ---------------------------------------------------------------------------

enum class Articulation : uint8_t {
	Tongued,   ///< switch MIDDLE: a chiff on every new note
	Legato,    ///< switch UP: glide between notes, no chiff, plus vibrato
};

/// Chiff: a momentary dip in the level at the start of a note, which is what
/// separates one note from the next when they are the same pitch.
///
/// The noise burst that used to accompany this is gone with the rest of the air
/// path — there is no noise generator any more. What remains is purely the dip,
/// which is the part that actually articulates: a short gap reads as a new
/// note, whereas a noise transient on a pure tone just reads as a click.
///
/// Expected to need tuning by ear. Too short and repeated notes smear together;
/// too deep and it is a gap rather than an articulation.
constexpr int32_t kChiffTicks     = kCtrlRate / 80;    ///< ~12ms
constexpr int32_t kChiffDipQ12    = 1600;              ///< how far the level dips

/// The shortest gap between two chiffs.
///
/// Without this a fast trill fires a chiff per note — at 10Hz that is a
/// stutter rather than a trill. 60ms lets ordinary playing articulate every
/// note while a deliberate trill smooths into one gesture.
constexpr int32_t kChiffMinGapTicks = (kCtrlRate * 6) / 100;

/// Vibrato is now the instrument's whole expression rather than a legato
/// garnish, so its rate and depth come from the knobs — see VibratoFor() in
/// flute.h. What lives here is only the oscillator that runs at that rate.
///
/// Rate arrives as Q8 Hz and has to become a Q32 phase increment per control
/// tick. Dividing by the control rate would be a runtime divide, so it is a
/// precomputed reciprocal: 2^32 / (256 * 3000), in Q16.
///
/// This constant is tied to kCtrlRate. Change one, change both.
constexpr uint32_t kVibHzToIncQ16 = 366503876u;

/// Glide rate for legato, as a slew shift. Larger is slower.
constexpr uint8_t kGlideShift = 9;

// ---------------------------------------------------------------------------

/// Everything about the air and how notes are begun and ended.
class Breath
{
public:
	void Init();

	/// Control rate. `knob` and `cvAdd` are raw 0..4095 / signed CV.
	void SetKnob(int32_t knob, int32_t cvAdd);

	void SetArticulation(Articulation a) { artic_ = a; }
	Articulation Art() const             { return artic_; }

	/// Momentary switch-down: silence, and damp the bore.
	void SetStopped(bool s);
	bool Stopped() const { return stopped_; }

	/// A new fingering arrived. Fires a chiff if articulation and timing allow.
	void NoteOn();

	/// Control rate, after SetKnob(). Advances chiff and vibrato.
	void Tick();

	/// LEVEL: what the VCA uses, 0..4095. Zero means silent.
	///
	/// Deliberately NOT the same as Effort(). The knob is log-shaped for level,
	/// so the instrument is at nearly full volume by about half its travel —
	/// which is how ears hear loudness and how a wind instrument actually
	/// behaves. A linear or squared level curve spends most of the sweep still
	/// getting louder, which reads as an unresponsive knob.
	int32_t BreathQ12() const { return breath_; }

	/// EFFORT: how hard you are blowing, 0..4095, LINEAR to the top.
	///
	/// This is what brightness and harmonic drive follow. Once level has
	/// flattened out, effort keeps climbing, so the upper half of the knob
	/// stops being louder and starts being richer — the note leans in rather
	/// than just getting bigger. Without a separate curve the top third of the
	/// travel would do nothing audible at all.
	int32_t EffortQ12() const { return effort_; }

	/// True while the air is above the sounding threshold — drives the gate.
	bool Sounding() const { return breath_ > 0; }

	/// Vibrato rate and depth, set from both knobs at control rate.
	void SetVibrato(int32_t rateQ8, int32_t cents)
	{
		vibRateQ8_ = rateQ8;
		vibCentsMax_ = cents;
	}

	/// True if a chiff started on this tick — one blip on Pulse Out 2.
	bool ChiffFired() const { return chiffFired_; }

	/// Which register: 0 = as fingered, 1 = an octave up.
	int Register() const { return register_; }

	/// Current vibrato offset in cents, signed. Zero when depth is zero.
	int32_t VibratoCents() const { return vibCents_; }

private:
	int32_t curved_     = 0;   ///< LEVEL after the curve, before chiff/stop
	int32_t effort_     = 0;   ///< how hard you are blowing, linear
	int32_t breath_     = 0;   ///< what the bore actually gets
	int32_t chiffTicks_ = 0;
	int32_t sinceChiff_ = kChiffMinGapTicks;
	bool    chiffFired_ = false;
	bool    stopped_    = false;
	int     register_   = 0;
	int32_t vibCents_    = 0;
	int32_t vibRateQ8_   = 0;
	int32_t vibCentsMax_ = 0;
	uint32_t vibPhase_   = 0;
	Articulation artic_ = Articulation::Tongued;
};

} // namespace nib
