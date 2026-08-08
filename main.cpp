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
//   CV In 2      breath CV, adds to the knob
//   Pulse In 1   tongue: re-articulates the current note
//
//   Main         breath          (FINE TUNE during calibration)
//   X            character: breathy -> pure
//   Y            scale           (COARSE TUNE during calibration)
//
//   Switch UP    legato: glide + vibrato — and nothing else, ever
//   Switch MID   tongued
//   Switch DOWN  mute / chiff stop        DOWN held 2s -> calibrate
//
// Calibration drones a reference note the whole time it runs, and Y/Main tune
// it while you teach the fingering — the two use different controls, so they
// cost each other nothing.
//
//   Audio Out 1  the flute
//   Audio Out 2  its breath noise alone
//   CV Out 1     1V/oct pitch, root at 0V
//   CV Out 2     breath envelope
//   Pulse Out 1  gate: high while sounding
//   Pulse Out 2  a blip per articulated note
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
constexpr int32_t kDroneBreath = 900;

/// The drone's own timbre. Plain and clean: mostly tone with a little air so
/// the pitch is easy to hear against an oscillator, a moderate filter so it is
/// not shrill, and low drive so it stays a reference rather than a voice.
constexpr int32_t kDroneAir   = 1400;
constexpr int32_t kDroneCut   = 5200;
constexpr int32_t kDroneDrive = 5000;

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
		voice_.Init(0xC0FFEEu);
		breath_.Init();
		ApplyTimbre();
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
		// During calibration the voice drones at a fixed breath so there is a
		// steady reference to tune against; in play it follows the knob.
		const int32_t air = (ui_ == UiMode::Learn) ? kDroneBreath
		                                          : breath_.BreathQ12();
		const int32_t v = voice_.Step(air);

		AudioOut1(clamp12(v >> 2));

		// Audio Out 2: the breath-noise component alone. Nearly free — the
		// noise is already computed for the jet — and genuinely useful patched:
		// a breath-controlled noise source that tracks the performance.
		AudioOut2(clamp12(voice_.LastNoise() >> 1));

		// --- pulse outs ----------------------------------------------------
		PulseOut1(air > 0);
		if (pulseTimer_ > 0)
		{
			pulseTimer_--;
			if (pulseTimer_ == 0) PulseOut2(false);
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

		// Breath: knob plus CV In 2 when something is patched there.
		const int32_t cvAdd = Connected(Input::CV2) ? CVIn2() : 0;
		breath_.SetKnob(KnobVal(Knob::Main), cvAdd);
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

		// ChiffFired() is an edge that Tick() CONSUMES, so it has to be read
		// first. Checking it after Tick() reads false every time — Pulse Out 2
		// would never fire and the chiff's extra noise would never be applied,
		// both silently. The models cannot catch this one: breathsim tests
		// Breath in isolation and gets the order right by construction; it is
		// the caller's sequencing that is wrong or right.
		const bool chiff = breath_.ChiffFired();

		breath_.Tick();

		if (chiff)
		{
			PulseOut2(true);
			pulseTimer_ = kSampleRate / 500;      // 2ms
		}

		ReadScale();
		ApplyTimbre();
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

	void NoteOn(int8_t combo)
	{
		combo_ = combo;
		breath_.NoteOn();
		pitchDirty_ = true;
	}

	void Retrigger()
	{
		breath_.NoteOn();
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

	/// Push the current breath and X knob into the voice.
	///
	/// Both knobs feed all four voice parameters, which is what makes them
	/// interact rather than sit in separate lanes: breath sets loudness AND
	/// brightness AND harmonic richness, while X decides how airy the whole
	/// range is. Soft playing at X fully CCW is nearly all breath; hard
	/// playing at X fully CW is a clear, strong tone.
	///
	/// Cheap enough to run every control tick — four multiplies — so unlike v1
	/// there is no "has the knob moved" gate to get wrong. That gate was how
	/// the chiff's extra noise went missing.
	void __not_in_flash_func(ApplyTimbre)()
	{
		// EFFORT, not level.
		//
		// This is the whole point of the two curves. Level reaches nearly full
		// by half the knob's travel, so a timbre driven by level would also
		// stop changing there and the top half of the sweep would be dead.
		// Effort keeps climbing to the stop, so past the point where it stops
		// getting louder the note keeps getting brighter and richer — which is
		// what "blowing harder" does on a real instrument.
		Timbre t = TimbreFor(breath_.EffortQ12(), KnobVal(Knob::X));
		// The chiff rides on top of the standing air amount.
		t.air += breath_.ChiffNoiseQ15() >> 3;
		if (t.air > 4096) t.air = 4096;
		voice_.SetTimbre(t.air, t.cut, t.res, t.drive);
	}

	/// combo -> degree -> semitone -> increment, and the CV to match.
	void __not_in_flash_func(UpdatePitch)()
	{
		const int32_t vib = breath_.VibratoCents();
		const bool gliding = (breath_.Art() == Articulation::Legato)
		                   && (glideSemiQ8_ != targetSemiQ8_);

		// The glide has to keep stepping toward its target across many ticks,
		// so "nothing changed" is not a reason to stop — it is the normal state
		// DURING a glide. Early-returning on it left the pitch advancing only
		// when the vibrato happened to move, which is a slur that arrives in
		// jerks or, with vibrato off, never arrives at all.
		if (!pitchDirty_ && !gliding
		    && breath_.Register() == regLast_ && vib == vibLast_)
			return;

		pitchDirty_ = false;
		regLast_ = breath_.Register();
		vibLast_ = vib;

		int degree = (combo_ < 0) ? 0 : combo_;
		// Degrees the bore cannot reach repeat the top note rather than going
		// silently out of tune — see kUsableDegrees in pitch.h.
		const int usable = kUsableDegrees[scale_];
		if (degree >= usable) degree = usable - 1;

		// Coarse tune shifts the root, in BOTH directions.
		//
		// Clamping negative coarse away would leave the bore at the root while
		// PitchMillivolts() still applied the offset to CV Out 1 — the two
		// outputs would disagree by up to a full octave, and only when tuning
		// flat, which is the hardest kind of discrepancy to notice.
		int32_t root = kBaseNote + coarse_;
		const int32_t maxRoot = kBaseNote + kMaxRootFor[scale_];
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
		const int32_t glideCents = ((glideSemiQ8_ - (glideSemi << 8)) * 100) >> 8;

		uint32_t inc = SemiToIncQ32(glideSemi);
		const int32_t cents = fine_ + vib + glideCents;
		if (cents) inc = ApplyFineCents(inc, cents);
		voice_.SetIncQ32(inc);

		// The CV comes from the same two numbers, so voice and CV cannot
		// disagree during a slur.
		cvSemi_  = glideSemi;
		cvCents_ = fine_ + vib + glideCents;
	}

	void __not_in_flash_func(UpdateCVs)()
	{
		// cvSemi_/cvCents_ are set by UpdatePitch() from the SAME two numbers
		// that positioned the bore, so voice and CV cannot disagree during a
		// slur — see the note on gliding in pitch space there.
		//
		// CVOutMillivolts is flash-resident, so calling it every tick would put
		// XIP reads in the control path. Only call it when the value changes,
		// which for a stepped instrument is once per note.
		const int32_t mv = PitchMillivolts(cvSemi_, cvCents_);
		if (mv != cvPitchLast_)
		{
			cvPitchLast_ = mv;
			CVOutMillivolts(0, mv);
		}

		const int32_t env = (breath_.BreathQ12() * 5000) >> 12;
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
			if (d < kCollisionMin15) collided = true;
		}
		if (collided) collisions_++;

		learnStep_++;

		if (learnStep_ >= kMaxLevels)
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

		// Drone the scale root, fingering ignored, so there is a stable
		// reference to tune against while the combos are being captured.
		//
		// Coarse applies in BOTH directions, exactly as in UpdatePitch():
		// clamping the negative half away would make the drone ignore flat
		// tuning while CV Out 1 still followed it, so the two would disagree
		// precisely while being used to tune something.
		int32_t root = kBaseNote + coarse_;
		const int32_t maxRoot = kBaseNote + kMaxRootFor[scale_];
		if (root > maxRoot)      root = maxRoot;
		if (root < kPitchLoNote) root = kPitchLoNote;

		const int32_t semi = QuantizeNote(root, scale_, 0);
		uint32_t inc = SemiToIncQ32(semi);
		if (fine_) inc = ApplyFineCents(inc, fine_);
		voice_.SetIncQ32(inc);

		// The drone sets its OWN timbre, and must.
		//
		// ApplyTimbre() derives everything from the breath knob, but during
		// calibration that knob is the FINE TUNE — so leaving the timbre to it
		// would make the reference note change character every time you nudged
		// the tuning, and start from whatever was last set in play.
		//
		// Deliberately plain and quiet: mostly tone with a little air so the
		// pitch is easy to hear against an oscillator, a moderate filter so it
		// is not shrill over fifteen taps, and low drive so it stays a clean
		// reference rather than a rich one.
		voice_.SetTimbre(kDroneAir, kDroneCut, kResMin, kDroneDrive);

		// Keep the glide anchored, so the first note after calibration does not
		// slide from wherever the drone happened to be.
		glideSemiQ8_ = targetSemiQ8_ = (semi << 8);
		cvSemi_  = semi;
		cvCents_ = fine_;

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

	/// The minGap bar: how close this Four Voltages output came to 15-mode,
	/// in quarters of the threshold.
	///
	/// This is the number worth writing down after a hardware session. A bare
	/// pass/fail cannot steer anything — it does not say whether the patch was
	/// five units short or eighty, so there is no way to tell "try the knob"
	/// from "try another output".
	void GapBar()
	{
		const int32_t g = levels_.MinGap15();
		const int q = (g <= 0) ? 0
		            : (g >= kGapNeeded15) ? 4
		            : (int)((g * 4) / kGapNeeded15) + ((g * 4) % kGapNeeded15 ? 1 : 0);
		for (int i = 0; i < 4; i++)
			LedBrightness(i, (i < q) ? kLedFull : 0);
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
			// Which mode won, and — when it did not win — how close it got.
			const bool wide = (levels_.Mode() == LevelMode::Wide15);
			if (wide)
			{
				const uint16_t b = static_cast<uint16_t>(
					(phaseTimer_ * kLedFull) / kDecidedTicks);
				SetRow(0xF, b, b);
				const bool blink = ((phaseTimer_ >> 7) & 1) != 0;
				LedBrightness(4, blink ? kLedFull : 0);
				LedBrightness(5, blink ? kLedFull : 0);
			}
			else
			{
				GapBar();
				LedBrightness(4, (phaseTimer_ > kDecidedTicks / 2) ? kLedDim : 0);
				LedBrightness(5, 0);
			}
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

		// Popcount phase marker: one LED for singles, the other for pairs, both
		// for triples, both blinking for the quad. "More light = more fingers",
		// the same organising idea as the block itself.
		const uint8_t pop = kComboPop[kLearnOrder[learnStep_]];
		const bool fast = ((uiTicks_ >> 5) & 1) != 0;
		LedBrightness(4, (pop == 1 || pop == 3) ? kLedDim
		                : (pop == 4 && fast)    ? kLedFull : 0);
		LedBrightness(5, (pop == 2 || pop == 3) ? kLedDim
		                : (pop == 4 && fast)    ? kLedFull : 0);
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

		// LED 4: breath. LED 5: which mode calibration chose, because that is
		// genuinely actionable — it says whether a third finger does anything.
		const int32_t air = breath_.BreathQ12();
		LedBrightness(4, static_cast<uint16_t>((air * kLedFull) >> 12));
		LedBrightness(5, (levels_.Mode() == LevelMode::Wide15) ? kLedGlow : 0);
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
	int32_t    captured_[kMaxLevels] = {};

	bool    tapped_    = false;
	bool    stopLast_  = false;
	int32_t downTicks_ = 0;
	bool    downFired_ = false;

	int8_t   combo_      = kComboNone;
	int      scale_      = 0;
	int32_t  glideSemiQ8_  = 0;   ///< where the glide is now, Q8 semitones
	int32_t  targetSemiQ8_ = 0;   ///< where it is heading
	int32_t  cvSemi_       = kBaseNote;
	int32_t  cvCents_      = 0;
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
