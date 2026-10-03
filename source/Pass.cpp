#include "Pass.h"

#include "Timing.h"

#include <algorithm>
#include <cmath>

namespace wipe
{
namespace
{
int optionIndex( float value, int count )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}
} // namespace

//---------------------------------------------------------------------------
// The CPU half of a frame. This was the top of Wipe::ProcessOpenGL, moved
// here unchanged in arithmetic: with flipped from the Flip-Flop, a pixel
// scale and a pixel aspect of exactly 1, every number below is the one the
// FFGL plugin computed before it moved, to the bit.
//---------------------------------------------------------------------------
Pass BeginPass( const HostValues& host, bool flipped, int outW, int outH, double elapsedSeconds, double pixelScale,
                double pixelAspect )
{
	Pass pass;
	pass.position = std::clamp( static_cast< double >( host.position ), 0.0, 1.0 );
	pass.law      = optionIndex( host.law, LAW_COUNT );

	//The generator's frame, in physical units.
	Frame& frame      = pass.frame;
	frame.pattern     = optionIndex( host.pattern, PAT_COUNT );
	frame.reverse     = ( host.reverse > 0.5f ) != flipped;
	frame.aspectComp  = host.aspectComp > 0.5f;
	frame.centreX     = std::clamp( host.centreX, 0.0f, 1.0f );
	frame.centreY     = std::clamp( host.centreY, 0.0f, 1.0f );
	frame.rotation    = RotationRadiansFromParam( host.rotation );
	frame.aspect      = AspectFromParam( host.aspect );
	frame.multipleH   = MultipleFromParam( host.multipleH );
	frame.multipleV   = MultipleFromParam( host.multipleV );
	frame.softnessPx  = SoftnessPxFromParam( host.softness ) * pixelScale;
	frame.outW        = outW;
	frame.outH        = outH;
	frame.pixelAspect = pixelAspect > 0.0 ? pixelAspect : 1.0;

	pass.derived = Derive( frame );

	//Pixels on the horizontal ramp become W units through the pattern's
	//reference slope; every other pattern's edge follows its own slope from
	//there.
	const double unitsPerPixel = pass.derived.unitsPerPixel;
	pass.softW       = frame.softnessPx * unitsPerPixel;
	pass.borderW     = BorderWidthPxFromParam( host.borderWidth ) * pixelScale * unitsPerPixel;
	pass.borderSoftW = BorderSoftnessPxFromParam( host.borderSoftness ) * pixelScale * unitsPerPixel;
	pass.modW        = ModAmountPxFromParam( host.modAmount ) * pixelScale * unitsPerPixel;
	pass.modFreq     = ModFrequencyFromParam( host.modFrequency );
	pass.modPhase    = timing::ModPhase( elapsedSeconds, ModSpeedHzFromParam( host.modSpeed ) );

	pass.borderColour[ 0 ] = host.borderRed;
	pass.borderColour[ 1 ] = host.borderGreen;
	pass.borderColour[ 2 ] = host.borderBlue;
	return pass;
}

double LevelFor( const Pass& pass )
{
	if( pass.law == LAW_AREA )
		return AreaLevel( pass.frame, pass.derived, pass.position, pass.softW );
	return EdgeLevel( pass.derived, pass.position );
}

Uniforms UniformsFor( const Pass& pass )
{
	const Frame& frame = pass.frame;
	const Derived& d   = pass.derived;

	Uniforms u;
	u.position         = static_cast< float >( pass.position );
	u.pattern          = static_cast< float >( frame.pattern );
	u.reverse          = frame.reverse ? 1.0f : 0.0f;
	u.centre[ 0 ]      = static_cast< float >( d.centreX );
	u.centre[ 1 ]      = static_cast< float >( d.centreY );
	u.scaleX           = static_cast< float >( d.scaleX );
	u.rotCS[ 0 ]       = static_cast< float >( d.cosR );
	u.rotCS[ 1 ]       = static_cast< float >( d.sinR );
	u.norm             = static_cast< float >( d.norm );
	u.multiple[ 0 ]    = static_cast< float >( frame.multipleH );
	u.multiple[ 1 ]    = static_cast< float >( frame.multipleV );
	u.wRange[ 0 ]      = static_cast< float >( d.wMin );
	u.wRange[ 1 ]      = static_cast< float >( d.wMax );
	u.matrixCells[ 0 ] = static_cast< float >( d.matrixCols );
	u.matrixCells[ 1 ] = static_cast< float >( d.matrixRows );

	u.level       = static_cast< float >( pass.level );
	u.softW       = static_cast< float >( pass.softW );
	u.borderW     = static_cast< float >( pass.borderW );
	u.borderSoftW = static_cast< float >( pass.borderSoftW );
	for( int c = 0; c < 3; ++c )
		u.borderColour[ c ] = pass.borderColour[ c ];

	u.modW     = static_cast< float >( pass.modW );
	u.modFreq  = static_cast< float >( pass.modFreq );
	u.modPhase = static_cast< float >( pass.modPhase );
	return u;
}

