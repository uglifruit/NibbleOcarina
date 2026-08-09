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
//   Pulse In 1   tongue: re-articulates the current note
//
//   Main         level, then VIBRATO DEPTH   (FINE TUNE during calibration)
//   X            vibrato character + level tilt + fold  (OCTAVE during cal)
//   Y            scale                       (COARSE TUNE during calibration)
//
//   Switch UP    legato: glide — and nothing else, ever
//   Switch MID   tongued
//   Switch DOWN  mute                        DOWN held 2s -> calibrate
//
// Calibration drones a reference note the whole time it runs; Y and Main tune
// it and X picks its octave while you teach the fingering. Calibration reads
// only CV In 1 and the switch, so the three knobs are free for this.
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
				// Swallow the release of whatever the switch is already doing.
				downFired_ = (SwitchVal() == Switch::Down);
				splash_    = kSplashSamples;
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

		// Pulse Out 1 is a note-change TRIGGER, not a gate: a fixed-width blip
		// each time the fingering changes, so it can fire envelopes elsewhere
		// in time with the card's own note changes.
		if (pulseTimer_ > 0)
		{
			pulseTimer_--;
			if (pulseTimer_ == 0) PulseOut1(false);
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
		int8_t idx = kComboNone;
		if (levels_.Step(CVIn1(), idx) == LevelEvent::Trigger)
		{
			NoteOn(idx);
		}

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

		breath_.SetKnob(KnobVal(Knob::Main), mainCv);
		breath_.SetArticulation(SwitchVal() == Switch::Up ? Articulation::Legato
		                                                  : Articulation::Tongued);

		// The chiff stop damps the BORE as well as cutting the air. Zeroing the
		// breath alone leaves the resonator ringing for its full decay, which
		// is the one thing a stop exists to prevent — it would be a gate close
		// wearing a stop's name.
		const bool stopped = (SwitchVal() == Switch::Down);
		if (stopped != stopLast_)
		{
			stopLast_ = stopped;
			breath_.SetStopped(stopped);
			if (stopped) voice_.Mute(); else voice_.Unmute();
		}

		// Pulse In 1 re-articulates without changing the fingering.
		if (PulseIn1RisingEdge()) Retrigger();

		// Vibrato comes from BOTH knobs: Main sets how much, X sets what kind.
		// Set before Tick() so the oscillator advances with this tick's values.
		const Vibrato vib = VibratoFor(breath_.EffortQ12(), xNow_);
		breath_.SetVibrato(vib.rateQ8, vib.centsQ4);

		breath_.Tick();

		// Level: the breath curve, plus X's small tilt, minus the chiff dip.
		int32_t lvl = breath_.BreathQ12();
		if (lvl > 0)
		{
			lvl += XVolumeBoost(xNow_);
			if (lvl > 4095) lvl = 4095;
		}
		level_ = lvl;

		voice_.SetFold(FoldFor(xNow_));

		ReadScale();
		UpdatePitch();
		UpdateCVs();
	}

	// -----------------------------------------------------------------------
	// Gestures
	// -----------------------------------------------------------------------

	/// One switch, four meanings, and a staged hold on UP.
	///
	/// The tap fires on PRESS, not release. Release-firing is "correct" in the
	/// sense that a hold never also fires a tap, and it is unplayable: the
	/// event arrives when you let go, so every capture lands late. The cost is
	/// that beginning a hold also fires one tap, which the learn machine
	/// absorbs deliberately.
	void __not_in_flash_func(ReadSwitch)()
	{
		const Switch sw = SwitchVal();
		tapped_ = false;

		if (sw == Switch::Down)
		{
			if (downFired_) { downTicks_ = 0; return; }
			if (downTicks_ == 0) tapped_ = true;
			if (downTicks_ < kHoldCalTicks) downTicks_++;
			if (downTicks_ >= kHoldCalTicks && !downFired_)
			{
				downFired_ = true;
				// Live from ANYWHERE, including mid-announcement: hunting for a
				// 15-mode calibration means doing this repeatedly, and waiting
				// for a flash to finish first would make that a chore.
				if (ui_ == UiMode::Learn) AbortLearn(); else EnterLearn();
			}
			return;
		}

		downTicks_ = 0;
		downFired_ = false;

		// SWITCH UP IS LEGATO AND NOTHING ELSE. No timer, no stages, no hidden
		// gesture — holding it is how you play a slur, and a playing position
		// cannot also be a hold gesture.
		//
		// v2.0 put the gap bar on a 1s up-hold and TUNE on a 3s one. Both fire
		// while you are simply playing legato, so a slur lasting three seconds
		// dropped the card into tune mode — LEDs cycling, drone running, and no
		// way out, because tune exits on a TAP and a tap means switch DOWN,
		// which is the mute you are not holding. Reported from hardware as
		// "gliss mode seems to hang up".
		//
		// The general rule this cost us: never overload a switch position that
		// is ALSO a continuous playing mode. Momentary positions can carry
		// gestures; held ones cannot.
	}

	/// The scale root, as chosen by the X knob during calibration.
	int32_t BaseNote() const { return kOctaveBase[octave_]; }

	void NoteOn(int8_t combo)
	{
		combo_ = combo;
		breath_.NoteOn();
		pitchDirty_ = true;

		// Pulse Out 1: a trigger on every note change.
		PulseOut1(true);
		pulseTimer_ = kSampleRate / 500;      // 2ms
	}

	void Retrigger()
	{
		breath_.NoteOn();
		PulseOut1(true);
		pulseTimer_ = kSampleRate / 500;
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
		const bool gliding = (breath_.Art() == Articulation::Legato)
		                   && (glideSemiQ8_ != targetSemiQ8_);

		// The glide has to keep stepping toward its target across many ticks,
		// so "nothing changed" is not a reason to stop — it is the normal state
		// DURING a glide. Early-returning on it left the pitch advancing only
		// when the vibrato happened to move, which is a slur that arrives in
		// jerks or, with vibrato off, never arrives at all.
		if (!pitchDirty_ && !gliding
		    && breath_.Register() == regLast_ && vibQ4 == vibLast_)
			return;

		pitchDirty_ = false;
		regLast_ = breath_.Register();
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

		// The explicit register. Overblowing does not emerge from the bore's
		// physics (see flute.h), so hard blowing adds an octave here.
		if (regLast_) semi += 12;
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

		if (breath_.Art() == Articulation::Legato && glideSemiQ8_ != 0)
		{
			// slew_exact and NOT slew: the plain shift stalls short of its
			// target, which on a pitch is a permanent detune rather than a
			// harmless approximation.
			glideSemiQ8_ = slew_exact(glideSemiQ8_, targetSemiQ8, kGlideShift);
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

		// LED 4 is a level meter.
		const int32_t air = breath_.BreathQ12();
		LedBrightness(4, static_cast<uint16_t>((air * kLedFull) >> 12));

		// LED 5 used to report which combo mode was live. There is only one
		// now, so it carries the calibration warning instead: dimly lit if two
		// learned levels came out too close to tell apart reliably.
		LedBrightness(5, levels_.CollisionCount() ? kLedGlow : 0);
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

	bool    tapped_    = false;
	bool    stopLast_  = false;
	int32_t downTicks_ = 0;
	bool    downFired_ = false;

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
	int      regLast_    = 0;
	int32_t  vibLast_    = 0;

	int32_t coarse_ = 0;
	int32_t fine_   = 0;

	int32_t cvPitchLast_ = -99999;
	int32_t cvEnvLast_   = -99999;
	int32_t scaleShow_   = 0;
	int32_t pulseTimer_  = 0;
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
