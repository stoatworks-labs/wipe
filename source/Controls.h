#pragma once

/**
    Host parameters are 0..1; these are what they mean.

    Every ranged parameter this plugin declares is a plain FF_TYPE_STANDARD
    float in 0..1, including the ones that stand for pixels, degrees or
    cycles per second. That is not a style preference: `SetParamInfo` clamps
    a standard default into 0..1 *before* returning, and `SetParamRange` can
    only be called afterwards -- so a parameter declared in pixels cannot
    declare a default in pixels. The conversions live here, in one file the
    plugin and the harness both use, so there is only ever one answer to what
    a slider position means. The two exceptions are `Multiple H` and
    `Multiple V`, which are FF_TYPE_INTEGER and hold real integers, because
    that type is exempt from the clamp.

    ---------------------------------------------------------- the pixel unit

    Softness, Border Width, Border Softness and Mod Amount are all stated in
    **output pixels, measured on the horizontal ramp** -- the one pattern
    whose waveform has a known, constant slope across the picture. On every
    other pattern the same number is converted into the comparator's own
    units through the pattern's reference slope (see Waveform.h), and the
    width you actually see follows the waveform's slope from there. That is
    the plugin's whole idea and it is deliberate: a circle's edge really is
    softer near the middle than a box's, because the parabola is flatter
    there.
*/
namespace wipe
{

/// The patterns, in the order the option parameter declares them.
enum Pattern : int
{
	PAT_HORIZONTAL = 0,
	PAT_VERTICAL   = 1,
	PAT_BOX        = 2,
	PAT_DIAMOND    = 3,
	PAT_CIRCLE     = 4,
	PAT_CLOCK      = 5,
	PAT_MATRIX     = 6,
	PAT_COUNT      = 7
};

/// The fader law, in declaration order.
enum Law : int
{
	LAW_EDGE  = 0,///< the level is linear in the fader, as the hardware's was
	LAW_AREA  = 1,///< the level is chosen so the B area is exactly the fader
	LAW_COUNT = 2
};

/// The most copies of the waveform Multiple H / Multiple V will run.
inline constexpr int kMultipleMax = 8;

/// The matrix wipe's base grid, before Multiple H / Multiple V multiply it.
/// 32 x 18 puts square-ish cells on a 16:9 picture and divides both 640x360
/// and 320x180 -- the two rasters the harness measures at -- into whole
/// pixels, which is what lets `--area` count the matrix's cells exactly.
inline constexpr int kMatrixBaseCols = 32;
inline constexpr int kMatrixBaseRows = 18;

/// 0 to 64 output pixels, linear. The comparator's soft edge, measured on
/// the horizontal ramp. 0 is a hard cut.
float SoftnessPxFromParam( float value );

/// 0 to 64 output pixels, linear, on the horizontal ramp. The border is a
/// second comparator this far above the level.
float BorderWidthPxFromParam( float value );

/// 0 to 64 output pixels, linear, on the horizontal ramp. The border's own
/// comparator softness.
float BorderSoftnessPxFromParam( float value );

/// 0 to 360 degrees, in radians. Rotates the waveform generator's frame.
float RotationRadiansFromParam( float value );

/// 0.25 to 4, geometrically, with 0.5 as unity. Stretches the pattern
/// horizontally: the aspect knob beside a real positioner.
float AspectFromParam( float value );

/// 0 to 64 output pixels, linear, on the horizontal ramp. The modulator's
/// amplitude: how far the sine pushes the edge.
float ModAmountPxFromParam( float value );

/// 0.5 to 32 cycles across the picture, geometrically. The modulator's
/// frequency.
float ModFrequencyFromParam( float value );

/// 0 to 4 cycles per second, linear. How fast the modulator's sine travels
/// along the edge.
float ModSpeedHzFromParam( float value );

/// A Multiple parameter, as the integer it holds: 1..kMultipleMax.
int MultipleFromParam( float value );

/// The slider position for a pixel count on the linear 0..64 controls.
float PxParamFor( double px );

} // namespace wipe
