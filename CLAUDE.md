# wipe

A vision mixer's analogue pattern generator as an FFGL **mixer** for Resolume
Arena/Avenue. C++/GLSL, CMake MODULE → universal `.bundle` (macOS) + Windows
`.dll`. MIT. Not yet public, not yet released, never itself loaded into
Resolume (genlock, the first mixer, has been).

Read `AGENTS.md` before changing the waveforms, the level laws, or any
tolerance in the harness. Read `~/dev/genlock/AGENTS.md` (the fleet's account
of how an FFGL mixer behaves; the released copy with the Arena measurements is
`~/Projects/resolume/genlock/AGENTS.md`) before touching `ProcessOpenGL`.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install into Arena: `cmake --install build` → `~/Documents/Resolume Arena/Extra Effects`
  (yes, Extra Effects, although this is a mixer: Arena has one FFGL folder and
  no `Extra Mixers` — measured on genlock. See AGENTS.md.)
- Render a frame offline: `./build/wptest --out /tmp/f.png --size 1920x1080`
- Choose the two inputs: `--input-a video --input-b graphic`
  (a is **A**, the layer below; b is **B**, this layer, wiped in. Also
  `quads-a`, `quads-b`, `black`, `white`, `flat`.)
- Set anything by name: `--set "Pattern=4" --set "Opacity=0.3" --set "Softness=0.25"`
  (options by index: Pattern 0..6 = Horizontal, Vertical, Box, Diamond, Circle,
  Clock, Matrix; Law 0 Edge, 1 Area)
- Drive the fader to alternate ends first: `--transitions N` (for Flip-Flop)
- List parameters: `./build/wptest --list`
- Film through it (the fleet's `--pipe` format, with a second input):
  `ffmpeg … -f rawvideo -pix_fmt rgba - | ./build/wptest --pipe --size WxH
  [--pipe-src FILE_OR_FIFO] [--src-size WxH] [--fps N] [--script cues.txt] |
  ffmpeg -f rawvideo -pix_fmt rgba -s WxH -i - out.mov`. stdin is A (the layer
  below), `--pipe-src` is B (this layer; without it `--input-b` is held).
  Cues are `frame Name value`, value in the parameter's host units (0..1;
  the option index; Multiples 1..8), linear between keys; unknown names and
  the About block are refused. The clock is SetTime in **ms**, frame-relative,
  as Arena sends it. A partial frame at EOF ends the run.

## Verify
- Everything: `tools/verify.sh` (fresh universal build + every check, ~1 min)
- No name over 16 characters, and parameter 0 is Aspect Comp: `./build/wptest --names`
- Two inputs, two sizes, two MaxUVs, and the guards: `./build/wptest --mixer`
- Opacity 0 IS A and 1 IS B, every pattern, bitwise: `./build/wptest --ends`
- The edge where Edge law puts it, circle vs ellipse: `./build/wptest --edge`
- The B area where Area law puts it, every pattern: `./build/wptest --area`
- The edge width follows the waveform's slope: `./build/wptest --softness`
- So does the border's: `./build/wptest --border`
- The wobble is the stated sine and it travels: `./build/wptest --modulation`
- Alternate transitions reverse: `./build/wptest --flipflop`
- ms/frame, 720p through 4K, and the Area law's CPU cost: `./build/wptest --bench`
- No dead controls: `python3 tools/sweep.py` (`--size WxH`, `--jobs N`)
- Any check on Apple's software renderer, as the GPU-less CI runner gets it:
  `WPTEST_RENDERER=software ./build/wptest --area` (verify.sh runs them all)

Every check runs at 640x360 and 320x180 and carries its own negative control.

## Notes
- **This is an `FF_MIXER`.** The type is the eighth argument of
  `CFFGLPluginInfo`; `SetMinInputs`/`SetMaxInputs` are a **separate**
  declaration. `verify.sh` asserts both through `oxbow probe`.
