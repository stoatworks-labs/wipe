#include "Wipe.h"

#include "Controls.h"
#include "Diag.h"
#include "Shaders.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

using namespace ffglex;
using namespace wipe;

//---------------------------------------------------------------------------
// The eighth argument is the plugin TYPE, and it is the only thing in the
// whole repo that makes this a mixer rather than an effect. The input count
// is a SEPARATE declaration, in the constructor. See genlock's AGENTS.md.
//---------------------------------------------------------------------------
static CFFGLPluginInfo PluginInfo(
	PluginFactory< Wipe >,// Create method
	"WP01",               // Plugin unique ID of maximum length 4.
	"SW Wipe",            // Plugin name
	2,                    // API major version number
	1,                    // API minor version number
	0,                    // Plugin major version number
	1,                    // Plugin minor version number
	FF_MIXER,             // Plugin type
	"A 1970s vision mixer's analogue pattern generator. Every wipe is a waveform -- ramps and parabolas combined in a small matrix -- and a comparator against the fader. Softness is the comparator's gain, the border is a second comparator, modulation is a sine on the waveform, and a circle's edge is softer near its middle because the parabola is flatter there.\n\nThis is a MIXER: it wipes this layer in over the layer below.",
	"Wipe FFGL mixer" );

static_assert( Wipe::PT_COUNT - Wipe::PT_ABOUT_FIRST == stoatworks::about::kParamCount,
               "the About block's size changed with the generated header" );

namespace
{
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

const char* const kPatternNames[ PAT_COUNT ] = { "Horizontal", "Vertical", "Box", "Diamond", "Circle", "Clock", "Matrix" };
const char* const kLawNames[ LAW_COUNT ]     = { "Edge", "Area" };

int optionIndex( float value, int count )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}

bool sameFrame( const Frame& a, const Frame& b )
{
	return std::memcmp( &a, &b, sizeof( Frame ) ) == 0;
}
} // namespace

