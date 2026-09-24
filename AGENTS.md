# wipe — orientation for another LLM (or a newcomer)

**What it is:** an FFGL 2.1 **mixer** for Resolume Arena/Avenue that makes its
wipes the way a 1970s vision mixer did: from waveforms and a comparator, not
from pictures. C++17 + GLSL 4.10, CMake, universal macOS `.bundle` and a
Windows `.dll`. MIT. Home `github.com/stoatworks-labs/wipe`, released v0.1.0.
genlock, the fleet's first mixer, was measured in Arena 7.27.1 (Windows,
2026-09-23), and this build was corrected to what that session measured; Wipe
itself was then probed in the same Arena the same day — see "What genlock
measured in Arena" and "What Wipe showed in Arena" below.

`CLAUDE.md` is the command reference. This file is the *why*: the idea, every
number in the harness and where it comes from, the traps this build actually
hit, what is verified and what is assumed, and the decisions taken without
asking.

For how an FFGL mixer behaves at the ABI — the type being one argument, the
input count being a separate declaration, which base class and why, what
Resolume does and does not do — read genlock's `AGENTS.md`
(**`~/Projects/resolume/genlock/AGENTS.md`**, the released repo; `~/dev/genlock`
is an older local copy that predates the Arena run). Nothing here contradicts
it; what this build learned *beyond* it is under "The traps".

---

## The one idea

**Every wipe is a waveform and a comparator.**

A vision mixer of that era did not store its wipes. It ran a few analogue
waveform generators at line and field rate — a horizontal ramp H, a vertical
ramp V, and the two parabolas H² and V² — combined them in a small matrix, and
compared the result W against the fader's level. Below the level: B. Above it:
A. So:

    Horizontal   W = H                    H = q.x + 0.5
    Vertical     W = V                    V = q.y + 0.5
    Box          W = NAM( |H|, |V| ) / R  the larger of the two ramps
    Diamond      W = ( |H| + |V| ) / R    the two ramps added
    Circle       W = ( H² + V² ) / R²     the two parabolas added
    Clock        W = angle( q ) / 2π      clockwise from twelve
    Matrix       W = rank( cell ) / N     a per-cell threshold order

where `q` is picture space moved to the positioner, scaled by the aspect
compensation and the pattern aspect, and rotated (`Waveform.h`); with Multiple
N ≥ 2 the ramps wrap N times. R is the farthest picture corner in the pattern's
own norm, so W runs 0..1 from the centre to that corner and the fader's end
covers the picture; with Multiple on, R is the half-cell.

Everything else is a consequence:

- **Softness is the comparator's gain.** The key is
  `clamp( 0.5 + ( level − W ) / softW )`, a linear clip. Its width on screen is
  `softW / |∇W|`, so it follows the waveform's slope — and a parabola's slope
  grows with radius, so a circle's edge is softer near its middle. Measured:
  41.9 px at r = 82, 14.3 px at r = 233, both the closed form.
- **The border is a second comparator** at `level + borderW`, and the border is
  where the two disagree. Same slope dependence, same measurement.
- **Modulation is a sine added to W**, down the picture (across it for the
  vertical wipe), with its phase advanced by the clock.
- **A circle is an ellipse** unless `Aspect Comp` scales q.x by the picture's
  aspect first, as a real generator had to.

The comparator level is where the two laws differ. **Edge law** is the
hardware's: `level = wMin + p ( wMax − wMin )`, linear in the fader over the
waveform's range across the picture. **Area law** solves for the level at
which the *soft* B area — the mean of the hard area over the comparator's band,
which is exactly the integral of the key — equals the fader. Every pattern has
that area in closed form: half-plane, polygon and disc clipping against the
picture's parallelogram in q-space (`Waveform.cpp`), wedges for the clock, a
sum over ranks for the matrix. Bisection to 1e-10 in level. There is no table.

The fader's ends are a **branch**: `Opacity ≤ 0` fetches A, `≥ 1` fetches B,
and nothing else runs, so neither a soft edge nor a border can leak into an end
stop. That is what makes `--ends` a bitwise claim.

---

## Every number in the harness

Every check runs at **640×360 and 320×180**, and each tolerance is derived from
something physical, never fitted. The measurements go through a **float
framebuffer**: an 8-bit readback quantises every partial pixel to 1/255 and,
on an axis-aligned edge, does it identically on every row, so the error is
systematic and does not average out.

