# Wipe

> **AI-assisted project.** This codebase was created with [Claude](https://claude.com/claude-code)
> (Anthropic), directed and reviewed by a human author. The pattern generator is
> not asserted but measured: an offline harness drives the real plugin class in
> a headless GL context with **two** input textures at two different
> resolutions and checks each claim against a closed form — a hard edge landing
> on its column **bitwise**, a soft edge's position integrated to **0.0000 px**,
> every pattern's B area within **0.33 px** of the fader in Area law, a circle's
> soft edge widening towards its centre **exactly as the parabola predicts**
> (41.855 px measured, 41.855 predicted), the modulator's sine recovered to
> **16.0000 px** of 16 (see [Status](#status)). It has **never been loaded into
> Resolume on macOS**. On Windows, a build of v0.1.0 loads in Resolume Arena
> 7.27.1, is offered as a layer's Blend Mode and is driven by the layer's opacity
> fader, on software rendering; no frame of its picture inside Resolume has been
> captured. It is the fleet's second FFGL *mixer*, built to what the first,
> genlock, measured in Arena. Check it in your own rig before trusting it in a
> show.

A 1970s vision mixer's analogue pattern generator, as an FFGL **mixer** for
[Resolume](https://resolume.com) Arena and Avenue.

![A soft circle wipe with a modulated border, revealing one test card over another](docs/hero.png)

<sub>The repo's two test cards through the plugin — a circle at Opacity 0.42
with a soft edge, a gold border and the modulator on — rendered by `wptest`,
the offline harness, not captured from Resolume.</sub>

<!-- downloads:start -->

## Download

**[v0.1.0](https://github.com/stoatworks-labs/wipe/releases/tag/v0.1.0)** — prebuilt for macOS and Windows. Pick your platform:

<details>
<summary><b>macOS</b> — Universal (Apple Silicon + Intel)</summary>

| Build | Download | Size |
| --- | --- | --- |
| Universal (Apple Silicon + Intel) · .dmg disk image | [`wipe-0.1.0-macos-universal.dmg`](https://github.com/stoatworks-labs/wipe/releases/download/v0.1.0/wipe-0.1.0-macos-universal.dmg) | 212 KB |
| Universal (Apple Silicon + Intel) · .zip archive | [`wipe-macos-universal.zip`](https://github.com/stoatworks-labs/wipe/releases/latest/download/wipe-macos-universal.zip) | 176 KB |

</details>

<details>
<summary><b>Windows</b> — x64</summary>

| Build | Download | Size |
| --- | --- | --- |
| x64 · .exe installer | [`wipe-0.1.0-windows-x86_64-setup.exe`](https://github.com/stoatworks-labs/wipe/releases/download/v0.1.0/wipe-0.1.0-windows-x86_64-setup.exe) | 222 KB |
| x64 · .zip archive | [`wipe-windows-x86_64.zip`](https://github.com/stoatworks-labs/wipe/releases/latest/download/wipe-windows-x86_64.zip) | 113 KB |

</details>

All builds, checksums and release notes: [github.com/stoatworks-labs/wipe/releases](https://github.com/stoatworks-labs/wipe/releases).

macOS builds are signed and notarised and open normally. The Windows builds are unsigned, so SmartScreen warns once.

<!-- downloads:end -->

## Every wipe is a waveform and a comparator

A vision mixer of that era did not store its wipes as pictures. It made them
live, from a few **analogue waveform generators** running at line and field
rate: a horizontal ramp, a vertical ramp, and horizontal and vertical
parabolas. It combined those in a small matrix — adding them, or taking the
larger with a non-additive mix, NAM — and compared the result against the
fader's level. Where the waveform was below the level you saw B; above it, A.

So every pattern in the menu is a formula in those two ramps, H and V:

| Pattern | Waveform |
| --- | --- |
| Horizontal, Vertical | H, or V |
| Box | NAM(\|H\|, \|V\|) |
| Diamond | \|H\| + \|V\| |
| Circle | H² + V² — the two parabolas, added |
| Clock | the angle, the one wipe that needed an arctangent generator |
| Matrix | a per-cell threshold order |

**What falls out of that**, rather than being added on:

- **Softness is the comparator's gain.** A low-gain comparator is a soft edge
  whose width is set by the waveform's slope — so a circle's edge is softer
  near its middle than at its rim, because the parabola is flatter there. The
  harness measures it: at Softness 16 px the circle's 10–90% width is 41.9 px
  at a radius of 82 px and 14.3 px at 233, and both are the closed form.
- **The border is a second comparator** a little above the level, and its
  width varies around the pattern for the same reason.
- **Modulation is a sine added to the waveform**, which wobbles the edge
  exactly as the modulator on a real generator did.
- **A circle is an ellipse** unless the generator compensates for the picture's
  aspect, as real ones had to. `Aspect Comp` off reproduces the ellipse, and the
  harness asserts its axes.
- The **positioner** moves where the waveforms cross zero. **Multiple** runs
  them at N times the line rate.

**It is a mixer, not an effect.** It needs a layer below it: that layer is A,
and the clip on this layer is B, which the fader wipes in. The
[user guide](docs/USER-GUIDE.md) covers installing it, picking it as a layer's
Blend Mode, and every control.

[![Wipe — a 1970s vision mixer's pattern generator, for Resolume](docs/video-thumb.png)](https://www.youtube.com/watch?v=HIclO2s5ylw)

*[Watch it](https://www.youtube.com/watch?v=HIclO2s5ylw) — 55 seconds:
a hard Horizontal wipe, Box and Diamond, a soft Circle whose edge is softer near its middle, a Clock with a border, a modulated edge, Multiple, and the positioner turning a box as it opens. Every frame is the real plugin's output: an FFGL plugin has no window,
so the footage is rendered by this repository's own offline harness
(`wptest --pipe`, driven by a cue sheet) rather than filmed off a screen, and
the clips are Resolume's bundled demo media.*

**[Try it in your browser](https://wipe-demo.stoatworks-labs.com)** — the
plugin's own wipe shader ported to WebGL2, with its pattern frame and both
fader laws (the closed-form Area solve included) ported to JavaScript, wiping
between two generated clips with every control. It is a port and not the
plugin: read what
[the page itself says it does not reproduce](https://wipe-demo.stoatworks-labs.com).

## The controls

**Pattern** — Aspect Comp, Pattern, Reverse, Flip-Flop (reverse on alternate
transitions: A box-wipes in, then B box-wipes in). Aspect Comp is first on
purpose: Resolume Arena does not show a mixer's first parameter (measured on
genlock, and confirmed on Wipe), so index 0 holds the one control whose
default — on, a round circle — is right if nobody can ever reach it.

**Fader** — Opacity, and Law. **Opacity is the fader**: 0 is A, the layer below,
and 1 is B, this layer. It is named Opacity on purpose, because Resolume binds a
mixer parameter of that name to the **layer's opacity fader** (measured on
genlock and on Wipe), so the layer's own fader drives the wipe and the Opacity
slider in the mixer's panel is overridden. *Edge* is what the hardware did: the level is
linear in the fader. *Area* chooses the level so that the B area is exactly
the fader, solved in closed form for every pattern.

**Edge** — Softness, Border Width, Border Softness, all in pixels on the
horizontal ramp (every other pattern's width follows its slope from there),
and the Border colour as a swatch.

**Positioner** — Centre X, Centre Y, Rotation, Aspect. The positioner moves
the closed patterns and the clock; the ramps and the matrix ignore it, as the
hardware's did.

**Modulation** — Mod Amount (pixels), Mod Frequency (cycles across the
picture), Mod Speed, and Multiple H / Multiple V (1–8).

The spec's `Size` control is not here: in Edge law it would be the fader by
another name, and in Area law it cancels out exactly. See
[AGENTS.md](AGENTS.md).

## Build

Needs CMake and the Resolume FFGL SDK, which is a submodule.

```bash
git clone --recursive https://github.com/stoatworks-labs/wipe
cd wipe
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build    # → ~/Documents/Resolume Arena/Extra Effects
```

macOS builds universal (arm64 + x86_64) by default. Add
`-DCMAKE_OSX_ARCHITECTURES=arm64` for a faster development build.

The install path is **Extra Effects**, although this is a mixer. Resolume has one
FFGL folder, and sources, effects and mixers all load from it: genlock, the
fleet's first mixer, was loaded from there by Resolume Arena 7.27.1 and offered
as a layer Blend Mode. (Before v0.1.0 this said `Extra Mixers`, which Arena never
reads.)

## Building and testing

The harness renders the real plugin class headlessly, with **two** inputs —
`--input-a` is A, the layer below; `--input-b` is B, this layer — which may be
different sizes, with different hardware padding, rendered to a third size.

    ./build/wptest --out /tmp/frame.png     both cards, through the plugin
    ./build/wptest --list                   every parameter, kind and default
    ./build/wptest --mixer                  two inputs, two MaxUVs, and the guards
    ./build/wptest --ends                   Opacity 0 IS A and 1 IS B, every pattern
    ./build/wptest --edge                   the edge where Edge law puts it; circle vs ellipse
    ./build/wptest --area                   the B area where Area law puts it, every pattern
    ./build/wptest --softness               the edge width follows the waveform's slope
    ./build/wptest --border                 so does the border's
    ./build/wptest --modulation             the wobble is the stated sine, and it travels
    ./build/wptest --flipflop               alternate transitions reverse
    ./build/wptest --bench                  720p through 4K
    ./build/wptest --pipe --pipe-src F      two raw RGBA streams in, frames out (filming, not a check)
    python3 tools/sweep.py                  no control is silently dead
    tools/verify.sh                         all of it, on a fresh universal build

Every check runs at **two rasters**, 640×360 and 320×180, and every tolerance
is derived from something physical — one pixel, one float ULP through the
comparator, the curvature of a parabola — rather than from the number this
machine printed first. Each check carries a **negative control**: something it
must be able to reject, run every time. [AGENTS.md](AGENTS.md) lists every
number and where it comes from.

## Status

**v0.1.0, and honestly early.** Verified by measurement on an Apple
M4 Max, macOS 26.4.1, 2026-09-23, at 640×360 **and** 320×180 unless stated:

| Check | Result |
| --- | --- |
| Two inputs, two sizes, two MaxUVs | A 200×120 of 256×256, B 96×70 of 128×128, out 320×200: **0 padding pixels** reached the picture, every quadrant within **0.000 of 255**, the marker within one source texel |
| The missing-input guards | a null input array, zero inputs, one input, a null A and a null B all return `FF_FAIL` without crashing |
| The fader's ends | Opacity 0 is A and Opacity 1 is B, **bitwise, on all seven patterns**, with softness, border, modulation, multiple, positioner and rotation all on |
| Edge law, whole pixel | a hard edge at column p·W: B left, A right, **0 pixels wrong** at three columns |
| Edge law, fractional | a soft edge at 160.5, 320.25, 480.75 px recovered by integration to **0.0000 px** (tolerance 0.02) |
| Circle vs ellipse | Aspect Comp on: eight radii agree to **0.0007 px**; off: axes in the ratio **1.7778** of the picture's 1.7778, diagonal within 0.001 px of the ellipse |
| Area law | every pattern's B area within **0.33 px of area** of the fader (continuous, 4× supersampled); the corner-free patterns also within **0.68 px** summed over the displayed pixels. Tolerance 1 px plus the GL spec's 1 part in 10^5 of the picture coordinate; on Apple's software renderer the worst is 1.50 px of 3.40 |
| Softness | horizontal 10–90% width **12.8000 px** of 12.8; on the circle **41.855 / 23.449 / 14.318 px** at three radii, each the parabola's closed form to 0.001 |
| Border | **12 red columns** from column 320, contiguous; soft, the red integrates to **12.0000 px**; on the circle 31.801 and 14.938 px at two radii against 31.800 and 14.938 |
| Modulation | amplitude **16.0000 px** of 16, RMS residual from a sine **0.0000 px**, and a quarter second at 1 Hz moves the phase by exactly −π/2 |
| Flip-Flop | four transitions in a row alternate the box bitwise; the first arrival at an end counts for nothing |
| Mutation test | one character of the shipped GLSL (the comparator's 0.5 → 0.6) fails **five** checks; the circle built from \|H\|+\|V\| fails `--softness`; one MaxUV for both inputs fails `--mixer` |
| No dead controls | all **21** sweepable of the 26 parameters change the picture; the other five are the About block |
| macOS binary | universal (`x86_64 arm64`), exports `plugMain`, ad-hoc signs |
| Host metadata | `oxbow probe` reads **SW Wipe / WP01 / mixer / inputs 2..2**, parameter 0 **Aspect Comp** |
| Render cost | **0.03 ms/frame at 720p, 0.04 at 1080p, 0.12 at 4K** (0.7% of a 60 fps frame), worst of several runs. Area law adds a CPU solve on the frames where something changed: under 0.06 ms for any single pattern, **2.8 ms for a box at Multiple 8×8 and 7.8 ms for a circle** on a quiet machine (6.9 and 16.9 with other builds loading the CPU) |

Run `tools/verify.sh` before believing any of it.

**Windows, in Resolume Arena 7.27.1** (win-lab, Mesa llvmpipe, no GPU,
2026-09-23). The fleet's Arena gate cannot gate a mixer, so a CI build of this
source was probed by hand over Arena's REST API and the plugin's own log, as
genlock was. It loads from **Extra Effects**; Arena's log registers it as
`'SW Wipe' uid: WP01 category: 2` beside Resolume's own blend modes, and it is
offered in every layer's **Blend Mode** list. Set as layer 3's blend mode over a
still on layer 2, its log shows it initialised on Mesa llvmpipe (GL 4.5 Core),
loaded by Arena 7.27.1 build 15990 from Extra Effects, with no error line in
either log. Setting the **layer's** opacity to 0.3, 0.7 and 1.0 read back as the
mixer's `Opacity` 0.3, 0.7 and 1.0, and a write of 0.2 to the mixer's own
Opacity was overridden (it read back 1.0, the layer's): the layer's fader drives
the wipe, as designed. Writing Pattern = Circle over REST took. The mixer panel
shows **25 of the 26** declared parameters, and the one missing is **Aspect
Comp, index 0**: Arena hides a mixer's first parameter, as it hid genlock's, and
parking Aspect Comp there worked. No frame of the mixer's output was captured
(Arena's REST does not serve a mixer's picture), so a correct render inside
Resolume is not claimed.

**Not done, and the honest list is long.** Wipe has **never been loaded into
Resolume on macOS**, and its picture has not been looked at inside Resolume on
either platform. That Arena hands a mixer both inputs **padded** and calls
`SetTime` **every frame, in milliseconds** (so the modulator will travel) was
measured on genlock and not re-checked on Wipe. Whether a layer transition or
the autopilot moves `Opacity`, and whether Arena ever calls a mixer with one
input, are still open. CI runs on GitHub and is green: the Windows DLL compiles with MSVC,
and on the GPU-less macOS runner all nine suites and the control sweep pass on
Apple's software renderer. `--area` failed there at first — the software
renderer interpolates picture coordinates only to the GL spec's 1 part in
10^5, which is 2.3 px of area on a 640×360 vertical wipe, and the check had
allowed one pixel. The check was wrong, not the plugin; its tolerance now
includes that allowance, and `tools/verify.sh` runs every suite on the same
software renderer locally. Nothing has run on a **GPU other than this
Mac's**. The spec's `Size` control was dropped, for a stated reason.
Area law with both Multiples high is the one setting whose CPU cost is worth
knowing about. There are **no presets** and no OpenFX port. The
[browser demo](https://wipe-demo.stoatworks-labs.com) is a port, not the plugin.
The [user guide](docs/USER-GUIDE.md) covers every control, and the About
block's fourth button opens it.

[AGENTS.md](AGENTS.md) has the full list of what is assumed rather than
measured, the open questions, and the traps.

<!-- attributions:start -->
This project is built on other people's work — see [ATTRIBUTIONS.md](ATTRIBUTIONS.md).
<!-- attributions:end -->

## Licence

MIT — see [LICENSE](LICENSE).