//---------------------------------------------------------------------------
// The per-pixel half, mirrored from kWipeShader in Shaders.cpp.
//
// Every function below is the GLSL function of the same name, statement for
// statement, in float, using the GLSL built-ins' own definitions: fract is
// x - floor(x), mix is x(1-a) + ya, clamp is min(max()). The matrix's
// Feistel order is not copied a third time -- MatrixRank in Waveform.cpp is
// already the GLSL's integer for integer, and is what the CPU's Area law
// counts cells with.
//
// wptest --cpu holds this to the GPU. A change here without the same change
// in the shader (or the other way round) fails it.
//---------------------------------------------------------------------------
namespace
{
constexpr float kTwoPi = 6.283185307179586f;//= mirrored

float fract( float x )
{
	return x - std::floor( x );
}

float mix( float x, float y, float a )
{
	return x * ( 1.0f - a ) + y * a;
}

float matrixRank( const Uniforms& un, float px, float py )
{
	//= mirrored
	const unsigned cols  = static_cast< unsigned >( un.matrixCells[ 0 ] );
	const unsigned rows  = static_cast< unsigned >( un.matrixCells[ 1 ] );
	const unsigned count = cols * rows;
	//GLSL's uint() of a negative float is undefined and C++'s is undefined
	//behaviour; a picture coordinate is never negative, but say so.
	const unsigned cx = std::min( static_cast< unsigned >( std::max( std::floor( px * un.matrixCells[ 0 ] ), 0.0f ) ), cols - 1u );
	const unsigned cy = std::min( static_cast< unsigned >( std::max( std::floor( py * un.matrixCells[ 1 ] ), 0.0f ) ), rows - 1u );
	const unsigned index = cy * cols + cx;
	return ( static_cast< float >( MatrixRank( index, count ) ) + 0.5f ) / static_cast< float >( count );
}

float cper( float u, float n )
{
	//= mirrored
	return n < 1.5f ? u : ( fract( n * u + 0.5f ) - 0.5f ) / n;
}

float waveform( const Uniforms& un, float px, float py )
{
	//= mirrored
	float dx = px - un.centre[ 0 ];
	const float dy = py - un.centre[ 1 ];
	dx *= un.scaleX;
	const float qx = un.rotCS[ 0 ] * dx - un.rotCS[ 1 ] * dy;
	const float qy = un.rotCS[ 1 ] * dx + un.rotCS[ 0 ] * dy;

	float w;
	if( un.pattern < 0.5f )
		w = un.multiple[ 0 ] < 1.5f ? qx + 0.5f : fract( un.multiple[ 0 ] * ( qx + 0.5f ) );
	else if( un.pattern < 1.5f )
		w = un.multiple[ 1 ] < 1.5f ? qy + 0.5f : fract( un.multiple[ 1 ] * ( qy + 0.5f ) );
	else if( un.pattern < 4.5f )
	{
		const float h = cper( qx, un.multiple[ 0 ] );
		const float v = cper( qy, un.multiple[ 1 ] );
		if( un.pattern < 2.5f )
			w = std::max( std::fabs( h ), std::fabs( v ) ) / un.norm;//NAM of the two ramps
		else if( un.pattern < 3.5f )
			w = ( std::fabs( h ) + std::fabs( v ) ) / un.norm;//the two ramps added
		else
			w = ( h * h + v * v ) / ( un.norm * un.norm );//the two parabolas added
	}
	else if( un.pattern < 5.5f )
	{
		//Clockwise from twelve. GLSL's atan( y, x ) is atan2, so atan( q.x,
		//q.y ) is 0 straight up and pi/2 at three o'clock.
		const float a = std::atan2( qx, qy ) / kTwoPi;
		w             = fract( un.multiple[ 0 ] * a );
	}
	else
		w = matrixRank( un, px, py );

	if( un.reverse > 0.5f )
		w = un.wRange[ 0 ] + un.wRange[ 1 ] - w;

	//The modulator, down the picture for every pattern but the vertical wipe.
	const float along = ( un.pattern > 0.5f && un.pattern < 1.5f ) ? px : py;
	w += un.modW * std::sin( kTwoPi * ( un.modFreq * along - un.modPhase ) );
	return w;
}

float compare( float w, float level, float soft )
{
	//= mirrored
	if( soft <= 0.0f )
		return w < level ? 1.0f : 0.0f;
	return std::min( std::max( 0.5f + ( level - w ) / soft, 0.0f ), 1.0f );
}
} // namespace

void Shade( const Uniforms& un, float u, float v, const float a[ 4 ], const float b[ 4 ], float out[ 4 ] )
{
	//= mirrored
	//The fader's ends are pure fetches: nothing else is evaluated there, so
	//neither a soft edge nor a border can leak into an end stop.
	if( un.position <= 0.0f )
	{
		for( int c = 0; c < 4; ++c )
			out[ c ] = a[ c ];
		return;
	}
	if( un.position >= 1.0f )
	{
		for( int c = 0; c < 4; ++c )
			out[ c ] = b[ c ];
		return;
	}

	const float w   = waveform( un, u, v );
	const float key = compare( w, un.level, un.softW );

	//The border: a second comparator a little above the level, out of
	//circuit at zero width.
	float border = 0.0f;
	if( un.borderW > 0.0f )
	{
		const float outer = compare( w, un.level + un.borderW, un.borderSoftW );
		border            = std::max( outer - key, 0.0f );
	}

	for( int c = 0; c < 4; ++c )
	{
		const float mixed  = mix( a[ c ], b[ c ], key );
		const float colour = c < 3 ? un.borderColour[ c ] : 1.0f;
		out[ c ]           = mix( mixed, colour, border );
	}
}

} // namespace wipe
