#pragma once

#include "Controls.h"
#include "Waveform.h"

/**
    One frame of the wipe, worked out on the CPU -- with no GL in it, so the
    FFGL plugin and the OpenFX plugin run the same code.

    Everything `Wipe::ProcessOpenGL` used to work out inline before it drew
    lives here now, in three steps, each a plain function of the one before:

        HostValues   the controls as a host holds them: every ranged control
                     0..1, an option as its index, the Multiples as counts.
                     The FFGL plugin fills one from its parameters, the
                     OpenFX plugin from its own at the frame's time.
        Pass         the same frame in physical units -- the generator's
                     frame, the comparators in W units, the modulator's
                     phase, and (separately, see LevelFor) the level.
        Uniforms     what the wipe shader is handed, as floats. The casts
                     from double happen once, here, so the GLSL and its C++
                     mirror below start from the same bits.

    Then the per-pixel half, `Shade`: the wipe shader's main -- the waveform,
    the two comparators and the mix -- written again in C++ for the OpenFX
    build, which renders on the CPU over the host's buffer.

    ------------------------------------------------- the mirror, and its test

    `Shade` and everything under it in Pass.cpp is marked `//= mirrored`, and
    the GLSL it copies says so in the comment above `kWipeShader` in
    Shaders.cpp -- outside the raw string, because the browser demo carries
    that string too and `demo/tools/check_shaders.py` holds the two to the
    character. **Edit both.** A one-sided edit is not left to a reader to
    notice: `wptest --cpu` renders the real plugin on the GPU and `Shade` on
    the CPU from the same inputs at several fader positions and settings, and
    fails on any pixel the two disagree on beyond what the GL spec lets the
    rasteriser's `uv` be off by. verify.sh and CI run it.

    -------------------------------------------------------- what is not here

    The Flip-Flop is the FFGL plugin's alone: it remembers which end the
    fader last rested at, which needs a previous frame, and an OpenFX host
    renders frames alone, out of order and on several threads. The FFGL
    plugin folds its state into `flipped` before it calls BeginPass; the
    OpenFX plugin passes false and offers Reverse. The Area law's cache is
    the FFGL plugin's for the same reason.
*/
namespace wipe
{

/// The controls, as a host holds them. The defaults ARE the plugin's
/// defaults: the FFGL constructor reads them from here and so does the
/// OpenFX describe, so the two builds cannot start from different places.
struct HostValues
{
	float aspectComp = 1.0f;///< on, so a circle is round
	float pattern    = static_cast< float >( PAT_HORIZONTAL );
	float reverse    = 0.0f;

	/// The fader. FFGL `Opacity`, which Resolume binds to the layer's opacity;
	/// OpenFX `Transition`, which a host's transition drives. 0 is A -- the
	/// layer below, the transition's SourceFrom -- and 1 is B, this layer, the
	/// transition's SourceTo. Half way, so a mixer dropped on a layer shows
	/// what it does at once.
	float position = 0.5f;
	float law      = static_cast< float >( LAW_EDGE );

	float softness       = 0.0f;
	float borderWidth    = 0.0f;
	float borderSoftness = 0.0f;
	float borderRed      = 1.0f;
	float borderGreen    = 1.0f;
	float borderBlue     = 1.0f;

	float centreX  = 0.5f;
	float centreY  = 0.5f;
	float rotation = 0.0f;
	float aspect   = 0.5f;///< unity

	float modAmount    = 0.0f;
	float modFrequency = 0.4f;//about 2.6 cycles across the picture
	float modSpeed     = 0.25f;//1 Hz
	float multipleH    = 1.0f;
	float multipleV    = 1.0f;
};

/// One frame in physical units. Doubles, like the rest of the CPU side.
struct Pass
{
	Frame frame;
	Derived derived;
	double position = 0.0;///< the fader, clamped to 0..1
	int law         = LAW_EDGE;

	/// The comparators and the modulator, in W units: pixels on the
	/// horizontal ramp converted through the pattern's reference slope.
	double softW       = 0.0;
	double borderW     = 0.0;
	double borderSoftW = 0.0;
	double modW        = 0.0;
	double modFreq     = 0.0;
	double modPhase    = 0.0;///< cycles, reduced into [0, 1) in double

	/// The comparator level. NOT set by BeginPass: the FFGL plugin caches the
	/// Area law's solve between frames and the OpenFX plugin must not, so
	/// each sets it itself -- the OpenFX plugin through LevelFor.
	double level = 0.0;

	float borderColour[ 3 ] = { 1.0f, 1.0f, 1.0f };
};

/**
    Everything but the level, for an output of outW x outH pixels.

    `flipped` is the FFGL Flip-Flop's state, folded into Reverse.
    `elapsedSeconds` is the clock the modulator's phase is taken from: the
    FFGL plugin's frame-relative host clock, or an OpenFX frame's time.
    `pixelScale` is an OpenFX render scale: Softness, Border Width, Border
    Softness and Mod Amount are pixels of the FULL-resolution output, so a
    half-resolution proxy draws them half as many pixels wide and looks the
    same. `pixelAspect` is the output's pixel aspect ratio (see Frame).
*/
Pass BeginPass( const HostValues& host, bool flipped, int outW, int outH, double elapsedSeconds,
                double pixelScale = 1.0, double pixelAspect = 1.0 );

/// The level for the pass's law, solved afresh: Edge law, or the Area law's
/// bisection over the closed-form soft area.
double LevelFor( const Pass& pass );

/// The wipe shader's uniforms, by the names it declares, as the floats it
/// receives them in.
struct Uniforms
{
	float position = 0.0f;
	float pattern  = 0.0f;
	float reverse  = 0.0f;
	float centre[ 2 ]      = { 0.5f, 0.5f };
	float scaleX           = 1.0f;
	float rotCS[ 2 ]       = { 1.0f, 0.0f };
	float norm             = 1.0f;
	float multiple[ 2 ]    = { 1.0f, 1.0f };
	float wRange[ 2 ]      = { 0.0f, 1.0f };
	float matrixCells[ 2 ] = { 1.0f, 1.0f };

	float level       = 0.0f;
	float softW       = 0.0f;
	float borderW     = 0.0f;
	float borderSoftW = 0.0f;
	float borderColour[ 3 ] = { 1.0f, 1.0f, 1.0f };

	float modW     = 0.0f;
	float modFreq  = 0.0f;
	float modPhase = 0.0f;
};

Uniforms UniformsFor( const Pass& pass );

/**
    The wipe shader's main, in C++: one output pixel from the two inputs at
    that pixel.

    `u`, `v` are the fragment's picture coordinate, 0..1 across the output
    with v UP -- the GL convention, and OpenFX's, whose rows count from the
    bottom too -- at the pixel's centre. `a` and `b` are the two inputs'
    RGBA there (A the layer below / SourceFrom, B this layer / SourceTo),
    and `out` is written RGBA. Float arithmetic throughout, as the GLSL's is.

    //= mirrored -- kWipeShader's main() in Shaders.cpp. Edit both.
*/
void Shade( const Uniforms& un, float u, float v, const float a[ 4 ], const float b[ 4 ], float out[ 4 ] );

} // namespace wipe
