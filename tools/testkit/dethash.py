"""Reading and comparing the guest-memory fingerprints of the deterministic mode (Python 3.8+, standard library only).

With NO100F_DETERMINISTIC=1 the game runs on a virtual clock (wiicompiled/runtime/include/det_clock.h), and with
NO100F_DET_HASH=<file>:<every> it appends one line every <every> retraces (det_hash.h):

    <retrace> <virtual ticks> <MEM1 hash> <MEM2 hash>

Two runs of the same build with the same input should write the same lines. Two builds that should behave the same
(a change behind an option that is off, a refactor) should too. `compare` finds the first line where they part.
"""
from pathlib import Path


class HashLine:
    __slots__ = ("retrace", "ticks", "mem1", "mem2")

    def __init__(self, retrace, ticks, mem1, mem2):
        self.retrace, self.ticks, self.mem1, self.mem2 = retrace, ticks, mem1, mem2

    def same(self, other) -> bool:
        return (self.retrace, self.ticks, self.mem1, self.mem2) == (other.retrace, other.ticks, other.mem1, other.mem2)

    def __repr__(self):
        return f"{self.retrace} {self.ticks} {self.mem1} {self.mem2}"


def parse(text):
    """The complete lines of a hash file, in order. A last line still being written (no newline yet) is left out."""
    lines = []
    complete = text if text.endswith("\n") else text[:text.rfind("\n") + 1]
    for raw in complete.splitlines():
        parts = raw.split()
        if len(parts) != 4:
            continue
        try:
            lines.append(HashLine(int(parts[0]), int(parts[1]), parts[2], parts[3]))
        except ValueError:
            continue
    return lines


def read(path):
    try:
        return parse(Path(path).read_text(encoding="ascii", errors="replace"))
    except OSError:
        return []


def last_retrace(path) -> int:
    lines = read(path)
    return lines[-1].retrace if lines else -1


class Comparison:
    """How two hash files compare over the retraces both reached."""

    def __init__(self, common, first_diff, a_len, b_len):
        self.common = common            # lines compared (both files have them)
        self.first_diff = first_diff    # (line of a, line of b) at the first difference, or None
        self.a_len, self.b_len = a_len, b_len

    @property
    def identical(self) -> bool:
        return self.first_diff is None and self.common > 0

    def describe(self) -> str:
        if self.common == 0:
            return "nothing to compare (no line in one of the files)"
        if self.first_diff is None:
            last = self.common
            return f"identical over {last} fingerprints"
        a, b = self.first_diff
        what = []
        if a.retrace != b.retrace:
            what.append(f"retrace {a.retrace} vs {b.retrace}")
        if a.ticks != b.ticks:
            what.append(f"virtual clock {a.ticks} vs {b.ticks}")
        if a.mem1 != b.mem1:
            what.append("MEM1")
        if a.mem2 != b.mem2:
            what.append("MEM2")
        return f"differ from retrace {a.retrace} ({', '.join(what)}); {self.common} fingerprints compared"


def compare(a_lines, b_lines) -> Comparison:
    n = min(len(a_lines), len(b_lines))
    for i in range(n):
        if not a_lines[i].same(b_lines[i]):
            return Comparison(n, (a_lines[i], b_lines[i]), len(a_lines), len(b_lines))
    return Comparison(n, None, len(a_lines), len(b_lines))
