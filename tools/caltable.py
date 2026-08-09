#!/usr/bin/env python3
"""caltable.py — regenerate the README's tables from the source.

Everything here is READ OUT OF THE HEADERS, never typed twice. A fingering
chart that disagrees with the firmware is worse than no chart, and the way that
happens is someone changing a constant and not the prose.

Run:  python tools/caltable.py            print the tables
      python tools/caltable.py --check    fail if README.md is out of date
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

NAMES = ["A", "B", "C", "D",
         "AB", "AC", "AD", "BC", "BD", "CD",
         "ABC", "ABD", "ACD", "BCD",
         "ABCD"]

SCALE_NAMES = ["Phrygian", "Hirajoshi", "Harmonic Minor", "Natural Minor",
               "Minor Pentatonic", "m7 Arpeggio", "Dorian", "Major Pentatonic",
               "Ionian (Major)", "Maj7 Arpeggio", "Whole Tone", "Chromatic"]

NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]

BEGIN = "<!-- BEGIN GENERATED -->"
END = "<!-- END GENERATED -->"


def read(fname):
    return open(os.path.join(ROOT, fname), encoding="utf-8").read()


def uint_array(fname, decl):
    """Parse a constexpr array's initialiser.

    Comments are stripped first — several of these arrays carry trailing notes
    containing numbers ("span 24"), which would otherwise be read as entries.
    Hex and decimal are both handled, since kComboMask is written in hex to
    make the bit patterns legible.
    """
    src = read(fname)
    body = src[src.index(decl):]
    body = body[body.index("{") + 1:body.index("};")]
    body = re.sub(r"//[^\n]*", "", body)
    return [int(x, 16) if x.lower().startswith("0x") else int(x)
            for x in re.findall(r"0[xX][0-9a-fA-F]+|\d+", body)]


def combo_array(fname, decl):
    """Parse an array written with the Combo enum's symbolic names.

    kLearnOrder is spelled `kA, kB, kAB, ...` rather than numerically, because
    the whole point of it is being readable as a sequence of fingerings. So the
    names have to be resolved rather than the digits scraped.
    """
    sym = {f"k{n}": i for i, n in enumerate(NAMES)}
    src = read(fname)
    body = src[src.index(decl):]
    body = body[body.index("{") + 1:body.index("};")]
    body = re.sub(r"//[^\n]*", "", body)
    return [sym[t] for t in re.findall(r"k[A-D]+\b", body) if t in sym]


def scales():
    src = read("scales.h")
    body = src[src.index("kScales[kNumScales]"):]
    body = body[body.index("{"):body.index("\n};")]
    out = []
    for m in re.finditer(r"\{\s*(\d+),\s*\{([0-9,\s]+)\}\s*\}", body):
        ln = int(m.group(1))
        steps = [int(x) for x in m.group(2).split(",") if x.strip()]
        out.append((ln, steps[:ln]))
    return out


def constant(fname, name):
    src = read(fname)
    m = re.search(rf"constexpr\s+\w+\s+{name}\s*=\s*(-?\d+)", src)
    return int(m.group(1))


def quantize(root, scale, degree):
    ln, steps = scale
    return max(0, min(127, root + (degree // ln) * 12 + steps[degree % ln]))


def note_name(midi):
    return f"{NOTE_NAMES[midi % 12]}{midi // 12 - 1}"


def holes(mask):
    """The 2x2 block as the player sees it: filled = covered."""
    top = ("●" if mask & 1 else "○") + ("●" if mask & 2 else "○")
    bot = ("●" if mask & 4 else "○") + ("●" if mask & 8 else "○")
    return f"{top} / {bot}"


def build():
    masks = uint_array("ocarina.h", "kComboMask[15]")
    order = combo_array("ocarina.h", "kLearnOrder[kNumLevels]")
    max_root = uint_array("pitch.h", "kMaxRootForOctave[4]")
    base = constant("ocarina.h", "kBaseNote")
    octaves = uint_array("ocarina.h", "kOctaveBase[kNumOctaves]")
    sc = scales()

    # A parser that quietly returns nothing writes an EMPTY table into the
    # README and --check happily confirms it matches. That is worse than
    # crashing, and it happened: kLearnOrder is spelled with enum names, so a
    # numeric scrape found zero entries and silently deleted the calibration
    # chart. Fail loudly instead.
    assert len(masks) == 15, f"kComboMask: got {len(masks)} entries"
    assert len(order) == 10, f"kLearnOrder: got {len(order)} entries"
    assert sorted(order) == list(range(10)), "kLearnOrder is not a permutation"
    assert len(max_root) == 4, f"kMaxRootForOctave: got {len(max_root)}"
    assert len(octaves) == 4, f"kOctaveBase: got {len(octaves)}"
    assert len(sc) == 12, f"kScales: got {len(sc)}"

    L = []
    L.append("### Fingering")
    L.append("")
    L.append("Ten combinations: four singles and six pairs. The LEDs mirror the")
    L.append("Four Voltages buttons, so the panel shows the fingering directly.")
    L.append("")
    L.append("| Degree | Buttons | Holes |")
    L.append("|-------:|---------|-------|")
    for i in range(10):
        L.append(f"| {i} | {NAMES[i]} | `{holes(masks[i])}` |")
    L.append("")
    L.append("Pressing three or four buttons does **nothing** \u2014 those voltages")
    L.append("land far from every learned level, so the card ignores them and holds")
    L.append("the note you were already playing rather than jumping somewhere wrong.")
    L.append("")

    L.append("### Calibration order")
    L.append("")
    L.append("Hold each combination and tap the switch. This is NIBBLE's own")
    L.append("order, which is the one part of the calibration with real hardware")
    L.append("history behind it.")
    L.append("")
    L.append("| Step | Hold | Holes |")
    L.append("|-----:|------|-------|")
    for step, combo in enumerate(order):
        L.append(f"| {step + 1} | {NAMES[combo]} | `{holes(masks[combo])}` |")
    L.append("")

    L.append("### Scales")
    L.append("")
    L.append("Ten combinations are ten DEGREES of the chosen scale, so the scale")
    L.append("also sets the range. Everything below is derived from `scales.h`")
    L.append("and `pitch.h` \u2014 see `tools/caltable.py`.")
    L.append("")
    L.append("| Y | Scale | Notes/oct | Range from C4 |")
    L.append("|--:|-------|----------:|---------------|")
    for i, sc_i in enumerate(sc):
        top = quantize(base, sc_i, 9)
        bot = quantize(base, sc_i, 0)
        L.append(f"| {i} | {SCALE_NAMES[i]} | {sc_i[0]} | "
                 f"{note_name(bot)}\u2013{note_name(top)} |")
    L.append("")

    L.append("### Octaves")
    L.append("")
    L.append("Chosen with the X knob during calibration.")
    L.append("")
    L.append("| X | Base | Transpose | Widest scale reaches |")
    L.append("|--:|------|----------:|----------------------|")
    for i, ob in enumerate(octaves):
        top = max(quantize(ob + max_root[i], s2, 9) for s2 in sc)
        star = "  *(default)*" if ob == base else ""
        L.append(f"| {i} | {note_name(ob)}{star} | +{max_root[i]} | "
                 f"{note_name(top)} |")
    L.append("")

    return "\n".join(L)


def main():
    # The hole diagrams are filled/hollow circles, and the Windows console
    # defaults to cp1252, which cannot encode them.
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")

    table = build()
    path = os.path.join(ROOT, "README.md")

    if "--check" in sys.argv:
        if not os.path.exists(path):
            print("README.md missing")
            return 1
        src = open(path, encoding="utf-8").read()
        if BEGIN not in src or END not in src:
            print("README.md has no generated block")
            return 1
        cur = src[src.index(BEGIN) + len(BEGIN):src.index(END)].strip()
        if cur != table.strip():
            print("README.md is OUT OF DATE — run: python tools/caltable.py --write")
            return 1
        print("README.md generated block is current")
        return 0

    if "--write" in sys.argv:
        src = open(path, encoding="utf-8").read()
        out = (src[:src.index(BEGIN) + len(BEGIN)] + "\n" + table + "\n"
               + src[src.index(END):])
        open(path, "w", encoding="utf-8", newline="\n").write(out)
        print("README.md updated")
        return 0

    print(table)
    return 0


if __name__ == "__main__":
    sys.exit(main())