| Check | The number | Where it comes from |
|---|---|---|
| `--mixer` | genlock's numbers exactly | Same rig, same cards, same three assertions per input, plus a mid-fader render that fetches both inputs through the wipe. See genlock's AGENTS.md. |
| `--ends` | **zero bytes**, 7 patterns × 2 ends × 2 rasters | A branch on a uniform, then a fetch at the pixel's own texel centre with MaxUV 1: `mix` never runs. Rendered with softness, border, modulation, Multiple 2, an off-centre positioner and a rotation all on, so anything that could leak has the chance. Negative control: half way is neither card (>1 code from each). |
| `--edge` whole pixel | **zero pixels wrong** | A hard comparator with the edge at column p·W, on pixel centres: every pixel is wholly B or wholly A. Three columns. |
| `--edge` fractional | **0.02 px**, derived bound 1.5e-3 px | The soft edge's position is the sum of the key along the row — a linear functional, so it translates exactly. The only error is float32 through the comparator over the ~10 partial columns: (softPx+2) · 2⁻²³ · W · 2. The check asserts bound × 3 ≤ tolerance. Measured 0.0000. Negative control: the sum is not a whole column. |
| `--edge` circle | **0.05 px** spread over eight radii | The 50% crossing along a ray of pixel centres from the positioner (put on a pixel centre), by interpolation. The key along a ray is quadratic in r, so the interpolation bias is ≤ 1/(8r) px per crossing — asserted ≤ tol/3 — and the spread cancels most of it anyway. Measured 0.0007 and 0.0011. |
| `--edge` ellipse | axes ratio within 2·tol/r_y; diagonal within 0.05 px | With Aspect Comp off the level set in the unit square is a circle, so on screen it is an ellipse with axes in the picture's ratio, and its 45° radius is the ellipse's polar form of the two measured axes. Negative control: the axes differ by 63 px. |
| `--area` continuous | **1 px of area + the GL's allowance**, at the test raster, judged per fader position | The sum of the key over a 4× supersampled render of the same geometry (softness scaled with it). The solver is exact to 1e-10 in level; the band quadrature to 1e-10/softW; the pixel sum's own error is the sum-vs-integral residual of a piecewise-linear key, which is what the supersample is for (see the trap) — that is the 1 px. The GL's allowance is the OpenGL 4.1 core spec's own precision (§2.1.1: results "accurate to about 1 part in 10^5"), applied to the interpolated `uv` the waveform is drawn from: each axis may be off by ε = 1e-5, which moves an edge element with normal n by \|nₓ\|·ε·W + \|n_y\|·ε·H pixels. Integrated along the edge that is ε·∫(\|nₓ\|W + \|n_y\|H)dl, and by the co-area formula that integral is Σ(\|Δₓk\|·W + \|Δ_yk\|·H) over the rendered key — read off the picture, so no per-pattern perimeter formula can drift from what was drawn — plus ε per pixel inside the band for the comparator's own arithmetic. A vertical wipe at 640×360: 1e-5 × 640 × 360 = 2.3 px. Worst on this Mac's GPU 0.33 px (Box 2×2); worst on Apple's software renderer 1.504 px of 3.40 (Vertical). See the trap. |
| `--area` resolution | every tolerance **under a quarter-pixel shift** of its own edge | The negative control on the allowance: a one-pixel shift of the whole edge moves the area by the edge's length in pixels (co-area again, Euclidean gradient), and the spec's share of that is only 1e-5 × 640 = 0.006. Asserted per case and position; the coarsest is the plain Horizontal wipe, 3.33 px of tolerance against a quarter-pixel shift of 89.8 px. Mutation-checked: the fader off by 1 part in 10⁴ (18 px) fails 17 of the 22 cases on the software renderer. |
| `--area` displayed | **1 px**, corner-free patterns only | The same sum over the plain pixel centres. Softness is **8 px, an integer**, so on an axis-aligned edge the two kinks of the linear ramp sit at the same fractional phase and Euler–Maclaurin cancels exactly; on a curved edge the phases are spread and the residual is ~1/(8w) per unit length with random sign. A soft box **corner** is a 45° kink over a w×w patch and carries ~0.1 px each — four corners give 0.3–0.5 px — and a 45° **edge** projects the pixel centres onto its normal as a single-phase lattice, so the diamond and the 2×2 lattice are measured continuous-only and say so. Worst 0.68 px (Circle at 320×180). |
| `--area` negative | Edge law on the box is **> 100 px** off | Level linear in the fader gives 0.4445 of the picture at 0.5, 12,778 px from the fader: the check tells the two laws apart. |
| `--softness` horizontal | **0.02 px** | 10–90% crossings on a linear ramp, exact by interpolation; 0.8 × the stated width because the comparator is a linear clip. Measured 12.8000 of 12.8. At Softness 0, **0 partial pixels**. |
| `--softness` circle | **0.05 px** at three radii, bound 2/(8r) | Predicted from the plugin's own softW and R: r_k = R √( level − softW ( k − ½ ) ). The check is whether the *widths at three radii* follow that; the "slope alone" figure is printed beside it to show the first-order estimate and how far it sits from the closed form near the centre. Negative control: a uniform width — what \|H\|+\|V\| or a plain radius would give — is rejected by 27 px. |
| `--border` hard | **12 columns**, contiguous, from column W/2 | Two hard comparators 12 px apart on a unit-slope ramp. Counted, not summed. |
| `--border` soft | **0.02 px** | The border fraction per pixel is red − green (A black, B white, border red), and its integral along the row is the border's width whatever the two softnesses are, provided the outer comparator stays above the inner one (asserted by the settings). Measured 12.0000. |
| `--border` circle | **0.05 px** at two radii | The inner comparator's 50% and the outer's, the outer recovered as key + border from the two channels (red alone is a blend of three). Closed form R( √(level+bW) − √level ). Negative control: a constant width is rejected. |
| `--modulation` amplitude | **0.02 px** | Edge per row by integration, projected onto the stated frequency. Four whole cycles over the rows makes the projection of a pure sine at pixel centres exact. Measured 16.0000 of 16. |
| `--modulation` shape | RMS residual **0.02 px** | After removing the fitted sine; a triangle wave would leave 1.6 px. Measured 0.0000. Negative control: at 3 cycles the projection is 0. |
| `--modulation` travel | **0.02/16 rad** | The edge moves by −m sin( 2πfv − 2πφ ), so its fitted phase is π − 2πφ and a quarter second at 1 Hz is −π/2 = 3π/2 mod 2π. Asserted against that AND against the plugin's own reduced phase (0.250000). |
| `--flipflop` | **zero bytes**, four transitions | Each arrival at an end from the other end flips the direction; the box after an odd transition is the Reverse-on box bitwise, after an even one the Reverse-off box. The first arrival counts for nothing; with Flip-Flop off nothing counts. |
| `--pipe` | not asserted | A filming mode in genlock's shape (`gltest --pipe`, e10444b): stdin is A, `--pipe-src` B, cues through the plugin's own setter, `SetTime` in milliseconds as Arena sends it. Verified by an ffmpeg round trip at 640×360 with B from a FIFO at 480×270: an `Opacity` ramp 0→1 opens a circle from all-A to all-B, a held Opacity with Mod Amount on changes the picture frame to frame (the clock moves), 2.5 frames on stdin give 2 out, and a misspelled cue is refused. |
| `--bench` | not asserted | No threshold is worth asserting on somebody else's GPU. The Area-law solve is timed beside it because it is the one cost that is not trivial. |

