// NIBBLE OCARINA — a wind instrument for the Music Thing Modular Workshop
// Computer.
//
// Fingering comes from the Workshop System's Four Voltages module: one cable
// into CV In 1 carries all four buttons. The Main knob is breath. The card is
// silent when you stop blowing, which is the only way it CAN be silent — see
// the note on rest voltage in ocarina.h.
//
// ---------------------------------------------------------------------------
// PANEL
// ---------------------------------------------------------------------------
//
//   CV In 1      Four Voltages output — the fingering
//   CV In 2      pitch offset, +/- semitones
//   Audio In 1   offsets the Main knob   (used as CV)
//   Audio In 2   offsets the X knob      (used as CV)
//   Pulse In 1   THE BOW, same as the switch: gate high sounds a note
//
//   Main         note peak + release length, then VIBRATO DEPTH
//                                             (FINE TUNE during calibration)
//   X            attack shape + vibrato character + fold  (OCTAVE during cal)
//   Y            scale                       (COARSE TUNE during calibration)
//
//   Switch UP    SESSION PARAMETERS. Press A/B/C/D then turn Main to set:
//                  A portamento glide time   B vibrato depth
//                  C wavefold amount         D reserved
//                Held (not momentary), a third stable position alongside
//                DOWN — see ParamTick(). A note already releasing keeps
//                ringing; nothing here re-fingers or re-strikes it.
//   Switch MID   rest position
//   Switch DOWN  THE BOW — tap for a struck note, hold to sustain
//
// Calibration runs once at power-on and has NO gesture: the switch is a
// playing control now and cannot carry one. Reset to recalibrate. It drones a
// reference note throughout; Y and Main tune it and X picks its octave while
// you teach the fingering.
//
// THE INSTRUMENT IS BOWED. It is silent until the switch is held or Pulse In 1
// goes high. A tap is a struck note; a hold sustains for as long as you hold
// it. While held, moving Main swells the note and changing the fingering
// glides to the new pitch without re-attacking — every held note glides now,
// at whatever speed session parameter A sets; there is no separate toggle.
//
//   Audio Out 1  the tone, a sine
//   Audio Out 2  the same tone, wavefolded as X rises
//   CV Out 1     1V/oct pitch, root at 0V
//   CV Out 2     level — tracks what you hear
//   Pulse Out 1  a trigger on every note change
//   Pulse Out 2  the same tone as a square
//
// ---------------------------------------------------------------------------
// STRUCTURE
// ---------------------------------------------------------------------------
//
// ProcessSample() runs at 48kHz inside a DMA interrupt, against a 4000-cycle
// budget. It does the voice and nothing else expensive; everything that can wait
// runs on one of two staggered control ticks at 3kHz, so no single sample pays
// for both.

#include "ComputerCard.h"

#include "hardware/vreg.h"
#include "pico/stdlib.h"

#include "ocarina.h"
#include "levels.h"
#include "scales.h"
#include "pitch.h"
#include "flute.h"
#include "breath.h"
#include "fastmath.h"

using namespace nib;

namespace {

// ---------------------------------------------------------------------------
// UI state
// ---------------------------------------------------------------------------

enum class UiMode : uint8_t { Play, Learn };

enum class LearnPhase : uint8_t {
	Waiting,    ///< holding a combo, awaiting the confirming tap
	Confirm,    ///< captured cleanly
	Collision,  ///< captured but too close to an earlier one, or not settled
	Decided,    ///< all fifteen taken; announcing which mode won
	Failed,     ///< span too small — nothing patched in
	Aborted,
};

constexpr int32_t kLearnTimeoutTicks   = 30 * kCtrlRate;
constexpr int32_t kCaptureFlashTicks   = kCtrlRate / 5;
constexpr int32_t kCollisionFlashTicks = kCtrlRate / 2;
constexpr int32_t kDecidedTicks        = (kCtrlRate * 5) / 2;   ///< 2.5s
constexpr int32_t kFailFlashTicks      = (3 * kCtrlRate) / 2;
constexpr int32_t kAbortFlashTicks     = kCtrlRate;
constexpr int32_t kScaleShowTicks      = (kCtrlRate * 6) / 5;

/// The calibration drone: a steady reference note that sounds throughout, so
/// the card can be tuned while its fingering is being taught.
///
/// Deliberately QUIET. It plays continuously for the length of a fifteen-tap
/// calibration, so anything approaching performance level is wearing rather
/// than helpful — this is a reference pitch, not the instrument. It also sits
/// well below the register boundary, so the octave never jumps under the
/// player's hands while they are trying to tune to it.
constexpr int32_t kDroneLevel = 900;



// ---------------------------------------------------------------------------
// Offset-from-current-position knob pickup, for the tuning controls.
// ---------------------------------------------------------------------------
//
// Main and Y are breath and scale in Play, so they can be anywhere when a
// calibration starts. Taking their absolute position would jump the tuning;
// these track the DELTA from wherever they sat on entry, and only once they
// have actually been moved.
//
// Three things have to be right, and each of them is a bug NIBBLE shipped and
// then fixed:
//
//   1. The reference is latched ONCE, when calibration starts, and never
//      re-taken. Re-taking it on each threshold crossing lets ADC dither
//      ratchet the tuning away with nobody touching the knob.
//   2. The comparison uses a SMOOTHED reading. A latched reference alone is not
//      enough when the noise is comparable to the threshold — NIBBLE measured a
//      stationary knob handing control back 17,000 times in 200k ticks.
//   3. The knob must move past kKnobMoveThresh before it takes control at all,
//      so starting a calibration cannot move the tuning by a single cent.
struct TuneKnob
{
	int32_t smooth_ = 0;
	int32_t ref_    = 0;
	int32_t base_   = 0;
	bool    live_   = false;

