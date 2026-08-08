// levels.cpp — the level detector and the 15/10 mode decision.
//
// A line-by-line counterpart of tools/levelsim.py. Keep them in step.
//
// Step() runs at the 3kHz control rate, inline inside ProcessSample() — which
// is a DMA interrupt handler. Integer only, no division, no allocation, no
// blocking. Analyse() runs once at the end of a calibration and is allowed to
// be leisurely by comparison, so it favours clarity.

#include "levels.h"
#include "fastmath.h"

// For __not_in_flash_func. Included explicitly rather than relying on
// ComputerCard.h pulling it in transitively — this file does not need the rest
// of that header, and an implicit dependency on a macro is how a build breaks
// mysteriously later. (pico/platform.h refuses to be included directly.)
#include "pico.h"

namespace nib {

namespace {

/// abs() for int32_t without dragging in <cstdlib>.
inline int32_t iabs(int32_t v) { return v < 0 ? -v : v; }

/// Insertion sort of a small int32_t array. Not qsort: that is flash-resident
/// on this platform and absurd for fifteen elements.
void InsertionSort(int32_t *a, int n)
{
	for (int i = 1; i < n; i++)
	{
		const int32_t key = a[i];
		int j = i - 1;
		while (j >= 0 && a[j] > key) { a[j + 1] = a[j]; j--; }
		a[j + 1] = key;
	}
}

} // namespace

// ---------------------------------------------------------------------------
// Table construction — runs on calibration completion and at boot, never in
// the hot path, so clarity beats cleverness here.
// ---------------------------------------------------------------------------

void LevelTracker::Rebuild()
{
	const int n = activeCount_;

	for (int i = 0; i < n; i++) sorted_[i] = static_cast<uint8_t>(i);

	for (int i = 1; i < n; i++)
	{
		const uint8_t key = sorted_[i];
		int j = i - 1;
		while (j >= 0 && level_[sorted_[j]] > level_[key])
		{
			sorted_[j + 1] = sorted_[j];
			j--;
		}
		sorted_[j + 1] = key;
	}

	// A decision threshold sits midway between each adjacent pair of levels.
	for (int k = 0; k < n - 1; k++)
		thresh_[k] = (level_[sorted_[k]] + level_[sorted_[k + 1]]) >> 1;

	// Match() needs to ask "which slot is the combo I am currently on?", which
	// is the inverse permutation.
	for (int k = 0; k < n; k++)
		slotOf_[sorted_[k]] = static_cast<uint8_t>(k);
}

void LevelTracker::SetTolerances()
{
	if (activeCount_ == kMaxLevels)
	{
		settleTol_   = kSettleTol15;
		deadband_    = kDeadband15;
		matchWindow_ = kMatchWindow15;
	}
	else
	{
		settleTol_   = kSettleTol10;
		deadband_    = kDeadband10;
		matchWindow_ = kMatchWindow10;
	}
}

void LevelTracker::InitDefault()
{
	// An even spread over the ten, so an uncalibrated card still does something
	// musical rather than nothing at all. Ten and not fifteen: an uncalibrated
	// card should behave like the conservative case, and the LED indicator says
	// it is guessing either way.
	activeCount_ = kLevels10;
	mode_        = LevelMode::Safe10;

	for (int i = 0; i < kLevels10; i++)
		level_[i] = kDefaultLo + (kDefaultHi - kDefaultLo) * i / (kLevels10 - 1);

	learned_    = false;
	collisions_ = 0;
	minGap_     = 0;
	minGap15_   = 0;

	SetTolerances();
	Rebuild();
}

uint8_t LevelTracker::CountCollisions(int n, int32_t floor) const
{
	uint8_t c = 0;
	for (int i = 0; i < n; i++)
		for (int j = 0; j < i; j++)
			if (iabs(level_[i] - level_[j]) < floor && c < 255)
				c++;
	return c;
}

// ---------------------------------------------------------------------------
// The mode decision
// ---------------------------------------------------------------------------

LearnResult LevelTracker::Analyse(const int32_t *cap15)
{
	// --- 1. Span check FIRST ---------------------------------------------
	//
	// Nothing patched into CV In 1 gives fifteen near-identical readings. That
	// is a FAILURE, not "fall back to ten" — fifteen identical numbers do not
	// become ten good ones. Refusing keeps whatever calibration was there
	// before, which is strictly better than installing a card that looks
	// calibrated and plays one note forever.
	int32_t s[kMaxLevels];
	for (int i = 0; i < kMaxLevels; i++) s[i] = cap15[i];
	InsertionSort(s, kMaxLevels);

	if (s[kMaxLevels - 1] - s[0] < kMinLearnSpan) return LearnResult::Failed;

	// --- 2. How tight is the tightest gap, across all fifteen? -----------
	int32_t minGap15 = s[1] - s[0];
	for (int i = 2; i < kMaxLevels; i++)
	{
		const int32_t g = s[i] - s[i - 1];
		if (g < minGap15) minGap15 = g;
	}

	// --- 3. And across only the ten of the fallback set? ------------------
	//
	// Combo indices 0..9 ARE the 10-mode set, by construction of the enum in
	// ocarina.h, so this needs no mapping table.
	int32_t t[kLevels10];
	for (int i = 0; i < kLevels10; i++) t[i] = cap15[i];
	InsertionSort(t, kLevels10);

	int32_t minGap10 = t[1] - t[0];
	for (int i = 2; i < kLevels10; i++)
	{
		const int32_t g = t[i] - t[i - 1];
		if (g < minGap10) minGap10 = g;
	}

	// --- 4. Install and decide -------------------------------------------
	for (int i = 0; i < kMaxLevels; i++) level_[i] = cap15[i];
	minGap15_ = minGap15;

	int32_t floor;
	if (minGap15 >= kGapNeeded15)
	{
		mode_        = LevelMode::Wide15;
		activeCount_ = kMaxLevels;
		minGap_      = minGap15;
		floor        = kCollisionMin15;
	}
	else
	{
		mode_        = LevelMode::Safe10;
		activeCount_ = kLevels10;
		minGap_      = minGap10;
		floor        = kCollisionMin10;
	}

	// Collisions are counted for the CHOSEN set only: warning about a 15-mode
	// collision after falling back to ten is noise the player cannot act on.
	collisions_ = CountCollisions(activeCount_, floor);
	learned_    = true;

	SetTolerances();
	Rebuild();
	return LearnResult::Ok;
}

void LevelTracker::ResetHeld()
{
	current_ = kComboNone;
	// The settle plateau is left running: the CV has not moved, so re-deriving
	// it from scratch would only add a spurious 12ms of latency to the first
	// press after a calibration.
}

// ---------------------------------------------------------------------------
// Matching
// ---------------------------------------------------------------------------

int8_t LevelTracker::Match(int32_t v, int8_t cur) const
{
	const int n = activeCount_;

	// Walk the sorted thresholds. Fifteen entries at most, so a linear walk
	// beats a binary search on an M0+ once branch cost is counted.
	int k = 0;
	while (k < n - 1 && v > thresh_[k]) k++;
	int8_t cand = static_cast<int8_t>(sorted_[k]);

	// Schmitt hysteresis, biased toward the level we are already on.
	//
	// Applied to ADJACENT slots only: a jump of two or more slots is
	// unambiguous, and making it wait would add latency for nothing. The
	// `k >= 1` on the second branch is defensive only and deliberately so —
	// slotOf_ always holds 0..n-1, so reaching it with k == 0 would require
	// curSlot == -1, which cannot happen. It costs nothing and it stops a
	// future edit to the sort from turning a logic slip into an out-of-bounds
	// read of thresh_[-1]. Verified unreachable in tools/levelsim.py.
	if (cur >= 0 && cur < n && cand != cur)
	{
		const int curSlot = slotOf_[cur];
		if (curSlot == k + 1 && v > thresh_[k] - deadband_)
			cand = cur;
		else if (curSlot == k - 1 && k >= 1 && v < thresh_[k - 1] + deadband_)
			cand = cur;
	}

	// The value fell in this slot, but is it actually NEAR the level that owns
	// the slot? In 10-mode a triple or the all-four combo lands somewhere in
	// range but far from any learned centre, and rejecting on distance is what
	// makes them safely ignorable rather than silently snapping to a neighbour.
	if (iabs(v - level_[cand]) > matchWindow_) return kComboNone;
	return cand;
}

// ---------------------------------------------------------------------------
// The detect step
// ---------------------------------------------------------------------------

LevelEvent __not_in_flash_func(LevelTracker::Step)(int32_t cvIn, int8_t &idx)
{
	// 1. Smooth the raw input.
	//
	// slew_exact, NOT slew: the plain shift stalls short of its target and does
	// so asymmetrically, which makes a settled reading depend on the direction
	// it was approached from. That cost 17 units of error in NIBBLE and broke
	// its learn round-trip. See fastmath.h.
	smooth_ = slew_exact(smooth_, cvIn, kCvSmoothShift);

	// 2. Settle detector.
	//
	// The mean is RESTARTED on every excursion rather than a counter merely
	// being reset, so a slow drift can never accumulate into a false settle:
	// each tick outside tolerance begins a fresh plateau.
	if (candTicks_ > 0 && iabs(smooth_ - candMean_) <= settleTol_)
	{
		candMean_ = slew_exact(candMean_, smooth_, 4);
		if (candTicks_ < kSettleTicks) candTicks_++;
	}
	else
	{
		candMean_  = smooth_;
		candTicks_ = 1;
		return LevelEvent::None;          // still moving
	}

	if (candTicks_ < kSettleTicks) return LevelEvent::None;

	// 3. Match the settled plateau to a learned level.
	const int8_t m = Match(candMean_, current_);
	if (m == kComboNone) return LevelEvent::None;   // unrecognised: stay latched
	if (m == current_)   return LevelEvent::None;   // nothing changed

	// 4. Every settled change is a note.
	//
	// NO GHOST RULE — see the header. A release is not an artefact here: coming
	// off AB onto A means A should sound. That is what makes trilling work, and
	// it is why this is four lines rather than NIBBLE's twenty.
	current_ = m;
	idx      = m;

	// Swallow the very first settle after power-on: the Four Voltages output is
	// already sitting at whatever was last pressed before the card was switched
	// off, and firing a note for that is startling.
	if (!primed_)
	{
		primed_ = true;
		return LevelEvent::None;
	}

	return LevelEvent::Trigger;
}

} // namespace nib
