#include "Shaders.h"

namespace wipe
{

//---------------------------------------------------------------------------
// The vertex shader passes UV through UNSCALED. The SDK's mixer example
// folds each input's MaxUV in here; that is only right for a plugin whose
// every fetch is at the fragment's own position, and it is the arrangement
// genlock's --mixer check exists to catch. Here the waveform is a function of
// picture space, and picture space is what the fragment shader gets.
//---------------------------------------------------------------------------
const char* const kVertexShader = R"(#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV;
}
)";

//---------------------------------------------------------------------------
// The pattern generator, the comparators, and the mix.
//---------------------------------------------------------------------------
const char* const kWipeShader = R"(#version 410 core

//inputTextures[0] -- Dest, the layer BELOW: A.
uniform sampler2D TextureA;
//inputTextures[1] -- Src, THIS layer: B, the picture the fader wipes in.
uniform sampler2D TextureB;

//Each input has its own. They are not the same number and there is no
//circumstance in which using one for the other is safe.
uniform vec2 MaxUVA;
uniform vec2 MaxUVB;
uniform vec2 HalfTexelA;
uniform vec2 HalfTexelB;

//The fader: the Opacity parameter, which Resolume drives from the layer's
//opacity fader. 0 is A, 1 is B.
uniform float Position;

//The generator's frame -- see Waveform.h.
uniform float Pattern;   //0 horizontal, 1 vertical, 2 box, 3 diamond, 4 circle, 5 clock, 6 matrix
uniform float Reverse;   //the EFFECTIVE reverse, flip-flop already folded in
uniform vec2 Centre;
uniform float ScaleX;
uniform vec2 RotCS;      //cos, sin of the rotation
uniform float Norm;      //R
uniform vec2 Multiple;   //Multiple H, Multiple V
uniform vec2 WRange;     //the waveform's min and max over the picture
uniform vec2 MatrixCells;//columns, rows

//The comparators, all in W units (converted on the CPU from pixels).
uniform float Level;
uniform float SoftW;
uniform float BorderW;
uniform float BorderSoftW;
uniform vec3 BorderColour;

//The modulator.
uniform float ModW;
uniform float ModFreq;
uniform float ModPhase;//cycles, already reduced into [0,1) on the CPU

in vec2 uv;
out vec4 fragColor;

const float kTwoPi = 6.283185307179586;

//Clamped half a texel inside the used area: GL_LINEAR at the boundary takes
//half its weight from the texture's undrawn padding.
vec4 fetchA( vec2 p )
{
	vec2 q = clamp( p, HalfTexelA, vec2( 1.0 ) - HalfTexelA );
	return texture( TextureA, q * MaxUVA );
}

vec4 fetchB( vec2 p )
{
	vec2 q = clamp( p, HalfTexelB, vec2( 1.0 ) - HalfTexelB );
	return texture( TextureB, q * MaxUVB );
}

//---------------------------------------------------------------- the matrix
//A bijection on [0, count): a Feistel network over the next power of two,
//walked until it lands inside the count. Integer for integer the same as
//MatrixRank in Waveform.cpp, so the CPU's area of "cells below the level"
//is exactly what this draws.
uint mix32( uint x )
{
	x ^= x >> 16;
	x *= 0x7FEB352Du;
	x ^= x >> 15;
	x *= 0x846CA68Bu;
	x ^= x >> 16;
	return x;
}

uint feistel( uint x, uint halfBits )
{
	uint mask = ( 1u << halfBits ) - 1u;
	uint l = x >> halfBits;
	uint r = x & mask;
	for( uint rnd = 0u; rnd < 4u; ++rnd )
	{
		uint f = mix32( r + rnd * 0x9E3779B9u ) & mask;
		uint t = r;
		r = l ^ f;
		l = t;
	}
	return ( l << halfBits ) | r;
}

