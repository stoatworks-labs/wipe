# wipe

A vision mixer's analogue pattern generator as an FFGL **mixer** for Resolume
Arena/Avenue, and as an OpenFX **transition** for Resolve/Vegas/Nuke/Natron
(`source/ofx/WipeOFX.cpp`). C++/GLSL, CMake MODULE → universal `.bundle`
(macOS) + Windows `.dll`, and `Wipe.ofx.bundle` for macOS/Windows/Linux. MIT.
Public; released at v0.1.0 (2026-09-23), and v0.2.0 adds the OpenFX build.
Never loaded into Resolume on macOS; on Windows it was probed by hand in Arena
(see "Not done yet"), after genlock, the first mixer, was measured there.

Read `AGENTS.md` before changing the waveforms, the level laws, or any
tolerance in the harness. Read `~/dev/genlock/AGENTS.md` (the fleet's account
of how an FFGL mixer behaves; the released copy with the Arena measurements is
`~/Projects/resolume/genlock/AGENTS.md`) before touching `ProcessOpenGL`.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- The OpenFX plugin builds alongside: `build/Wipe.ofx.bundle` (`-DBUILD_OFX=OFF`
  to skip it). `-DWIPE_BUILD_FFGL=OFF` configures the OFX plugin alone with
  nothing but a compiler — no FFGL SDK, no GLEW (the Linux job).
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
- The OpenFX build's C++ shader IS the GLSL, pixel for pixel: `./build/wptest --cpu`
- The OpenFX build's CPU cost at 1080p, 1 thread and all: `./build/wptest --cpu-bench`
- The OFX bundle in a host: `ofxprobe --dir build` (the fleet's probe describes it
  but hosts only the Filter context); a probe with `--context transition`
  renders it — `OFXPROBE=/path/to/that/probe tools/verify.sh` makes verify.sh do so,
  and if it also has `--quirks fusion` (no frame rate anywhere: stricter than
  Resolve's Fusion page, which leaves it off the clips but gives the effect's)
  verify.sh renders the General context that way and checks the modulator
  falls back to 24 fps
- ms/frame, 720p through 4K, and the Area law's CPU cost: `./build/wptest --bench`
- No dead controls: `python3 tools/sweep.py` (`--size WxH`, `--jobs N`)
- Any check on Apple's software renderer, as the GPU-less CI runner gets it:
  `WPTEST_RENDERER=software ./build/wptest --area` (verify.sh runs them all)
- The browser demo's shaders are still the plugin's:
  `python3 demo/tools/check_shaders.py` (verify.sh runs it)

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
- **The CPU half of a frame is `Pass.cpp`, shared by both builds.**
  `BeginPass` (the frame, the comparators in W units, the modulator's phase),
  `LevelFor` (Edge law, or the Area solve) and `UniformsFor` (the floats the
  shader gets) were the top of `ProcessOpenGL`; the FFGL build still calls
  them, bit-for-bit as before. `HostValues` holds the defaults both builds
  declare. Only the Flip-Flop and the Area-law cache stay in `Wipe.cpp`.
- **`Shade` in Pass.cpp is the wipe shader mirrored in C++** (`//= mirrored`),
  for the OpenFX CPU render. **Edit both.** `wptest --cpu` fails if they
  drift. The GLSL's own marker is a C++ comment above `kWipeShader`, not
  inside it, because `demo/plugin.js` must match the raw string to the
  character.
- **OpenFX: a Transition.** `com.stoatworks.wipe`, label `Wipe`, group
  `Stoatworks`. SourceFrom = A (shown whole at Transition 0), SourceTo = B,
  `Transition` = Opacity. Also declares the General context (same clips,
  `Transition` an ordinary keyframeable param) for Nuke/Natron/Fusion. No
  Flip-Flop (state across frames); the modulator's phase is
  `time / fps × Mod Speed`; the Area law is solved every frame; inputs are read
  pixel for pixel; pixel sizes scale with the render scale; Aspect Comp folds
  in the pixel aspect (`Frame::pixelAspect`, 1 in FFGL). See AGENTS.md.
- `demo/` is the browser demo at wipe-demo.stoatworks-labs.com: the plugin's two
  shaders unedited, `demo/waveform.js` a hand port of Controls.cpp, Waveform.cpp
  and the modulator phase, `demo/area-worker.js` the lattice Area solves off the
  main thread. A **mixer**: A is the kit's clip, B a second generated clip
  (the transport's `Clip B`). `demo/vendor/` is the shared kit -- do not edit
  it; it is copied in by `stoatworks-backend/resolume-demo/sync.sh wipe`.
  Serve with `python3 -m http.server` in `demo/`; deploy from the repo root
  with `cf-run npx wrangler deploy` (no build step); `.github/workflows/deploy.yml`
  also ships it on every push to main that touches more than docs. Change a
  shader, Controls.cpp or Waveform.cpp and the demo needs the same change --
  the checker catches only the shaders. See AGENTS.md, "The browser demo".

## Not done yet
- **Never loaded into Resolume on macOS.** On Windows (Arena 7.27.1, llvmpipe,
  2026-09-23) the v0.1.0 CI build was probed by hand over REST: it loads from
  Extra Effects, is offered as a Blend Mode, initialises with a clean log, the
  layer's opacity drives `Opacity`, and Aspect Comp (index 0) is the one
  parameter hidden. No mixer frame was captured, so a correct render in
  Resolume is not claimed. Padded inputs and `SetTime` in ms were measured on
  genlock only; transition/autopilot on `Opacity` and a one-input call are open.
- CI is green: the Windows DLL compiles with MSVC, and on the GPU-less macOS
  runner every suite passes on Apple's software renderer since `--area`
  allowed for the GL spec's 1 part in 10^5 (dd445f2; see AGENTS.md).
- No `Size` control (dropped, see AGENTS.md), no presets.
- **The OpenFX build has been in one real host**: Resolve 21.1's Edit page,
  as a transition, with the clip order and the progress right (the lead,
  2026-10-03, and again after the Fusion fix). Fusion provides no frame rate
  on its clips, only on the effect, and the first build failed there;
  `framesPerSecond()` now catches every read, so in Fusion it reaches the
  effect's rate, and falls back to 24 only where a host reports none —
  checked under the test host's `--quirks fusion` (stricter: it withholds the
  effect's rate too), not yet in Fusion. Never read a host property in a
  render path without a catch. Vegas, Nuke and Natron untried.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume).

    ~/Library/Logs/wipe/wipe.YYYY-MM-DD.log
