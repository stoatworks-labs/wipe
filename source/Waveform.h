#pragma once

#include "Controls.h"

#include <vector>

/**
    The pattern generator's frame, and everything the CPU works out for it.

    A 1970s vision mixer made its wipes live from a few waveform generators:
    a horizontal ramp, a vertical ramp, and the horizontal and vertical
    parabolas, combined in a small analogue matrix -- added, or the larger
    taken with a non-additive mix (NAM) -- and compared against the fader's
    level. Every pattern here is a formula in those waveforms, and the shader
    evaluates it per pixel. What this file does is the part a generator's
    control panel did: it settles the FRAME the waveforms run in (where zero
    is, which way is up, how the two axes are scaled against each other), it
    works out the waveform's RANGE over the picture so the fader's ends mean
    A and B, and in Area law it solves for the level that gives the fader's
    area exactly.

    ------------------------------------------------------------ the frame

    Every pattern is evaluated in `q`, a rotated and scaled copy of the
    picture coordinate:

        d = uv - centre
        d.x *= ScaleX               (aspect compensation x pattern aspect)
        q   = rotate( d, Rotation )

    With Aspect Comp on, ScaleX carries the picture's own aspect, so one unit
    of q is one picture HEIGHT in both axes and a circle in q is a circle on
    screen. With it off, q is the unit square and the same circle is the
    ellipse an uncompensated generator drew.

    The centre is the positioner for the closed patterns and the clock. The
    ramps and the matrix ignore it, as the hardware's did: a positioner on a
    simple horizontal wipe would only slide a ramp that the fader already
    slides.

    ---------------------------------------------------------- the patterns

    H and V below are the ramps in q; with Multiple N >= 2 they wrap N times
    across the picture (a sawtooth for the ramps, a centred one for the
    closed patterns so each copy has its own centre).

        Horizontal   W = H                      H = q.x + 0.5
        Vertical     W = V                      V = q.y + 0.5
        Box          W = NAM( |H|, |V| ) / R    = max( |q.x|, |q.y| ) / R
        Diamond      W = ( |H| + |V| ) / R
        Circle       W = ( H^2 + V^2 ) / R^2    the two parabolas, added
        Clock        W = angle( q ) / 2pi       clockwise from twelve
        Matrix       W = rank of the cell       a per-cell threshold order

    R is the waveform's normaliser: the distance of the farthest picture
    corner from the centre in the pattern's own norm, so W runs 0..1 from the
    centre to that corner and the fader's end covers the picture. With
    Multiple on, R is the half-cell instead, so every copy fills its cell.

    The comparator then makes the key: B where W is below the level, A above
    it. `Reverse` mirrors W within its own range, so a box closes instead of
    opening and a ramp runs the other way.
*/
namespace wipe
{

/// The generator's settings, in physical units. What the plugin's parameters
/// become once Controls.h has converted them.
struct Frame
{
	int pattern      = PAT_HORIZONTAL;
	bool reverse     = false;
	bool aspectComp  = true;
	double centreX   = 0.5;
	double centreY   = 0.5;
	double rotation  = 0.0;///< radians
	double aspect    = 1.0;///< the pattern's own x stretch
	int multipleH    = 1;
	int multipleV    = 1;
	double softnessPx = 0.0;///< on the horizontal ramp; see Controls.h
	int outW         = 1280;
	int outH         = 720;
};

/// Everything the shader is handed about the frame, derived from a Frame.
struct Derived
{
	double centreX = 0.5, centreY = 0.5;///< 0.5 for the patterns that ignore the positioner
	double scaleX  = 1.0;
	double cosR    = 1.0, sinR = 0.0;
	double norm    = 1.0;///< R
	double wMin    = 0.0, wMax = 1.0;///< the waveform's range over the picture
	/// W units per output pixel at the pattern's reference slope. Softness,
	/// Border Width and Mod Amount in pixels become W units through this.
	double unitsPerPixel = 1.0;
	int matrixCols = kMatrixBaseCols, matrixRows = kMatrixBaseRows;
};

Derived Derive( const Frame& frame );

/// The comparator level in Edge law: linear in the fader, from the
/// waveform's minimum over the picture to its maximum.
double EdgeLevel( const Derived& d, double position );

/// The closed-form area of B -- the fraction of the picture where W < level
/// -- for the frame, with a hard comparator. This is the CPU statement of
/// the geometry, and the thing Area law inverts.
double HardArea( const Frame& frame, const Derived& d, double level );

/// The area the SOFT comparator produces: the mean of HardArea over the
/// comparator's band [level - soft/2, level + soft/2], where `softW` is the
/// softness in W units. With a linear-clip comparator that mean IS the
/// integral of the key over the picture, so this is what an operator sees
/// as "the B area". Reduces to HardArea when softW is 0.
double SoftArea( const Frame& frame, const Derived& d, double level, double softW );

/// Area law: the level at which SoftArea equals `position`, found by
/// bisection on a function that is monotone by construction. Exact to
/// `kAreaSolveTolerance` in level.
double AreaLevel( const Frame& frame, const Derived& d, double position, double softW );

inline constexpr double kAreaSolveTolerance = 1e-10;

/// The matrix's cell order. A bijection on [0, count) -- every rank used
/// exactly once -- built from a Feistel network over the next power of two
/// with cycle walking, so the CPU and the GLSL agree to the bit (integer
/// arithmetic on both sides) and the area is exactly the count of cells
/// below the level. The GLSL copy is in Shaders.cpp.
unsigned MatrixRank( unsigned index, unsigned count );

/// Same, as the 0..1 threshold the shader compares: (rank + 0.5) / count.
double MatrixThreshold( unsigned index, unsigned count );

} // namespace wipe