- **`inputTextures[0]` is A (the layer below), `[1]` is B (this layer).** Each
  has its own `MaxUV` and half-texel inset, applied once in its own fetch. The
  vertex shader passes UV through unscaled.
- **The scoped bindings are declared interleaved** — activate(0), bind(0),
  activate(1), bind(1) — because each clears to 0 on exit rather than restoring.
- **The waveform is evaluated in `q`**, a rotated, aspect-scaled copy of picture
  space about the positioner (`Waveform.h`). Every pattern is a formula in H
  and V; the CPU derives the frame, the waveform's range over the picture, and
  the pixel-to-waveform factor; the shader evaluates per pixel.
- **The fader is `Opacity`, and the name is load-bearing.** Resolume binds a
  mixer parameter named `Opacity` to the layer's opacity fader (measured on
  genlock; writes to the mixer's own slider are overridden). 0 is A, 1 is B.
  The shader's uniform is still called `Position`.
- **The fader's ends are a branch in the shader**: Opacity ≤ 0 fetches A,
  ≥ 1 fetches B, nothing else runs. That is what makes them bitwise.
- **Softness, Border Width and Mod Amount are pixels on the horizontal ramp.**
  Every other pattern converts through its reference slope
  (`Derived::unitsPerPixel`) and its edge follows its own slope from there.
- **A zero-width border is out of circuit.** Two comparators of different gain
  at one level disagree across the whole soft edge; the shader skips the second
  comparator when `BorderW` is 0.
- **Area law solves on the CPU** by bisection over a closed-form area
  (polygon and disc clipping, a rank permutation for the matrix), cached until
  something changes. The matrix's Feistel permutation exists twice, in C++ and
  GLSL, integer for integer.
- **The clock is frame-relative and reduced in double** before the shader sees a
  phase. Resolume counts milliseconds and a float stops resolving them at ~5e8.
- `SetParamInfo` clamps a STANDARD default into 0..1, so every ranged parameter
  is 0..1 and the conversions live in `Controls.cpp`; the two Multiples are
  `FF_TYPE_INTEGER`, which is exempt.
- Override `SetTextParameter` to return `FF_SUCCESS` for the About block, or no
  host can instantiate the plugin at all.
- `wipe_core` is an OBJECT library, not STATIC — the plugin registers itself
  from a file-scope constructor nothing references by name.
- macOS build must be universal. Verify with `lipo`, never the build log.
- **Parameter 0 is sacrificial.** Resolume Arena does not expose a mixer's
  first parameter (measured on genlock: its id 0 is missing from the panel and
  the REST JSON). So index 0 is `Aspect Comp`, whose default (on) is right if it
  can never be reached. Never put a control a mixer needs there. `--names` and
  `verify.sh`'s oxbow step assert it; Arena 7.27.1 confirmed it on Wipe
  (2026-09-23: the panel shows 25 of 26, and the missing one is Aspect Comp).
- FFGL id is `WP01`. Display name `SW Wipe`.

## Not done yet
- **Never loaded into Resolume on macOS.** On Windows (Arena 7.27.1, llvmpipe,
  2026-09-23) the v0.1.0 CI build was probed by hand over REST: it loads from
  Extra Effects, is offered as a Blend Mode, initialises with a clean log, the
  layer's opacity drives `Opacity`, and Aspect Comp (index 0) is the one
  parameter hidden. No mixer frame was captured, so a correct render in
  Resolume is not claimed. Padded inputs and `SetTime` in ms were measured on
  genlock only; transition/autopilot on `Opacity` and a one-input call are open.
- CI: the Windows DLL compiles with MSVC; the macOS job is red because
  `--area` fails at 640×360 on four patterns on the GPU-less runner (see
  AGENTS.md). The other eight suites pass there.
- No `Size` control (dropped, see AGENTS.md), no presets, no OpenFX port, no
  browser demo.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume).

    ~/Library/Logs/wipe/wipe.YYYY-MM-DD.log