	void Enter(int32_t live, int32_t currentOffset)
	{
		smooth_ = ref_ = live;
		base_   = currentOffset;
		live_   = false;
	}

	int32_t Update(int32_t live, int32_t den, int32_t lo, int32_t hi)
	{
		smooth_ = slew_exact(smooth_, live, 4);
		const int32_t d = smooth_ - ref_;
		if (!live_)
		{
			if (d > kKnobMoveThresh || d < -kKnobMoveThresh) live_ = true;
			else return base_;
		}
		int32_t v = base_ + d / den;
		if (v < lo) v = lo;
		if (v > hi) v = hi;
		return v;
	}
};

/// Coarse tune: +/-12 semitones over half the knob's travel, so a semitone is
/// ~170 counts and can be landed on by feel.
constexpr int32_t kCoarseDen = 170;
constexpr int32_t kCoarseLo = -12, kCoarseHi = 12;

/// Fine tune: +/-100 cents, one semitone either way, so coarse and fine overlap
/// and there is no gap between what the two controls can reach.
constexpr int32_t kFineDen = 20;
constexpr int32_t kFineLo = -100, kFineHi = 100;

} // namespace

// ---------------------------------------------------------------------------

class Ocarina : public ComputerCard
{
public:
	// NOTHING that touches hardware may happen here — the card object is
	// constructed before the SDK is ready and a peripheral access wedges the
	// chip. Setup lives in main().
	Ocarina() {}

	void Setup()
	{
		levels_.InitDefault();
		voice_.Init();
		breath_.Init();
	}

	virtual void __not_in_flash_func(ProcessSample)() override
	{
		// --- boot ---------------------------------------------------------
		//
		// The switch is derived from a mux channel through a ~60Hz filter that
		// starts at zero, and zero decodes as Down. So burn half a second and
		// then take exactly ONE reading. Latching on "Down seen at any point in
		// the window" latches on every single boot — two sibling cards shipped
		// precisely that bug.
		if (bootPhase_ < kBootWindowSamples)
		{
			if (++bootPhase_ == kBootWindowSamples)
			{
				// Swallow whatever the switch is already doing, so a card that
				// powers up with it held does not fire a note on release.
				gateLatched_ = (SwitchVal() == Switch::Down);
				splash_      = kSplashSamples;
			}
			AudioOut1(0);
			AudioOut2(0);
			return;
		}

		if (splash_ > 0)
		{
			splash_--;
			if (splash_ == 0) EnterLearn();
			AudioOut1(0);
			AudioOut2(0);
			return;
		}

		// --- control ticks, staggered so neither shares a sample -----------
		if (++ctrlDiv_ >= kCtrlDiv) ctrlDiv_ = 0;
		if      (ctrlDiv_ == 0) ControlTick();
		else if (ctrlDiv_ == 8) UiTick();

		// --- audio ---------------------------------------------------------
		// During calibration the voice drones at a fixed level so there is a
		// steady reference to tune against; in play it follows the knob.
		const int32_t level = (ui_ == UiMode::Learn) ? kDroneLevel : level_;
		voice_.Step(level);

		// Audio Out 1: the sine. Audio Out 2: the same oscillator wavefolded,
		// so the two are always at the same pitch and phase and can be mixed
		// or crossfaded without any comb filtering between them.
		AudioOut1(clamp12(voice_.Sine() >> 1));
		AudioOut2(clamp12(voice_.Folded() >> 1));

		// Pulse Out 2 is the tone as a square, straight off the oscillator.
		PulseOut2(voice_.Square());

		// Pulse Out 2's blip. Pulse Out 1 is a level, set at control rate.
		if (pulse2Timer_ > 0)
		{
			pulse2Timer_--;
			if (pulse2Timer_ == 0) PulseOut2(false);
		}
	}

private:
	// -----------------------------------------------------------------------
	// Control tick — 3kHz
	// -----------------------------------------------------------------------