**Negative controls actually run, on the committed tree (2026-09-23):**

- **Mutation test.** One character of the shipped GLSL — the comparator's
  `0.5` became `0.6` — failed `--edge`, `--area`, `--softness`, `--border` and
  `--modulation`, and correctly did not fail `--ends`, `--flipflop` or
  `--mixer`, none of which exercise the comparator's midpoint. That is the
  proof the harness drives the shaders it ships. Reverted with `git checkout`
  of the committed file, and `touch`ed against the same-second make trap.
- **Circle built from \|H\|+\|V\|** instead of the parabolas: `--softness`
  fails (the widths at three radii no longer follow the closed form),
  `--border`, `--edge` and `--area` fail with it.
- **One MaxUV for both inputs** (B fetched through A's, the genlock trap):
  `--mixer` fails — 2,400 sentinel pixels, quadrants 19.2 codes off, the marker
  0.077 of the picture away. `--ends` still passes, because at matched rasters
  the two MaxUVs are both 1; that is why `--mixer` exists.

**Would this hold on another rasteriser, at another raster?** One line per
check:

- `--mixer`: yes — constant regions, one source texel, as genlock argued.
- `--ends`: yes — a branch and a texel-centre fetch; the only rasteriser
  dependence is whether GL_LINEAR at an exact texel centre returns the texel,
  which the 8-bit rounding forgives.
- `--edge` whole: yes — pixel centres at (x+½)/W against a level that is p·W/W;
  the comparison is float32 with margins of half a pixel.
- `--edge` fractional and `--modulation`: yes — the integral is a linear
  functional; the bound is float32 arithmetic the spec guarantees.
- `--edge` circle, `--softness`, `--border` circle: yes — crossings by
  interpolation with a curvature bound stated in pixels of *this* raster and
  asserted ≤ tol/3; at 320×180 the bound doubles and still fits.
- `--area` continuous: yes, now — the pixel sum's residual is the geometry's,
  and the 4× supersample puts it under 0.4 px; what the RASTERISER may add is
  the spec's 1 part in 10^5 of `uv`, which is in the tolerance by derivation
  and was not before (see the trap).
- `--area` displayed: only for the corner-free cases, and only with an integer
  softness width — which is stated, and which is why the diamond and the
  lattice are not in it.
- `--flipflop`: yes — bitwise comparison of two renders of the same plugin.
- Apple's software renderer — what GitHub's GPU-less macOS runner falls back
  to — is selectable on this Mac with `WPTEST_RENDERER=software`
  (`kCGLPFARendererID` = `kCGLRendererGenericFloatID`), and it reproduces the
  runner's numbers to the last printed digit. `tools/verify.sh` runs every
  suite on it as well as on the GPU, so a check calibrated on the GPU fails
  here first.
- What has NOT been proved: any of this on llvmpipe (Mesa) or on a GPU other
  than this Mac's.

---

## The traps

Ordered by how much time they cost.

**A zero-width border is not no border.** The border is `outer − key` where
`outer` is a second comparator at `level + borderW`. At `borderW = 0` with the
border's softness at 0, `outer` is a hard step at the *same* level as the soft
`key`, and the two disagree across the whole soft edge: the border colour
(white by default) was being painted into every soft edge, shifting it 0.2 px
towards A and narrowing every measured width by 1.7%. Two comparators of
different gain at one level really do that on hardware. The shader now skips
the second comparator when `BorderW` is 0. Found by `--edge`'s integral, which
is why that check integrates rather than eyeballs.

**The software renderer's `uv` is good to 1 part in 10^5, and 1e-5 of the
picture is more than a pixel.** CI's first runs failed `--area` on GitHub's
GPU-less macOS runner: Vertical, Box, Box reversed and Clock 1.04–1.50 px of
continuous area off the fader at 640×360 against a one-pixel tolerance, while
the displayed area stayed within 0.5 px and 320×180 passed. Reproduced here to
the digit with `WPTEST_RENDERER=software`. Hard edges are exact there; soft
edges are not, and a vertical wipe shows why: every column's key sums to the
same wrong row — 1151.9906 for 1152 at 2560×1440 — and the key steps by
exactly −0.031250417 a row instead of −1/32, so the renderer interpolates `uv.y`
with a step 1.3e-5 too steep, not per-pixel noise. Swept over the fader,
`uv.y` is off by up to 1.6e-6, 2.5e-6, 4.7e-6 and 9.0e-6 at 180, 360, 720 and
1440 rows; `uv.x` by 1e-7; the GPU by 4e-8 in both. That is inside the GL
spec's "about 1 part in 10^5", so the renderer is allowed it — and on a
vertical wipe 1e-5 of the picture height, over the picture's width, is 2.3 px
of area at 640×360. It scales with the supersampled reference's height, which
is why the 2560×1440 figure failed and the 640×360 displayed one did not, and
why 320×180 (one pixel = 1.7e-5 of the picture) passed. The check now adds
that allowance, read off the key by the co-area formula (see "Every number"),
with a negative control that every tolerance stays under a quarter-pixel shift
of its own edge. The plugin was never wrong, and nothing in `source/` changed.
CI went green on the commit that made the change (dd445f2, run 35926715895):
all nine suites, and the control sweep, which the red `--area` had kept from
ever running (21 swept, 0 dead).
(Computing `uv` from `gl_FragCoord` would dodge the interpolator, but it is
still a GL float result under the same spec clause, and a plugin change needs
a release.)

**A soft box corner is not a ramp.** Summing a linear-clip ramp over pixel
centres equals its integral exactly when the ramp's width is a whole number of
pixels (Euler–Maclaurin: the two kinks sit at the same fractional phase). A
box *corner* is `min( kx, ky )` — a 45° kink across a w×w patch — and carries
about 0.1 px of area error each. Four corners: 0.4 px, inside the tolerance. A
2×2 lattice at 16:9 has some sixty corners and read 5.3 px off. The plugin was
right; the *measurement* was the displayed area, not the continuous one.
`--area` now measures both and says which is which.

**A 45° edge has a single-phase lattice.** The pixel centres project onto a
diagonal edge's normal as a lattice of spacing 1/√2 with one phase, so the
cancellation that makes an integer width exact on an axis-aligned edge does
not happen, and the diamond read 4.5 px off for the same reason as the box
lattice. Same fix.

**Red is not the outer comparator.** With A black, B white and the border red,
green is `key ( 1 − border )` and red is `key ( 1 − border ) + border`. Their
difference is the border, exactly, and its integral is the border's width —
but red's own 50% crossing is not the outer comparator's. `--border` on the
circle recovers `outer = key + border` from the two channels first.

**The modulator's edge moves the other way from its phase.** W gets
`+m sin( 2π( f·v − φ ) )`, so the edge in x moves by `−m sin(...)`, whose
fitted phase is `π − 2πφ`. A quarter cycle of φ is −π/2 on the edge, which is
3π/2 mod 2π. The plugin was right; the harness's prediction had the sign
wrong, and the assertion against the plugin's own reduced phase is what said
which of the two to believe.

**`round` is a GLSL built-in.** A Feistel loop written `for( uint round ... )`
does not compile, and the error names neither the keyword nor the line in a
file that exists. The loop variable is `rnd`.

**The Area-law solve was 86 ms on a circle at Multiple 8×8.** Every
evaluation of the area was re-clipping a hundred lattice cells against the
picture before intersecting the disc with them. The cells do not depend on
the level; they are now clipped once per solve, and the band quadrature's
tolerance was loosened from 1e-12 to 1e-10 (a tenth of a pixel at 4K). 7.8 ms
now, and 2.8 ms for the box — still the only non-trivial cost in the plugin,
and only when Area law, both Multiples and a moving fader coincide.

Inherited from genlock and tinsel, and all bitten again on cue: the scoped
bindings clearing rather than restoring; `StoatworksAboutParams.h` needing the
SDK first; the OBJECT library; the 0..1 clamp on STANDARD defaults; the
synthetic clock; the same-second make.

**There is no `Extra Mixers`.** Until v0.1.0 the install prefix, the README,
CLAUDE.md and the release workflow all sent the bundle to
`~/Documents/Resolume Arena/Extra Mixers`, inherited from genlock's assumption
that Resolume keeps mixers in a folder of their own. It does not: genlock was
loaded by Resolume Arena 7.27.1 (Windows, 2026-09-23) out of **Extra Effects**
and offered as a layer **Blend Mode**, and the Arena binary names no other FFGL
folder. An install to Extra Mixers installs nothing Arena sees.

---

## Shape of the code

    source/Shaders.cpp      the pass. One vertex, one fragment. The waveform,
                            two comparators, the mix, and the matrix's Feistel.
    source/Wipe.*           the plugin: type, parameters, the two inputs, the
                            flip-flop, the Area-law cache.
    source/Waveform.*       the frame, the waveform's range, the pixel-to-W
                            factor, the closed-form areas, the solver, and
                            the C++ copy of the Feistel.
    source/Controls.*       0..1 host parameters to pixels, radians, Hz.
    source/Timing.*         the host clock, the epoch, the modulator's phase.
    source/Diag.*           a log file, for the shader that will not compile.
    tools/wptest/           the offline harness. Two inputs, a float FBO.
    tools/sweep.py          no control is silently dead.
    tools/verify.sh         all of it.

---

## Decisions taken without asking

**`Size` was dropped.** The spec listed it under the positioner. With the
level range normalised to the waveform's range over the picture (which is what
makes Opacity 1 cover the picture from any positioner), a size multiplier on
the waveform is the fader by another name in Edge law, and in Area law it
cancels out of the solve exactly. A *wipe limit* — a control that stops the
wipe short at full fader, which some desks had — would be a different control
and would have to override the Opacity-1 end stop; that is an open question
below, not a slider that does nothing.

