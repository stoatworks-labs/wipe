"""Every parameter must actually change the picture.

A uniform name that does not match between the C++ and the GLSL is silently
ignored: glGetUniformLocation returns -1, glUniform on -1 is a documented no-op,
and nothing in the build says a word. A control can therefore be completely dead
while everything compiles, links, loads and renders. Nothing else in this repo
catches that.

So: render each parameter at two positions against the two input cards, and
report any that made no difference at all.

    python3 tools/sweep.py [--size WxH] [--jobs N] [--binary PATH]

Exit code 1 means something is dead.

------------------------------------------------------------------ the traps

**This is a MIXER, so every render needs two inputs.** The harness feeds them
itself -- `--input-a` is A, the layer below, and `--input-b` is B, this layer.

**At the fader's ends nothing else exists.** Opacity 0 is A and Opacity 1 is B
by construction, so every control is swept with Opacity at 0.5 (the default),
and Opacity itself -- the fader, which Resolume drives from the layer's opacity
-- is the one control swept end to end.

**A control only exists on the pattern that uses it.** Centre X/Y and Aspect
move the closed patterns, not the ramps; Multiple V is on the vertical wipe;
Aspect Comp makes a circle round; Law needs a pattern whose area is not linear
in the fader. Each is swept on a pattern that shows it.

**Rotation 0 and Rotation 1 are the same picture** -- 0 and 360 degrees -- so
it is swept to a quarter turn.

**Flip-Flop needs a transition.** It is swept with `--transitions 1`, so the
fader has arrived at 1 from 0 before the frame is rendered.

**Mod Speed needs time to pass, and not a whole cycle of it.** Swept twenty
frames in: at frame 30 a 4 Hz modulator has done exactly two cycles and reads
as dead against 0 Hz.

**Border Softness and Border Colour need a border.** Border Width is 0 by
default, and at 0 the second comparator is out of circuit.

**Every name must be unique.** `--set` finds a parameter by name and takes the
first match.

**A dropdown holds its element VALUE.** `wptest --list` prints an option's real
range for exactly this reason: the SDK's own range reads back 0..1.

**Never sweep the About block.** Those are buttons that open a web browser.
"""
import argparse
import concurrent.futures
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
BIN = str(ROOT / "build" / "wptest")
SCRATCH = tempfile.mkdtemp(prefix="wpsweep")

WIDTH, HEIGHT = 480, 270
FRAMES = 1

# Parameters that cannot or must not be swept, with the reason.
SKIP = {}

BORDER = {"Border Width": 0.3}

CONTEXT = {
    "Reverse": {"Pattern": 2},
    "Flip-Flop": {"Pattern": 2, "_transitions": 1},
    "Aspect Comp": {"Pattern": 4},
    "Law": {"Pattern": 2, "Opacity": 0.3},
    "Border Softness": BORDER,
    "Border Red": BORDER,
    "Border Green": BORDER,
    "Border Blue": BORDER,
    "Centre X": {"Pattern": 2},
    "Centre Y": {"Pattern": 2},
    "Rotation": {"_high": 0.25},
    "Aspect": {"Pattern": 2},
    "Mod Frequency": {"Mod Amount": 0.5},
    "Mod Speed": {"Mod Amount": 0.5, "_frames": 20},
    "Multiple V": {"Pattern": 1},
}


def parameters():
    """id, name, kind, low, high from the harness's own declaration."""
    out = subprocess.run([BIN, "--list"], capture_output=True, text=True)
    if out.returncode != 0:
        print("could not list parameters:", out.stdout, out.stderr)
        sys.exit(1)

    found = []
    for line in out.stdout.splitlines():
        m = re.match(
            r"\s*(\d+)\s+(.+?)\s{2,}(\S+)\s+([\d.eE+-]+)\s+\[\s*([\d.eE+-]+)\s*\.\.\s*([\d.eE+-]+)\s*\]",
            line,
        )
        if m:
            found.append(
                (int(m.group(1)), m.group(2).strip(), m.group(3),
                 float(m.group(5)), float(m.group(6)))
            )
        else:
            m = re.match(r"\s*(\d+)\s+(.+?)\s{2,}(about|text|buffer)\s", line)
            if m:
                found.append((int(m.group(1)), m.group(2).strip(), m.group(3), 0.0, 0.0))
    return found


