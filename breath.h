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
// There is no register switch any more. The card used to add an octave past
// about 70% of the knob, faking the overblow a waveguide voice could not
// produce on its own. That voice is long gone, and on the current one the jump
// was simply a 12-semitone STEP in the middle of the vibrato stage — reported
// from hardware as "when vibrato boundary on Main knob is added it seems to be
// an octave higher".
//
// The octave is chosen deliberately during calibration with the X knob, which
// is where an octave control belongs.

#pragma once
#include <stdint.h>
#include "ocarina.h"

namespace nib {

// ---------------------------------------------------------------------------
// Breath
// ---------------------------------------------------------------------------

/// Below this knob position the instrument is silent.
///
/// ~1.5% of travel, and small on purpose. It exists ONLY to give the knob a
/// definite "off" that can be found by feel — the gentle approach to silence
/// is the S-curve's job now, not this.
///
/// It was 300 (~7%) when the curve was steepest at the bottom, so the deadband
/// was doing double duty as a mute AND as a buffer against the sound slamming
/// on. With an S-curve that is dead travel: the first audible sound now arrives
/// at about 3% of the knob rather than 4.5%.
constexpr int32_t kBreathThresh = 60;

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

	/// Current vibrato offset in Q4 CENTS (sixteenths), signed.
	///
	/// Q4 and not whole cents: at small depths whole-cent arithmetic quantises
	/// the entire modulation to zero, so vibrato did nothing until its depth
	/// reached two cents and then arrived abruptly. That was the audible step.
	int32_t VibratoCentsQ4() const { return vibCents_; }

private:
	int32_t curved_     = 0;   ///< LEVEL after the curve, before chiff/stop
	int32_t effort_     = 0;   ///< how hard you are blowing, linear
	int32_t breath_     = 0;   ///< what the bore actually gets
	int32_t chiffTicks_ = 0;
	int32_t sinceChiff_ = kChiffMinGapTicks;
	bool    chiffFired_ = false;
	bool    stopped_    = false;
	int32_t vibCents_    = 0;
	int32_t vibRateQ8_   = 0;
	int32_t vibCentsMax_ = 0;
	uint32_t vibPhase_   = 0;
	Articulation artic_ = Articulation::Tongued;
};

} // namespace nib