	void __not_in_flash_func(ControlTick)()
	{
		ReadSwitch();

		if (ui_ == UiMode::Learn) { LearnTick(); return; }

		// --- Play ----------------------------------------------------------

		const bool editingParams = (SwitchVal() == Switch::Up);

		// THE BOW. Either source sounds the instrument, and they behave
		// identically: a short press is a struck note, a long one sustains.
		// So a sequencer gate plays the card exactly as a finger does.
		//
		// gateLatched_ swallows a switch that was already held at boot, so the
		// card cannot fire a note on the release of a switch nobody pressed.
		//
		// Forced false while editing params: UP is its own mode, and presses
		// there select a parameter rather than sounding a note. A note already
		// releasing keeps ringing regardless — see breath_.Tick() below, which
		// always runs — this only stops NEW strikes.
		//
		// Computed before the fingering is read, so NoteOn() below can tell
		// whether a settled change happened while held or while releasing.
		bool gate = !editingParams &&
		            ((SwitchVal() == Switch::Down) || PulseIn1());
		if (gateLatched_)
		{
			if (!gate) gateLatched_ = false;   // released; arm normally
			gate = false;
		}

		// levels_ keeps tracking regardless of mode, so nothing here is ever
		// one tick stale when the player leaves either UP or a release tail.
		int8_t idx = kComboNone;
		const bool settled = (levels_.Step(CVIn1(), idx) == LevelEvent::Trigger);

		if (editingParams)
		{
			// Buttons select a PARAMETER here, not a pitch — see ParamTick().
			// combo_/pitch are untouched, exactly as during a release tail:
			// whatever is sounding keeps sounding at its own note.
			ParamTick(settled, idx);
		}
		else
		{
			// A settled fingering change only becomes a NOTE while the bow is
			// down. During the release tail it is READ (levels_ keeps
			// tracking, so the very next strike is instant rather than one
			// tick stale) but not ACTED on: the pitch stays exactly where the
			// bow left it for the whole tail, however the fingers move
			// underneath it. Re-fingering a dying note would be a glide
			// nobody asked for and a pitch that doesn't match what was struck
			// — a held note re-fingers because a bowed string does; a
			// released one is already committed to the note it was given.
			if (settled && gate) NoteOn(idx);

			// A fresh strike ARRIVING during a release tail must pick up
			// whatever is under the fingers RIGHT NOW, not the frozen note
			// the tail was still playing — otherwise a re-strike after a
			// silent re-fingering (no settled event fires because the
			// fingering already changed once, mid-tail, before this rising
			// edge) would attack the stale pitch. levels_.Current() is always
			// live, freeze or no freeze, so the edge alone is enough to
			// resync.
			if (gate && !gateLast_) NoteOn(levels_.Current());
		}
		gateLast_ = gate;

		// The two knobs, each offset by an audio input used as CV.
		//
		// AudioIn returns +/-2048 where the knobs are 0..4095, so the offsets
		// are doubled to cover the full travel — a full-scale CV can sweep the
		// knob end to end from either extreme. Only active when something is
		// actually patched, so an unpatched input cannot bias anything.
		const int32_t mainCv = Connected(Input::Audio1) ? (AudioIn1() << 1) : 0;
		const int32_t xCv    = Connected(Input::Audio2) ? (AudioIn2() << 1) : 0;

		int32_t xKnob = KnobVal(Knob::X) + xCv;
		if (xKnob < 0) xKnob = 0;
		if (xKnob > 4095) xKnob = 4095;
		xNow_ = xKnob;

		// While editing params, Main sets the selected parameter's value
		// instead of breath's peak — the note already sounding (if any) is a
		// release tail continuing on its own, untouched by Main here, exactly
		// as Main during any other release trims what already exists rather
		// than being read as a fresh peak. SetKnob() is simply not called.
		if (!editingParams)
		{
			// Main is read every tick regardless of gate state — which of its
			// two jobs (set peak, or trim the release) this position does is
			// Breath's call, based on its own gate state. See breath.h.
			breath_.SetKnob(KnobVal(Knob::Main), mainCv);
		}
		breath_.SetAttack(xNow_);

		breath_.SetGate(gate);

		// Vibrato comes from BOTH knobs: Main sets how much, X sets what kind.
		// Set before Tick() so the oscillator advances with this tick's values.
		// param B (vibMulQ8_) scales the depth ON TOP of that, session-wide —
		// 0 silences vibrato outright regardless of Main/X, 256 (default) is
		// unchanged from pre-4.2 behaviour.
		const Vibrato vib = VibratoFor(breath_.EffortQ12(), xNow_);
		const int32_t vibCentsQ4 = (vib.centsQ4 * vibMulQ8_) >> 8;
		breath_.SetVibrato(vib.rateQ8, vibCentsQ4);

		breath_.Tick();

		// Level: the envelope, plus X's small tilt.
		int32_t lvl = breath_.LevelQ12();
		if (lvl > 0)
		{
			lvl += XVolumeBoost(xNow_);
			if (lvl > 4095) lvl = 4095;
		}
		level_ = lvl;

		// Pulse Out 1 is the gate: high for exactly as long as a note sounds,
		// including its release tail, so an envelope elsewhere can follow the
		// whole note rather than just its start.
		PulseOut1(breath_.Sounding());

		// param C (foldMulQ8_) scales the fold amount X asked for, same shape
		// as the vibrato multiplier above — 0 keeps Audio Out 2 a plain sine
		// regardless of X, 256 (default) is unchanged.
		int32_t fold = (FoldFor(xNow_) * foldMulQ8_) >> 8;
		if (fold < 0) fold = 0;
		voice_.SetFold(fold);

		ReadScale();
		UpdatePitch();
		UpdateCVs();

		// One blip per struck note, for anything that wants the attack.
		if (breath_.Struck())
		{
			PulseOut2(true);
			pulse2Timer_ = kSampleRate / 500;      // 2ms
		}
	}

	// -----------------------------------------------------------------------
	// Gestures
	// -----------------------------------------------------------------------

	/// The scale root, as chosen by the X knob during calibration.
	int32_t BaseNote() const { return kOctaveBase[octave_]; }

