// levels.h — turning one Four Voltages output into a stream of note events.
//
// This is the heart of the card, and the one piece of logic that cannot be
// re-derived by reading the rest of the code. It is modelled and tested in
// tools/levelsim.py, which is a line-by-line port of levels.cpp. IF YOU CHANGE
// THIS, CHANGE THAT — or delete it rather than let it drift.
//
// ---------------------------------------------------------------------------
// THE PROBLEM, AND WHY IT IS SMALLER HERE THAN IN NIBBLE
// ---------------------------------------------------------------------------
//
// Four Voltages does not return to a rest voltage when you let go: the output
// sits at the last-pressed combination's level. Release AB and the CV falls to
// A's voltage, which is exactly what pressing A looks like.
//
// NIBBLE could not tolerate that, because its only way to be silent WAS the
// fingering — so it suppressed those releases with a "ghost rule".
//
// OCARINA gets silence from the breath knob instead, and that changes the
// answer completely: a release is not an artefact to suppress, it is a NOTE.
// Lift a finger from AB and A should sound, because that is what a wind
// instrument does. Hold one hole and waggle another and you get an alternation
// — which is a trill, and it is the gesture this instrument is built around.
//
// So there is NO GHOST RULE HERE. Every settled change fires, in both
// directions. The rule NIBBLE needed is a bug in this card.
//
// The trade, stated plainly: NIBBLE's hold-and-tap "bank select" gesture is
// gone, because it worked only by virtue of releases being silent. Hold C, tap
// A, release, and you now hear C -> AC -> C. That is the right way round for an
// instrument you blow into.
//
// ---------------------------------------------------------------------------
// WHAT REMAINS, AND WHY EACH PIECE IS LOAD-BEARING
// ---------------------------------------------------------------------------
//
//   Smoothing      one-pole on the raw CV, to kill ADC dither.
//   Settle         a plateau must hold still for kSettleTicks before it counts.
//                  This is the debounce, and it also sets the fastest trill.
//   Schmitt        hysteresis biased toward the level already held, so two
//                  close neighbours cannot flicker.
//   Match window   a settled value far from EVERY learned centre is rejected
//                  and the current note stays latched. In 10-mode this is what
//                  makes triples and the quad safely ignorable.
//   primed_        swallow exactly one settle at power-on, because the module
//                  is already sitting at whatever was last pressed.

#pragma once
#include <stdint.h>
#include "ocarina.h"

namespace nib {

enum class LevelEvent : uint8_t {
	None,      ///< nothing to do, or unrecognised — keep the note latched
	Trigger,   ///< a genuine change of fingering: play `idx`
};

/// What a completed calibration decided.
enum class LearnResult : uint8_t {
	Ok,        ///< installed; check Mode() for which set is active
	Failed,    ///< degenerate capture, nothing installed, old calibration kept
};

/// Which set of combinations is live.
enum class LevelMode : uint8_t {
	Safe10,    ///< four singles + six pairs — NIBBLE's proven set
	Wide15,    ///< all fifteen
};

class LevelTracker
{
public:
	/// Install the even-spread default. Safe to call before any learn.
	void InitDefault();

	/// Measure a completed 15-capture set, choose the mode, and install it.
	///
	/// This is where the card's central question gets answered by the hardware
	/// rather than by us: if the tightest adjacent gap across all fifteen
	/// clears kGapNeeded15, all fifteen become playable; otherwise it falls
	/// back to the ten and MinGap15() reports how close it got, so the player
	/// can try another Four Voltages output and see whether it improved.
	LearnResult Analyse(const int32_t *cap15);

	/// Forget which combo is held, without touching the learned table.
	///
	/// Needed on the way out of calibration: the detector keeps running during
	/// a learn (its settle state is what validates each capture tap), so by the
	/// end current_ names whichever combo was calibrated last. Carrying that
	/// into play means the first real press is compared against stale state,
	/// and if it happens to match it is silently swallowed as "no change".
	///
	/// Deliberately leaves primed_ alone: the boot blip has long since been
	/// swallowed, and re-arming it would eat the player's first note after a
	/// calibration — exactly when they are listening to hear whether it worked.
	void ResetHeld();

	/// One control tick. `idx` receives the combo index on a non-None result.
	LevelEvent Step(int32_t cvIn, int8_t &idx);

	/// The combo currently sounding. With no ghost rule there is only one
	/// notion of "current", so LEDs and pitch read the same field.
	int8_t Current() const { return current_; }

	/// True once the CV has held still long enough to be trusted. The learn
	/// machine uses this to reject a capture tap that arrived mid-transition.
	bool Settled() const { return candTicks_ >= kSettleTicks; }

	/// The settled value, valid when Settled(). This is what a capture takes.
	int32_t SettledValue() const { return candMean_; }

	bool      Learned() const        { return learned_; }
	LevelMode Mode() const           { return mode_; }
	int       ActiveCount() const    { return activeCount_; }
	uint8_t   CollisionCount() const { return collisions_; }

	/// The tightest adjacent gap in the ACTIVE set.
	int32_t MinGap() const { return minGap_; }

	/// The tightest adjacent gap across all FIFTEEN, whichever mode won.
	///
	/// This is the number the player is shown as a 4-bit bar after a fallback,
	/// and it is the one worth writing down after a hardware session: it says
	/// whether this Four Voltages output was nearly good enough or nowhere
	/// near, which is what decides whether trying another one is worthwhile.
	int32_t MinGap15() const { return minGap15_; }

private:
	int8_t Match(int32_t v, int8_t cur) const;
	void   Rebuild();
	void   SetTolerances();
	uint8_t CountCollisions(int n, int32_t floor) const;

	// --- learned data. RAM ONLY, never flash -----------------------------
	// The Four Voltages knob moves every one of these, so a saved calibration
	// would silently restore a WRONG one on the next power-up: a card that
	// looks calibrated and plays the wrong notes. Not persisting is a
	// deliberate design decision, not an omission. Nothing in this card is
	// written to flash at all.
	int32_t level_[kMaxLevels]      = {};
	uint8_t sorted_[kMaxLevels]     = {};   // combo indices, ascending by level_
	uint8_t slotOf_[kMaxLevels]     = {};   // inverse of sorted_, for hysteresis
	int32_t thresh_[kMaxLevels - 1] = {};   // midpoints between adjacent levels

	bool      learned_    = false;
	uint8_t   collisions_ = 0;
	LevelMode mode_       = LevelMode::Safe10;
	int       activeCount_ = kLevels10;
	int32_t   minGap_     = 0;
	int32_t   minGap15_   = 0;

	// --- active tolerances, swapped by the mode decision -----------------
	// Held as members rather than read as constants so that Match() and Step()
	// are mode-agnostic: there is exactly one detector, and 10-mode is it with
	// activeCount_ = 10. Two implementations would drift.
	int32_t settleTol_   = kSettleTol10;
	int32_t deadband_    = kDeadband10;
	int32_t matchWindow_ = kMatchWindow10;

	// --- running detection -----------------------------------------------
	int32_t smooth_    = 0;
	int32_t candMean_  = 0;
	int32_t candTicks_ = 0;
	int8_t  current_   = kComboNone;

	/// At power-on the Four Voltages output already sits at whatever was last
	/// pressed before power-off — it survives the power cycle. Swallow exactly
	/// one settle so the card does not fire a note nobody asked for.
	bool primed_ = false;
};

} // namespace nib
