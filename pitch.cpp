// pitch.cpp — the oscillator increment table.
//
// Verified against exact arithmetic in tools/pitchsim.py. If you edit a value
// here by hand, run that first: a mistyped entry is one note that is wrong and
// nothing else, which is the hardest kind of bug to notice by ear.

#include "pitch.h"
#include "pico.h"   // __not_in_flash

namespace nib {

/// round(f * 2^32 / 48000) for MIDI notes 36..47 (C2..B2).
///
/// The reference octave is the LOWEST one, and the shift in SemiToIncQ32() goes
/// LEFT, so the largest value the card ever uses is this table's top entry
/// shifted up four octaves — about 1.4e8, comfortably inside uint32_t.
///
/// __not_in_flash so the lookup cannot put an XIP read in a control tick. 48
/// bytes of RAM is a trivial price for that guarantee.
const uint32_t __not_in_flash("inctab") kIncQ32Base[12] = {
	 5852465u,   // C2    65.406Hz
	 6200470u,   // C#2   69.296Hz
	 6569170u,   // D2    73.416Hz
	 6959793u,   // D#2   77.782Hz
	 7373644u,   // E2    82.407Hz
	 7812103u,   // F2    87.307Hz
	 8276635u,   // F#2   92.499Hz
	 8768789u,   // G2    97.999Hz
	 9290209u,   // G#2  103.826Hz
	 9842633u,   // A2   110.000Hz
	10427907u,   // A#2  116.541Hz
	11047982u,   // B2   123.471Hz
};

} // namespace nib
