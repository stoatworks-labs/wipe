/**
    wptest -- render Wipe offline, and measure what the pattern generator
    is doing.

    A harness that drives **two inputs**, because Wipe is a mixer.
    `inputTextures[0]` is Dest, the layer below -- A. `inputTextures[1]` is
    Src, this layer -- B, the picture the fader wipes in. They are
    `--input-a` and `--input-b` here, and they may be different sizes, with
    different hardware padding, rendered to an output that is a third size.

        wptest --out /tmp/frame.png     a picture, through the real plugin
        wptest --list                   every parameter, for the sweep
        wptest --names                  no name over FFGL's 16 characters
        wptest --mixer                  two inputs, two MaxUVs, and the guards
        wptest --ends                   Opacity 0 IS A and 1 IS B, every pattern
        wptest --edge                   the edge sits where Edge law says
        wptest --area                   the B area IS the fader, in Area law
        wptest --softness               the edge width follows the waveform's slope
        wptest --border                 so does the border's
        wptest --modulation             the edge wobble is the stated sine
        wptest --flipflop               alternate transitions reverse
        wptest --bench                  ms/frame at 720p through 4K
        wptest --pipe                   raw frames in, raw frames out (two inputs)

    WPTEST_RENDERER=software in the environment renders on Apple's software
    renderer instead of the GPU: what a GPU-less CI runner gets.

    `--pipe` is the fleet's frame format with a second input. stdin is Dest,
    the layer below (A); `--pipe-src` is Src, this layer (B), from a file or
    FIFO:

        ffmpeg -i below.mov -f rawvideo -pix_fmt rgba - \
          | wptest --pipe --size 1920x1080 --pipe-src above.fifo \
                   [--src-size WxH] [--fps N] [--script cues.txt] \
          | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -i - out.mov

    Every check renders through the REAL plugin class in a headless CGL
    context and measures the property out of the picture. What is compared
    against is a closed form -- a column number, an area, a square root --
    never a transcription of the shader.

    ------------------------------------------------------- about the numbers

    Every tolerance is derived from something physical -- one output pixel,
    one float ULP through the comparator, one cell -- and never from the
    number this machine printed first. AGENTS.md lists them. Two rules:

      * A hard edge that lands on a pixel boundary is a byte-equality claim.
        A soft edge's position is recovered by INTEGRATING the key across
        the transition, which is a linear functional and so translates
        exactly; a 50% crossing found by interpolation is used only where
        the key is linear (a ramp) or its curvature is bounded and stated.
      * Area sums are read back through a FLOAT framebuffer. An 8-bit
        readback quantises every partial pixel to 1/255 and, on an
        axis-aligned edge, does it identically on every row, so the error
        is systematic rather than random and is not "within a pixel".
*/

#include "Controls.h"
#include "Shaders.h"
#include "Timing.h"
#include "Waveform.h"
#include "Wipe.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace wipe;