	/// Switch UP is a third STABLE position — session parameter editing — not
	/// a flicked gesture. This is deliberately not the "switch position held
	/// while playing carries a gesture" mistake CLAUDE.md warns about twice
	/// over: those bugs came from overlaying a timed gesture on a position
	/// also used for normal play. UP overlays nothing; it IS its own mode,
	/// the same way DOWN already is the bow.
	///
	/// Entering UP must ignore whatever fingering happens to be held already
	/// — the player did not choose that combo for this purpose, it is just
	/// wherever their fingers were. So the edge into UP disarms selection;
	/// ControlTick()'s param-edit branch only arms on the NEXT settled change
	/// to a fresh single (A/B/C/D).
	void __not_in_flash_func(ReadSwitch)()
	{
		const bool up = (SwitchVal() == Switch::Up);
		if (up && !upLast_)
		{
			paramSel_   = kComboNone;
			paramArmed_ = false;
		}
		upLast_ = up;

		// The capture tap for calibration is the same switch, pressed. It fires
		// on the PRESS edge, not the release: release-firing never
		// double-fires, and is unplayable, because every capture lands when you
		// let go rather than when you meant it.
		const bool down = (SwitchVal() == Switch::Down);
		tapped_ = (down && !downLast_);
		downLast_ = down;
	}

	/// Switch-UP mode: buttons select a session parameter, Main sets it.
	///
	/// `settled`/`idx` are this tick's levels_.Step() result, already read by
	/// ControlTick() — passed in rather than re-read, since Step() advances
	/// state and must only run once per tick.
	///
	/// Entering UP disarms selection (see ReadSwitch()) so whatever fingering
	/// happened to be held is ignored; this only arms on the FIRST settled
	/// change afterward that lands on a fresh single (A/B/C/D). Pairs,
	/// triples and the quad don't select anything — they aren't one of the
	/// four parameters, so treating them as a selection would be a wrong
	/// answer rather than simply an unused gesture.
	void __not_in_flash_func(ParamTick)(bool settled, int8_t idx)
	{
		if (settled && idx >= kA && idx <= kD)
		{
			paramSel_   = idx;
			paramArmed_ = true;
		}

		if (!paramArmed_ || paramSel_ == kComboNone) return;

		int32_t knob = KnobVal(Knob::Main);
		if (knob < 0) knob = 0;
		if (knob > 4095) knob = 4095;

		switch (paramSel_)
		{
		case kA:   // portamento glide time — CCW is fast, CW is slow, matching
		           // every other "CCW = less/faster" convention on this card.
			glideShift_ = static_cast<uint8_t>(kGlideShiftMin +
				(((kGlideShiftMax - kGlideShiftMin) * knob) >> 12));
			break;
		case kB:   // vibrato depth multiplier
			vibMulQ8_ = kVibMulQ8Min +
				(((kVibMulQ8Max - kVibMulQ8Min) * knob) >> 12);
			break;
		case kC:   // wavefold multiplier
			foldMulQ8_ = kFoldMulQ8Min +
				(((kFoldMulQ8Max - kFoldMulQ8Min) * knob) >> 12);
			break;
		case kD:   // reserved — no-op
		default:
			break;
		}
	}

	/// Sets the target pitch. Called from ControlTick() in two situations
	/// only: a settled fingering change WHILE THE BOW IS DOWN, and the rising
	/// edge of the gate itself (to resync a fresh strike to whatever is under
	/// the fingers, even if it changed silently during the previous note's
	/// release). Never called for a fingering change during a release tail —
	/// see the comment above that call site.
	///
	/// It does NOT start a note — the bow does that. Changing fingering while
	/// a note is sounding simply moves its pitch, which is how a bowed string
	/// behaves and is what makes held-and-refingered phrases possible.
	void NoteOn(int8_t combo)
	{
		combo_ = combo;
		pitchDirty_ = true;
	}

	// -----------------------------------------------------------------------
	// Pitch
	// -----------------------------------------------------------------------

	void __not_in_flash_func(ReadScale)()
	{
		// Twelve scales across the knob.
		int s = (KnobVal(Knob::Y) * kNumScales) >> 12;
		if (s < 0) s = 0;
		if (s >= kNumScales) s = kNumScales - 1;
		if (s != scale_)
		{
			scale_ = s;
			scaleShow_ = kScaleShowTicks;
			pitchDirty_ = true;
		}
	}