Wipe::Wipe()
{
	//A mixer takes exactly two. This is a separate declaration from the type
	//above, read by the host through different function codes entirely.
	SetMinInputs( 2 );
	SetMaxInputs( 2 );
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Defaults. SetParamInfof reads each one back out of GetFloatParameter,
	// so these assignments are what the host is told the defaults are.
	//---------------------------------------------------------------------
	params[ PT_PATTERN ]     = static_cast< float >( PAT_HORIZONTAL );
	params[ PT_REVERSE ]     = 0.0f;
	params[ PT_FLIPFLOP ]    = 0.0f;
	params[ PT_ASPECT_COMP ] = 1.0f;

	//Half way, so a mixer dropped on a layer shows what it does at once.
	params[ PT_POSITION ] = 0.5f;
	params[ PT_LAW ]      = static_cast< float >( LAW_EDGE );

	params[ PT_SOFTNESS ]     = 0.0f;
	params[ PT_BORDER_WIDTH ] = 0.0f;
	params[ PT_BORDER_SOFT ]  = 0.0f;
	params[ PT_BORDER_R ]     = 1.0f;
	params[ PT_BORDER_G ]     = 1.0f;
	params[ PT_BORDER_B ]     = 1.0f;

	params[ PT_CENTRE_X ] = 0.5f;
	params[ PT_CENTRE_Y ] = 0.5f;
	params[ PT_ROTATION ] = 0.0f;
	params[ PT_ASPECT ]   = 0.5f;//unity

	params[ PT_MOD_AMOUNT ] = 0.0f;
	params[ PT_MOD_FREQ ]   = 0.4f;//about 2.6 cycles across the picture
	params[ PT_MOD_SPEED ]  = 0.25f;//1 Hz
	params[ PT_MULT_H ]     = 1.0f;
	params[ PT_MULT_V ]     = 1.0f;

	//---------------------------------------------------------------------
	// Declaration. Every ranged parameter is a plain 0..1 float, with the
	// conversions in Controls.cpp; the two Multiples are real integers.
	//---------------------------------------------------------------------
	auto option = [ this ]( unsigned int id, const char* name, int count, const char* const* names ) {
		SetOptionParamInfo( id, name, static_cast< unsigned int >( count ), params[ id ] );
		for( int i = 0; i < count; ++i )
			SetParamElementInfo( id, static_cast< unsigned int >( i ), names[ i ], static_cast< float >( i ) );
	};
	auto colour = [ this ]( unsigned int first, const char* label ) {
		const std::string base = label;
		SetParamInfo( first + 0, ( base + " Red" ).c_str(), FF_TYPE_RED, params[ first + 0 ] );
		SetParamInfo( first + 1, ( base + " Green" ).c_str(), FF_TYPE_GREEN, params[ first + 1 ] );
		SetParamInfo( first + 2, ( base + " Blue" ).c_str(), FF_TYPE_BLUE, params[ first + 2 ] );
	};

	option( PT_PATTERN, "Pattern", PAT_COUNT, kPatternNames );
	SetParamInfo( PT_REVERSE, "Reverse", FF_TYPE_BOOLEAN, false );
	SetParamInfo( PT_FLIPFLOP, "Flip-Flop", FF_TYPE_BOOLEAN, false );
	SetParamInfo( PT_ASPECT_COMP, "Aspect Comp", FF_TYPE_BOOLEAN, true );

	//Named Position, not Opacity. The SDK's Add example says Resolume looks
	//for a parameter named Opacity for the mix value; genlock recorded that
	//as unverified, so this plugin does not rely on it. Renaming is one line
	//once a session in front of Arena has answered the question.
	SetParamInfof( PT_POSITION, "Position", FF_TYPE_STANDARD );
	option( PT_LAW, "Law", LAW_COUNT, kLawNames );

	SetParamInfof( PT_SOFTNESS, "Softness", FF_TYPE_STANDARD );
	SetParamInfof( PT_BORDER_WIDTH, "Border Width", FF_TYPE_STANDARD );
	SetParamInfof( PT_BORDER_SOFT, "Border Softness", FF_TYPE_STANDARD );
	colour( PT_BORDER_R, "Border" );

	SetParamInfof( PT_CENTRE_X, "Centre X", FF_TYPE_STANDARD );
	SetParamInfof( PT_CENTRE_Y, "Centre Y", FF_TYPE_STANDARD );
	SetParamInfof( PT_ROTATION, "Rotation", FF_TYPE_STANDARD );
	SetParamInfof( PT_ASPECT, "Aspect", FF_TYPE_STANDARD );

	SetParamInfof( PT_MOD_AMOUNT, "Mod Amount", FF_TYPE_STANDARD );
	SetParamInfof( PT_MOD_FREQ, "Mod Frequency", FF_TYPE_STANDARD );
	SetParamInfof( PT_MOD_SPEED, "Mod Speed", FF_TYPE_STANDARD );
	//FF_TYPE_INTEGER is exempt from the 0..1 clamp, so these hold real
	//counts with real ranges.
	SetParamInfo( PT_MULT_H, "Multiple H", FF_TYPE_INTEGER, params[ PT_MULT_H ] );
	SetParamRange( PT_MULT_H, 1.0f, static_cast< float >( kMultipleMax ) );
	SetParamInfo( PT_MULT_V, "Multiple V", FF_TYPE_INTEGER, params[ PT_MULT_V ] );
	SetParamRange( PT_MULT_V, 1.0f, static_cast< float >( kMultipleMax ) );

	// Groups, the way Resolume shows them: each group one contiguous run.
	for( FFUInt32 i = PT_PATTERN; i <= PT_ASPECT_COMP; ++i )
		SetParamGroup( i, "Pattern" );
	for( FFUInt32 i = PT_POSITION; i <= PT_LAW; ++i )
		SetParamGroup( i, "Fader" );
	for( FFUInt32 i = PT_SOFTNESS; i <= PT_BORDER_B; ++i )
		SetParamGroup( i, "Edge" );
	for( FFUInt32 i = PT_CENTRE_X; i <= PT_ASPECT; ++i )
		SetParamGroup( i, "Positioner" );
	for( FFUInt32 i = PT_MOD_AMOUNT; i <= PT_MULT_V; ++i )
		SetParamGroup( i, "Modulation" );

	// The About block. Declared inline rather than through a helper, because
	// SetParamInfo is protected on CFFGLPlugin.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	FFGLLog::LogToHost( "Created Wipe mixer" );

	diag::init();
}

