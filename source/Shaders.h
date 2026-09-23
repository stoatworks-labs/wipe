#pragma once

/**
    The pass, as GLSL. There is only one.

    A wipe is one decision per pixel -- A or B, with a border between -- so
    nothing here needs a buffer of its own, and this repo carries no
    `PassBuffer`. What it does need is **two inputs**: `TextureA` is the
    layer below (Dest, `inputTextures[0]`) and `TextureB` is this layer (Src,
    `inputTextures[1]`). The fader wipes B in over A.

    The two can be **different resolutions**, and each therefore has its own
    `MaxUV` and its own half-texel inset. Getting one of those wrong is
    silent: the picture still appears, at the wrong scale, or with a band of
    a neighbouring clip's texture memory down one side. The vertex shader
    passes UV through unscaled and each fetch applies its own MaxUV once,
    exactly as genlock does and for the same reason.

    The waveform itself is evaluated in picture space, 0..1 across the
    output, so a pattern is the same shape whatever the two inputs' rasters
    are.
*/

namespace wipe
{

extern const char* const kVertexShader;
extern const char* const kWipeShader;

} // namespace wipe