**The positioner moves the closed patterns and the clock, not the ramps.** A
positioner on a horizontal wipe would only slide a ramp the fader already
slides, and with the range normalisation it would do nothing at all. The
hardware's positioner was for the box, circle, diamond and clock; so is this
one. The matrix ignores it too, and ignores rotation and aspect: its cells
are on the picture.

**Pixels mean pixels on the horizontal ramp.** Softness, Border Width, Border
Softness and Mod Amount are converted to waveform units through each pattern's
*reference* slope (`Derived::unitsPerPixel`: the ramp, a box's vertical side,
the diamond's axis, the circle's slope where the level is 0.5, the clock's at
a quarter-height radius) and then follow the pattern's own slope. So "16 px"
is exactly 16 on the ramp and on a box's side with Aspect Comp on, and on a
circle it is 16 at one radius and the parabola's answer everywhere else. That
is the idea, not a compromise.

**The fader is `Opacity`, not `Position`.** The spec said `Position`, and until
v0.1.0 that is what it was called, with the note that "if one session in Arena
shows it binds, renaming is a one-line change". genlock's session showed it
binds: in Resolume Arena 7.27.1 a mixer parameter named `Opacity` is driven by
the **layer's opacity fader** — genlock logged `1 → 0.42 → 1` exactly as the
layer's opacity was set — and REST writes to the mixer's own `Opacity` (0.61,
0.13) never reached the plugin. So the fader was renamed, and in Resolume the
layer's own fader drives the wipe, which is the right behaviour for a mixer.
The semantics and the maths are unchanged: 0 is A (the layer below), 1 is B
(this layer), the ends are the same bitwise branch, and Flip-Flop counts
arrivals at the layer fader's ends. The shader's uniform keeps the name
`Position`, which is what the value is to the comparator. Whether the layer's
*transition* or autopilot also moves it has not been tried on any mixer.