//---------------------------------------------------------------------------
FFResult Wipe::InitGL( const FFGLViewportStruct* vp )
{
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR )
	            + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	if( !shader.Compile( kVertexShader, kWipeShader ) )
	{
		//Returning FF_FAIL here is invisible to the operator: the mixer
		//simply does nothing in Resolume. These lines are the only record.
		diag::error( "the wipe shader failed to compile - the mixer will do nothing" );
		FFGLLog::LogToHost( "Wipe: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	diag::info( "initialised" );

	//Use base-class init as the success result so it retains the viewport.
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
FFResult Wipe::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	//The SDK's Add example guards on the input count and on each pointer,
	//with a comment saying a host calls a mixer with one input while the
	//operator is still patching. Its claim, not something measured here --
	//but a mixer that dereferenced a null would take Resolume down with it.
	if( pGL == nullptr || pGL->inputTextures == nullptr )
		return FF_FAIL;
	if( pGL->numInputTextures < 2 )
		return FF_FAIL;
	if( pGL->inputTextures[ 0 ] == nullptr || pGL->inputTextures[ 1 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& a = *pGL->inputTextures[ 0 ];//the layer below
	const FFGLTextureStruct& b = *pGL->inputTextures[ 1 ];//this layer
	//HardwareWidth and HardwareHeight are the DENOMINATORS in
	//GetMaxGLTexCoords, so a zero there is an infinite MaxUV.
	if( a.Width == 0 || a.Height == 0 || a.HardwareWidth == 0 || a.HardwareHeight == 0 )
		return FF_FAIL;
	if( b.Width == 0 || b.Height == 0 || b.HardwareWidth == 0 || b.HardwareHeight == 0 )
		return FF_FAIL;

	const double elapsed = clock.Tick();

	//-----------------------------------------------------------------
	// The fader, and the flip-flop. A transition is an arrival at one end
	// from the other; each one flips the direction while Flip-Flop is on.
	//-----------------------------------------------------------------
	const double position = std::clamp( static_cast< double >( params[ PT_POSITION ] ), 0.0, 1.0 );
	const bool flipflop   = params[ PT_FLIPFLOP ] > 0.5f;
	if( position >= 1.0 )
	{
		if( lastEnd == 0 )
		{
			++transitions;
			if( flipflop )
				flipped = !flipped;
		}
		lastEnd = 1;
	}
	else if( position <= 0.0 )
	{
		if( lastEnd == 1 )
		{
			++transitions;
			if( flipflop )
				flipped = !flipped;
		}
		lastEnd = 0;
	}
	if( !flipflop )
		flipped = false;

	//-----------------------------------------------------------------
	// The generator's frame, in physical units.
	//-----------------------------------------------------------------
	Frame frame;
	frame.pattern    = optionIndex( params[ PT_PATTERN ], PAT_COUNT );
	frame.reverse    = ( params[ PT_REVERSE ] > 0.5f ) != flipped;
	frame.aspectComp = params[ PT_ASPECT_COMP ] > 0.5f;
	frame.centreX    = std::clamp( params[ PT_CENTRE_X ], 0.0f, 1.0f );
	frame.centreY    = std::clamp( params[ PT_CENTRE_Y ], 0.0f, 1.0f );
	frame.rotation   = RotationRadiansFromParam( params[ PT_ROTATION ] );
	frame.aspect     = AspectFromParam( params[ PT_ASPECT ] );
	frame.multipleH  = MultipleFromParam( params[ PT_MULT_H ] );
	frame.multipleV  = MultipleFromParam( params[ PT_MULT_V ] );
	frame.softnessPx = SoftnessPxFromParam( params[ PT_SOFTNESS ] );
	frame.outW       = static_cast< int >( currentViewport.width );
	frame.outH       = static_cast< int >( currentViewport.height );
	if( frame.outW <= 0 || frame.outH <= 0 )
		return FF_FAIL;

	const Derived derived = Derive( frame );

	//Pixels on the horizontal ramp become W units through the pattern's
	//reference slope; every other pattern's edge follows its own slope from
	//there.
	const double softW       = frame.softnessPx * derived.unitsPerPixel;
	const double borderW     = BorderWidthPxFromParam( params[ PT_BORDER_WIDTH ] ) * derived.unitsPerPixel;
	const double borderSoftW = BorderSoftnessPxFromParam( params[ PT_BORDER_SOFT ] ) * derived.unitsPerPixel;
	const double modW        = ModAmountPxFromParam( params[ PT_MOD_AMOUNT ] ) * derived.unitsPerPixel;
	const double modFreq     = ModFrequencyFromParam( params[ PT_MOD_FREQ ] );
	const double modPhase    = timing::ModPhase( elapsed, ModSpeedHzFromParam( params[ PT_MOD_SPEED ] ) );

	//-----------------------------------------------------------------
	// The level. Edge law is the hardware's: linear in the fader. Area law
	// solves for the level whose (soft) B area is the fader, and is cached
	// because the solve is real CPU work.
	//-----------------------------------------------------------------
	double level;
	if( optionIndex( params[ PT_LAW ], LAW_COUNT ) == LAW_AREA )
	{
		if( !areaCache.valid || !sameFrame( areaCache.frame, frame ) || areaCache.position != position
		    || areaCache.softW != softW )
		{
			areaCache.level    = AreaLevel( frame, derived, position, softW );
			areaCache.frame    = frame;
			areaCache.position = position;
			areaCache.softW    = softW;
			areaCache.valid    = true;
		}
		level = areaCache.level;
	}
	else
		level = EdgeLevel( derived, position );

	lastState.frame            = frame;
	lastState.derived          = derived;
	lastState.level            = level;
	lastState.softW            = softW;
	lastState.borderW          = borderW;
	lastState.borderSoftW      = borderSoftW;
	lastState.modW             = modW;
	lastState.modPhase         = modPhase;
	lastState.effectiveReverse = frame.reverse;
	lastState.flipped          = flipped;
	lastState.transitions      = transitions;
	lastState.elapsedSeconds   = elapsed;

	//-----------------------------------------------------------------
	// Bind both inputs. The declaration ORDER matters: every
	// ffglex::Scoped* clears its binding on exit rather than restoring it,
	// so they must unwind as activate(1), bind(1) then activate(0), bind(0).
	//-----------------------------------------------------------------
	ScopedShaderBinding shaderBinding( shader.GetGLID() );
	ScopedSamplerActivation activateA( 0 );
	Scoped2DTextureBinding bindA( a.Handle );
	ScopedSamplerActivation activateB( 1 );
	Scoped2DTextureBinding bindB( b.Handle );

	shader.Set( "TextureA", 0 );
	shader.Set( "TextureB", 1 );

	//One MaxUV per input. They are different numbers whenever the two
	//layers are different sizes, which for a mixer is the normal case.
	const FFGLTexCoords maxA = GetMaxGLTexCoords( a );
	const FFGLTexCoords maxB = GetMaxGLTexCoords( b );
	shader.Set( "MaxUVA", maxA.s, maxA.t );
	shader.Set( "MaxUVB", maxB.s, maxB.t );
	shader.Set( "HalfTexelA", 0.5f / static_cast< float >( a.Width ), 0.5f / static_cast< float >( a.Height ) );
	shader.Set( "HalfTexelB", 0.5f / static_cast< float >( b.Width ), 0.5f / static_cast< float >( b.Height ) );

	shader.Set( "Position", static_cast< float >( position ) );

	shader.Set( "Pattern", static_cast< float >( frame.pattern ) );
	shader.Set( "Reverse", frame.reverse ? 1.0f : 0.0f );
	shader.Set( "Centre", static_cast< float >( derived.centreX ), static_cast< float >( derived.centreY ) );
	shader.Set( "ScaleX", static_cast< float >( derived.scaleX ) );
	shader.Set( "RotCS", static_cast< float >( derived.cosR ), static_cast< float >( derived.sinR ) );
	shader.Set( "Norm", static_cast< float >( derived.norm ) );
	shader.Set( "Multiple", static_cast< float >( frame.multipleH ), static_cast< float >( frame.multipleV ) );
	shader.Set( "WRange", static_cast< float >( derived.wMin ), static_cast< float >( derived.wMax ) );
	shader.Set( "MatrixCells", static_cast< float >( derived.matrixCols ), static_cast< float >( derived.matrixRows ) );

	shader.Set( "Level", static_cast< float >( level ) );
	shader.Set( "SoftW", static_cast< float >( softW ) );
	shader.Set( "BorderW", static_cast< float >( borderW ) );
	shader.Set( "BorderSoftW", static_cast< float >( borderSoftW ) );
	shader.Set( "BorderColour", params[ PT_BORDER_R ], params[ PT_BORDER_G ], params[ PT_BORDER_B ] );

	shader.Set( "ModW", static_cast< float >( modW ) );
	shader.Set( "ModFreq", static_cast< float >( modFreq ) );
	shader.Set( "ModPhase", static_cast< float >( modPhase ) );

	quad.Draw();

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Wipe::DeInitGL()
{
	shader.FreeGLResources();
	quad.Release();
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Wipe::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// The About buttons open a browser and store nothing.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	params[ index ] = value;
	return FF_SUCCESS;
}

float Wipe::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;

	return params[ index ];
}

//---------------------------------------------------------------------------
char* Wipe::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}

	return CFFGLPlugin::GetTextParameter( index );
}

FFResult Wipe::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only, so there is genuinely
	// nothing to store -- but it has to say so successfully.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;

	return CFFGLPlugin::SetTextParameter( index, value );
}

FFResult Wipe::SetTime( double time )
{
	clock.Observe( time );
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
// The three host callbacks nothing renders from. Their whole job is to leave
// a log a session in front of Resolume can be read from afterwards.
//---------------------------------------------------------------------------
void Wipe::SetHostInfo( const char* hostname, const char* version )
{
	CFFGLPlugin::SetHostInfo( hostname, version );
	diag::info( std::string( "host=" ) + ( hostname ? hostname : "?" ) + " version=" + ( version ? version : "?" )
	            + " loaded from " + diag::modulePath() );
}

void Wipe::SetBeatInfo( float bpm, float barPhase )
{
	CFFGLPlugin::SetBeatInfo( bpm, barPhase );
}

void Wipe::SetSampleRate( unsigned int sampleRate )
{
	CFFGLPlugin::SetSampleRate( sampleRate );
}
