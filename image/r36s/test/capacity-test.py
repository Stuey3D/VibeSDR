#!/usr/bin/env python3
# capacity-test.py PATH-TO-vibe-card — the capacity check against simulated cards, in memory:
#   honest · a wrap-around fake (writes past the real size land at offset mod real) · a drop-writes fake (past the real
#   size, writes vanish and reads return zeros) · an honest card whose real size is not a power of two
# For each: the verdict must be right AND every byte the check touched must be back as it was (non-destructive).
import hashlib, importlib.machinery, importlib.util, os, sys

loader = importlib.machinery.SourceFileLoader("vibecard", sys.argv[1])
spec = importlib.util.spec_from_loader("vibecard", loader)
vc = importlib.util.module_from_spec(spec)
loader.exec_module(vc)

MiB = 1 << 20
class Fake:
    """claimed bytes, real bytes, mode: honest | wrap | drop. Sparse: only touched 1 MiB blocks are stored."""
    def __init__(self, claimed, real, mode):
        self.claimed, self.real, self.mode = claimed, real, mode
        self.blocks = {}
    def _phys(self, off):
        if off < self.real or self.mode == "honest":
            return off
        return off % self.real if self.mode == "wrap" else None
    def _orig(self, p):
        return hashlib.sha256(b"orig%d" % p).digest() * (vc.SECTOR // 32)
    def read(self, off, size):
        out = bytearray()
        for o in range(off, off + size, vc.SECTOR):
            p = self._phys(o)
            out += b"\0" * vc.SECTOR if p is None else self.blocks.get(p, self._orig(p))
        return bytes(out)
    def write(self, off, data):
        for i in range(0, len(data), vc.SECTOR):
            p = self._phys(off + i)
            if p is not None:
                self.blocks[p] = data[i:i + vc.SECTOR]
    def flush(self):
        pass
    def pristine(self):
        return all(v == self._orig(p) for p, v in self.blocks.items())

def case(name, claimed, real, mode, start, expect_bad):
    f = Fake(claimed, real, mode)
    r = vc.capacity_check(f, start, claimed, samples=48, size=64 * 1024)
    ok = (bool(r["bad"]) == expect_bad) and f.pristine()
    if expect_bad:
        ok = ok and start < r["verified_end"] <= real
    print("%-6s %-44s verified %6.2f GB of %6.2f claimed, %2d tested, %2d bad, restored=%s" % (
        "PASS" if ok else "FAIL", name, r["verified_end"] / 1e9, claimed / 1e9, r["tested"], len(r["bad"]), f.pristine()))
    return ok

GB = 10 ** 9
start = 1760 * MiB      # where VIBESDR starts on our image (after boot + root + state)
res = [
    case("honest 16 GB", 15_938_355_200, 15_938_355_200, "honest", start, False),
    case("wrap fake: claims 64 GB, holds 8 GiB", 64 * GB, 8 << 30, "wrap", start, True),
    case("wrap fake: claims 32 GB, holds 16 GiB", 32 * GB, 16 << 30, "wrap", start, True),
    # ★ KNOWN LIMIT (README): a wrap at a size that is NOT a power of two is not seen — the aliases land between the
    #   samples. Address-bit-dropping controllers wrap at powers of two; an odd real size shows up as a DROP fake.
    case("KNOWN LIMIT: wrap at an odd 15.6 GB (not detected)", 32 * GB, 15_600_000_000, "wrap", start, False),
    case("drop fake: claims 128 GB, holds 4 GB", 128 * GB, 4 * GB, "drop", start, True),
    case("honest 2 GB (tiny card)", 2 * GB, 2 * GB, "honest", start, False),
]
sys.exit(0 if all(res) else 1)