namespace
{
int failures = 0;

void Check( bool ok, const std::string& what )
{
	std::printf( "  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str() );
	if( !ok )
		++failures;
}

/// A negative control: something the check must be able to REJECT. Passes
/// when `rejected` is true.
void Negative( bool rejected, const std::string& what )
{
	std::printf( "  %s  negative control: %s\n", rejected ? "ok  " : "FAIL", what.c_str() );
	if( !rejected )
		++failures;
}

/// EVERY conversion must be a floating-point one -- see genlock.
__attribute__( ( format( printf, 1, 0 ) ) ) std::string fmt( const char* format, double a, double b = 0.0, double c = 0.0, double d = 0.0 )
{
	char buffer[ 320 ];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
	std::snprintf( buffer, sizeof( buffer ), format, a, b, c, d );
#pragma clang diagnostic pop
	return buffer;
}

//---------------------------------------------------------------------------
// PNG writer. zlib ships with the OS. Rows BOTTOM-UP in, top-down out.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

using Image  = std::vector< unsigned char >;
using ImageF = std::vector< float >;

bool writePng( const std::string& path, int width, int height, const Image& bottomUp )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = height - 1; y >= 0; --y )
	{
		raw.push_back( 0 );
		const unsigned char* row = bottomUp.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}
	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );
	ihdr.push_back( 6 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// Pictures. Bottom-up, GL's orientation.
//---------------------------------------------------------------------------
struct Rgb
{
	unsigned char r, g, b;
};

unsigned char toByte( float v )
{
	return static_cast< unsigned char >( std::lround( std::min( 1.0f, std::max( 0.0f, v ) ) * 255.0f ) );
}

constexpr Rgb kBlack = { 0, 0, 0 };
constexpr Rgb kWhite = { 255, 255, 255 };

void put( Image& image, int width, int x, int y, Rgb c, unsigned char a = 255 )
{
	const size_t at = ( static_cast< size_t >( y ) * width + x ) * 4;
	image[ at + 0 ] = c.r;
	image[ at + 1 ] = c.g;
	image[ at + 2 ] = c.b;
	image[ at + 3 ] = a;
}

Rgb pixelAt( const Image& image, int width, int x, int y )
{
	const size_t at = ( static_cast< size_t >( y ) * width + x ) * 4;
	return { image[ at + 0 ], image[ at + 1 ], image[ at + 2 ] };
}

Image flatField( int width, int height, Rgb c )
{
	Image image( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
			put( image, width, x, y, c );
	return image;
}

void hsv( float h, float s, float v, float& r, float& g, float& b )
{
	const float c = v * s;
	const float x = c * ( 1.0f - std::fabs( std::fmod( h * 6.0f, 2.0f ) - 1.0f ) );
	const float m = v - c;
	float rr = 0, gg = 0, bb = 0;
	const int sector = static_cast< int >( h * 6.0f ) % 6;
	switch( sector )
	{
	case 0: rr = c; gg = x; break;
	case 1: rr = x; gg = c; break;
	case 2: gg = c; bb = x; break;
	case 3: gg = x; bb = c; break;
	case 4: rr = x; bb = c; break;
	default: rr = c; bb = x; break;
	}
	r = rr + m;
	g = gg + m;
	b = bb + m;
}

/// A: bars across the top, a grey ramp, a hue field.
Image videoCard( int width, int height )
{
	Image image( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const float u = ( x + 0.5f ) / width;
			const float v = ( y + 0.5f ) / height;
			float r = 0, g = 0, b = 0;
			if( v > 0.72f )
			{
				static const float bars[ 7 ][ 3 ] = {
					{ 0.75f, 0.75f, 0.75f }, { 0.75f, 0.75f, 0.0f }, { 0.0f, 0.75f, 0.75f }, { 0.0f, 0.75f, 0.0f },
					{ 0.75f, 0.0f, 0.75f }, { 0.75f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.75f },
				};
				const int which = std::min( 6, static_cast< int >( u * 7.0f ) );
				r = bars[ which ][ 0 ];
				g = bars[ which ][ 1 ];
				b = bars[ which ][ 2 ];
			}
			else if( v > 0.58f )
				r = g = b = u;
			else
				hsv( u, 0.8f, 0.3f + 0.65f * ( v / 0.58f ), r, g, b );
			put( image, width, x, y, { toByte( r ), toByte( g ), toByte( b ) } );
		}
	return image;
}

/// B: a warm picture with diagonal stripes, a disc and a dark panel, so a
/// wipe has something recognisably different to reveal.
Image graphicCard( int width, int height )
{
	Image image( static_cast< size_t >( width ) * height * 4 );
	const double cx = 0.62 * width, cy = 0.45 * height, rad = 0.22 * height;
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const double u = ( x + 0.5 ) / width, v = ( y + 0.5 ) / height;
			Rgb c;
			const int stripe = static_cast< int >( std::floor( ( u * 1.6 + v ) * 8.0 ) ) & 1;
			c = stripe ? Rgb { 0xF2, 0x8C, 0x28 } : Rgb { 0xF7, 0xC5, 0x6B };
			if( u < 0.3 && v > 0.15 && v < 0.85 )
				c = { 0x22, 0x1E, 0x38 };
			const double dx = ( x + 0.5 ) - cx, dy = ( y + 0.5 ) - cy;
			if( dx * dx + dy * dy < rad * rad )
				c = { 0x2E, 0x8B, 0xC0 };
			put( image, width, x, y, c );
		}
	return image;
}

/// Four quadrants of flat colour and one marker square, for `--mixer`.
struct QuadCard
{
	Image image;
	Rgb quadrant[ 4 ];
	Rgb marker;
	double markerCentreU = 0.0;
	double markerCentreV = 0.0;
};

QuadCard quadCard( int width, int height, bool srcSide )
{
	QuadCard card;
	if( srcSide )
	{
		card.quadrant[ 0 ] = { 0x00, 0xB4, 0xB4 };
		card.quadrant[ 1 ] = { 0xB4, 0xB4, 0x00 };
		card.quadrant[ 2 ] = { 0x3C, 0x78, 0xFF };
		card.quadrant[ 3 ] = { 0x60, 0x60, 0x60 };
		card.marker        = { 0xA0, 0xFF, 0x00 };
	}
	else
	{
		card.quadrant[ 0 ] = { 0xC8, 0x00, 0x00 };
		card.quadrant[ 1 ] = { 0x00, 0xC8, 0x00 };
		card.quadrant[ 2 ] = { 0x00, 0x00, 0xC8 };
		card.quadrant[ 3 ] = { 0xF0, 0xE0, 0xC0 };
		card.marker        = { 0xFF, 0x80, 0x00 };
	}
	card.image = Image( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const int q = ( y >= height / 2 ? 2 : 0 ) + ( x >= width / 2 ? 1 : 0 );
			put( card.image, width, x, y, card.quadrant[ q ] );
		}
	const int x0 = static_cast< int >( std::lround( 0.1875 * width ) );
	const int x1 = static_cast< int >( std::lround( 0.3125 * width ) );
	const int y0 = static_cast< int >( std::lround( 0.1875 * height ) );
	const int y1 = static_cast< int >( std::lround( 0.3125 * height ) );
	for( int y = y0; y < y1; ++y )
		for( int x = x0; x < x1; ++x )
			put( card.image, width, x, y, card.marker );
	card.markerCentreU = ( 0.5 * ( x0 + x1 ) ) / static_cast< double >( width );
	card.markerCentreV = ( 0.5 * ( y0 + y1 ) ) / static_cast< double >( height );
	return card;
}

Image generate( const std::string& name, int width, int height )
{
	if( name == "video" )
		return videoCard( width, height );
	if( name == "graphic" )
		return graphicCard( width, height );
	if( name == "quads-a" )
		return quadCard( width, height, false ).image;
	if( name == "quads-b" )
		return quadCard( width, height, true ).image;
	if( name == "black" )
		return flatField( width, height, kBlack );
	if( name == "white" )
		return flatField( width, height, kWhite );
	return flatField( width, height, { 0x00, 0x99, 0x00 } );//"flat"
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	//WPTEST_RENDERER=software asks for Apple's software renderer by id, on
	//a Mac that has a GPU. It is what a GPU-less CI runner falls back to,
	//so a check that fails only in CI can be reproduced here.
	const CGLPixelFormatAttribute generic[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFARendererID, static_cast< CGLPixelFormatAttribute >( kCGLRendererGenericFloatID ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	const char* renderer     = std::getenv( "WPTEST_RENDERER" );
	if( renderer != nullptr && std::strcmp( renderer, "software" ) == 0 )
	{
		if( CGLChoosePixelFormat( generic, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
		std::fprintf( stderr, "wptest: WPTEST_RENDERER=software, Apple's software renderer\n" );
	}
	else if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;
	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, const unsigned char* pixels, bool floatFormat = false )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	if( floatFormat )
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr );
	else
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

//---------------------------------------------------------------------------
// The rig: the plugin, TWO input textures and an output framebuffer, each
// at its own size. `hardware` may exceed `used` -- that is what MaxUV is
// for -- and the padding is a sentinel colour that appears nowhere else.
//---------------------------------------------------------------------------
constexpr Rgb kSentinel = { 0xFF, 0x00, 0xFF };
constexpr int kSentinelNear = 135;

int l1( Rgb a, Rgb b )
{
	return std::abs( a.r - b.r ) + std::abs( a.g - b.g ) + std::abs( a.b - b.b );
}

struct InputSpec
{
	int usedW = 0, usedH = 0;
	int hwW = 0, hwH = 0;
	static InputSpec Exact( int w, int h )
	{
		return { w, h, w, h };
	}
	static InputSpec Padded( int w, int h, int hw, int hh )
	{
		return { w, h, hw, hh };
	}
};

struct Rig
{
	Wipe plugin;
	int width = 0, height = 0;
	InputSpec aSpec, bSpec;
	bool floatOut = false;

	GLuint aTexture = 0, bTexture = 0;
	GLuint outputTexture = 0, outputFBO = 0;
	FFGLTextureStruct aStruct = {}, bStruct = {};
	FFGLTextureStruct* inputs[ 2 ] = { nullptr, nullptr };
	ProcessOpenGLStruct process    = {};
	bool ready                     = false;

	bool Init( int outW, int outH, InputSpec a, InputSpec b, bool floatOutput = false )
	{
		width    = outW;
		height   = outH;
		aSpec    = a;
		bSpec    = b;
		floatOut = floatOutput;

		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( outW );
		viewport.height             = static_cast< FFUInt32 >( outH );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see the diagnostics log for the shader\n" );
			return false;
		}
		plugin.SetClockScaleForTest( 1.0 );

		aTexture = makeTexture( a.hwW, a.hwH, flatField( a.hwW, a.hwH, kSentinel ).data() );
		bTexture = makeTexture( b.hwW, b.hwH, flatField( b.hwW, b.hwH, kSentinel ).data() );

		outputTexture = makeTexture( outW, outH, nullptr, floatOutput );
		glGenFramebuffers( 1, &outputFBO );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outputTexture, 0 );

		aStruct.Width          = static_cast< FFUInt32 >( a.usedW );
		aStruct.Height         = static_cast< FFUInt32 >( a.usedH );
		aStruct.HardwareWidth  = static_cast< FFUInt32 >( a.hwW );
		aStruct.HardwareHeight = static_cast< FFUInt32 >( a.hwH );
		aStruct.Handle         = aTexture;
		bStruct.Width          = static_cast< FFUInt32 >( b.usedW );
		bStruct.Height         = static_cast< FFUInt32 >( b.usedH );
		bStruct.HardwareWidth  = static_cast< FFUInt32 >( b.hwW );
		bStruct.HardwareHeight = static_cast< FFUInt32 >( b.hwH );
		bStruct.Handle         = bTexture;

		inputs[ 0 ]              = &aStruct;
		inputs[ 1 ]              = &bStruct;
		process.numInputTextures = 2;
		process.inputTextures    = inputs;
		process.HostFBO          = outputFBO;
		ready                    = true;
		return true;
	}

	bool Init( int outW, int outH, bool floatOutput = false )
	{
		return Init( outW, outH, InputSpec::Exact( outW, outH ), InputSpec::Exact( outW, outH ), floatOutput );
	}

	void UploadA( const Image& bottomUp )
	{
		glBindTexture( GL_TEXTURE_2D, aTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, aSpec.usedW, aSpec.usedH, GL_RGBA, GL_UNSIGNED_BYTE, bottomUp.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	void UploadB( const Image& bottomUp )
	{
		glBindTexture( GL_TEXTURE_2D, bTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, bSpec.usedW, bSpec.usedH, GL_RGBA, GL_UNSIGNED_BYTE, bottomUp.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	bool Set( const std::string& name, float value )
	{
		for( unsigned int i = 0; i < Wipe::PT_COUNT; ++i )
		{
			const char* declared = plugin.GetParamName( i );
			if( declared != nullptr && name == declared )
			{
				plugin.SetFloatParameter( i, value );
				return true;
			}
		}
		std::fprintf( stderr, "no parameter called '%s'\n", name.c_str() );
		return false;
	}

	/// Synthetic clock: left to the wall clock a hundred frames render in a
	/// few milliseconds and the modulator never moves.
	bool Render( int frame, double fps = 60.0 )
	{
		plugin.SetClockScaleForTest( 1.0 );
		plugin.SetTime( static_cast< double >( frame ) / fps );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		return plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
	}

	/// The clock as Resolume drives it: SetTime every frame in MILLISECONDS
	/// (measured on genlock in Arena 7.27.1), the unit declared rather than
	/// voted, because a pipe renders as fast as it can and the wall clock the
	/// vote compares against means nothing there. Frame-relative: the
	/// plugin's clock takes the first reading as its epoch.
	bool RenderMs( int frame, double fps )
	{
		plugin.SetClockScaleForTest( 0.001 );
		plugin.SetTime( static_cast< double >( frame ) * 1000.0 / fps );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		return plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
	}

	bool RenderFrames( int frames, double fps = 60.0 )
	{
		for( int frame = 0; frame < frames; ++frame )
			if( !Render( frame, fps ) )
				return false;
		return true;
	}

	/// Drive the fader to an end, `count` times, alternating ends: what a
	/// transition looks like to the flip-flop. Starts by resting at 0.
	bool Transitions( int count )
	{
		if( !Set( "Opacity", 0.0f ) || !Render( 0 ) )
			return false;
		for( int i = 0; i < count; ++i )
		{
			if( !Set( "Opacity", ( i % 2 == 0 ) ? 1.0f : 0.0f ) || !Render( 0 ) )
				return false;
		}
		return true;
	}

	FFResult RenderBroken( int numInputs, int nullIndex )
	{
		FFGLTextureStruct* saved[ 2 ] = { inputs[ 0 ], inputs[ 1 ] };
		if( nullIndex >= 0 && nullIndex < 2 )
			inputs[ nullIndex ] = nullptr;
		if( nullIndex == -2 )
			process.inputTextures = nullptr;
		process.numInputTextures = static_cast< FFUInt32 >( numInputs );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		const FFResult result = plugin.ProcessOpenGL( &process );
		inputs[ 0 ]              = saved[ 0 ];
		inputs[ 1 ]              = saved[ 1 ];
		process.inputTextures    = inputs;
		process.numInputTextures = 2;
		return result;
	}

	Image Pixels()
	{
		Image pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
		return pixels;
	}

	ImageF PixelsF()
	{
		ImageF pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );
		return pixels;
	}

	~Rig()
	{
		if( !ready )
			return;
		plugin.DeInitGL();
		glDeleteFramebuffers( 1, &outputFBO );
		glDeleteTextures( 1, &outputTexture );
		glDeleteTextures( 1, &bTexture );
		glDeleteTextures( 1, &aTexture );
	}
};

//---------------------------------------------------------------------------
// Measurement.
//---------------------------------------------------------------------------
double channelF( const ImageF& image, int width, int x, int y, int c )
{
	return image[ ( static_cast< size_t >( y ) * width + x ) * 4 + static_cast< size_t >( c ) ];
}

/// The sum of a channel along one row: with A black and B white the red
/// channel IS the key, and the sum IS the edge's column, to sub-pixel
/// precision, for any symmetric soft edge that is wholly inside the row.
double rowSum( const ImageF& image, int width, int y, int c = 0 )
{
	double sum = 0.0;
	for( int x = 0; x < width; ++x )
		sum += channelF( image, width, x, y, c );
	return sum;
}

/// The sum of a channel over the whole picture, in double.
double imageSum( const ImageF& image, int width, int height, int c = 0 )
{
	double sum = 0.0;
	for( int y = 0; y < height; ++y )
		sum += rowSum( image, width, y, c );
	return sum;
}

/// Where a falling sequence crosses `value`, by linear interpolation
/// between the two samples that straddle it. Exact on a linear ramp;
/// biased by curvature/8 elsewhere, and the caller states that bound.
/// Returns -1 if there is no crossing.
double crossing( const std::vector< double >& v, double value )
{
	for( size_t i = 0; i + 1 < v.size(); ++i )
		if( v[ i ] >= value && v[ i + 1 ] < value )
		{
			const double t = ( v[ i ] - value ) / ( v[ i ] - v[ i + 1 ] );
			return static_cast< double >( i ) + t;
		}
	return -1.0;
}

/// A channel sampled along a ray of pixel centres from (x0, y0) in steps
/// of (dx, dy), as far as the picture allows.
std::vector< double > ray( const ImageF& image, int width, int height, int x0, int y0, int dx, int dy, int c = 0 )
{
	std::vector< double > out;
	for( int x = x0, y = y0; x >= 0 && x < width && y >= 0 && y < height; x += dx, y += dy )
		out.push_back( channelF( image, width, x, y, c ) );
	return out;
}

int maxByteDifference( const Image& a, const Image& b )
{
	int worst = 0;
	for( size_t i = 0; i < a.size() && i < b.size(); ++i )
		worst = std::max( worst, std::abs( static_cast< int >( a[ i ] ) - static_cast< int >( b[ i ] ) ) );
	return worst;
}

void meanOver( const Image& image, int width, int x0, int y0, int x1, int y1, double out[ 3 ] )
{
	double sum[ 3 ] = { 0, 0, 0 };
	long n          = 0;
	for( int y = y0; y < y1; ++y )
		for( int x = x0; x < x1; ++x )
		{
			const size_t at = ( static_cast< size_t >( y ) * width + x ) * 4;
			sum[ 0 ] += image[ at + 0 ];
			sum[ 1 ] += image[ at + 1 ];
			sum[ 2 ] += image[ at + 2 ];
			++n;
		}
	for( int c = 0; c < 3; ++c )
		out[ c ] = n > 0 ? sum[ c ] / n : -1.0;
}

const char* const kPatternLabel[ PAT_COUNT ] = { "Horizontal", "Vertical", "Box", "Diamond", "Circle", "Clock", "Matrix" };

/// The plain settings every measurement starts from: A black, B white,
/// nothing soft, nothing modulated, Edge law, the positioner centred.
void plainRig( Rig& rig )
{
	rig.UploadA( flatField( rig.aSpec.usedW, rig.aSpec.usedH, kBlack ) );
	rig.UploadB( flatField( rig.bSpec.usedW, rig.bSpec.usedH, kWhite ) );
	rig.Set( "Pattern", 0.0f );
	rig.Set( "Reverse", 0.0f );
	rig.Set( "Flip-Flop", 0.0f );
	rig.Set( "Aspect Comp", 1.0f );
	rig.Set( "Law", 0.0f );
	rig.Set( "Softness", 0.0f );
	rig.Set( "Border Width", 0.0f );
	rig.Set( "Border Softness", 0.0f );
	rig.Set( "Centre X", 0.5f );
	rig.Set( "Centre Y", 0.5f );
	rig.Set( "Rotation", 0.0f );
	rig.Set( "Aspect", 0.5f );
	rig.Set( "Mod Amount", 0.0f );
	rig.Set( "Multiple H", 1.0f );
	rig.Set( "Multiple V", 1.0f );
}

/// The Centre parameter that puts the positioner on the centre of pixel
/// (width/2, height/2), so a ray of pixel centres from there is a true
/// radial.
float centreOnPixel( int extent )
{
	return static_cast< float >( ( extent / 2 + 0.5 ) / extent );
}

//---------------------------------------------------------------------------
// --list, --names
//---------------------------------------------------------------------------
int runList()
{
	Wipe plugin;
	std::printf( "%-4s %-22s %-9s %10s   %-16s %s\n", "id", "name", "kind", "value", "range", "means" );
	for( unsigned int id = 0; id < Wipe::PT_COUNT; ++id )
	{
		const char* name = plugin.GetParamName( id );
		if( id >= Wipe::PT_ABOUT_FIRST )
		{
			std::printf( "%-4u %-22s %-9s %10s   %-16s %s\n", id, name ? name : "", "about", "-", "-",
			             "the Stoatworks About block; not swept" );
			continue;
		}
		const char* kind = "standard";
		switch( plugin.GetParamType( id ) )
		{
		case FF_TYPE_BOOLEAN: kind = "boolean"; break;
		case FF_TYPE_EVENT: kind = "event"; break;
		case FF_TYPE_INTEGER: kind = "integer"; break;
		case FF_TYPE_OPTION: kind = "option"; break;
		case FF_TYPE_BUFFER: kind = "buffer"; break;
		case FF_TYPE_TEXT: kind = "text"; break;
		case FF_TYPE_RED:
		case FF_TYPE_GREEN:
		case FF_TYPE_BLUE: kind = "colour"; break;
		default: break;
		}
		RangeStruct range = plugin.GetParamRange( id );
		//An option's range reads back 0..1 whatever its element count.
		if( plugin.GetParamType( id ) == FF_TYPE_OPTION )
		{
			range.min = 0.0f;
			range.max = static_cast< float >( std::max( 1u, plugin.GetNumParamElements( id ) ) ) - 1.0f;
		}
		char rangeText[ 32 ] = {};
		std::snprintf( rangeText, sizeof( rangeText ), "[%g .. %g]", range.min, range.max );
		std::printf( "%-4u %-22s %-9s %10.4f   %-16s\n", id, name ? name : "", kind, plugin.GetFloatParameter( id ), rangeText );
	}
	return 0;
}

int runNames()
{
	Wipe plugin;
	std::printf( "names longer than FFGL's 16 characters:\n\n" );
	int over = 0;
	for( unsigned int id = 0; id < Wipe::PT_COUNT; ++id )
	{
		const char* name = plugin.GetParamName( id );
		if( name != nullptr && std::strlen( name ) > 16 )
		{
			std::printf( "  %-3u  %-28s %zu\n", id, name, std::strlen( name ) );
			++over;
		}
		for( unsigned int e = 0; e < plugin.GetNumParamElements( id ); ++e )
		{
			const char* el = plugin.GetParamElementName( id, e );
			if( el != nullptr && std::strlen( el ) > 16 )
			{
				std::printf( "  %-3u  %-28s element %u: %s (%zu)\n", id, name, e, el, std::strlen( el ) );
				++over;
			}
		}
	}
	std::printf( "\n  %d over the limit\n", over );

	//Index 0 is sacrificial: Resolume Arena does not expose a mixer's first
	//parameter (measured on genlock), so it must be a control whose default
	//is right for ever. This checks the declaration; only Arena can say
	//which parameter it actually hides.
	const char* first  = plugin.GetParamName( 0 );
	const bool firstOk = first != nullptr && std::strcmp( first, "Aspect Comp" ) == 0
	                     && plugin.GetParamType( 0 ) == FF_TYPE_BOOLEAN && plugin.GetFloatParameter( 0 ) == 1.0f;
	std::printf( "  %s  parameter 0, which Arena hides, is %s (boolean, default on)\n", firstOk ? "ok  " : "FAIL",
	             first ? first : "(none)" );
	return over == 0 && firstOk ? 0 : 1;
}

//---------------------------------------------------------------------------
// --mixer: genlock's two checks, re-run here.
//---------------------------------------------------------------------------
int runMixer()
{
	std::printf( "a mixer takes two inputs, of two sizes, with two MaxUVs\n\n" );
	{
		Rig rig;
		if( !rig.Init( 320, 200 ) )
			return 1;
		rig.UploadA( videoCard( 320, 200 ) );
		rig.UploadB( graphicCard( 320, 200 ) );
		Check( rig.RenderBroken( 2, -2 ) == FF_FAIL, "a null input ARRAY returns FF_FAIL" );
		Check( rig.RenderBroken( 0, -1 ) == FF_FAIL, "zero input textures returns FF_FAIL" );
		Check( rig.RenderBroken( 1, -1 ) == FF_FAIL, "one input texture returns FF_FAIL" );
		Check( rig.RenderBroken( 2, 0 ) == FF_FAIL, "a null A returns FF_FAIL" );
		Check( rig.RenderBroken( 2, 1 ) == FF_FAIL, "a null B returns FF_FAIL" );
		Check( rig.Render( 0 ), "and two real inputs still render afterwards" );
	}

	const int outW = 320, outH = 200;
	const InputSpec a = InputSpec::Padded( 200, 120, 256, 256 );
	const InputSpec b = InputSpec::Padded( 96, 70, 128, 128 );
	const QuadCard aCard = quadCard( a.usedW, a.usedH, false );
	const QuadCard bCard = quadCard( b.usedW, b.usedH, true );
	std::printf( "  A %dx%d used of %dx%d  (MaxUV %.5f, %.5f)\n", a.usedW, a.usedH, a.hwW, a.hwH,
	             static_cast< double >( a.usedW ) / a.hwW, static_cast< double >( a.usedH ) / a.hwH );
	std::printf( "  B %dx%d used of %dx%d  (MaxUV %.5f, %.5f)\n", b.usedW, b.usedH, b.hwW, b.hwH,
	             static_cast< double >( b.usedW ) / b.hwW, static_cast< double >( b.usedH ) / b.hwH );
	std::printf( "  out %dx%d\n\n", outW, outH );

	Rig rig;
	if( !rig.Init( outW, outH, a, b ) )
		return 1;
	rig.UploadA( aCard.image );
	rig.UploadB( bCard.image );

	struct Side
	{
		const char* name;
		float position;
		const QuadCard* card;
		int usedW, usedH;
	};
	const Side sides[ 2 ] = { { "A", 0.0f, &aCard, a.usedW, a.usedH }, { "B", 1.0f, &bCard, b.usedW, b.usedH } };

	//The mid-fader render fetches BOTH inputs through the wipe, which is
	//where a shared MaxUV would show: a horizontal wipe at 0.5 with a
	//hard edge, A on the right half and B on the left.
	for( int pass = 0; pass < 3; ++pass )
	{
		const bool mid   = pass == 2;
		const Side& side = sides[ mid ? 0 : pass ];
		rig.Set( "Pattern", 0.0f );
		rig.Set( "Softness", 0.0f );
		rig.Set( "Opacity", mid ? 0.5f : side.position );
		if( !rig.Render( 0 ) )
		{
			Check( false, std::string( side.name ) + ": ProcessOpenGL failed" );
			continue;
		}
		const Image out = rig.Pixels();
		const std::string label = mid ? "A|B at 0.5" : side.name;

		int sentinelPixels = 0;
		for( int y = 0; y < outH; ++y )
			for( int x = 0; x < outW; ++x )
				if( l1( pixelAt( out, outW, x, y ), kSentinel ) < kSentinelNear )
					++sentinelPixels;
		if( !mid )
		{
			int nearestCard = 1000;
			for( int q = 0; q < 4; ++q )
				nearestCard = std::min( nearestCard, l1( side.card->quadrant[ q ], kSentinel ) );
			nearestCard = std::min( nearestCard, l1( side.card->marker, kSentinel ) );
			Check( nearestCard >= 2 * kSentinelNear,
			       label + fmt( ": no card colour is within %.0f of the padding (needs %.0f)", nearestCard, 2.0 * kSentinelNear ) );
		}
		Check( sentinelPixels == 0, label + fmt( ": no texture padding reached the picture (%.0f sentinel pixels)", sentinelPixels ) );
		if( mid )
			continue;

		const int insetX = static_cast< int >( std::ceil( static_cast< double >( outW ) / side.usedW ) ) + 1;
		const int insetY = static_cast< int >( std::ceil( static_cast< double >( outH ) / side.usedH ) ) + 1;
		double worst     = 0.0;
		for( int q = 1; q < 4; ++q )
		{
			const int qx = ( q & 1 ) ? outW / 2 : 0;
			const int qy = ( q & 2 ) ? outH / 2 : 0;
			double mean[ 3 ];
			meanOver( out, outW, qx + insetX, qy + insetY, qx + outW / 2 - insetX, qy + outH / 2 - insetY, mean );
			const Rgb want = side.card->quadrant[ q ];
			worst          = std::max( { worst, std::fabs( mean[ 0 ] - want.r ), std::fabs( mean[ 1 ] - want.g ), std::fabs( mean[ 2 ] - want.b ) } );
		}
		Check( worst <= 1.0, label + fmt( ": each quadrant is its own flat colour (worst %.3f of 255, tolerance 1)", worst ) );

		double sx = 0.0, sy = 0.0;
		long n    = 0;
		const int half = l1( side.card->marker, side.card->quadrant[ 0 ] ) / 2;
		for( int y = 0; y < outH / 2; ++y )
			for( int x = 0; x < outW / 2; ++x )
				if( l1( pixelAt( out, outW, x, y ), side.card->marker ) < half )
				{
					sx += x + 0.5;
					sy += y + 0.5;
					++n;
				}
		const double gotU = n > 0 ? sx / n / outW : -1.0;
		const double gotV = n > 0 ? sy / n / outH : -1.0;
		const double tolU = 1.0 / side.usedW, tolV = 1.0 / side.usedH;
		Check( n > 0 && std::fabs( gotU - side.card->markerCentreU ) <= tolU && std::fabs( gotV - side.card->markerCentreV ) <= tolV,
		       label + fmt( ": the marker is where it was put (%.4f, %.4f", gotU, gotV )
		           + fmt( " vs %.4f, %.4f; tolerance one source texel %.4f, %.4f)", side.card->markerCentreU, side.card->markerCentreV, tolU, tolV ) );
	}
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --ends: Opacity 0 IS A and Opacity 1 IS B, bitwise, on every pattern,
// with everything that could leak into an end stop switched on.
//---------------------------------------------------------------------------
int runEnds()
{
	std::printf( "the fader's ends are pure fetches, on every pattern\n\n" );
	const int rasters[ 2 ][ 2 ] = { { 640, 360 }, { 320, 180 } };
	for( const auto& r : rasters )
	{
		const int W = r[ 0 ], H = r[ 1 ];
		Rig rig;
		if( !rig.Init( W, H ) )
			return 1;
		const Image aCard = videoCard( W, H ), bCard = graphicCard( W, H );
		rig.UploadA( aCard );
		rig.UploadB( bCard );
		rig.Set( "Softness", 0.5f );
		rig.Set( "Border Width", 0.3f );
		rig.Set( "Border Softness", 0.2f );
		rig.Set( "Border Red", 1.0f );
		rig.Set( "Border Green", 0.0f );
		rig.Set( "Border Blue", 0.0f );
		rig.Set( "Mod Amount", 0.3f );
		rig.Set( "Multiple H", 2.0f );
		rig.Set( "Centre X", 0.3f );
		rig.Set( "Centre Y", 0.6f );
		rig.Set( "Rotation", 0.1f );
		std::printf( "  %dx%d, soft edge, border, modulation, multiple, off-centre, rotated:\n", W, H );
		for( int pattern = 0; pattern < PAT_COUNT; ++pattern )
		{
			rig.Set( "Pattern", static_cast< float >( pattern ) );
			rig.Set( "Opacity", 0.0f );
			if( !rig.Render( 0 ) )
				return 1;
			const int atA = maxByteDifference( rig.Pixels(), aCard );
			rig.Set( "Opacity", 1.0f );
			if( !rig.Render( 0 ) )
				return 1;
			const int atB = maxByteDifference( rig.Pixels(), bCard );
			rig.Set( "Opacity", 0.5f );
			if( !rig.Render( 0 ) )
				return 1;
			const Image mid = rig.Pixels();
			const int midA = maxByteDifference( mid, aCard ), midB = maxByteDifference( mid, bCard );
			Check( atA == 0 && atB == 0,
			       std::string( kPatternLabel[ pattern ] ) + fmt( ": 0 is A (worst %.0f), 1 is B (worst %.0f)", atA, atB ) );
			Negative( midA > 1 && midB > 1, fmt( "and half way is neither (%.0f, %.0f codes off)", midA, midB ) );
		}
	}
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --edge
//---------------------------------------------------------------------------
int runEdge()
{
	std::printf( "in Edge law the edge sits where the fader says\n\n" );
	const int rasters[ 2 ][ 2 ] = { { 640, 360 }, { 320, 180 } };
	for( const auto& r : rasters )
	{
		const int W = r[ 0 ], H = r[ 1 ];
		std::printf( "  %dx%d\n", W, H );

		//---------------- horizontal, whole pixel: a byte-equality claim
		{
			Rig rig;
			if( !rig.Init( W, H ) )
				return 1;
			plainRig( rig );
			const int columns[ 3 ] = { W / 4, W / 2, 3 * W / 4 };
			for( int col : columns )
			{
				rig.Set( "Opacity", static_cast< float >( col ) / W );
				if( !rig.Render( 0 ) )
					return 1;
				const Image out = rig.Pixels();
				int bad = 0;
				for( int y = 0; y < H; ++y )
					for( int x = 0; x < W; ++x )
					{
						const Rgb p    = pixelAt( out, W, x, y );
						const Rgb want = x < col ? kWhite : kBlack;
						if( p.r != want.r || p.g != want.g || p.b != want.b )
							++bad;
					}
				Check( bad == 0, fmt( "hard edge at column %.0f: B left, A right, bitwise (%.0f pixels wrong)", col, bad ) );
			}
		}

		//---------------- horizontal, fractional: partial coverage, integrated
		{
			Rig rig;
			if( !rig.Init( W, H, true ) )
				return 1;
			plainRig( rig );
			const double softPx = 8.0;
			rig.Set( "Softness", PxParamFor( softPx ) );
			const double edges[ 3 ] = { W / 4 + 0.5, W / 2 + 0.25, 3 * W / 4 + 0.75 };
			//Bound: the comparator divides a float difference of order 1 by
			//softW, so each partial column carries ~2^-23 * (1/softW)/(1/softW)
			//... in pixels, 2^-23 * W per column of uv interpolation, over
			//about softPx + 2 partial columns.
			const double bound = ( softPx + 2.0 ) * std::pow( 2.0, -23 ) * W * 2.0;
			const double tol   = 0.02;
			Check( bound * 3.0 <= tol, fmt( "the float bound %.2e px is at least 3x inside the tolerance %.2f px", bound, tol ) );
			for( double edge : edges )
			{
				rig.Set( "Opacity", static_cast< float >( edge / W ) );
				if( !rig.Render( 0 ) )
					return 1;
				const ImageF out = rig.PixelsF();
				double worst     = 0.0;
				for( int y = 0; y < H; y += H / 8 )
					worst = std::max( worst, std::fabs( rowSum( out, W, y ) - edge ) );
				Check( worst <= tol, fmt( "soft edge at %.2f px, integrated: worst %.4f px off (tolerance %.2f)", edge, worst, tol ) );
				//The check can tell the fraction from the nearest whole pixel.
				const double sum = rowSum( out, W, H / 2 );
				Negative( std::fabs( sum - std::round( sum ) ) > tol, fmt( "%.4f is not a whole column", sum ) );
			}
		}

		//---------------- circle: a circle with Aspect Comp, the ellipse without
		{
			Rig rig;
			if( !rig.Init( W, H, true ) )
				return 1;
			plainRig( rig );
			rig.Set( "Pattern", static_cast< float >( PAT_CIRCLE ) );
			rig.Set( "Softness", PxParamFor( 8.0 ) );
			rig.Set( "Centre X", centreOnPixel( W ) );
			rig.Set( "Centre Y", centreOnPixel( H ) );
			const double p = 0.1;
			rig.Set( "Opacity", static_cast< float >( p ) );
			const int cx = W / 2, cy = H / 2;
			const int dirs[ 8 ][ 2 ] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }, { 1, 1 }, { -1, 1 }, { 1, -1 }, { -1, -1 } };

			for( int comp = 1; comp >= 0; --comp )
			{
				rig.Set( "Aspect Comp", static_cast< float >( comp ) );
				if( !rig.Render( 0 ) )
					return 1;
				const ImageF out = rig.PixelsF();
				double radius[ 8 ];
				double rmin = 1e9, rmax = 0.0;
				for( int d = 0; d < 8; ++d )
				{
					const double step = std::sqrt( static_cast< double >( dirs[ d ][ 0 ] * dirs[ d ][ 0 ] + dirs[ d ][ 1 ] * dirs[ d ][ 1 ] ) );
					radius[ d ]       = crossing( ray( out, W, H, cx, cy, dirs[ d ][ 0 ], dirs[ d ][ 1 ] ), 0.5 ) * step;
					rmin = std::min( rmin, radius[ d ] );
					rmax = std::max( rmax, radius[ d ] );
				}
				//Interpolation bias on the parabola: 1/(8 r) px per crossing.
				const double bound = 1.0 / ( 8.0 * rmin ) + std::pow( 2.0, -20 );
				const double tol   = 0.05;
				if( comp )
				{
					Check( bound * 3.0 <= tol, fmt( "the crossing bound %.4f px is 3x inside the tolerance %.2f", bound, tol ) );
					Check( rmax - rmin <= tol, fmt( "Aspect Comp on: eight radii agree, %.3f to %.3f px (spread %.4f)", rmin, rmax, rmax - rmin ) );
				}
				else
				{
					const double A     = static_cast< double >( W ) / H;
					const double rx    = 0.5 * ( radius[ 0 ] + radius[ 1 ] ), ry = 0.5 * ( radius[ 2 ] + radius[ 3 ] );
					const double r45   = 0.25 * ( radius[ 4 ] + radius[ 5 ] + radius[ 6 ] + radius[ 7 ] );
					const double want45 = 1.0 / std::sqrt( 0.5 / ( rx * rx ) + 0.5 / ( ry * ry ) );
					Check( std::fabs( rx / ry - A ) <= 2.0 * tol / ry,
					       fmt( "Aspect Comp off: the ellipse's axes are %.3f and %.3f px, ratio %.4f vs %.4f", rx, ry, rx / ry, A ) );
					Check( std::fabs( r45 - want45 ) <= tol, fmt( "and its diagonal radius %.3f px is the ellipse's %.3f", r45, want45 ) );
					Negative( rx - ry > 10.0 * tol, fmt( "the check can tell this ellipse from a circle (%.1f px apart)", rx - ry ) );
				}
			}
		}
	}
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --area
//---------------------------------------------------------------------------

/// What the GL itself may move the B area by, in pixels of `image`: the
/// rasteriser's allowance, not the plugin's.
///
/// The OpenGL 4.1 core spec (2.1.1, Floating-Point Computation) asks only
/// that "individual results of floating-point operations are accurate to
/// about 1 part in 10^5". The interpolated `uv` a fragment receives is such
/// a result, so it may be off by kGLRelative x |uv| <= kGLRelative in each
/// axis -- and an edge drawn from a waveform in uv moves by that much. An
/// edge element with unit normal n, displaced by (ex, ey) picture-widths,
/// sweeps |n.x ex W + n.y ey H| pixels of area per pixel of length, so the
/// area can move by at most
///
///     kGLRelative x integral over the edge of ( |n.x| W + |n.y| H ) dl
///
/// and by the co-area formula that integral, averaged over the comparator's
/// band, IS the sum over pixels of |dk/dx| W + |dk/dy| H: it is read off the
/// rendered key with forward differences, so it needs no per-pattern
/// perimeter formula and cannot drift from the geometry actually drawn.
/// The comparator's own arithmetic is the same 1 part in 10^5 of a key that
/// is at most 1, on every pixel inside the band. Returns image pixels.
///
/// Measured, it is not academic. Apple's GPU interpolates uv to about one
/// float ULP (4e-8). Apple's SOFTWARE renderer -- what a GPU-less CI runner
/// gets, and what WPTEST_RENDERER=software selects here -- lands uv.y up to
/// 9e-6 off at 2560x1440 (uv.x stays at ~1e-7), and the error grows with the
/// raster's height: 1.6e-6, 2.5e-6, 4.7e-6, 9.0e-6 at 180, 360, 720 and
/// 1440 rows. Inside the spec's 1e-5, and on a vertical wipe 1e-5 of the
/// picture is 2.3 px at 640x360 -- more than the one pixel the check allowed.
constexpr double kGLRelative = 1e-5;

double glAllowance( const ImageF& image, int width, int height )
{
	double edge = 0.0, band = 0.0;
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const double k = channelF( image, width, x, y, 0 );
			if( x + 1 < width )
				edge += std::fabs( channelF( image, width, x + 1, y, 0 ) - k ) * width;
			if( y + 1 < height )
				edge += std::fabs( channelF( image, width, x, y + 1, 0 ) - k ) * height;
			if( k > 0.0 && k < 1.0 )
				band += 1.0;
		}
	return kGLRelative * ( edge + band );
}

/// The length of the edge in pixels of `image`, by the same co-area formula
/// with the Euclidean gradient: the area a one-pixel shift of the whole edge
/// would move. The resolution the check must keep.
double edgeLength( const ImageF& image, int width, int height )
{
	double length = 0.0;
	for( int y = 0; y + 1 < height; ++y )
		for( int x = 0; x + 1 < width; ++x )
		{
			const double k  = channelF( image, width, x, y, 0 );
			const double dx = channelF( image, width, x + 1, y, 0 ) - k;
			const double dy = channelF( image, width, x, y + 1, 0 ) - k;
			length += std::sqrt( dx * dx + dy * dy );
		}
	return length;
}

int runArea()
{
	std::printf( "in Area law the B area IS the fader, for every pattern\n\n" );
	const int rasters[ 2 ][ 2 ] = { { 640, 360 }, { 320, 180 } };
	struct Case
	{
		const char* name;
		int pattern;
		float reverse, centreX, centreY, rotation, multH, multV;
		bool direct;///< also measured by plain pixel-centre sampling
		double positions[ 3 ];
	};
	//Which cases can be summed over pixel centres directly: the ones whose
	//soft edge has no corners and no 45-degree runs. A soft box CORNER is a
	//diagonal kink in the key over a patch w pixels square, and summing a
	//kink over pixel centres carries about 0.1 px of error per corner; a
	//2x2 lattice has some sixty of them. A 45-degree EDGE projects the pixel
	//centres onto its normal as a single-phase lattice of spacing 1/sqrt2,
	//so the cancellation that makes an integer width exact on an
	//axis-aligned edge does not apply. Those cases get the supersampled
	//measurement only, which is the CONTINUOUS area; the direct sum is the
	//DISPLAYED area, and where the two are made both are reported.
	const Case cases[] = {
		{ "Horizontal", PAT_HORIZONTAL, 0, 0.5f, 0.5f, 0.0f, 1, 1, true, { 0.2, 0.5, 0.8 } },
		{ "Vertical", PAT_VERTICAL, 0, 0.5f, 0.5f, 0.0f, 1, 1, true, { 0.2, 0.5, 0.8 } },
		{ "Horizontal x2", PAT_HORIZONTAL, 0, 0.5f, 0.5f, 0.0f, 2, 1, true, { 0.2, 0.5, 0.8 } },
		{ "Box", PAT_BOX, 0, 0.5f, 0.5f, 0.0f, 1, 1, true, { 0.2, 0.5, 0.8 } },
		{ "Box reversed", PAT_BOX, 1, 0.5f, 0.5f, 0.0f, 1, 1, true, { 0.2, 0.5, 0.8 } },
		{ "Box 2x2", PAT_BOX, 0, 0.5f, 0.5f, 0.0f, 2, 2, false, { 0.2, 0.5, 0.8 } },
		{ "Diamond off-centre", PAT_DIAMOND, 0, 0.4f, 0.6f, 0.0f, 1, 1, false, { 0.2, 0.5, 0.8 } },
		{ "Circle", PAT_CIRCLE, 0, 0.5f, 0.5f, 0.0f, 1, 1, true, { 0.2, 0.5, 0.8 } },
		{ "Circle rotated", PAT_CIRCLE, 0, 0.45f, 0.55f, 0.083f, 1, 1, true, { 0.2, 0.5, 0.8 } },
		{ "Clock", PAT_CLOCK, 0, 0.5f, 0.5f, 0.0f, 1, 1, true, { 0.3, 0.6, 0.85 } },
		{ "Matrix", PAT_MATRIX, 0, 0.5f, 0.5f, 0.0f, 1, 1, true, { 0.2, 0.5, 0.8 } },
	};
	constexpr int kSuper = 4;
	for( const auto& r : rasters )
	{
		const int W = r[ 0 ], H = r[ 1 ];
		const double pixelArea = 1.0 / ( static_cast< double >( W ) * H );
		std::printf( "  %dx%d: tolerance one pixel = %.2e of the picture; the continuous area is\n"
		             "  measured on a %dx%d supersampled render of the same geometry\n", W, H, pixelArea, W * kSuper, H * kSuper );
		Rig rig, fine;
		if( !rig.Init( W, H, true ) || !fine.Init( W * kSuper, H * kSuper, true ) )
			return 1;
		//The tolerance against the area a one-pixel shift of the same edge
		//would move, worst over every case and position.
		double coarsest = 0.0, coarsestTol = 0.0, coarsestLength = 0.0;
		std::string coarsestName;
		for( const Case& c : cases )
		{
			//The worst position, judged as off / tolerance.
			struct Measured
			{
				double ratio = 0.0, off = 0.0, tolerance = 1.0, gl = 0.0;
			} onDirect, onFine;
			for( int pass = 0; pass < 2; ++pass )
			{
				const bool super = pass == 1;
				if( !super && !c.direct )
					continue;
				Rig& use = super ? fine : rig;
				plainRig( use );
				use.Set( "Law", static_cast< float >( LAW_AREA ) );
				use.Set( "Pattern", static_cast< float >( c.pattern ) );
				use.Set( "Reverse", c.reverse );
				use.Set( "Centre X", c.centreX );
				use.Set( "Centre Y", c.centreY );
				use.Set( "Rotation", c.rotation );
				use.Set( "Multiple H", c.multH );
				use.Set( "Multiple V", c.multV );
				//Eight pixels of softness at the test raster: an INTEGER
				//width, so that on an axis-aligned edge the two kinks of the
				//linear ramp sit at the same fractional pixel phase and the
				//sum over pixel centres equals the integral exactly. On a
				//curved edge the phases are spread and the residual is
				//~1/(8w) per unit length, random in sign. Scaled with the
				//supersample so the geometry is identical.
				use.Set( "Softness", PxParamFor( 8.0 * ( super ? kSuper : 1 ) ) );
				const double useArea = 1.0 / ( static_cast< double >( use.width ) * use.height );
				//Everything below in pixels of the TEST raster.
				const double toTest = useArea / pixelArea;
				Measured& m         = super ? onFine : onDirect;
				for( double p : c.positions )
				{
					use.Set( "Opacity", static_cast< float >( p ) );
					if( !use.Render( 0 ) )
						return 1;
					const ImageF out    = use.PixelsF();
					const double off    = std::fabs( imageSum( out, use.width, use.height ) * useArea - p ) / pixelArea;
					const double gl     = glAllowance( out, use.width, use.height ) * toTest;
					const double length = edgeLength( out, use.width, use.height ) / ( super ? kSuper : 1 );
					//One pixel for the geometry and the quadrature, plus what
					//the GL is allowed. Judged position by position: the
					//allowance belongs to the picture it was read from.
					const double tolerance = 1.0 + gl;
					if( off / tolerance > m.ratio )
						m = { off / tolerance, off, tolerance, gl };
					if( tolerance / length > coarsest )
					{
						coarsest       = tolerance / length;
						coarsestTol    = tolerance;
						coarsestLength = length;
						coarsestName   = c.name;
					}
				}
			}
			const std::string worst = fmt( ": worst %.3f px of area off the fader, tolerance %.2f (1 + %.2f GL)", onFine.off, onFine.tolerance, onFine.gl );
			if( c.direct )
				Check( onFine.ratio <= 1.0 && onDirect.ratio <= 1.0,
				       std::string( c.name ) + worst + fmt( "; displayed %.3f of %.2f", onDirect.off, onDirect.tolerance ) );
			else
				Check( onFine.ratio <= 1.0, std::string( c.name ) + worst + " (continuous only: corners)" );
		}

		//The GL's allowance must not have bought the check its resolution.
		//A one-pixel shift of the whole edge moves the area by the edge's
		//length in pixels. The spec's allowance alone is 1e-5 x 640 = 0.006
		//of that; with the one pixel of quadrature every tolerance must still
		//be under a QUARTER-pixel shift of its own edge, so a solve that put
		//the edge a quarter of a pixel wrong would fail.
		Negative( coarsest < 0.25,
		          std::string( "every tolerance is under a quarter-pixel shift of its own edge; coarsest " ) + coarsestName
		              + fmt( ", %.2f px against %.1f px", coarsestTol, 0.25 * coarsestLength ) );

		//Negative control: Edge law on the box does NOT give the fader's
		//area, so this check tells the two laws apart.
		plainRig( rig );
		rig.Set( "Pattern", static_cast< float >( PAT_BOX ) );
		rig.Set( "Softness", PxParamFor( 8.0 ) );
		rig.Set( "Opacity", 0.5f );
		if( !rig.Render( 0 ) )
			return 1;
		const double edgeArea = imageSum( rig.PixelsF(), W, H ) * pixelArea;
		Negative( std::fabs( edgeArea - 0.5 ) / pixelArea > 100.0,
		          fmt( "Edge law on the box at 0.5 covers %.4f, %.0f px away from the fader", edgeArea, std::fabs( edgeArea - 0.5 ) / pixelArea ) );
	}
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --softness
//---------------------------------------------------------------------------
int runSoftness()
{
	std::printf( "the edge width is the comparator's gain over the waveform's slope\n\n" );
	const int rasters[ 2 ][ 2 ] = { { 640, 360 }, { 320, 180 } };
	for( const auto& r : rasters )
	{
		const int W = r[ 0 ], H = r[ 1 ];
		std::printf( "  %dx%d\n", W, H );
		Rig rig;
		if( !rig.Init( W, H, true ) )
			return 1;

		//---------------- horizontal: a linear ramp, so 10-90 is 0.8 of the stated width
		plainRig( rig );
		const double softPx = 16.0;
		rig.Set( "Softness", PxParamFor( softPx ) );
		rig.Set( "Opacity", 0.5f );
		if( !rig.Render( 0 ) )
			return 1;
		{
			const ImageF out = rig.PixelsF();
			const auto row   = ray( out, W, H, 0, H / 2, 1, 0 );
			const double x10 = crossing( row, 0.9 ), x90 = crossing( row, 0.1 );
			const double width = x90 - x10, want = 0.8 * softPx;
			const double tol = 0.02;
			Check( std::fabs( width - want ) <= tol, fmt( "horizontal: 10-90%% width %.4f px, stated %.1f px x 0.8 = %.1f (tolerance %.2f)", width, softPx, want, tol ) );
		}
		rig.Set( "Softness", 0.0f );
		if( !rig.Render( 0 ) )
			return 1;
		{
			const ImageF out = rig.PixelsF();
			int partial      = 0;
			for( int x = 0; x < W; ++x )
			{
				const double k = channelF( out, W, x, H / 2, 0 );
				if( k > 0.0 && k < 1.0 )
					++partial;
			}
			Check( partial == 0, fmt( "horizontal at Softness 0: a hard cut, %.0f partial pixels", partial ) );
		}

		//---------------- circle: the parabola's slope grows with radius
		plainRig( rig );
		rig.Set( "Pattern", static_cast< float >( PAT_CIRCLE ) );
		rig.Set( "Softness", PxParamFor( softPx ) );
		rig.Set( "Centre X", centreOnPixel( W ) );
		rig.Set( "Centre Y", centreOnPixel( H ) );
		const double levels[ 3 ] = { 0.05, 0.15, 0.4 };
		double widths[ 3 ], r50[ 3 ], predicted[ 3 ], local[ 3 ];
		for( int i = 0; i < 3; ++i )
		{
			rig.Set( "Opacity", static_cast< float >( levels[ i ] ) );
			if( !rig.Render( 0 ) )
				return 1;
			const ImageF out = rig.PixelsF();
			const auto radial = ray( out, W, H, W / 2, H / 2, 1, 0 );
			const double r10 = crossing( radial, 0.9 ), r90 = crossing( radial, 0.1 );
			r50[ i ]    = crossing( radial, 0.5 );
			widths[ i ] = r90 - r10;
			//The closed form, from the plugin's own softW and R: the key is
			//0.5 + (level - r^2/R^2)/softW, so the 10% and 90% radii are
			//R sqrt( level -/+ 0.4 softW ), in picture heights.
			const auto& st = rig.plugin.StateForTest();
			const double R = st.derived.norm, sW = st.softW;
			predicted[ i ] = R * ( std::sqrt( levels[ i ] + 0.4 * sW ) - std::sqrt( levels[ i ] - 0.4 * sW ) ) * H;
			//And the first-order figure the spec states: gain over slope.
			local[ i ] = 0.8 * sW / ( 2.0 * std::sqrt( levels[ i ] ) / R ) * H;
		}
		const double bound = 2.0 / ( 8.0 * r50[ 0 ] ) + std::pow( 2.0, -18 );
		const double tol   = 0.05;
		Check( bound * 3.0 <= tol, fmt( "the crossing bound %.4f px is 3x inside the tolerance %.2f", bound, tol ) );
		for( int i = 0; i < 3; ++i )
			Check( std::fabs( widths[ i ] - predicted[ i ] ) <= tol,
			       fmt( "circle at r = %.1f px: 10-90%% width %.3f px, parabola predicts %.3f (slope alone: %.3f)", r50[ i ], widths[ i ], predicted[ i ], local[ i ] ) );
		//A waveform linear in r -- |H|+|V|, or a plain radius -- would give
		//the SAME width at every radius. The measurement must reject that.
		Negative( std::fabs( widths[ 2 ] - widths[ 0 ] ) > 10.0 * tol,
		          fmt( "a uniform width is rejected: %.3f at r=%.0f vs %.3f at r=%.0f", widths[ 0 ], r50[ 0 ], widths[ 2 ], r50[ 2 ] ) );
	}
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --border
//---------------------------------------------------------------------------
int runBorder()
{
	std::printf( "the border is a second comparator, and its width follows the slope too\n\n" );
	const int rasters[ 2 ][ 2 ] = { { 640, 360 }, { 320, 180 } };
	for( const auto& r : rasters )
	{
		const int W = r[ 0 ], H = r[ 1 ];
		std::printf( "  %dx%d\n", W, H );
		Rig rig;
		if( !rig.Init( W, H, true ) )
			return 1;
		plainRig( rig );
		rig.Set( "Border Red", 1.0f );
		rig.Set( "Border Green", 0.0f );
		rig.Set( "Border Blue", 0.0f );
		const double borderPx = 12.0;
		rig.Set( "Border Width", PxParamFor( borderPx ) );

		//---------------- horizontal, hard: exactly 12 red columns
		rig.Set( "Opacity", 0.5f );
		if( !rig.Render( 0 ) )
			return 1;
		{
			const ImageF out = rig.PixelsF();
			int red = 0, first = -1, last = -1;
			for( int x = 0; x < W; ++x )
			{
				const bool isRed = channelF( out, W, x, H / 2, 0 ) == 1.0 && channelF( out, W, x, H / 2, 1 ) == 0.0;
				if( isRed )
				{
					++red;
					if( first < 0 )
						first = x;
					last = x;
				}
			}
			Check( red == borderPx && first == W / 2 && last - first + 1 == red,
			       fmt( "horizontal, hard: %.0f red columns from %.0f, contiguous (stated %.0f)", red, first, borderPx ) );
		}
		//---------------- horizontal, soft: the integrals still say 12
		rig.Set( "Softness", PxParamFor( 8.0 ) );
		rig.Set( "Border Softness", PxParamFor( 6.0 ) );
		if( !rig.Render( 0 ) )
			return 1;
		{
			const ImageF out = rig.PixelsF();
			const double borderArea = rowSum( out, W, H / 2, 0 ) - rowSum( out, W, H / 2, 1 );
			Check( std::fabs( borderArea - borderPx ) <= 0.02, fmt( "horizontal, soft both sides: the red integrates to %.4f px (stated %.0f)", borderArea, borderPx ) );
		}

		//---------------- circle: inner and outer crossings, two radii
		rig.Set( "Pattern", static_cast< float >( PAT_CIRCLE ) );
		rig.Set( "Centre X", centreOnPixel( W ) );
		rig.Set( "Centre Y", centreOnPixel( H ) );
		const double levels[ 2 ] = { 0.05, 0.3 };
		double widths[ 2 ], want[ 2 ], local[ 2 ], rin[ 2 ];
		for( int i = 0; i < 2; ++i )
		{
			rig.Set( "Opacity", static_cast< float >( levels[ i ] ) );
			if( !rig.Render( 0 ) )
				return 1;
			const ImageF out = rig.PixelsF();
			//With A black, B white and the border red: green is key(1-border)
			//and red is key(1-border) + border. So border = r - g, key is
			//g/(1-border), and the OUTER comparator is key + border --
			//recovered from the two channels, not read off red, which is a
			//blend of all three.
			const auto green = ray( out, W, H, W / 2, H / 2, 1, 0, 1 );
			const auto red   = ray( out, W, H, W / 2, H / 2, 1, 0, 0 );
			std::vector< double > keyRay( green.size() ), outer( green.size() );
			for( size_t k = 0; k < green.size(); ++k )
			{
				const double border = red[ k ] - green[ k ];
				keyRay[ k ]         = border < 1.0 ? green[ k ] / ( 1.0 - border ) : 0.0;
				outer[ k ]          = keyRay[ k ] + border;
			}
			rin[ i ]    = crossing( keyRay, 0.5 );
			widths[ i ] = crossing( outer, 0.5 ) - rin[ i ];
			const auto& st = rig.plugin.StateForTest();
			const double R = st.derived.norm, bW = st.borderW;
			want[ i ]  = R * ( std::sqrt( levels[ i ] + bW ) - std::sqrt( levels[ i ] ) ) * H;
			local[ i ] = bW / ( 2.0 * std::sqrt( levels[ i ] ) / R ) * H;
		}
		const double tol = 0.05;
		for( int i = 0; i < 2; ++i )
			Check( std::fabs( widths[ i ] - want[ i ] ) <= tol,
			       fmt( "circle at r = %.1f px: border %.3f px wide, closed form %.3f (Border Width over the local slope: %.3f)", rin[ i ], widths[ i ], want[ i ], local[ i ] ) );
		Negative( std::fabs( widths[ 0 ] - widths[ 1 ] ) > 10.0 * tol, fmt( "a constant border width is rejected: %.3f vs %.3f px", widths[ 0 ], widths[ 1 ] ) );
	}
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --modulation
//---------------------------------------------------------------------------
int runModulation()
{
	std::printf( "the edge's wobble is a sine of the stated amplitude and period\n\n" );
	const int rasters[ 2 ][ 2 ] = { { 640, 360 }, { 320, 180 } };
	for( const auto& r : rasters )
	{
		const int W = r[ 0 ], H = r[ 1 ];
		std::printf( "  %dx%d\n", W, H );
		Rig rig;
		if( !rig.Init( W, H, true ) )
			return 1;
		plainRig( rig );
		const double ampPx  = 16.0;
		const double cycles = 4.0;//Mod Frequency 0.5 is exactly 0.5 * 64^0.5
		rig.Set( "Softness", PxParamFor( 8.0 ) );
		rig.Set( "Opacity", 0.5f );
		rig.Set( "Mod Amount", PxParamFor( ampPx ) );
		rig.Set( "Mod Frequency", 0.5f );
		rig.Set( "Mod Speed", 0.25f );//1 Hz
		const double fps = 60.0;
		const int frames[ 2 ] = { 0, 15 };//a quarter of a cycle apart at 1 Hz
		double phase[ 2 ];
		for( int f = 0; f < 2; ++f )
		{
			if( !rig.Render( frames[ f ], fps ) )
				return 1;
			const ImageF out = rig.PixelsF();
			//The edge per row, integrated, minus the fader's column.
			std::vector< double > e( static_cast< size_t >( H ) );
			for( int y = 0; y < H; ++y )
				e[ static_cast< size_t >( y ) ] = rowSum( out, W, y ) - 0.5 * W;
			//Project onto the stated frequency. Four whole cycles over the
			//rows makes the projection of a pure sine at pixel centres exact.
			auto project = [ & ]( double k, double& a, double& b ) {
				a = b = 0.0;
				for( int y = 0; y < H; ++y )
				{
					const double v = ( y + 0.5 ) / H;
					a += e[ static_cast< size_t >( y ) ] * std::sin( 2.0 * M_PI * k * v );
					b += e[ static_cast< size_t >( y ) ] * std::cos( 2.0 * M_PI * k * v );
				}
				a *= 2.0 / H;
				b *= 2.0 / H;
			};
			double a, b;
			project( cycles, a, b );
			const double amplitude = std::sqrt( a * a + b * b );
			phase[ f ]             = std::atan2( b, a );
			double residual        = 0.0;
			for( int y = 0; y < H; ++y )
			{
				const double v = ( y + 0.5 ) / H;
				const double fit = a * std::sin( 2.0 * M_PI * cycles * v ) + b * std::cos( 2.0 * M_PI * cycles * v );
				residual += ( e[ static_cast< size_t >( y ) ] - fit ) * ( e[ static_cast< size_t >( y ) ] - fit );
			}
			residual = std::sqrt( residual / H );
			const double tol = 0.02;
			Check( std::fabs( amplitude - ampPx ) <= tol, fmt( "frame %.0f: amplitude %.4f px, stated %.0f (tolerance %.2f)", frames[ f ], amplitude, ampPx, tol ) );
			Check( residual <= tol, fmt( "frame %.0f: RMS residual from a sine %.4f px -- a triangle would leave %.1f", frames[ f ], residual, 0.1 * ampPx ) );
			double a3, b3;
			project( cycles - 1.0, a3, b3 );
			Negative( std::sqrt( a3 * a3 + b3 * b3 ) < tol, fmt( "at %.0f cycles the projection is %.4f px: the period is %.0f, not that", cycles - 1.0, std::sqrt( a3 * a3 + b3 * b3 ), cycles ) );
		}
		//The waveform gets m sin( 2pi( f v - phase ) ), so the EDGE moves by
		//-m sin( 2pi f v - 2pi phase ) = m sin( 2pi f v + pi - 2pi phase ):
		//the fitted phase is pi - 2pi*phase and FALLS as the phase advances.
		//A quarter cycle at 1 Hz is therefore -pi/2, i.e. 3pi/2 modulo 2pi,
		//and the wave travels up the picture (+v).
		const double advance = frames[ 1 ] / fps * 1.0;
		double delta = phase[ 1 ] - phase[ 0 ];
		while( delta < 0.0 )
			delta += 2.0 * M_PI;
		const double want     = 2.0 * M_PI * ( 1.0 - advance );
		const double phaseTol = 0.02 / ampPx;
		Check( std::fabs( delta - want ) <= phaseTol,
		       fmt( "Mod Speed 1 Hz: %.4f rad between frames a quarter second apart, -pi/2 = %.4f predicted (tolerance %.4f)", delta, want, phaseTol ) );
		Check( std::fabs( rig.plugin.StateForTest().modPhase - advance ) < 1e-12,
		       fmt( "and the plugin's own reduced phase is %.6f cycles", rig.plugin.StateForTest().modPhase ) );
	}
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --flipflop
//---------------------------------------------------------------------------
int runFlipFlop()
{
	std::printf( "Flip-Flop reverses on alternate transitions\n\n" );
	const int W = 320, H = 180;
	Rig rig;
	if( !rig.Init( W, H ) )
		return 1;
	plainRig( rig );
	rig.Set( "Pattern", static_cast< float >( PAT_BOX ) );
	rig.Set( "Opacity", 0.5f );
	if( !rig.Render( 0 ) )
		return 1;
	const Image opening = rig.Pixels();
	rig.Set( "Reverse", 1.0f );
	if( !rig.Render( 0 ) )
		return 1;
	const Image closing = rig.Pixels();
	rig.Set( "Reverse", 0.0f );
	Negative( maxByteDifference( opening, closing ) > 1, "the opening and closing boxes differ" );

	rig.Set( "Flip-Flop", 1.0f );
	//Resting at 0 first: the first arrival at an end is not a transition.
	if( !rig.Transitions( 0 ) )
		return 1;
	rig.Set( "Opacity", 0.5f );
	if( !rig.Render( 0 ) )
		return 1;
	Check( maxByteDifference( rig.Pixels(), opening ) == 0, "before any transition the box opens" );
	Check( rig.plugin.StateForTest().transitions == 0, "and no transition has been counted" );

	for( int t = 1; t <= 4; ++t )
	{
		rig.Set( "Opacity", ( t % 2 == 1 ) ? 1.0f : 0.0f );
		if( !rig.Render( 0 ) )
			return 1;
		rig.Set( "Opacity", 0.5f );
		if( !rig.Render( 0 ) )
			return 1;
		const bool flipped = ( t % 2 ) == 1;
		Check( maxByteDifference( rig.Pixels(), flipped ? closing : opening ) == 0 && rig.plugin.StateForTest().transitions == t,
		       fmt( "after transition %.0f the box ", t ) + ( flipped ? "closes" : "opens" ) + ", bitwise" );
	}
	rig.Set( "Flip-Flop", 0.0f );
	rig.Set( "Opacity", 1.0f );
	if( !rig.Render( 0 ) )
		return 1;
	rig.Set( "Opacity", 0.5f );
	if( !rig.Render( 0 ) )
		return 1;
	Check( maxByteDifference( rig.Pixels(), opening ) == 0, "with Flip-Flop off a transition changes nothing" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --bench
//---------------------------------------------------------------------------
double benchAt( int width, int height, int frames, double fps )
{
	Rig rig;
	if( !rig.Init( width, height ) )
		return -1.0;
	rig.UploadA( videoCard( width, height ) );
	rig.UploadB( graphicCard( width, height ) );
	rig.Set( "Pattern", static_cast< float >( PAT_MATRIX ) );//the most arithmetic per pixel
	rig.Set( "Softness", 0.3f );
	rig.Set( "Border Width", 0.2f );
	rig.Set( "Mod Amount", 0.2f );
	rig.Set( "Opacity", 0.5f );
	const int warmup = 20;
	for( int frame = 0; frame < warmup; ++frame )
		rig.Render( frame, fps );
	glFinish();
	const auto start = std::chrono::steady_clock::now();
	for( int frame = 0; frame < frames; ++frame )
		rig.Render( warmup + frame, fps );
	glFinish();
	const auto end = std::chrono::steady_clock::now();
	return std::chrono::duration< double >( end - start ).count() * 1000.0 / frames;
}

int runBench( int frames, double fps )
{
	struct Size
	{
		const char* name;
		int width, height;
	};
	const Size sizes[] = {
		{ "1280x720  ", 1280, 720 },
		{ "1920x1080 ", 1920, 1080 },
		{ "2560x1440 ", 2560, 1440 },
		{ "3840x2160 ", 3840, 2160 },
	};
	std::printf( "%d frames each, after a 20-frame warm-up, glFinish both sides.\n\n", frames );
	std::printf( "resolution     ms/frame   equivalent fps   %% of a 60fps frame\n" );
	for( const Size& size : sizes )
	{
		const double ms = benchAt( size.width, size.height, frames, fps );
		if( ms < 0.0 )
			return 1;
		std::printf( "%s    %7.3f       %8.0f            %5.1f%%\n", size.name, ms, ms > 0.0 ? 1000.0 / ms : 0.0, ms / 16.667 * 100.0 );
	}

	//The Area law's solve is CPU work on the render thread, and its cost
	//depends on the pattern and the multiples. Measured here, not asserted.
	std::printf( "\nArea law solve, CPU, per changed frame (soft edge 8 px, 1080p):\n" );
	const struct
	{
		const char* name;
		int pattern, mh, mv;
	} solves[] = { { "Horizontal", PAT_HORIZONTAL, 1, 1 }, { "Box", PAT_BOX, 1, 1 }, { "Circle", PAT_CIRCLE, 1, 1 }, { "Clock", PAT_CLOCK, 1, 1 }, { "Matrix", PAT_MATRIX, 1, 1 }, { "Box 8x8", PAT_BOX, 8, 8 }, { "Circle 8x8", PAT_CIRCLE, 8, 8 } };
	for( const auto& s : solves )
	{
		Frame frame;
		frame.pattern   = s.pattern;
		frame.multipleH = s.mh;
		frame.multipleV = s.mv;
		frame.outW      = 1920;
		frame.outH      = 1080;
		const Derived d = Derive( frame );
		const double softW = 8.0 * d.unitsPerPixel;
		const auto start   = std::chrono::steady_clock::now();
		const int n        = 10;
		double sink        = 0.0;
		for( int i = 0; i < n; ++i )
			sink += AreaLevel( frame, d, 0.1 + 0.08 * i, softW );
		const auto end = std::chrono::steady_clock::now();
		std::printf( "  %-12s %8.3f ms\n", s.name, std::chrono::duration< double >( end - start ).count() * 1000.0 / n + sink * 0.0 );
	}
	return 0;
}

//---------------------------------------------------------------------------
// --pipe: the fleet's frame format, extended to a second input. The same
// shape as genlock's gltest --pipe.
//
// Every one-input FFGL harness in the fleet takes raw RGBA frames, top row
// first, on stdin and writes the processed frames to stdout, so one filming
// script can drive any of them. A mixer has two inputs, so:
//
//   * stdin is DEST, inputTextures[0], the layer below -- A. Output-sized,
//     exactly as in the one-input harnesses.
//   * `--pipe-src PATH` is SRC, inputTextures[1], this layer -- B, the
//     picture the fader wipes in. Raw RGBA frames, top row first, at
//     `--src-size` (default: the output size). PATH may be a FIFO. Without
//     it, the `--input-b` generator is uploaded once and held.
//
// One frame of each is read per output frame; the run ends when either
// stream does, and a partial frame at the end is the end, not a frame. The
// clock is synthetic and in milliseconds, as Arena sends it -- frame * 1000
// / fps -- so Mod Speed moves and every take is the same.
//
// `--script` is the fleet's cue sheet: one `frame Name value` per line, '#'
// to end of line a comment, linear between keys, the first key held before
// it and the last after it. The value goes straight to SetFloatParameter, so
// it is the parameter's own host value: 0..1 for a standard slider or a
// colour, 0 or 1 for a switch, the option INDEX for a dropdown (Pattern
// 0..6, Law 0..1), and for the two Multiples, which are FF_TYPE_INTEGER, the
// count itself (1..8). `wptest --list` prints each one's range. A name that
// is not a parameter is refused before a frame is read.
//
// This is a filming mode, not a check. It asserts nothing, and no check
// calls it.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

/// One 'frame Parameter Name value' per line, '#' to end of line is a
/// comment. The same format as the rest of the fleet.
std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}

	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );

		int frame = 0;
		if( !( in >> frame ) )
			continue;

		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}

		const float value = std::strtof( words.back().c_str(), nullptr );
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];

		tracks[ name ].emplace_back( frame, value );
	}

	for( auto& entry : tracks )
		std::sort( entry.second.begin(), entry.second.end() );
	return tracks;
}

/// Linear between keys; the first key holds before it, the last after it.
float valueAt( const Track& track, int frame )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;

	for( size_t i = 1; i < track.size(); ++i )
	{
		if( frame <= track[ i ].first )
		{
			const auto& a    = track[ i - 1 ];
			const auto& b    = track[ i ];
			const float span = static_cast< float >( b.first - a.first );
			const float t    = span > 0.0f ? ( static_cast< float >( frame - a.first ) / span ) : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
	}
	return track.back().second;
}

/// Fill `frame` from `fd`. False on a short read, which is the end of the
/// stream rather than an error.
bool readFrame( int fd, Image& frame )
{
	size_t filled = 0;
	while( filled < frame.size() )
	{
		const ssize_t got = read( fd, frame.data() + filled, frame.size() - filled );
		if( got <= 0 )
			return false;
		filled += static_cast< size_t >( got );
	}
	return true;
}

bool writeAll( int fd, const Image& frame )
{
	size_t written = 0;
	while( written < frame.size() )
	{
		const ssize_t put_ = write( fd, frame.data() + written, frame.size() - written );
		if( put_ <= 0 )
			return false;
		written += static_cast< size_t >( put_ );
	}
	return true;
}

/// Top row first to bottom row first, or back: the same operation.
void flipRows( const Image& in, Image& out, int width, int height )
{
	const size_t row = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::memcpy( out.data() + static_cast< size_t >( height - 1 - y ) * row, in.data() + static_cast< size_t >( y ) * row, row );
}

int runPipe( int width, int height, int srcWidth, int srcHeight, double fps, const std::string& scriptPath,
             const std::string& srcPath, const std::string& inputB, const std::vector< std::string >& settings )
{
	Rig rig;
	if( !rig.Init( width, height, InputSpec::Exact( width, height ), InputSpec::Exact( srcWidth, srcHeight ) ) )
		return 1;

	for( const std::string& setting : settings )
	{
		const size_t equals = setting.find( '=' );
		if( equals == std::string::npos
		    || !rig.Set( setting.substr( 0, equals ), std::strtof( setting.substr( equals + 1 ).c_str(), nullptr ) ) )
		{
			std::fprintf( stderr, "--set %s: expected Name=Value with a known name (try --list)\n", setting.c_str() );
			return 2;
		}
	}

	//Resolve the script's names up front and refuse an unknown one: a
	//misspelled cue that silently did nothing would produce a take that
	//looks deliberate and is wrong.
	std::map< unsigned int, Track > automation;
	if( !scriptPath.empty() )
	{
		std::string error;
		const std::map< std::string, Track > tracks = loadScript( scriptPath, error );
		if( !error.empty() )
		{
			std::fprintf( stderr, "%s\n", error.c_str() );
			return 2;
		}
		for( const auto& entry : tracks )
		{
			bool found = false;
			for( unsigned int id = 0; id < Wipe::PT_ABOUT_FIRST; ++id )
			{
				const char* name = rig.plugin.GetParamName( id );
				if( name != nullptr && entry.first == name )
				{
					automation[ id ] = entry.second;
					found            = true;
					break;
				}
			}
			if( !found )
			{
				std::fprintf( stderr, "script names '%s', which is not a parameter (try --list)\n", entry.first.c_str() );
				return 2;
			}
		}
	}

	int srcFd = -1;
	if( !srcPath.empty() )
	{
		srcFd = open( srcPath.c_str(), O_RDONLY );
		if( srcFd < 0 )
		{
			std::fprintf( stderr, "cannot open --pipe-src %s\n", srcPath.c_str() );
			return 2;
		}
	}
	else
		rig.UploadB( generate( inputB, srcWidth, srcHeight ) );

	Image destIn( static_cast< size_t >( width ) * height * 4 ), destUp( destIn.size() );
	Image srcIn( static_cast< size_t >( srcWidth ) * srcHeight * 4 ), srcUp( srcIn.size() );
	Image out( destIn.size() );

	int index = 0;
	for( ;; ++index )
	{
		if( !readFrame( STDIN_FILENO, destIn ) )
			break;
		if( srcFd >= 0 )
		{
			if( !readFrame( srcFd, srcIn ) )
				break;
			flipRows( srcIn, srcUp, srcWidth, srcHeight );
			rig.UploadB( srcUp );
		}
		flipRows( destIn, destUp, width, height );
		rig.UploadA( destUp );

		//Through the plugin's own setter, so a cue moves exactly what the
		//host's slider -- or, for Opacity, the layer's fader -- would.
		for( const auto& track : automation )
			rig.plugin.SetFloatParameter( track.first, valueAt( track.second, index ) );

		if( !rig.RenderMs( index, fps ) )
		{
			std::fprintf( stderr, "ProcessOpenGL failed at frame %d\n", index );
			if( srcFd >= 0 )
				close( srcFd );
			return 1;
		}
		flipRows( rig.Pixels(), out, width, height );
		if( !writeAll( STDOUT_FILENO, out ) )
			break;
	}

	if( srcFd >= 0 )
		close( srcFd );
	std::fprintf( stderr, "wptest --pipe: %d frames\n", index );
	return 0;
}

//---------------------------------------------------------------------------
void usage()
{
	std::printf(
		"wptest -- render and measure the Wipe FFGL mixer\n"
		"\n"
		"  --out PATH        render both inputs through the plugin (default /tmp/wipe.png)\n"
		"  --input-a NAME    the DEST generator, the layer below: A (default video)\n"
		"  --input-b NAME    the SRC generator, this layer: B (default graphic)\n"
		"                    video | graphic | quads-a | quads-b | black | white | flat\n"
		"  --card PATH       write input B alone\n"
		"  --size WxH        picture size (default 1280x720)\n"
		"  --frames N        frames to render before reading back (default 1)\n"
		"  --fps N           synthetic frame rate driving the clock (default 60)\n"
		"  --transitions N   drive the fader to alternate ends N times first\n"
		"  --set \"Name=V\"    set a parameter by its display name. Repeatable.\n"
		"  --list            print every parameter, its kind, default and range, then exit\n"
		"  --names           no parameter or element name over 16 characters\n"
		"  --mixer           two inputs, two MaxUVs, and the missing-input guards\n"
		"  --ends            Opacity 0 IS A and 1 IS B, on every pattern\n"
		"  --edge            the edge sits where Edge law says; circle vs ellipse\n"
		"  --area            the B area IS the fader in Area law, every pattern\n"
		"  --softness        the edge width follows the waveform's slope\n"
		"  --border          the border's width follows it too\n"
		"  --modulation      the edge wobble is the stated sine, and it travels\n"
		"  --flipflop        alternate transitions reverse\n"
		"  --bench           time ProcessOpenGL at 720p through 4K\n"
		"  --pipe            raw RGBA Dest (A) frames on stdin, raw RGBA frames on stdout\n"
		"  --pipe-src PATH   raw RGBA Src (B) frames for --pipe (a file or FIFO); default: --input-b, held\n"
		"  --src-size WxH    the Src frames' size for --pipe (default: the output size)\n"
		"  --script PATH     parameter cues for --pipe: 'frame Name value', value in the\n"
		"                    parameter's host units (0..1; option index; Multiples 1..8)\n"
		"  --help\n" );
}
} // namespace

int main( int argc, char** argv )
{
	std::string outPath = "/tmp/wipe.png";
	std::string cardPath;
	std::string inputA = "video";
	std::string inputB = "graphic";
	int width = 1280, height = 720;
	int frames      = 1;
	int transitions = 0;
	double fps      = 60.0;
	bool wantList = false, wantBench = false, wantPipe = false;
	std::string scriptPath, srcPath;
	int srcWidth = 0, srcHeight = 0;
	std::string check;
	std::vector< std::string > settings;

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;
		if( argument == "--help" )
		{
			usage();
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--card" && hasNext )
			cardPath = argv[ ++i ];
		else if( argument == "--input-a" && hasNext )
			inputA = argv[ ++i ];
		else if( argument == "--input-b" && hasNext )
			inputB = argv[ ++i ];
		else if( argument == "--size" && hasNext )
		{
			const std::string size = argv[ ++i ];
			const size_t x         = size.find( 'x' );
			if( x == std::string::npos )
			{
				std::fprintf( stderr, "--size wants WxH\n" );
				return 2;
			}
			width  = std::atoi( size.substr( 0, x ).c_str() );
			height = std::atoi( size.substr( x + 1 ).c_str() );
		}
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--transitions" && hasNext )
			transitions = std::atoi( argv[ ++i ] );
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--list" )
			wantList = true;
		else if( argument == "--bench" )
			wantBench = true;
		else if( argument == "--pipe" )
			wantPipe = true;
		else if( argument == "--pipe-src" && hasNext )
			srcPath = argv[ ++i ];
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--src-size" && hasNext )
		{
			const std::string size = argv[ ++i ];
			const size_t x         = size.find( 'x' );
			if( x == std::string::npos )
			{
				std::fprintf( stderr, "--src-size wants WxH\n" );
				return 2;
			}
			srcWidth  = std::atoi( size.substr( 0, x ).c_str() );
			srcHeight = std::atoi( size.substr( x + 1 ).c_str() );
			if( srcWidth <= 0 || srcHeight <= 0 )
			{
				std::fprintf( stderr, "--src-size wants a positive WxH\n" );
				return 2;
			}
		}
		else if( argument == "--names" || argument == "--mixer" || argument == "--ends" || argument == "--edge"
		         || argument == "--area" || argument == "--softness" || argument == "--border"
		         || argument == "--modulation" || argument == "--flipflop" )
			check = argument;
		else
		{
			std::fprintf( stderr, "unknown argument: %s\n", argument.c_str() );
			usage();
			return 2;
		}
	}

	if( width <= 0 || height <= 0 || frames <= 0 || fps <= 0.0 )
	{
		std::fprintf( stderr, "width, height, frames and fps must all be positive\n" );
		return 2;
	}

	if( wantList )
		return runList();
	if( check == "--names" )
		return runNames();

	if( !cardPath.empty() )
	{
		if( !writePng( cardPath, width, height, generate( inputB, width, height ) ) )
		{
			std::fprintf( stderr, "could not write %s\n", cardPath.c_str() );
			return 1;
		}
		std::printf( "wrote %s\n", cardPath.c_str() );
		return 0;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL context\n" );
		return 1;
	}

	int result = 0;
	if( check == "--mixer" )
		result = runMixer();
	else if( check == "--ends" )
		result = runEnds();
	else if( check == "--edge" )
		result = runEdge();
	else if( check == "--area" )
		result = runArea();
	else if( check == "--softness" )
		result = runSoftness();
	else if( check == "--border" )
		result = runBorder();
	else if( check == "--modulation" )
		result = runModulation();
	else if( check == "--flipflop" )
		result = runFlipFlop();
	else if( wantBench )
		result = runBench( frames > 1 ? frames : 60, fps );
	else if( wantPipe )
		result = runPipe( width, height, srcWidth > 0 ? srcWidth : width, srcHeight > 0 ? srcHeight : height, fps,
		                  scriptPath, srcPath, inputB, settings );
	else
	{
		Rig rig;
		if( !rig.Init( width, height ) )
			return 1;
		rig.UploadA( generate( inputA, width, height ) );
		rig.UploadB( generate( inputB, width, height ) );

		float position = rig.plugin.GetFloatParameter( Wipe::PT_OPACITY );
		for( const std::string& setting : settings )
		{
			const size_t equals = setting.find( '=' );
			if( equals == std::string::npos
			    || !rig.Set( setting.substr( 0, equals ), std::strtof( setting.substr( equals + 1 ).c_str(), nullptr ) ) )
			{
				std::fprintf( stderr, "--set %s: expected Name=Value with a known name (try --list)\n", setting.c_str() );
				return 2;
			}
			if( setting.substr( 0, equals ) == "Opacity" )
				position = std::strtof( setting.substr( equals + 1 ).c_str(), nullptr );
		}
		if( transitions > 0 )
		{
			if( !rig.Transitions( transitions ) )
			{
				std::fprintf( stderr, "ProcessOpenGL failed during the transitions\n" );
				return 1;
			}
			rig.Set( "Opacity", position );
		}

		if( !rig.RenderFrames( frames, fps ) )
		{
			std::fprintf( stderr, "ProcessOpenGL failed\n" );
			result = 1;
		}
		else if( !writePng( outPath, width, height, rig.Pixels() ) )
		{
			std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
			result = 1;
		}
		else
			std::printf( "wrote %s (%dx%d, %d frames, A=%s B=%s)\n", outPath.c_str(), width, height, frames, inputA.c_str(), inputB.c_str() );
	}

	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return result != 0 || failures != 0 ? 1 : 0;
}
