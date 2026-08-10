// breath.h — the envelope, and how a note is begun, held and ended.
//
// ---------------------------------------------------------------------------
// THE INSTRUMENT IS BOWED, NOT BLOWN
// ---------------------------------------------------------------------------
//
// The card is SILENT until you sound it. The momentary switch (or a gate on
// Pulse In 1) is the bow:
//
//   TAP      a struck note. Attack, then release.
//   HOLD     the note sustains for as long as you hold it, then releases.
//   WHILE HELD
//            moving the Main knob swells the note in real time, and changing
//            the fingering GLIDES to the new pitch without re-attacking — the
//            way a finger moves on a bowed string.
//
// Main sets the note's peak level AND, coupled to it, how long it takes to die
// away: a loud note rings on, a quiet one is short. X sets the attack shape,
// from an instant strike to a slow swell.
//
// v3.3 and earlier had the Main knob sounding a continuous drone and the
// switch acting as a MUTE. That is backwards for an instrument you play rather
// than one you leave running, and it meant every note had the same shape.
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
// The envelope
// ---------------------------------------------------------------------------

/// Attack shift range, set by X. Smaller is faster.
///
/// 2 is about 3ms — a strike, effectively instant. 11 is about 1.5s, a swell
/// you can hear arriving. X sweeps between them alongside its vibrato duties,
/// so the anticlockwise end is percussive with a wide vibrato while the
/// clockwise end is a slow swell with a slow one.
///
/// The slow end was 9, chosen from the one-pole's time to -60dB. That is the
/// wrong measure for an ATTACK: a one-pole reaches 90% in about 2.3 time
/// constants, not 6.9, so shift 9 arrived in 196ms rather than the intended
/// second and the "swell" end of the knob was barely slower than the middle.
/// Measured in tools/breathsim.py.
constexpr uint8_t kAttackShiftFast = 2;
constexpr uint8_t kAttackShiftSlow = 11;

/// Release shift range, COUPLED TO LEVEL.
///
/// This is the "generally louder will last longer" coupling: the release shift
/// is derived from the note's peak rather than from a knob of its own, so a
/// quiet note is gone quickly and a loud one rings on. One gesture, two
/// musical consequences, which is what makes the knob feel like dynamics
/// rather than like a volume fader.
constexpr uint8_t kReleaseShiftMin = 6;
constexpr uint8_t kReleaseShiftMax = 10;

/// Where the release EASES OFF, as output levels.
///
/// A plain exponential decays at a constant rate in dB — which is a straight
/// line, forever. It has no ending: it simply gets quieter at the same speed
/// until the arithmetic runs out, and that is heard as a note that FINISHES
/// rather than fades. Reported from hardware twice, the second time as "I am
/// still hearing notes finish".
///
/// Below kEaseLevel1 the release stops being exponential in `env_` at all and
/// becomes a LINEAR ramp in the audible output (LevelQ12()) down to zero over
/// kEaseTailTicks. This second attempt replaced a first one that slowed the
/// exponential further (a bigger shift) instead of changing its shape — that
/// looked right in the accumulator but was invisible at the ear, because
/// LevelQ12() is env_ >> kEnvFrac and a slowed-further exponential's per-tick
/// step becomes SMALLER than one count of that shifted-down output. The
/// result was silent to the ear: the last ~19 audible levels each held dead
/// flat for 43-85ms and then the last one, level 1, snapped straight to zero
/// — a plateau followed by a cliff, which is exactly "stopping", not fading.
/// A linear ramp guarantees every one of the last few audible steps is the
/// same size and the final step is no bigger than the others.
constexpr int32_t kEaseLevel1 = 32;      ///< below this, switch to a linear ramp
constexpr int32_t kEaseTailTicks = 96;   ///< ticks to cross kEaseLevel1 -> 0, ~32ms

/// Where the envelope is considered finished and is snapped to zero.
///
/// A one-pole approaches zero asymptotically and would otherwise run forever,
/// holding the gate output high and blocking the next note from starting
/// cleanly. So it has to be cut somewhere — but WHERE matters a great deal.
///
/// This was 8 of 4095, which is -54dB. That sounds negligible written down and
/// is plainly audible in the room: it lops the last 18dB off every note, so
/// the tail stops rather than fades. Reported from hardware as "the decay to
/// silence is always too abrupt - I can hear it cut off".
///
/// The right floor is one step below what the DAC can render. Output is 12-bit
/// and LevelQ12() shifts the accumulator down by kEnvFrac, so the last audible
/// level is env == (1 << kEnvFrac). Cutting there means the envelope runs
/// exactly as long as it is audible and not one tick longer — the tail plays
/// out in full, and the ~85ms it used to spend counting down below audibility
/// (with the gate still high) is gone too.
constexpr int32_t kEnvFloor = 1;