	/// combo -> degree -> semitone -> increment, and the CV to match.
	void __not_in_flash_func(UpdatePitch)()
	{
		const int32_t vibQ4 = breath_.VibratoCentsQ4();
		// Portamento's on/off toggle is gone: every held note glides on a
		// fingering change now, at whatever speed param A (glideShift_) sets
		// — see ocarina.h. The low end is near-instant, which is what "off"
		// used to mean.
		const bool gliding = (glideSemiQ8_ != targetSemiQ8_);

		// The glide has to keep stepping toward its target across many ticks,
		// so "nothing changed" is not a reason to stop — it is the normal state
		// DURING a glide. Early-returning on it left the pitch advancing only
		// when the vibrato happened to move, which is a slur that arrives in
		// jerks or, with vibrato off, never arrives at all.
		if (!pitchDirty_ && !gliding
		    && vibQ4 == vibLast_)
			return;

		pitchDirty_ = false;
		vibLast_ = vibQ4;

		const int degree = (combo_ < 0) ? 0 : combo_;

		// Coarse tune shifts the root, in BOTH directions.
		//
		// Clamping negative coarse away would leave the bore at the root while
		// PitchMillivolts() still applied the offset to CV Out 1 — the two
		// outputs would disagree by up to a full octave, and only when tuning
		// flat, which is the hardest kind of discrepancy to notice.
		int32_t root = BaseNote() + coarse_;

		// CV In 2 transposes, in semitones, in both directions.
		//
		// Quantised to whole semitones on purpose: an imprecise voltage should
		// move the instrument by a musical interval, not leave everything
		// slightly sharp. +/-2048 of CV maps to +/-24 semitones, two octaves
		// either way.
		if (Connected(Input::CV2)) root += (CVIn2() * 24) >> 11;

		const int32_t maxRoot = BaseNote() + kMaxRootForOctave[octave_];
		if (root > maxRoot)     root = maxRoot;
		if (root < kPitchLoNote) root = kPitchLoNote;

		int32_t semi = QuantizeNote(root, scale_, degree);

		if (semi > kPitchHiNote) semi = kPitchHiNote;

		// GLIDE IN PITCH SPACE, NOT DELAY SPACE.
		//
		// The obvious thing is to slew the delay length, and it is wrong twice
		// over. Delay is proportional to 1/frequency, so a linear slew through
		// it sweeps pitch non-linearly — fast at the top, crawling at the
		// bottom. And it forces the CV, which IS linear in pitch, to be
		// recovered from a ratio: a log, or an approximation that is 500 cents
		// out over the octave a glide routinely spans, or a runtime 64-bit
		// divide of exactly the kind that blew NIBBLE's sample budget.
		//
		// Sliding the SEMITONE instead (in Q8, so the steps are smooth) makes
		// the glide musically even, and both outputs fall out of the same
		// number with no conversion at all.
		const int32_t targetSemiQ8 = (semi << 8);
		targetSemiQ8_ = targetSemiQ8;

		if (glideSemiQ8_ != 0)
		{
			// slew_exact and NOT slew: the plain shift stalls short of its
			// target, which on a pitch is a permanent detune rather than a
			// harmless approximation. glideShift_ is param A — always glides
			// now, from near-instant at the fast end to over a second at the
			// slow end; there is no separate on/off any more.
			glideSemiQ8_ = slew_exact(glideSemiQ8_, targetSemiQ8, glideShift_);
		}
		else
		{
			glideSemiQ8_ = targetSemiQ8;
		}

		// Whole semitones pick the table entry; the Q8 remainder becomes cents
		// and rides along with the fine tune and vibrato.
		const int32_t glideSemi  = glideSemiQ8_ >> 8;
		// Q4 cents, matching the vibrato and the fine tune.
		const int32_t glideCentsQ4 = ((glideSemiQ8_ - (glideSemi << 8)) * 1600) >> 8;

		uint32_t inc = SemiToIncQ32(glideSemi);
		// fine_ is whole cents from the tuning knob; the other two are Q4.
		const int32_t centsQ4 = (fine_ << 4) + vibQ4 + glideCentsQ4;
		if (centsQ4) inc = ApplyFineCents(inc, centsQ4);
		voice_.SetIncQ32(inc);

		// The CV comes from the same two numbers, so voice and CV cannot
		// disagree during a slur.
		cvSemi_  = glideSemi;
		cvCentsQ4_ = centsQ4;
	}

	void __not_in_flash_func(UpdateCVs)()
	{
		// cvSemi_/cvCents_ are set by UpdatePitch() from the SAME two numbers
		// that positioned the bore, so voice and CV cannot disagree during a
		// slur — see the note on gliding in pitch space there.
		//
		// CVOutMillivolts reaches a flash-resident helper, so the cache below
		// exists to keep XIP reads out of the control path.
		//
		// It no longer hits most of the time, and that is expected rather than
		// a regression: with vibrato running the pitch genuinely changes every
		// tick, so the call has to happen — a pitch CV that only updated once
		// per note would simply not carry the vibrato, which is the whole
		// expression. Measured, the two calls are ~80 cycles of a 4000-cycle
		// budget and the XIP line stays hot at a 3kHz call rate.
		//
		// The cache still earns its place when vibrato is at zero, which is the
		// entire lower half of the knob.
		const int32_t mv = PitchMillivolts(cvSemi_, cvCentsQ4_);
		if (mv != cvPitchLast_)
		{
			cvPitchLast_ = mv;
			CVOutMillivolts(0, mv);
		}

		// level_, not BreathQ12(): the audible level includes X's tilt and the
		// chiff dip, and this output exists so the rest of the rack can follow
		// what is actually being heard. Deriving it from the raw breath curve
		// would make it disagree with the sound whenever X was off centre.
		const int32_t env = (level_ * 5000) >> 12;
		if (env != cvEnvLast_)
		{
			cvEnvLast_ = env;
			CVOutMillivolts(1, env);
		}
	}

	// -----------------------------------------------------------------------
	// Calibration
	// -----------------------------------------------------------------------

	void EnterLearn()
	{
		ui_          = UiMode::Learn;
		learnStep_   = 0;
		learnPhase_  = LearnPhase::Waiting;
		learnTimer_  = kLearnTimeoutTicks;
		phaseTimer_  = 0;
		collisions_  = 0;
		// Arm the tuning knobs so they pick up from where they already are
		// rather than jumping the drone to their absolute positions.
		EnterTune();
		// Entering on a hold means the release of that hold is still to come.
		// downFired_ is already true here, which swallows it — without that the
		// release would immediately read as the capture tap for step 0.
	}

	void AbortLearn()
	{
		learnPhase_ = LearnPhase::Aborted;
		phaseTimer_ = kAbortFlashTicks;
		// Deliberately stays in Learn until the flash finishes, so the feedback
		// is actually rendered rather than skipped.
	}

