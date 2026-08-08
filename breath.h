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

/// Where the octave jump happens, as a CURVED breath value (0..4095).
///
/// These are post-curve numbers, which is why they look low: the curve is
/// squared, so 1871 of 4095 is 70% of the KNOB's travel, not 46%. Setting them
/// by eye against the knob rather than against the curve puts the jump at 88%
/// of travel — technically working, but crammed into the last centimetre where
/// it cannot be played deliberately.
///
/// The band between the two is hysteresis. Once the register has flipped up it
/// takes a drop back below kRegisterDown to return. Without a band, breath
/// noise at the boundary flips the octave several times a second, which is the
/// single most unmusical thing this card could do — tools/breathsim.py dithers
/// the knob across the threshold and asserts it stays put.
constexpr int32_t kRegisterUp   = 1871;   // ~70% of knob travel
constexpr int32_t kRegisterDown = 1585;   // ~65%, one full curve-step below

// ---------------------------------------------------------------------------
// Articulation
// ---------------------------------------------------------------------------

enum class Articulation : uint8_t {
	Tongued,   ///< switch MIDDLE: a chiff on every new note
	Legato,    ///< switch UP: glide between notes, no chiff, plus vibrato
};

/// Chiff: a burst of extra noise plus a momentary dip in the air, which is
/// physically what a tongue stroke does — it interrupts the flow and releases.
///
/// These are grouped and flagged because they are EXPECTED TO NEED TUNING BY
/// EAR on hardware. A chiff that is too short vanishes into a breathy
/// instrument; one that cuts the air too hard is a click rather than a
/// consonant. Neither can be modelled usefully — this is the one part of the
/// card that has to be judged by listening.
constexpr int32_t kChiffTicks     = kCtrlRate / 80;    ///< ~12ms
constexpr int32_t kChiffNoiseQ15  = 14000;             ///< extra noise at peak
constexpr int32_t kChiffDipQ12    = 1200;              ///< how far the air dips

/// The shortest gap between two chiffs.
///
/// Without this a fast trill fires a chiff per note — at 10Hz that is a
/// stutter rather than a trill. 60ms lets ordinary playing articulate every
/// note while a deliberate trill smooths into one gesture.
constexpr int32_t kChiffMinGapTicks = (kCtrlRate * 6) / 100;

/// Vibrato, on legato only: legato wind playing is where vibrato lives, so
/// pairing them costs no control and reads as one gesture.
constexpr int32_t kVibratoCents   = 15;
constexpr uint32_t kVibratoIncQ32 = static_cast<uint32_t>(
	(5.0 * 4294967296.0) / 3000.0);      ///< ~5Hz at the 3kHz control rate

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

	/// The air, 0..4095. Zero means silent.
	int32_t BreathQ12() const { return breath_; }

	/// True while the air is above the sounding threshold — drives the gate.
	bool Sounding() const { return breath_ > 0; }

	/// Extra noise the chiff is asking for, Q15, on top of the timbre's own.
	int32_t ChiffNoiseQ15() const { return chiffNoise_; }

	/// True if a chiff started on this tick — one blip on Pulse Out 2.
	bool ChiffFired() const { return chiffFired_; }

	/// Which register: 0 = as fingered, 1 = an octave up.
	int Register() const { return register_; }

	/// Vibrato depth in cents, signed. Zero unless legato.
	int32_t VibratoCents() const { return vibCents_; }

private:
	int32_t curved_     = 0;   ///< breath after the curve, before chiff/stop
	int32_t breath_     = 0;   ///< what the bore actually gets
	int32_t chiffNoise_ = 0;
	int32_t chiffTicks_ = 0;
	int32_t sinceChiff_ = kChiffMinGapTicks;
	bool    chiffFired_ = false;
	bool    stopped_    = false;
	int     register_   = 0;
	int32_t vibCents_   = 0;
	uint32_t vibPhase_  = 0;
	Articulation artic_ = Articulation::Tongued;
};

} // namespace nib