**Index 0 is deliberately sacrificial: `Aspect Comp` sits there.** Resolume
Arena 7.27.1 does not expose a mixer's first parameter — measured on genlock,
whose Key Source (id 0) is absent from Arena's mixer panel and REST JSON and so
is stuck at its default. Why is not known. Wipe was built with `Pattern` at
index 0, which in Resolume would have left every wipe a horizontal one. So the
parameter at index 0 must be one whose default is the right value if nobody
can ever change it: `Aspect Comp`, on, which keeps a circle round and a box
square — the ellipse it turns off is a curiosity, not a working setting. It
moved from the end of the Pattern group to the front; every other control
kept its relative order, and the IDs were renumbered, which is harmless for a
plugin that had never been released (nothing persisted them). `wptest --names`
and `verify.sh`'s oxbow step assert the declaration. **Arena must confirm
which parameter it hides**: both checks read what the plugin declares, and the
declaration was right on genlock too. If Arena turns out to hide something
other than id 0, the fix is still only what is declared first. (Arena did
confirm it, on Wipe, 2026-09-23: its mixer panel shows 25 of the 26 declared
parameters, and the missing one is Aspect Comp.)

**Reverse mirrors W within its range**, `wMin + wMax − W`, rather than
swapping A and B: a reversed box *closes* on A rather than opening on B, and a
reversed ramp runs from the other side, which is what the switch did.

**Flip-Flop counts arrivals.** A transition is an arrival at one end from the
other; the first arrival after the plugin is created counts for nothing,
because a transition that never started cannot complete. Turning Flip-Flop
off clears the state.

