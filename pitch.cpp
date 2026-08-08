// pitch.cpp — the delay-length table.
//
// Verified against exact arithmetic in tools/pitchsim.py. If you edit a value
// here by hand, run that first: a mistyped entry is one note that is wrong and
// nothing else, which is the hardest kind of bug to notice by ear.

#include "pitch.h"

namespace nib {

/// round(65536 * 48000 / f) for MIDI notes 36..47 (C2..B2).
///
/// __not_in_flash so the lookup in SemiToDelayQ16() cannot put an XIP read in
/// a control tick. 48 bytes of RAM is a trivial price for that guarantee.
const uint32_t __not_in_flash("delaytab") kDelayQ16Base[12] = {
	48095116u,   // C2    65.406Hz -> 733.87 samples
	45395745u,   // C#2   69.296Hz -> 692.68 samples
	42847877u,   // D2    73.416Hz -> 653.81 samples
	40443011u,   // D#2   77.782Hz -> 617.11 samples
	38173119u,   // E2    82.407Hz -> 582.48 samples
	36030627u,   // F2    87.307Hz -> 549.78 samples
	34008383u,   // F#2   92.499Hz -> 518.93 samples
	32099639u,   // G2    97.999Hz -> 489.80 samples
	30298025u,   // G#2  103.826Hz -> 462.31 samples
	28597527u,   // A2   110.000Hz -> 436.36 samples
	26992471u,   // A#2  116.541Hz -> 411.87 samples
	25477500u,   // B2   123.471Hz -> 388.76 samples
};

} // namespace nib