def render(path, overrides, frames):
    args = [BIN, "--out", path, "--size", f"{WIDTH}x{HEIGHT}", "--frames", str(frames)]
    if overrides.get("_transitions"):
        args += ["--transitions", str(overrides["_transitions"])]
    merged = {k: v for k, v in overrides.items() if not k.startswith("_")}
    for name, value in merged.items():
        args += ["--set", f"{name}={value}"]
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        print("render failed:", " ".join(args), r.stdout, r.stderr)
        sys.exit(1)
    return pathlib.Path(path).read_bytes()


def pixels(png):
    """Raw RGBA out of the harness's own PNG (filter 0 rows), so nothing else
    is a dependency."""
    i = 8
    idat = b""
    width = height = 0
    while i < len(png):
        length = int.from_bytes(png[i:i + 4], "big")
        kind = png[i + 4:i + 8]
        data = png[i + 8:i + 8 + length]
        if kind == b"IHDR":
            width = int.from_bytes(data[0:4], "big")
            height = int.from_bytes(data[4:8], "big")
        elif kind == b"IDAT":
            idat += data
        i += 12 + length
    raw = zlib.decompress(idat)
    stride = width * 4
    out = bytearray()
    for row in range(height):
        out += raw[row * (stride + 1) + 1:(row + 1) * (stride + 1)]
    return out


def difference(a, b):
    pa, pb = pixels(a), pixels(b)
    if len(pa) != len(pb):
        return 1.0, len(pa)
    changed = sum(1 for x, y in zip(pa, pb) if x != y)
    return changed / max(len(pa), 1), changed


def sweep_one(job):
    pid, name, low, high, context = job
    frames = context.get("_frames", FRAMES)

    lo = dict(context)
    hi = dict(context)
    lo[name] = context.get("_low", low)
    hi[name] = context.get("_high", high)

    a = render(f"{SCRATCH}/{pid}_lo.png", lo, frames)
    b = render(f"{SCRATCH}/{pid}_hi.png", hi, frames)
    fraction, count = difference(a, b)
    # Progress as it happens, on stderr, so a run that is cut off by a CI
    # timeout still says how far it got.
    print(f"  swept {pid:3d} {name}", file=sys.stderr, flush=True)
    return pid, name, fraction, count


def main():
    global WIDTH, HEIGHT, BIN

    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--size", default="%dx%d" % (WIDTH, HEIGHT))
    ap.add_argument("--jobs", type=int, default=0)
    ap.add_argument("--binary", default=BIN)
    args = ap.parse_args()
    BIN = args.binary
    if "x" in args.size:
        WIDTH, HEIGHT = (int(v) for v in args.size.split("x", 1))
    jobs = args.jobs or min(8, os.cpu_count() or 1)

    if not pathlib.Path(BIN).exists():
        print(f"{BIN} is not built")
        return 1

    skipped = []
    work = []
    for pid, name, kind, low, high in parameters():
        if kind == "about":
            skipped.append((name, "a button that opens a web browser"))
            continue
        if kind in ("text", "buffer") or name in SKIP:
            skipped.append((name, SKIP.get(name, kind)))
            continue
        context = CONTEXT.get(name, {})
        work.append((pid, name, low, high, context))

    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for r in pool.map(sweep_one, work):
            results.append(r)

    dead = []
    for pid, name, fraction, count in sorted(results):
        if count == 0:
            dead.append(name)
            print(f"DEAD  {pid:4d}  {name}")
        else:
            print(f"ok    {pid:4d}  {name}  ({count} subpixels, {fraction * 100:.2f}%)")

    print()
    for name, why in skipped:
        print(f"skip  {name}: {why}")

    print(f"\n{len(results)} swept, {len(dead)} dead, {len(skipped)} skipped, {jobs} at a time")
    if dead:
        print("\nDEAD CONTROLS: " + ", ".join(dead))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