	void FinishLearn()
	{
		levels_.ResetHeld();
		ui_ = UiMode::Play;
		combo_ = kComboNone;
		pitchDirty_ = true;
		// The drone forced the fold to zero (see TuneOverlay). ControlTick sets
		// it from the X knob on the very next tick, but do it here too so the
		// restore does not depend on the order the two happen to run in — that
		// is precisely the kind of implicit sequencing that has bitten this
		// file before.
		voice_.SetFold(FoldFor(KnobVal(Knob::X)));
	}

	void __not_in_flash_func(LearnTick)()
	{
		// Keep the detector running: its settle state is what validates a tap.
		int8_t dummy = kComboNone;
		(void)levels_.Step(CVIn1(), dummy);

		// Tuning runs concurrently — different knobs, no conflict.
		TuneOverlay();

		if (phaseTimer_ > 0)
		{
			if (--phaseTimer_ == 0)
			{
				switch (learnPhase_)
				{
				case LearnPhase::Decided:
				case LearnPhase::Failed:
				case LearnPhase::Aborted:
					FinishLearn();
					return;
				default:
					learnPhase_ = LearnPhase::Waiting;
					break;
				}
			}
			return;
		}

		if (--learnTimer_ <= 0) { AbortLearn(); return; }
		if (!tapped_) return;

		// A tap that arrived mid-transition would capture a voltage nobody is
		// holding. Say so rather than silently recording a number from the
		// middle of a slew.
		if (!levels_.Settled())
		{
			learnPhase_ = LearnPhase::Collision;
			phaseTimer_ = kCaptureFlashTicks;
			return;
		}

		const int slot = kLearnOrder[learnStep_];
		captured_[slot] = levels_.SettledValue();

		// Warn DURING the walk, not just at the end. Fifteen captures take a
		// while, and being told at step 15 that step 6 collided wastes the lot.
		// A flash here means "this patch is not going to make 15" in time to
		// abort and try another Four Voltages output.
		bool collided = false;
		for (int j = 0; j < learnStep_; j++)
		{
			int32_t d = captured_[slot] - captured_[kLearnOrder[j]];
			if (d < 0) d = -d;
			if (d < kCollisionMin) collided = true;
		}
		if (collided) collisions_++;

		learnStep_++;

		if (learnStep_ >= kNumLevels)
		{
			if (levels_.Analyse(captured_) == LearnResult::Failed)
			{
				learnPhase_ = LearnPhase::Failed;
				phaseTimer_ = kFailFlashTicks;
			}
			else
			{
				learnPhase_ = LearnPhase::Decided;
				phaseTimer_ = kDecidedTicks;
			}
			return;
		}

		learnPhase_ = collided ? LearnPhase::Collision : LearnPhase::Confirm;
		phaseTimer_ = collided ? kCollisionFlashTicks : kCaptureFlashTicks;
	}

	// -----------------------------------------------------------------------
	// Tuning — live DURING calibration, not a mode of its own
	// -----------------------------------------------------------------------
	//
	// Calibration and tuning use disjoint controls, which is what makes this
	// work: calibration reads CV In 1 and the switch tap, tuning reads Y and
	// Main. Nothing is shared, so they can run at the same time and the player
	// tunes the drone while walking the fifteen combinations.
	//
	// v2.0 had TUNE as a separate mode on a 3-second switch-up hold. That
	// collided with legato — see ReadSwitch() — and the fix of moving it to
	// "after calibration" was still a phase you had to sit through. Running it
	// concurrently costs nothing and removes a mode.

	void EnterTune()
	{
		coarseKnob_.Enter(KnobVal(Knob::Y), coarse_);
		fineKnob_.Enter(KnobVal(Knob::Main), fine_);
		pitchDirty_ = true;
	}

	/// Track the tuning knobs and hold the drone at the scale root.
	/// Called from LearnTick() every control tick.
	void __not_in_flash_func(TuneOverlay)()
	{
		coarse_ = coarseKnob_.Update(KnobVal(Knob::Y), kCoarseDen,
		                             kCoarseLo, kCoarseHi);
		fine_   = fineKnob_.Update(KnobVal(Knob::Main), kFineDen,
		                           kFineLo, kFineHi);

		// X picks the OCTAVE while calibrating.
		//
		// Free to take, because X is the vibrato character in PLAY and the
		// vibrato is silent during a calibration anyway — and the drone gives
		// immediate feedback on which octave you have landed on, which is the
		// only way to choose one by ear.
		//
		// Taken as an ABSOLUTE position rather than through TuneKnob's pickup:
		// there are only four choices and they are an octave apart, so "the
		// knob points at the octave" is easier to use than "the knob nudges the
		// octave from wherever it happened to be".
		int o = (KnobVal(Knob::X) * kNumOctaves) >> 12;
		if (o < 0) o = 0;
		if (o >= kNumOctaves) o = kNumOctaves - 1;
		octave_ = o;

		// Drone the scale root, fingering ignored, so there is a stable
		// reference to tune against while the combos are being captured.
		//
		// Coarse applies in BOTH directions, exactly as in UpdatePitch():
		// clamping the negative half away would make the drone ignore flat
		// tuning while CV Out 1 still followed it, so the two would disagree
		// precisely while being used to tune something.
		int32_t root = BaseNote() + coarse_;
		const int32_t maxRoot = BaseNote() + kMaxRootForOctave[octave_];
		if (root > maxRoot)      root = maxRoot;
		if (root < kPitchLoNote) root = kPitchLoNote;

		const int32_t semi = QuantizeNote(root, scale_, 0);
		uint32_t inc = SemiToIncQ32(semi);
		if (fine_) inc = ApplyFineCents(inc, fine_ << 4);
		voice_.SetIncQ32(inc);

		// The drone is a PURE SINE, deliberately.
		//
		// Folding is X's other job in play, but here X is the octave select and
		// the reference note should be as easy to tune against as possible — a
		// fold would put harmonics on it that beat against whatever you are
		// tuning to.
		voice_.SetFold(0);

		// Keep the glide anchored, so the first note after calibration does not
		// slide from wherever the drone happened to be.
		glideSemiQ8_ = targetSemiQ8_ = (semi << 8);
		cvSemi_    = semi;
		cvCentsQ4_ = fine_ << 4;

		UpdateCVs();
	}

