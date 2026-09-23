# Attributions

Wipe is built on other people's work. This file lists what that work is, who
did it, and what it is doing here.

> **Provisional.** Across the fleet this file is generated from master lists in
> `stoatworks-backend` by `scripts/sync-attributions.py`. Wipe is not in that
> script's lists at all yet — it is a new repo — so this copy is hand-written,
> adapted from genlock's; v0.1.0 ships that way. Registering the project and
> re-running the sync is the fix — and note that the script's `--only` flag
> truncates the file rather than filtering it.

## Third-party code this project uses

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>
Licence: BSD-3-Clause
Copyright: FreeFrame

Vendored as a git submodule at `external/ffgl`, pinned to `b1afaf9`.

The plugin ABI itself. An FFGL plugin is defined by this SDK's headers — there
is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Windows only, from vcpkg, statically linked. The SDK's headers pull it in for
the OpenGL function pointers; macOS uses the system OpenGL framework instead.

### zlib

<https://zlib.net>
Licence: zlib
Copyright: Jean-loup Gailly and Mark Adler

Ships with macOS. Linked by the offline harness only, which writes its PNGs
with it rather than carrying an image library.

## Work from elsewhere in the fleet

### genlock, tinsel, graticule

<https://github.com/stoatworks-labs>
Licence: MIT
Copyright: Stoatworks Labs

genlock is the primary template: the mixer mechanics of `ProcessOpenGL` (guard
on the input count, guard on each pointer, one `MaxUV` per input, interleaved
scoped bindings), the two-input harness rig and its `--mixer` check, the
host-clock `Timing.*` and the CMake shape are its, adapted. `source/Diag.*` is
tinsel's, renamed into this namespace. `tools/sweep.py` and the
release-job-locally checks in `tools/verify.sh` follow genlock's and
graticule's. The About header and `StoatworksAboutLinks.h` are hand copies of
the generated ones, with `guide=""` because no user guide exists.

## Method

The pattern generator modelled here is implemented from the published
description of how analogue wipe generators worked — ramps and parabolas at
line and field rate, combined by addition or non-additive mix, compared against
a fader-derived level, with softness as comparator gain, a border as a second
comparator and modulation as an added sine. The matrix wipe's cell order is
this repo's own (a Feistel permutation), not any vendor's ROM. No vendor's
schematic, firmware or captured output was used, and nothing was measured off
real hardware — there is none here. The look is a model, not a characterisation
of anybody's desk.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or
you would rather not be listed — open an issue and it will be fixed.