/// Extra fractional bits carried in the envelope accumulator.
///
/// Without these `v -= (v >> shift)` stalls: once `v >> shift` rounds to zero
/// the decay stops dead, which caps the achievable release length regardless
/// of the shift. NIBBLE hit exactly this and every setting past shift 11
/// decayed in the same 43ms. Eight extra bits push the stall far below the
/// audible floor.
constexpr int kEnvFrac = 8;

// ---------------------------------------------------------------------------
// Vibrato
// ---------------------------------------------------------------------------

/// Vibrato rate and depth come from the knobs — see VibratoFor() in flute.h.
/// What lives here is only the oscillator that runs at that rate.
///
/// Rate arrives as Q8 Hz and has to become a Q32 phase increment per control
/// tick. Dividing by the control rate would be a runtime divide, so it is a
/// precomputed reciprocal: 2^32 / (256 * 3000), in Q16.
///
/// This constant is tied to kCtrlRate. Change one, change both.
constexpr uint32_t kVibHzToIncQ16 = 366503876u;

// ---------------------------------------------------------------------------
// Portamento
// ---------------------------------------------------------------------------

/// Glide rate, as a slew shift. Smaller is FASTER.
///
/// 5 is about 70ms between adjacent notes — quick enough to read as an
/// articulation rather than an effect. It was 9 (over a second across a wide
/// interval), which on a struck instrument meant the glide was still arriving
/// when the note had already decayed.
constexpr uint8_t kGlideShift = 5;

// ---------------------------------------------------------------------------

/// Everything about the air and how notes are begun and ended.
class Breath
{
public:
	void Init();

	/// Control rate. `knob` and `cvAdd` are raw 0..4095 / signed CV.
	///
	/// Sets the note's PEAK, not its level: while a note is sounding this is
	/// what the envelope is climbing toward, so moving the knob mid-note swells
	/// or eases it. While nothing sounds it is simply how loud the next note
	/// will be.
	void SetKnob(int32_t knob, int32_t cvAdd);

	/// Attack shape, 0..4095 from the X knob. 0 is a strike, 4095 a slow swell.
	void SetAttack(int32_t xKnob);

	/// THE BOW. True while the switch is held or Pulse In 1 is high.
	///
	/// Rising edge starts a note; the note sustains for as long as this stays
	/// true; the falling edge releases it. A tap is simply a very short hold,
	/// which is why one control gives both struck and sustained notes without
	/// a mode.
	void SetGate(bool on);
	bool Gated() const { return gate_; }

	/// True on the tick a note started — one blip for Pulse Out 2.
	bool Struck() const { return struck_; }

	/// Control rate, after SetKnob()/SetGate(). Advances the envelope.
	void Tick();

	/// The envelope, 0..4095. This is what the voice's VCA uses.
	int32_t LevelQ12() const { return env_ >> kEnvFrac; }

	/// True while anything is audible — drives the gate output and lets the
	/// card skip work when it is silent.
	bool Sounding() const { return env_ > 0; }

	/// EFFORT: the knob position, LINEAR to the top.
	///
	/// Timbre follows this rather than the envelope, so brightness tracks how
	/// hard you are playing rather than flickering with every note's decay.
	int32_t EffortQ12() const { return effort_; }

	/// Vibrato rate (Q8 Hz) and depth (Q4 cents), from both knobs.
	void SetVibrato(int32_t rateQ8, int32_t centsQ4)
	{
		vibRateQ8_   = rateQ8;
		vibCentsMax_ = centsQ4;
	}

	/// Current vibrato offset in Q4 CENTS (sixteenths), signed.
	///
	/// Q4 and not whole cents: at small depths whole-cent arithmetic quantises
	/// the entire modulation to zero, so vibrato did nothing until its depth
	/// reached two cents and then arrived abruptly. That was an audible step.
	int32_t VibratoCentsQ4() const { return vibCents_; }

private:
	int32_t  peak_    = 0;   ///< what the envelope climbs toward, Q12
	int32_t  effort_  = 0;   ///< raw knob position, linear
	int32_t  env_     = 0;   ///< the envelope itself, Q12 << kEnvFrac
	bool     gate_    = false;
	bool     struck_  = false;
	uint8_t  attack_  = kAttackShiftFast;

	/// State of the final linear ramp. tailStep_ is 0 when not in the ramp;
	/// both are set ONCE, the tick the envelope first drops below
	/// kEaseLevel1, from the level at that instant — never recomputed against
	/// the shrinking remainder, or it would just be another exponential.
	int32_t  tailStep_      = 0;   ///< env_ units subtracted per tick, fixed
	int32_t  tailTicksLeft_ = 0;

	int32_t  vibCents_    = 0;
	int32_t  vibRateQ8_   = 0;
	int32_t  vibCentsMax_ = 0;
	uint32_t vibPhase_    = 0;
};

} // namespace nib
