# wipe

A vision mixer's analogue pattern generator as an FFGL **mixer** for Resolume
Arena/Avenue. C++/GLSL, CMake MODULE → universal `.bundle` (macOS) + Windows
`.dll`. MIT. Not yet public, not yet released, never loaded into Resolume.

Read `AGENTS.md` before changing the waveforms, the level laws, or any
tolerance in the harness. Read `~/dev/genlock/AGENTS.md` (the fleet's account
of how an FFGL mixer behaves) before touching `ProcessOpenGL`.

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
- Set anything by name: `--set "Pattern=4" --set "Position=0.3" --set "Softness=0.25"`
  (options by index: Pattern 0..6 = Horizontal, Vertical, Box, Diamond, Circle,
  Clock, Matrix; Law 0 Edge, 1 Area)
- Drive the fader to alternate ends first: `--transitions N` (for Flip-Flop)
- List parameters: `./build/wptest --list`

## Verify
- Everything: `tools/verify.sh` (fresh universal build + every check, ~1 min)
- No name over 16 characters, and parameter 0 is Aspect Comp: `./build/wptest --names`
- Two inputs, two sizes, two MaxUVs, and the guards: `./build/wptest --mixer`
- Position 0 IS A and 1 IS B, every pattern, bitwise: `./build/wptest --ends`
- The edge where Edge law puts it, circle vs ellipse: `./build/wptest --edge`
- The B area where Area law puts it, every pattern: `./build/wptest --area`
- The edge width follows the waveform's slope: `./build/wptest --softness`
- So does the border's: `./build/wptest --border`
- The wobble is the stated sine and it travels: `./build/wptest --modulation`
- Alternate transitions reverse: `./build/wptest --flipflop`
- ms/frame, 720p through 4K, and the Area law's CPU cost: `./build/wptest --bench`
- No dead controls: `python3 tools/sweep.py` (`--size WxH`, `--jobs N`)

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
- **The fader's ends are a branch in the shader**: Position ≤ 0 fetches A,
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
  `verify.sh`'s oxbow step assert it; only Arena can confirm which one it hides.
- FFGL id is `WP01`. Display name `SW Wipe`.

## Not done yet
- Never loaded into Resolume, on any platform. Never run on any rasteriser but
  this Mac's. Windows never compiled.
- No release tag, no remote, not registered on the website. `StoatworksAbout.h`
  and `ATTRIBUTIONS.md` are provisional hand copies.
- No `Size` control (dropped, see AGENTS.md), no presets, no OpenFX port, no
  browser demo, no user guide.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume).

    ~/Library/Logs/wipe/wipe.YYYY-MM-DD.log