**The matrix's order is a Feistel permutation**, so the ranks are exactly the
integers 0..N−1 once each, the area below any level is exactly a count, and
the same integer arithmetic runs on the CPU and the GPU. A hash would have
been simpler and would have made "the B area is the fader" false by a random
few percent on every matrix.

**Area law integrates the soft band.** Solving for the *hard* area and then
softening it would leave the visible area off by area″·softW²/12 — tens of
pixels on a box at a soft setting — because a soft box's mixed region is not
symmetric about its edge. The solve uses the mean of the hard area over the
comparator's band, which is exactly the integral of the key. Adaptive
Simpson, because the hard area has kinks where a shape's corner reaches the
picture's edge.

**The modulator runs down the picture** for every pattern but the vertical
wipe, where it runs across, because a sine added to a vertical wipe's own
coordinate does not wobble its edge, it only shifts it.

**No presets.** Twenty-one controls in five groups did not seem to need one,
and the preset machinery is the fleet's largest source of host-behaviour
assumptions. It can be added later without moving anything.

---

## What genlock measured in Arena

genlock v0.1.0 was set as a layer's Blend Mode in **Resolume Arena 7.27.1**
(build 15990) on win-lab — Windows x64, Mesa llvmpipe, no GPU — through Arena's
REST API on 2026-09-23, and its own log read back. What that established about
*any* FFGL mixer, and what Wipe changed because of it:

| Measured on genlock | What Wipe does about it |
|---|---|
| Mixers load from **Extra Effects**; there is no Extra Mixers | Install path, README, CLAUDE, CMake and the release workflow now say Extra Effects |
| Registered as category 2 and chosen in a layer's **Blend Mode** list (the same list serves transitions) | The docs say how to pick it |
| Both inputs arrive **padded** (1280×720 of 1280×768) | Nothing: one MaxUV per input was already the design, and `--mixer` checks it |
| `SetTime` **every frame, in milliseconds**; `SetBeatInfo` every frame | Nothing: the clock already votes its unit; `--pipe` drives milliseconds like Arena |
| A parameter named **`Opacity` is bound to the layer's opacity fader**; REST writes to the mixer's own are overridden | The fader was renamed `Position` → `Opacity` |
| **The mixer's first parameter is not exposed** (genlock's Key Source, id 0) | `Aspect Comp`, default on, moved to index 0 |
| No one-input call observed while the layer below was cleared and refilled | The guards stay; not proven never |
| No mixer frame can be grabbed over REST | Nothing claims a picture inside Resolume |

Wipe's own session, below, re-checked the Extra Effects, Blend Mode, `Opacity`
and first-parameter rows on Wipe itself. The padding and clock rows have not
been re-run with Wipe.

---

## What Wipe showed in Arena

A CI build of v0.1.0 in **Resolume Arena 7.27.1** (build 15990) on win-lab —
Windows x64, Mesa llvmpipe, no GPU — on 2026-09-23. The fleet's Arena gate
cannot gate a mixer, so it was probed by hand over Arena's REST API and read
back from the plugin's own diag log, as genlock was.

- **Loads from Extra Effects.** Arena's log registers it as
  `'SW Wipe' uid: WP01 category: 2`, beside Resolume's own blend modes, and it
  is offered in every layer's Blend Mode list.
- **Initialises.** Set as layer 3's blend mode over a still on layer 2, the
  diag log shows `initialised` on `Mesa llvmpipe ... 4.5 (Core Profile)` and
  `host=Resolume Arena version=7.27.1 15990 loaded from C:\Users\lab\Documents\Resolume Arena\Extra Effects\Wipe.dll`.
  No error lines in either log.
- **The layer's opacity drives `Opacity`.** Setting the layer's opacity to 0.3,
  0.7 and 1.0 read back as the mixer's `Opacity` 0.3, 0.7 and 1.0; a REST write
  of 0.2 to the mixer's own `Opacity` was overridden (read back 1.0, the
  layer's). As designed.
- **Other parameters take REST writes.** Pattern = Circle took.
- **Index 0 is hidden, and it is Aspect Comp.** The mixer panel exposes 25 of
  the 26 declared parameters; the missing one is Aspect Comp, id 0. Second
  mixer to show Arena hiding a mixer's first parameter, after genlock, and
  parking Aspect Comp there worked.
- **Not established:** no frame of the mixer's output was captured (Arena's
  REST does not serve a mixer's picture), so a correct render in Resolume is
  not claimed. Whether a layer transition or the autopilot drives `Opacity`,
  and whether Arena ever calls a mixer with one input, remain open.

---

## What is genuinely verified, and what is assumed