	// -----------------------------------------------------------------------
	// LEDs — 3kHz, staggered off the control tick
	// -----------------------------------------------------------------------

	void __not_in_flash_func(UiTick)()
	{
		uiTicks_++;

		switch (ui_)
		{
		case UiMode::Learn: LearnLeds(); return;
		case UiMode::Play:  PlayLeds();  return;
		}
	}

	void SetRow(uint8_t mask, uint16_t on, uint16_t off)
	{
		for (int i = 0; i < 4; i++)
			LedBrightness(i, (mask & (1u << i)) ? on : off);
	}

	void LearnLeds()
	{
		switch (learnPhase_)
		{
		case LearnPhase::Confirm:
			for (int i = 0; i < kNumLeds; i++) LedBrightness(i, kLedFull);
			return;

		case LearnPhase::Collision:
			SetRow(0, 0, 0);
			LedBrightness(4, ((phaseTimer_ >> 6) & 1) ? kLedFull : 0);
			LedBrightness(5, ((phaseTimer_ >> 6) & 1) ? kLedFull : 0);
			return;

		case LearnPhase::Decided:
		{
			// A clean fade on all six: calibration finished and installed.
			//
			// There is no mode announcement any more. The card used to walk
			// fifteen combinations and blink out whether all of them or only
			// ten had survived, which needed a whole LED vocabulary and a
			// four-bar gap meter. Ten is the only mode now, so "it worked" is
			// the entire message.
			const uint16_t b = static_cast<uint16_t>(
				(phaseTimer_ * kLedFull) / kDecidedTicks);
			for (int i = 0; i < kNumLeds; i++) LedBrightness(i, b);
			return;
		}

		case LearnPhase::Failed:
			// Nothing usable came in — almost always nothing patched into
			// CV In 1. Urgent and unmistakably different: the two COLUMNS
			// alternating, fast. Not the fade of a success, not the double
			// blink of a deliberate abort.
			for (int i = 0; i < kNumLeds; i++)
				LedOn(i, (((phaseTimer_ >> 4) & 1) != 0) == ((i & 1) == 0));
			return;

		case LearnPhase::Aborted:
			for (int i = 0; i < kNumLeds; i++)
				LedOn(i, ((phaseTimer_ >> 6) & 1) != 0);
			return;

		case LearnPhase::Waiting:
		default:
			break;
		}

		// Which combo to hold next. The 2x2 block mirrors the Four Voltages
		// buttons, so the target can be read straight off the panel.
		const uint8_t want = ComboLedMask(static_cast<int8_t>(kLearnOrder[learnStep_]));
		const bool blink = ((uiTicks_ >> 7) & 1) != 0;
		for (int i = 0; i < 4; i++)
		{
			if (want & (1u << i))   LedBrightness(i, blink ? kLedFull : kLedDim);
			else if (Visited(i))    LedBrightness(i, kLedGlow);
			else                    LedOff(i);
		}

		// Phase marker: LED 4 through the four singles, LED 5 through the six
		// pairs. Two phases, now that the triples are gone.
		const uint8_t pop = kComboPop[kLearnOrder[learnStep_]];
		LedBrightness(4, (pop == 1) ? kLedDim : 0);
		LedBrightness(5, (pop == 2) ? kLedDim : 0);
	}

	/// Has button `i` appeared in any combo captured so far?
	bool Visited(int i) const
	{
		for (int s = 0; s < learnStep_; s++)
			if (kComboMask[kLearnOrder[s]] & (1u << i)) return true;
		return false;
	}

	// There is no TuneLeds() any more.
	//
	// Tuning runs concurrently with calibration now, and the LEDs are already
	// fully committed: 0-3 show which combination to hold, 4/5 show the
	// popcount phase. Overlaying the tuning offsets on top would make both
	// unreadable.
	//
	// The offsets are audible instead, which is the right channel for them —
	// you are tuning by ear against a drone, not by watching a light. The one
	// thing lost is the "offset is exactly zero" indicator; if that turns out
	// to matter, the honest place for it is a brief flash on entering
	// calibration rather than a permanent light.