float matrixRank( vec2 p )
{
	uint cols = uint( MatrixCells.x );
	uint rows = uint( MatrixCells.y );
	uint count = cols * rows;
	uint cx = min( uint( floor( p.x * MatrixCells.x ) ), cols - 1u );
	uint cy = min( uint( floor( p.y * MatrixCells.y ) ), rows - 1u );
	uint index = cy * cols + cx;
	uint bits = 2u;
	while( ( 1u << bits ) < count )
		bits += 2u;
	uint x = index;
	for( int walk = 0; walk < 64; ++walk )
	{
		x = feistel( x, bits / 2u );
		if( x < count )
			break;
	}
	return ( float( x ) + 0.5 ) / float( count );
}

//-------------------------------------------------------------- the waveform
//A ramp that wraps n times across the picture, centred so every copy has
//its own zero: the "multiple" switch on a pattern generator.
float cper( float u, float n )
{
	return n < 1.5 ? u : ( fract( n * u + 0.5 ) - 0.5 ) / n;
}

float waveform( vec2 p )
{
	vec2 d = p - Centre;
	d.x *= ScaleX;
	vec2 q = vec2( RotCS.x * d.x - RotCS.y * d.y, RotCS.y * d.x + RotCS.x * d.y );

	float w;
	if( Pattern < 0.5 )
		w = Multiple.x < 1.5 ? q.x + 0.5 : fract( Multiple.x * ( q.x + 0.5 ) );
	else if( Pattern < 1.5 )
		w = Multiple.y < 1.5 ? q.y + 0.5 : fract( Multiple.y * ( q.y + 0.5 ) );
	else if( Pattern < 4.5 )
	{
		float h = cper( q.x, Multiple.x );
		float v = cper( q.y, Multiple.y );
		if( Pattern < 2.5 )
			w = max( abs( h ), abs( v ) ) / Norm;      //NAM of the two ramps
		else if( Pattern < 3.5 )
			w = ( abs( h ) + abs( v ) ) / Norm;        //the two ramps added
		else
			w = ( h * h + v * v ) / ( Norm * Norm );   //the two parabolas added
	}
	else if( Pattern < 5.5 )
	{
		//Clockwise from twelve. atan( x, y ) is 0 straight up and pi/2 at
		//three o'clock.
		float a = atan( q.x, q.y ) / kTwoPi;
		w = fract( Multiple.x * a );
	}
	else
		w = matrixRank( p );

	if( Reverse > 0.5 )
		w = WRange.x + WRange.y - w;

	//The modulator: a sine added to the waveform, along the edge. Down the
	//picture for every pattern but the vertical wipe, whose edge runs across.
	float along = ( Pattern > 0.5 && Pattern < 1.5 ) ? p.x : p.y;
	w += ModW * sin( kTwoPi * ( ModFreq * along - ModPhase ) );
	return w;
}

//------------------------------------------------------------ the comparator
//A limited-gain comparator: linear between its rails, over a band `soft`
//wide in W, centred on the level. 1 where the waveform is below the level.
float compare( float w, float level, float soft )
{
	if( soft <= 0.0 )
		return w < level ? 1.0 : 0.0;
	return clamp( 0.5 + ( level - w ) / soft, 0.0, 1.0 );
}

void main()
{
	//The fader's ends are pure fetches. At 0 the layer below is untouched;
	//at 1 this layer is. Nothing else is evaluated there, so neither a soft
	//edge nor a border can leak into an end stop.
	if( Position <= 0.0 )
	{
		fragColor = fetchA( uv );
		return;
	}
	if( Position >= 1.0 )
	{
		fragColor = fetchB( uv );
		return;
	}

	float w = waveform( uv );

	float key = compare( w, Level, SoftW );

	//The border is a second comparator, a little above the level, and the
	//border is where the two disagree. It is OUT OF CIRCUIT at zero width:
	//two comparators of different gain at the same level disagree across
	//the whole soft edge, and a border width of nothing must mean no
	//border, not a hard comparator's opinion of a soft one. Found by
	//--edge, as a 0.2 px shift of every soft edge towards A.
	float border = 0.0;
	if( BorderW > 0.0 )
	{
		float outer = compare( w, Level + BorderW, BorderSoftW );
		border = max( outer - key, 0.0 );
	}

	vec4 a = fetchA( uv );
	vec4 b = fetchB( uv );
	vec4 mixed = mix( a, b, key );
	fragColor = mix( mixed, vec4( BorderColour, 1.0 ), border );
}
)";

} // namespace wipe