**Verified, by measurement, on this machine (Apple M4 Max, macOS 26.4.1),
2026-09-23**, at 640×360 and 320×180 unless stated — the numbers are in the
table above and in the README's Status. In one line each: two inputs at two
sizes with two MaxUVs resolve independently (0 sentinel pixels, 0.000 of 255,
the marker within one source texel); the five guards hold; Opacity 0 is A
and 1 is B bitwise on all seven patterns with everything on; a hard edge lands
on its column with 0 pixels wrong and a soft one integrates to 0.0000 px; the
circle is round to 0.0011 px and the ellipse has the picture's aspect to four
figures; every pattern's area is the fader to within 0.33 px continuous (0.68
displayed, corner-free); the horizontal soft edge is 12.8000 of 12.8 px and
the circle's follows the parabola to 0.001 px at three radii; the border is 12
columns exactly, integrates to 12.0000 px, and follows the parabola on the
circle; the modulator is a 16.0000 px sine with 0.0000 px residual whose phase
advances as the clock says; Flip-Flop alternates bitwise; all 21 controls
change the picture; the build is universal, exports `plugMain`, ad-hoc signs
and probes as `SW Wipe / WP01 / mixer / inputs 2..2`.

**The render cost**, `wptest --bench`, 120 frames after a 20-frame warm-up,
`glFinish` both sides, the matrix pattern with softness, border and
modulation on (the most arithmetic per pixel):

| | ms/frame, worst of three runs | % of a 60fps frame |
| --- | --- | --- |
| 1280×720 | 0.021 (0.032 in a verify run) | 0.1–0.2% |
| 1920×1080 | 0.035 (0.038) | 0.2% |
| 2560×1440 | 0.052 (0.055) | 0.3% |
| 3840×2160 | 0.107 (0.117) | 0.7% |

As genlock found, a tenth of a millisecond is close to what a `glFinish`
round trip costs to observe; take the ceiling, not the mean. The Area-law
solve, on the CPU, per frame in which something changed: under 0.06 ms for
any single pattern, 2.8 ms for a box at Multiple 8×8 and 7.8 ms for a circle on
a quiet machine — 6.9 and 16.9 ms in a verify run taken while seven other
plugin builds were loading the CPU. Take the ceiling.

**Assumed, or not yet done:**

- **Never loaded into Resolume on macOS.** On Windows it was probed by hand in
  Arena 7.27.1 (above): loading, Blend Mode, the `Opacity` binding and the
  hidden index 0 are measured on Wipe. Still inferred from genlock only: that
  both inputs arrive padded and that the modulator travels on Arena's
  millisecond clock. No mixer's picture has been looked at inside Resolume.
- **The checks have not all held on another rasteriser.** The tolerances are
  derived and the "would this hold" list above is argued. On CI's GPU-less
  macOS runner eight suites pass and `--area` fails at 640×360 (see that list);
  the harness has never run on llvmpipe. The Windows DLL compiles with MSVC in
  CI, and ran in Arena only for the probe above.
- **Premultiplied alpha is assumed.** `mix( a, b, key )` on whatever the host
  hands over; the border is composited opaque.
- **The spec's `Size` is not here**, for the reason above.
- **Area law with modulation on** is the area of the unmodulated waveform;
  the sine's mean over a non-integer number of periods is not accounted for.
- **No OpenFX port and no browser demo.** Neither is required for 0.1.0.

---

## Open questions

1. ~~Does Resolume bind `Opacity`?~~ **Yes, answered on genlock and then on
   Wipe itself** (2026-09-23): the layer's opacity 0.3/0.7/1.0 read back as
   Wipe's `Opacity`, and a write to the mixer's own was overridden. Still open:
   whether a layer transition or autopilot moves it too.
2. **Should there be a wipe limit?** A control that stops the wipe short at
   full fader — a split-screen or a spotlight — is what `Size` would have
   been if it overrode the Opacity-1 end stop. It would break `--ends` for
   that setting on purpose.
3. **Should softness be a fraction of the picture rather than pixels?** The
   hardware's softness was a fraction of the line time. Pixels is what the
   spec asked for, and it means a 4K edge is half the width of a 1080p one.
4. ~~Does a mixer get `SetTime`?~~ **Yes, answered on genlock**: every frame,
   in milliseconds since Arena started. `timing::Clock` votes the unit and
   works frame-relative, so `Mod Speed` should travel in Resolume; not yet
   seen on Wipe.
5. **Is the Area-law solve fast enough on a slow CPU at Multiple 8×8?** 7.8 ms
   here on the render thread; a table per (pattern, multiples) would remove
   it, at the cost of the closed form.
6. **Does Arena ever call a mixer with one input?** Not seen on genlock or on
   Wipe; the guards stay.
7. ~~Why does `--area` fail on the GPU-less runner at 640×360?~~ **Answered:
   the test was wrong, not the plugin.** Apple's software renderer interpolates
   `uv.y` up to 9e-6 off (the GPU: 4e-8), inside the GL spec's 1 part in 10^5,
   and the check had no room for it. See the trap "The software renderer's
   `uv` is good to 1 part in 10^5".

---

## The browser demo

**<https://wipe-demo.stoatworks-labs.com>**, served from `demo/` by this repo's
own Worker (`wrangler.toml`). Built 2026-09-24 on the shared kit
(`stoatworks-backend/resolume-demo`, vendored into `demo/vendor/`), the first
**mixer** in the suite.

