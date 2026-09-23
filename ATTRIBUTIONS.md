# Attributions

Wipe is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### Mixer mechanics, two-input harness and Timing — Stoatworks genlock

<https://github.com/stoatworks-labs/genlock>  
Licence: MIT  
Copyright: Stoatworks Labs

The mixer mechanics of ProcessOpenGL (guard on the input count, guard on each pointer, one MaxUV per input, interleaved scoped bindings), the two-input harness rig and its --mixer check, the host-clock Timing.* and the CMake shape are genlock's, adapted.

### Diagnostics log — Stoatworks tinsel

<https://github.com/stoatworks-labs/tinsel>  
Licence: MIT  
Copyright: Stoatworks Labs

source/Diag.* is tinsel's log, renamed into this namespace.

### Sweep and verify shape — Stoatworks graticule

<https://github.com/stoatworks-labs/graticule>  
Licence: MIT  
Copyright: Stoatworks Labs

tools/sweep.py and the release-job-locally checks in tools/verify.sh follow genlock's and graticule's.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### Analogue vision-mixer wipe generators

Implemented from the published description of how analogue wipe generators worked: ramps and parabolas at line and field rate, combined by addition or non-additive mix, compared against a fader-derived level, with softness as comparator gain, a border as a second comparator and modulation as an added sine. The matrix wipe's cell order is this repo's own (a Feistel permutation), not any vendor's ROM. No schematic, firmware or captured output was used; the look is a model, not a characterisation of anybody's desk.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
