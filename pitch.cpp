// pitch.cpp — the delay-length table.
//
// Verified against exact arithmetic in tools/pitchsim.py. If you edit a value
// here by hand, run that first: a mistyped entry is one note that is wrong and
// nothing else, which is the hardest kind of bug to notice by ear.

#include "pitch.h"
#include "pico.h"   // __not_in_flash

namespace nib {

/// round(65536 * 48000 / (1.5 * f)) for MIDI notes 36..47 (C2..B2).
///
/// The 1.5 is not a fudge factor, it is the loop geometry — see kLoopFactorNum
/// in pitch.h. Deriving it wrong puts the whole instrument a fourth sharp,
/// uniformly, which is exactly what the first version of this table did.
///
/// __not_in_flash so the lookup in SemiToDelayQ16() cannot put an XIP read in
/// a control tick. 48 bytes of RAM is a trivial price for that guarantee.
const uint32_t __not_in_flash("delaytab") kDelayQ16Base[12] = {
	32063411u,   // C2   65.406Hz -> 489.25 samples
	30263830u,   // C#2  69.296Hz -> 461.79 samples
	28565252u,   // D2   73.416Hz -> 435.87 samples
	26962007u,   // D#2  77.782Hz -> 411.41 samples
	25448746u,   // E2   82.407Hz -> 388.32 samples
	24020418u,   // F2   87.307Hz -> 366.52 samples
	22672255u,   // F#2  92.499Hz -> 345.95 samples
	21399759u,   // G2   97.999Hz -> 326.53 samples
	20198683u,   // G#2 103.826Hz -> 308.21 samples
	19065018u,   // A2  110.000Hz -> 290.91 samples
	17994981u,   // A#2 116.541Hz -> 274.58 samples
	16985000u,   // B2  123.471Hz -> 259.17 samples
};

} // namespace nib