**What runs for real.** The plugin's two shaders, `kVertexShader` and
`kWipeShader`, copied into `demo/plugin.js` unedited and compiled by the kit's
`port()` (the version line and precision qualifiers, nothing else) into
WebGL2. `demo/tools/check_shaders.py` compares them with `source/Shaders.cpp`
character for character and `tools/verify.sh` runs it; one changed character in
the comparator fails it (run 2026-09-24). The one escape is the backtick around
`soft` in a comment, which the checker decodes; any other backslash is refused.

**What is a port, checked by nobody but a reader.** `demo/waveform.js` is
Controls.cpp (every `...FromParam`, float32-rounded as the plugin's floats are),
Waveform.cpp (`Derive`, `EdgeLevel`, and the whole Area law: half-plane,
polygon, disc and wedge clipping, the lattice preparation, the adaptive Simpson
band integral and the 60-step bisection) and Timing.cpp's `ModPhase`.
`demo/plugin.js` carries `ProcessOpenGL`'s Flip-Flop state machine and area
cache and sets every uniform the plugin sets. A self-consistency run of the port
in Node (every pattern, 1×1 / 3×2 / 8×8, softness 0 / 32 / 64 px, both
directions) puts `SoftArea(AreaLevel(p))` within 6e-11 of p, except the hard
matrix, which is quantised to one cell of 576 as the C++ is. That is the port
agreeing with itself, not with the C++: no C++-vs-JS comparison exists.
`MatrixRank` is not ported — the Feistel order runs in the shader only, as in
the plugin.

**Decisions taken without asking:**

- **Two inputs from one kit.** The kit hands a demo one input. A (the layer
  below, `inputTextures[0]`) is the kit's clip, relabelled `Clip A`, and the only
  one "Use my own…" replaces. B (this layer) is a second `SourceRenderer` from the
  kit's own `sources.js`, at the same raster and on the same clock, picked by
  the kit's one extra transport dropdown (`demo.variants`, labelled `Clip B`).
  That dropdown exists for "which bundle"; Wipe ships one bundle, and B is
  transport, not a parameter the plugin declares, so it must not be in the
  inspector. Defaults: A colour bars, B the geometry card. Done in the page, not
  the kit, because this is the first mixer; a second one is the moment to teach
  the kit two inputs.
- **Opacity is a slider**, in the Fader group where the plugin declares it. In
  Arena it is the layer's opacity fader and the mixer's own control is
  overridden; the banner and the disclosure say so. There is no auto-fader —
  Resolume's transitions are the host's, not the plugin's.
- **Aspect Comp is shown**, although Arena hides a mixer's parameter 0: a
  browser does not, and hiding it would be inventing a host behaviour.
- **Multiple H / V are dropdowns** of 1..8 (FF_TYPE_INTEGER; the kit has no
  integer control — galvo's answer).
- **Both MaxUVs are 1**: the page's textures are unpadded, so the per-input
  MaxUV the `--mixer` check exists for has nothing to correct here. Disclosed.
  A visitor's own file does arrive at its own size, so the two HalfTexel clamps
  then differ, as they do in Resolume.
- **The lattice Area solve runs in a worker** (`demo/area-worker.js`). A
  Box/Diamond/Circle with a Multiple above 1 and the soft edge up costs the port
  0.6 to 2.4 s a solve in Node on this Mac, and a solve per slider event froze
  the page. The plugin blocks its render thread; the page draws with the last
  level it has until the answer lands, and the stats line says "solving".
  Everything else solves in line and caches on the same inputs as the plugin.
  (How fast the C++ is at 8×8 with softness is its `--bench`'s business; the
  page makes no claim about it.)
- **A statistics line under the picture** reports the level the plugin's code
  chose and the B area read back out of the plugin's own `SoftArea` at that
  level — so Edge law visibly gives an area that is not the fader and Area law
  one that is. The plugin draws no such thing; the disclosure says so.
- **The clock** is the page's, in seconds, straight into `ModPhase`. The plugin's
  unit detection (Resolume sends milliseconds) and frame-relative epoch are host
  plumbing and are not exercised.
- **Absent:** the About block (a web page has links of its own). Wipe has no
  audio path, so nothing is missing for want of one.
- **Presets** are only combinations of the plugin's own parameters.

**Verified 2026-09-24** headlessly (Chrome + SwiftShader through
`stoatworks-backend/release/cdpshot.py`'s Chrome class): no console errors or
warnings; Pattern Horizontal → Circle changes the canvas by a mean 40 levels
against 0.05 for motion alone; Edge law's circle at fader 0.5 reports 74.05 %
B and Area law 50.00 %; Flip-Flop counts transitions and the reversed box
reads 30 % B at a 30 % fader where the forward one reads 16 %. Deploy with
`cf-run npx wrangler deploy` from the repo root, or push to main:
`.github/workflows/deploy.yml` (idler's, added the same day) deploys `demo/`
and checks the live `<head>` is this build.

---

## Notes

Cross-cutting fleet knowledge lives in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes). The mixer
account is genlock's, and belongs there.
