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
///
/// This is the shift at Main fully CW during release — see kReleaseTrimShift
/// below for how Main scales it live once the gate has fallen.
constexpr uint8_t kReleaseShiftMin = 6;
constexpr uint8_t kReleaseShiftMax = 10;

/// How far Main can SPEED UP the release, once it is no longer setting peak.
///
/// Main has two jobs in sequence, not two knobs: while the gate is held it
/// sets the note's peak (and, through kReleaseShiftMin/Max, the release it is
/// coupled to). The moment the gate falls, that job is done — the peak is
/// already fixed — so Main's same physical position starts meaning something
/// else: how fast the tail you are already hearing dies. Turn it further CCW
/// during the tail and the release shortens, all the way down to a truncate;
/// leave it where it was (or turn it CW) and the release plays out at the
/// length the peak already earned it.
///
/// This is deliberately a SUBTRACTION from the coupled shift, not a
/// replacement of it — Main during release can only shorten what the peak
/// bought, never lengthen it past kReleaseShiftMax. Lengthening past the
/// peak's own coupling would decouple "louder rings on longer" from what
/// actually determines how long a note rings, which is the rule this whole
/// envelope is built around.
constexpr uint8_t kReleaseTrimShift = 5;   ///< max shift REMOVED at Main == 0
constexpr uint8_t kReleaseShiftFloor = 2;  ///< fastest possible release, ~1ms

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
/// exactly as long as it is audible and not one tick longer.
constexpr int32_t kEnvFloor = 1;

/// Extra fractional bits carried in the envelope accumulator.
///
/// Without ANY extra bits, `v -= (v >> shift)` STALLS: once `v >> shift`
/// rounds to zero the decay stops dead, which caps the achievable release
/// length regardless of the shift. NIBBLE hit exactly this and every setting
/// past shift 11 decayed in the same 43ms.
///
/// But too MANY extra bits reintroduces a different fault, found twice on
/// this card. LevelQ12() — what the ear and the VCA actually hear — is
/// env_ >> kEnvFrac, a 12-bit integer, and near the floor the exponential's
/// per-tick step in env_ becomes smaller than one whole LevelQ12() count. The
/// audible output then holds perfectly flat for `(1 << kEnvFrac)` ticks before
/// dropping by one count, and at kEnvFrac = 8 that plateau is 85ms on the last
/// few audible levels — inaudible in a coarse dB-per-200ms measurement, but a
/// held-then-cliff shape at the ear, reported twice: "the decay to silence is
/// always too abrupt", and again, after a v4.0.2 attempt that slowed the
/// exponential further instead of shortening this plateau, "still VERY
/// AUDIBLY stopping". A separate v4.0.3 "final linear ramp" patched the
/// symptom but concentrated 40+dB of perceived loudness into its last ~60ms
/// (a ramp linear in raw amplitude is nowhere near linear in the dB the ear
/// actually tracks) and simply moved the cliff later — "notes are STILL
/// ending very very audibly".
///
/// The actual fix needs no ramp and no easing at all: the worst-case plateau
/// at the bottom of a pure exponential is `(1 << kEnvFrac) / kCtrlRate`
/// seconds, independent of the release shift, so it is set directly by
/// kEnvFrac. 4 gives a ~5ms worst-case hold — well under audible-as-a-step —
/// while still comfortably avoiding the original stall (verified across the
/// full kReleaseShiftMin..kReleaseShiftMax + kReleaseTrimShift range in
/// tools/breathsim.py). The release is ONE shape, exponential, start to
/// finish: constant percentage loss per tick is constant dB per unit time,
/// which is what a fade actually is.
constexpr int kEnvFrac = 4;

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
//
// The glide shift itself is no longer a fixed constant here — it is a session
// parameter set from the switch-UP mode (kGlideShiftMin/Max/Default, in
// ocarina.h) and lives in main.cpp alongside the other three. Every held note
// glides on a fingering change now; there is no on/off, only how fast.

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
	///
	/// Also latches the raw position for Tick() to read during release — see
	/// kReleaseTrimShift. Main is read every tick regardless of gate state
	/// (main.cpp does not know or care which job it is currently doing); which
	/// job that raw position DOES is entirely Tick()'s call, based on gate_.
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
	int32_t  mainRaw_ = 0;   ///< Main's live 0..4095, for the release trim
	bool     gate_    = false;
	bool     struck_  = false;
	uint8_t  attack_  = kAttackShiftFast;

	int32_t  vibCents_    = 0;
	int32_t  vibRateQ8_   = 0;
	int32_t  vibCentsMax_ = 0;
	uint32_t vibPhase_    = 0;
};

} // namespace nib