	void PlayLeds()
	{
		if (SwitchVal() == Switch::Up)
		{
			ParamLeds();
			return;
		}

		if (scaleShow_ > 0)
		{
			scaleShow_--;
			// Scale as a six-LED bar, brightest-last, so the Y knob reads as
			// one axis from dark to bright.
			const int lit = (scale_ * kNumLeds) / kNumScales + 1;
			for (int i = 0; i < kNumLeds; i++)
				LedBrightness(i, (i < lit) ? kLedDim : 0);
			return;
		}

		// The fingering, mirrored on the block.
		SetRow(ComboLedMask(levels_.Current()), kLedDim, 0);

		// LED 4 is free of the old portamento on/off indicator — portamento
		// has no on/off any more, only a session-set time (param A, switch
		// UP) — so it is simply dark here now.
		LedBrightness(4, 0);

		// LED 5 is a level meter, following the envelope, so the panel shows
		// the note's shape as it rises and decays.
		LedBrightness(5, static_cast<uint16_t>((level_ * kLedFull) >> 12));
	}

	/// Switch-UP display: LEDs 0-3 show which parameter is selected (solid at
	/// the button's own position, matching the fingering block everywhere
	/// else on this panel), LEDs 4/5 show its current value as a two-LED bar.
	/// Nothing selected yet (paramArmed_ false) leaves 0-3 dark, since
	/// showing a stale selection from before this entry would be a lie.
	void ParamLeds()
	{
		SetRow((paramArmed_ && paramSel_ != kComboNone)
		           ? ComboLedMask(paramSel_) : 0,
		       kLedDim, 0);

		if (!paramArmed_ || paramSel_ == kComboNone)
		{
			LedBrightness(4, 0);
			LedBrightness(5, 0);
			return;
		}

		// Value as a fraction of each parameter's own range, then as a
		// two-LED bar — one lit, both lit, matching the coarse resolution a
		// two-LED readout can actually offer.
		int32_t frac;   // Q12, 0..4095
		switch (paramSel_)
		{
		case kA:
			frac = ((glideShift_ - kGlideShiftMin) << 12) /
			       (kGlideShiftMax - kGlideShiftMin);
			break;
		case kB:
			frac = ((vibMulQ8_ - kVibMulQ8Min) << 12) /
			       (kVibMulQ8Max - kVibMulQ8Min);
			break;
		case kC:
			frac = ((foldMulQ8_ - kFoldMulQ8Min) << 12) /
			       (kFoldMulQ8Max - kFoldMulQ8Min);
			break;
		case kD:
		default:
			frac = 0;
			break;
		}
		LedBrightness(4, (frac > 1365) ? kLedDim : 0);    // > 1/3
		LedBrightness(5, (frac > 2730) ? kLedDim : 0);    // > 2/3
	}

	// -----------------------------------------------------------------------

	LevelTracker levels_;
	Flute        voice_;
	Breath       breath_;

	TuneKnob coarseKnob_, fineKnob_;

	int32_t bootPhase_ = 0;
	int32_t splash_    = 0;
	int32_t ctrlDiv_   = 0;
	uint32_t uiTicks_  = 0;

	UiMode     ui_         = UiMode::Play;
	LearnPhase learnPhase_ = LearnPhase::Waiting;
	int        learnStep_  = 0;
	int32_t    learnTimer_ = 0;
	int32_t    phaseTimer_ = 0;
	uint8_t    collisions_ = 0;
	int32_t    captured_[kNumLevels] = {};

	bool    upLast_      = false;
	bool    downLast_    = false;
	bool    tapped_      = false;
	bool    gateLatched_ = false;
	bool    gateLast_    = false;   ///< for the re-strike pickup in ControlTick()

	// --- session parameters, switch UP --------------------------------------
	uint8_t glideShift_ = kGlideShiftDefault;   ///< param A
	int32_t vibMulQ8_   = kVibMulQ8Default;     ///< param B
	int32_t foldMulQ8_  = kFoldMulQ8Default;    ///< param C
	// param D is reserved — no storage yet.

	/// Which of A/B/C/D is selected in the switch-UP mode, or kComboNone.
	int8_t paramSel_    = kComboNone;
	/// Entering UP mode must ignore whatever is already held — see
	/// ReadSwitch(). False until the NEXT settled change to a fresh single.
	bool    paramArmed_ = false;

	int32_t  level_      = 0;   ///< what the voice is given, after every offset
	int32_t  xNow_       = 0;   ///< X knob after its CV offset
	int8_t   combo_      = kComboNone;
	int      scale_      = 0;
	int32_t  glideSemiQ8_  = 0;   ///< where the glide is now, Q8 semitones
	int32_t  targetSemiQ8_ = 0;   ///< where it is heading
	int32_t  cvSemi_       = kBaseNote;
	int      octave_       = kDefaultOctave;
	int32_t  cvCentsQ4_    = 0;
	bool     pitchDirty_ = true;
	int32_t  vibLast_    = 0;

	int32_t coarse_ = 0;
	int32_t fine_   = 0;

	int32_t cvPitchLast_ = -99999;
	int32_t cvEnvLast_   = -99999;
	int32_t scaleShow_   = 0;
	int32_t pulse2Timer_ = 0;
};

int main()
{
	// 1.15V and 192MHz: the card needs the headroom for the bore, and the
	// order matters — raise the voltage, let it settle, then the clock.
	vreg_set_voltage(VREG_VOLTAGE_1_15);
	sleep_ms(2);
	set_sys_clock_khz(192000, true);

	static Ocarina card;
	card.Setup();
	// Without this, Connected() always reads false and the audio inputs are
	// zeroed every sample. CV In 2 as a breath input depends on it.
	card.EnableNormalisationProbe();
	card.Run();
}
