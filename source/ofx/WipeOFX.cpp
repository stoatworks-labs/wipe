/// The OpenFX build of Wipe, for DaVinci Resolve, Vegas, Nuke, Natron and other
/// OFX hosts -- as a TRANSITION, the fleet's first.
///
/// ------------------------------------------------------- the two inputs
///
/// The FFGL build is a mixer: input 0 is A, the layer below, shown whole at
/// Opacity 0; input 1 is B, this layer, shown whole at Opacity 1. OpenFX's
/// Transition context has the same shape with the spec's names on it:
///
///     FFGL                          OpenFX (Transition context)
///     inputTextures[ 0 ]  A         SourceFrom   shown whole at Transition 0
///     inputTextures[ 1 ]  B         SourceTo     shown whole at Transition 1
///     Opacity (the fader)           Transition   the host's progress, 0 -> 1
///
/// So a transition in a timeline starts on the outgoing clip and ends on the
/// incoming one, and a Pattern means the same wipe in both builds: at Transition
/// 0.3 a Box has opened on SourceTo exactly as far as it opens on B at Opacity
/// 0.3. `Transition` is the spec's mandated parameter name
/// (kOfxImageEffectTransitionParamName); the host owns and animates it.
///
/// The General context is declared as well, with the same two clips and the
/// same parameter as an ordinary keyframeable control, because it costs nothing
/// and it is the only way Nuke, Natron or Fusion -- none of which hosts the
/// Transition context -- can use a two-input effect at all. Resolve's own
/// transition sample (DissolveTransitionPlugin, in Resolve's Developer/OpenFX)
/// declares exactly this pair.
///
/// ------------------------------------------------------- what is shared
///
/// The CPU half of every frame -- the parameter conversions, the generator's
/// frame, the waveform's range, both fader laws including the Area law's
/// closed-form solve, and the modulator's phase -- is `wipe::BeginPass` and
/// `wipe::LevelFor` in Pass.cpp, the code the FFGL plugin's ProcessOpenGL
/// calls. The per-pixel half is `wipe::Shade`, the wipe shader's main written
/// again in C++ and held to the GPU by `wptest --cpu`. There is no wipe
/// arithmetic in this file: what it does is marshalling, OFX's pixel formats in
/// and out, and the timeline's clock.
///
/// ------------------------------------------------------ what is different
///
/// **No Flip-Flop.** In Resolume it remembers which end the fader last rested
/// at and reverses every other wipe. That is state carried from one frame to
/// the next, and an OpenFX host renders frames alone, out of order and on
/// several threads -- and a transition in a timeline is its own instance with
/// no previous transition to remember. Reverse, which the FFGL Flip-Flop only
/// ever toggled, is here as itself; the plugin description says why the other
/// is not.
///
/// **The modulator runs on the timeline.** Its phase is `seconds * Mod Speed`,
/// with seconds the frame's time over the clip's frame rate, so any frame
/// renders on its own and scrubbing shows the wobble where it would be. The
/// FFGL build's phase runs from the first frame it drew instead -- the only
/// clock a mixer in a live host has. Resolve's Fusion page reports no frame
/// rate at all, so there it assumes 24 fps (see framesPerSecond): a host
/// property that is missing must never escape a render.
///
/// **The Area law is solved every frame.** The FFGL plugin caches the solve
/// between frames, which is mutable state across renders; here it is not
/// allowed, and the solve is under a tenth of a millisecond for any single
/// pattern (worst case, a Circle at Multiple 8 x 8 with the soft edge up, a
/// few milliseconds).
///
/// **Inputs are read pixel for pixel**, in the host's coordinates, as OpenFX
/// expects: a pixel outside an input's bounds is transparent black. The FFGL
/// build stretches each input over the output instead, because Resolume hands
/// a mixer two layers that may be any two sizes. A host conforms both clips of
/// a transition to the timeline first, and at matching sizes the two readings
/// are the same pixel.
///
/// **Pixel sizes follow the render scale and the pixel aspect.** Softness,
/// Border Width, Border Softness and Mod Amount are pixels of the full-size
/// output, so a half-resolution proxy draws them half as wide; and Aspect Comp
/// folds in the output's pixel aspect, so a circle on an anamorphic raster is
/// round on the display. Resolume has neither, so the FFGL build does neither.
///
/// Wipe has no audio path and no host-beat sync, so nothing is missing for want
/// of one.
///
/// ------------------------------------------------------------- and tiles
///
/// Declined. The waveform is a function of where a pixel sits in the whole
/// picture, and the Area law's level is a property of the whole picture, so a
/// tile cannot be rendered without the full frame's geometry.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>

#include "ofxsImageEffect.h"
#include "ofxsProcessing.h"

// After the OFX Support headers, which is where the OFX types come from.
#include "StoatworksAboutOFX.h"

#include "../Controls.h"
#include "../Pass.h"

namespace
{
constexpr const char* kPluginIdentifier = "com.stoatworks.wipe";
constexpr const char* kPluginName       = "Wipe";
constexpr const char* kPluginGrouping   = "Stoatworks";
constexpr const char* kPluginDescription =
	"A 1970s vision mixer's analogue pattern generator, as a transition. Every "
	"wipe is a waveform -- ramps and parabolas combined in a small matrix -- and "
	"a comparator against the fader. Softness is the comparator's gain, the "
	"border is a second comparator, modulation is a sine on the waveform, and a "
	"circle's edge is softer near its middle because the parabola is flatter "
	"there.\n\n"
	"Transition 0 is SourceFrom alone and 1 is SourceTo alone: the Resolume "
	"build's A (the layer below) and B (this layer), with Transition in place of "
	"its Opacity fader. In a host without transitions (Nuke, Natron, Fusion) wire "
	"the two inputs by hand and keyframe Transition.\n\n"
	"Not here, by design: Flip-Flop. In Resolume it remembers which end the fader "
	"last rested at and reverses every other wipe; a transition in a timeline has "
	"no previous one to remember, so use Reverse. The modulator's travel follows "
	"the timeline, so every frame renders on its own. Fusion reports no frame "
	"rate; there, Mod Speed assumes 24 fps.\n\n"
	"https://stoatworks-labs.com";

/// The frame rate when no host property supplies one: Resolve's default
/// timeline rate. Resolve's Fusion page reports none -- see framesPerSecond.
constexpr double kFallbackFps = 24.0;

constexpr const char* kParamTransition     = kOfxImageEffectTransitionParamName;
constexpr const char* kParamAspectComp     = "aspectComp";
constexpr const char* kParamPattern        = "pattern";
constexpr const char* kParamReverse        = "reverse";
constexpr const char* kParamLaw            = "law";
constexpr const char* kParamSoftness       = "softness";
constexpr const char* kParamBorderWidth    = "borderWidth";
constexpr const char* kParamBorderSoftness = "borderSoftness";
constexpr const char* kParamBorderColour   = "borderColour";
constexpr const char* kParamCentreX        = "centreX";
constexpr const char* kParamCentreY        = "centreY";
constexpr const char* kParamRotation       = "rotation";
constexpr const char* kParamAspect         = "aspect";
constexpr const char* kParamModAmount      = "modAmount";
constexpr const char* kParamModFrequency   = "modFrequency";
constexpr const char* kParamModSpeed       = "modSpeed";
constexpr const char* kParamMultipleH      = "multipleH";
constexpr const char* kParamMultipleV      = "multipleV";

//---------------------------------------------------------------------------
/// Everything one render needs, resolved once on the calling thread before a
/// pixel is touched: OFX forbids reading a parameter from the render threads,
/// and the parameters would otherwise be read once per pixel per thread.
//---------------------------------------------------------------------------
struct Setup
{
	wipe::Uniforms uniforms;

	/// The picture the waveform is drawn across: the output image's bounds.
	/// With tiles declined it is the whole output, at the render scale.
	OfxRectI picture = { 0, 0, 0, 0 };

	/// The fader's ends: 0 is SourceFrom alone, 1 SourceTo alone, -1 the wipe.
	/// Handled before any premultiplication so an end is the input to the bit.
	int end = -1;

	bool fromStraight = false;///< SourceFrom is RGBA with unpremultiplied alpha
	bool toStraight   = false;
	bool outStraight  = false;
};

inline float clamp01( float v )
{
	return std::min( std::max( v, 0.0f ), 1.0f );
}

/// One row of an input, read at the output's depth. Transparent black outside
/// the image, alpha 1 for an RGB clip.
template< class PIX, int maxValue >
struct SourceRow
{
	const PIX* pixels = nullptr;///< the row's first pixel, at bounds.x1
	int x1 = 0, x2 = 0;
	int components = 4;
	bool straight  = false;

	void at( const OFX::Image* image, int y, bool straightAlpha )
	{
		pixels = nullptr;
		if( image == nullptr )
			return;
		const OfxRectI bounds = image->getBounds();
		x1         = bounds.x1;
		x2         = bounds.x2;
		components = image->getPixelComponents() == OFX::ePixelComponentRGB ? 3 : 4;
		straight   = straightAlpha && components == 4;
		pixels     = static_cast< const PIX* >( image->getPixelAddress( bounds.x1, y ) );//null off the rows
	}

	/// The stored values as 0..1 floats, untouched: what an end stop copies.
	void raw( int x, float out[ 4 ] ) const
	{
		if( pixels == nullptr || x < x1 || x >= x2 )
		{
			out[ 0 ] = out[ 1 ] = out[ 2 ] = out[ 3 ] = 0.0f;
			return;
		}
		const PIX* p = pixels + static_cast< std::ptrdiff_t >( x - x1 ) * components;
		for( int c = 0; c < 3; ++c )
			out[ c ] = toFloat( p[ c ] );
		out[ 3 ] = components == 4 ? toFloat( p[ 3 ] ) : 1.0f;
	}

	/// Premultiplied, which is what the wipe mixes in -- the FFGL build mixes
	/// whatever Resolume hands it, and Resolume's layers are premultiplied.
	void premultiplied( int x, float out[ 4 ] ) const
	{
		raw( x, out );
		if( straight )
			for( int c = 0; c < 3; ++c )
				out[ c ] *= out[ 3 ];
	}

	static float toFloat( PIX value )
	{
		//The GPU reads an 8-bit texel as value / 255, correctly rounded, and
		//so does this: `wptest --cpu` depends on the two starting equal.
		return maxValue == 1 ? static_cast< float >( value )
		                     : static_cast< float >( value ) / static_cast< float >( maxValue );
	}
};

//---------------------------------------------------------------------------
class WipeProcessorBase : public OFX::ImageProcessor
{
public:
	explicit WipeProcessorBase( OFX::ImageEffect& effect ) :
		OFX::ImageProcessor( effect )
	{
	}

	void setSources( const OFX::Image* from, const OFX::Image* to, const Setup* value )
	{
		fromImg = from;
		toImg   = to;
		setup   = value;
	}

protected:
	const OFX::Image* fromImg = nullptr;
	const OFX::Image* toImg   = nullptr;
	const Setup* setup        = nullptr;
};

template< class PIX, int nComponents, int maxValue >
class WipeProcessor : public WipeProcessorBase
{
public:
	explicit WipeProcessor( OFX::ImageEffect& effect ) :
		WipeProcessorBase( effect )
	{
	}

	void multiThreadProcessImages( OfxRectI window ) override
	{
		const Setup& s     = *setup;
		const int pictureW = s.picture.x2 - s.picture.x1;
		const int pictureH = s.picture.y2 - s.picture.y1;
		if( pictureW <= 0 || pictureH <= 0 )
			return;

		SourceRow< PIX, maxValue > from, to;
		float a[ 4 ], b[ 4 ], out[ 4 ];

		for( int y = window.y1; y < window.y2; ++y )
		{
			if( _effect.abort() )
				break;

			PIX* dst = static_cast< PIX* >( _dstImg->getPixelAddress( window.x1, y ) );
			if( dst == nullptr )
				continue;

			from.at( fromImg, y, s.fromStraight );
			to.at( toImg, y, s.toStraight );

			//The fragment's picture coordinate at the pixel's centre, v UP --
			//OFX rows count from the bottom, as GL's do, so nothing flips.
			const float v = ( static_cast< float >( y - s.picture.y1 ) + 0.5f ) / static_cast< float >( pictureH );

			for( int x = window.x1; x < window.x2; ++x, dst += nComponents )
			{
				if( s.end >= 0 )
				{
					//An end stop is the one input, untouched -- a raw copy when
					//its alpha is stored the way the output's is, which is what
					//makes it bit for bit; through premultiplied when not.
					const SourceRow< PIX, maxValue >& only = s.end == 0 ? from : to;
					if( only.straight == s.outStraight )
					{
						only.raw( x, out );
						write( dst, out, false );
					}
					else
					{
						only.premultiplied( x, out );
						write( dst, out, s.outStraight );
					}
					continue;
				}

				const float u = ( static_cast< float >( x - s.picture.x1 ) + 0.5f ) / static_cast< float >( pictureW );
				from.premultiplied( x, a );
				to.premultiplied( x, b );
				wipe::Shade( s.uniforms, u, v, a, b, out );
				write( dst, out, s.outStraight );
			}
		}
	}

private:
	static void write( PIX* dst, const float in[ 4 ], bool unpremultiply )
	{
		float px[ 4 ] = { in[ 0 ], in[ 1 ], in[ 2 ], in[ 3 ] };
		if( unpremultiply && nComponents == 4 )
			for( int c = 0; c < 3; ++c )
				px[ c ] = px[ 3 ] > 0.0f ? px[ c ] / px[ 3 ] : 0.0f;

		for( int c = 0; c < nComponents; ++c )
		{
			//Integer formats clamp; float ones are left alone, as the rest of
			//the fleet's ports leave them -- a border colour or a clip past 1.0
			//in a float pipeline is the host's to keep.
			if( maxValue == 1 )
				dst[ c ] = static_cast< PIX >( px[ c ] );
			else
				dst[ c ] = static_cast< PIX >( std::lround( clamp01( px[ c ] ) * static_cast< float >( maxValue ) ) );
		}
	}
};

//---------------------------------------------------------------------------
class WipeOFXPlugin : public OFX::ImageEffect
{
public:
	explicit WipeOFXPlugin( OfxImageEffectHandle handle ) :
		OFX::ImageEffect( handle )
	{
		dstClip  = fetchClip( kOfxImageEffectOutputClipName );
		fromClip = fetchClip( kOfxImageEffectTransitionSourceFromClipName );
		toClip   = fetchClip( kOfxImageEffectTransitionSourceToClipName );

		transition     = fetchDoubleParam( kParamTransition );
		aspectComp     = fetchBooleanParam( kParamAspectComp );
		pattern        = fetchChoiceParam( kParamPattern );
		reverse        = fetchBooleanParam( kParamReverse );
		law            = fetchChoiceParam( kParamLaw );
		softness       = fetchDoubleParam( kParamSoftness );
		borderWidth    = fetchDoubleParam( kParamBorderWidth );
		borderSoftness = fetchDoubleParam( kParamBorderSoftness );
		borderColour   = fetchRGBParam( kParamBorderColour );
		centreX        = fetchDoubleParam( kParamCentreX );
		centreY        = fetchDoubleParam( kParamCentreY );
		rotation       = fetchDoubleParam( kParamRotation );
		aspect         = fetchDoubleParam( kParamAspect );
		modAmount      = fetchDoubleParam( kParamModAmount );
		modFrequency   = fetchDoubleParam( kParamModFrequency );
		modSpeed       = fetchDoubleParam( kParamModSpeed );
		multipleH      = fetchIntParam( kParamMultipleH );
		multipleV      = fetchIntParam( kParamMultipleV );
	}

	void render( const OFX::RenderArguments& args ) override
	{
		std::unique_ptr< OFX::Image > dst( dstClip->fetchImage( args.time ) );
		if( dst == nullptr )
			OFX::throwSuiteStatusException( kOfxStatFailed );

		const OFX::BitDepthEnum depth       = dst->getPixelDepth();
		const OFX::PixelComponentEnum comps = dst->getPixelComponents();
		if( comps != OFX::ePixelComponentRGBA && comps != OFX::ePixelComponentRGB )
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );

		//An unconnected input (possible in the General context) or a frame the
		//host has nothing for is transparent black, as OpenFX says it is.
		std::unique_ptr< OFX::Image > from( fetchSource( fromClip, args.time ) );
		std::unique_ptr< OFX::Image > to( fetchSource( toClip, args.time ) );
		for( const OFX::Image* source : { from.get(), to.get() } )
		{
			if( source == nullptr )
				continue;
			const OFX::PixelComponentEnum sc = source->getPixelComponents();
			if( source->getPixelDepth() != depth
			    || ( sc != OFX::ePixelComponentRGBA && sc != OFX::ePixelComponentRGB ) )
				OFX::throwSuiteStatusException( kOfxStatErrImageFormat );
		}

		const Setup setup = setupAt( args, *dst );

		switch( depth )
		{
		case OFX::eBitDepthUByte:
			comps == OFX::ePixelComponentRGBA
				? run< WipeProcessor< unsigned char, 4, 255 > >( args, dst.get(), from.get(), to.get(), setup )
				: run< WipeProcessor< unsigned char, 3, 255 > >( args, dst.get(), from.get(), to.get(), setup );
			break;
		case OFX::eBitDepthUShort:
			comps == OFX::ePixelComponentRGBA
				? run< WipeProcessor< unsigned short, 4, 65535 > >( args, dst.get(), from.get(), to.get(), setup )
				: run< WipeProcessor< unsigned short, 3, 65535 > >( args, dst.get(), from.get(), to.get(), setup );
			break;
		case OFX::eBitDepthFloat:
			comps == OFX::ePixelComponentRGBA
				? run< WipeProcessor< float, 4, 1 > >( args, dst.get(), from.get(), to.get(), setup )
				: run< WipeProcessor< float, 3, 1 > >( args, dst.get(), from.get(), to.get(), setup );
			break;
		default:
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );
		}
	}

	/// At the fader's ends the host can pass the one input straight through:
	/// the same pictures the FFGL build's end-stop branch fetches, bit for
	/// bit, without a render.
	bool isIdentity( const OFX::IsIdentityArguments& args, OFX::Clip*& identityClip, double& identityTime ) override
	{
		const double t = transition->getValueAtTime( args.time );
		if( t <= 0.0 )
			identityClip = fromClip;
		else if( t >= 1.0 )
			identityClip = toClip;
		else
			return false;
		identityTime = args.time;
		return true;
	}

	void changedParam( const OFX::InstanceChangedArgs& args, const std::string& paramName ) override
	{
		// The About links open a browser and change nothing about the render.
		if( stoatworks::about::ofx::changedParam( args, paramName ) )
			return;
	}

private:
	static OFX::Image* fetchSource( OFX::Clip* clip, double time )
	{
		if( clip == nullptr )
			return nullptr;
		//A host that does not say whether the clip is connected is asked for
		//the image anyway: an unconnected clip answers with no image, which
		//is transparent black, the same as saying so.
		bool connected = true;
		try
		{
			connected = clip->isConnected();
		}
		catch( ... )
		{
		}
		return connected ? clip->fetchImage( time ) : nullptr;
	}

	/// The controls at this frame's time, as the numbers the FFGL build's
	/// parameters hold: 0..1, an option's index, a Multiple's count.
	wipe::HostValues hostValuesAt( double t ) const
	{
		const auto value = [ t ]( OFX::DoubleParam* p ) {
			return static_cast< float >( p->getValueAtTime( t ) );
		};
		//ChoiceParam answers through an out parameter rather than a return
		//value, unlike every other param type in the Support library.
		const auto choice = [ t ]( OFX::ChoiceParam* p ) {
			int index = 0;
			p->getValueAtTime( t, index );
			return static_cast< float >( index );
		};
		const auto flag = [ t ]( OFX::BooleanParam* p ) {
			return p->getValueAtTime( t ) ? 1.0f : 0.0f;
		};

		wipe::HostValues host;
		host.aspectComp     = flag( aspectComp );
		host.pattern        = choice( pattern );
		host.reverse        = flag( reverse );
		host.position       = value( transition );
		host.law            = choice( law );
		host.softness       = value( softness );
		host.borderWidth    = value( borderWidth );
		host.borderSoftness = value( borderSoftness );

		double r = 1.0, g = 1.0, b = 1.0;
		borderColour->getValueAtTime( t, r, g, b );
		host.borderRed   = static_cast< float >( r );
		host.borderGreen = static_cast< float >( g );
		host.borderBlue  = static_cast< float >( b );

		host.centreX      = value( centreX );
		host.centreY      = value( centreY );
		host.rotation     = value( rotation );
		host.aspect       = value( aspect );
		host.modAmount    = value( modAmount );
		host.modFrequency = value( modFrequency );
		host.modSpeed     = value( modSpeed );
		host.multipleH    = static_cast< float >( multipleH->getValueAtTime( t ) );
		host.multipleV    = static_cast< float >( multipleV->getValueAtTime( t ) );
		return host;
	}

	/// The frame rate: the output clip's, else either input's, else the
	/// effect's -- the first positive, finite answer -- else kFallbackFps.
	///
	/// EVERY read is caught. Resolve's Fusion page (21.1, measured by the lead
	/// 2026-10-03) provides no frame rate at all, on the effect or on any clip,
	/// and the Support library turns the missing property into an exception
	/// that escapes `render` as kOfxStatErrMissingHostFeature: Fusion reports
	/// the composition "could not be processed" and draws nothing. The Edit
	/// page does provide one. Only Mod Speed reads the clock, so a missing rate
	/// costs the modulator its speed in real seconds, never the frame.
	double framesPerSecond() const
	{
		const auto positive = []( auto read ) {
			try
			{
				const double fps = read();
				return std::isfinite( fps ) && fps > 0.0 ? fps : 0.0;
			}
			catch( ... )
			{
				return 0.0;
			}
		};
		for( const OFX::Clip* clip : { dstClip, fromClip, toClip } )
		{
			if( clip == nullptr )
				continue;
			const double fps = positive( [ clip ] { return clip->getFrameRate(); } );
			if( fps > 0.0 )
				return fps;
		}
		const double fps = positive( [ this ] { return getFrameRate(); } );
		return fps > 0.0 ? fps : kFallbackFps;
	}

	/// OFX time is in FRAMES; the modulator wants seconds.
	double secondsAt( double t ) const
	{
		return t / framesPerSecond();
	}

	/// Straight (unpremultiplied) RGBA. A host that does not say is taken to
	/// be premultiplied, which is what Resolume hands the FFGL build and what
	/// most OFX hosts deliver.
	static bool straight( OFX::Clip* clip )
	{
		try
		{
			return clip->getPixelComponents() == OFX::ePixelComponentRGBA
			       && clip->getPreMultiplication() == OFX::eImageUnPreMultiplied;
		}
		catch( ... )
		{
			return false;
		}
	}

	Setup setupAt( const OFX::RenderArguments& args, const OFX::Image& dst ) const
	{
		Setup s;
		s.picture = dst.getBounds();

		const int outW     = s.picture.x2 - s.picture.x1;
		const int outH     = s.picture.y2 - s.picture.y1;
		const double scale = args.renderScale.x > 0.0 ? args.renderScale.x : 1.0;
		const double par   = dst.getPixelAspectRatio() > 0.0 ? dst.getPixelAspectRatio() : 1.0;

		//false: no Flip-Flop here (see the top of this file).
		wipe::Pass pass = wipe::BeginPass( hostValuesAt( args.time ), false, std::max( outW, 1 ), std::max( outH, 1 ),
		                                   secondsAt( args.time ), scale, par );

		//The ends draw no wipe, so they need no level -- and the Area law's
		//solve is the one cost here worth skipping.
		if( pass.position <= 0.0 )
			s.end = 0;
		else if( pass.position >= 1.0 )
			s.end = 1;
		else
			pass.level = wipe::LevelFor( pass );

		s.uniforms     = wipe::UniformsFor( pass );
		s.fromStraight = straight( fromClip );
		s.toStraight   = straight( toClip );
		s.outStraight  = straight( dstClip );
		return s;
	}

	template< class Processor >
	void run( const OFX::RenderArguments& args, OFX::Image* dst, const OFX::Image* from, const OFX::Image* to,
	          const Setup& setup )
	{
		Processor processor( *this );
		processor.setDstImg( dst );
		processor.setSources( from, to, &setup );
		processor.setRenderWindow( args.renderWindow );
		processor.process();
	}

	OFX::Clip* dstClip  = nullptr;
	OFX::Clip* fromClip = nullptr;
	OFX::Clip* toClip   = nullptr;

	OFX::DoubleParam* transition     = nullptr;
	OFX::BooleanParam* aspectComp    = nullptr;
	OFX::ChoiceParam* pattern        = nullptr;
	OFX::BooleanParam* reverse       = nullptr;
	OFX::ChoiceParam* law            = nullptr;
	OFX::DoubleParam* softness       = nullptr;
	OFX::DoubleParam* borderWidth    = nullptr;
	OFX::DoubleParam* borderSoftness = nullptr;
	OFX::RGBParam* borderColour      = nullptr;
	OFX::DoubleParam* centreX        = nullptr;
	OFX::DoubleParam* centreY        = nullptr;
	OFX::DoubleParam* rotation       = nullptr;
	OFX::DoubleParam* aspect         = nullptr;
	OFX::DoubleParam* modAmount      = nullptr;
	OFX::DoubleParam* modFrequency   = nullptr;
	OFX::DoubleParam* modSpeed       = nullptr;
	OFX::IntParam* multipleH         = nullptr;
	OFX::IntParam* multipleV         = nullptr;
};

//---------------------------------------------------------------------------
// Description.
//---------------------------------------------------------------------------
OFX::GroupParamDescriptor* defineGroup( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page,
                                        const char* name, const char* label )
{
	OFX::GroupParamDescriptor* group = desc.defineGroupParam( name );
	group->setLabels( label, label, label );
	page->addChild( *group );
	return group;
}

OFX::DoubleParamDescriptor* defineSlider( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page,
                                          OFX::GroupParamDescriptor* group, const char* name, const char* label,
                                          const char* hint, double value )
{
	OFX::DoubleParamDescriptor* param = desc.defineDoubleParam( name );
	param->setLabels( label, label, label );
	param->setHint( hint );
	param->setRange( 0.0, 1.0 );
	param->setDisplayRange( 0.0, 1.0 );
	param->setDefault( value );
	param->setIncrement( 0.001 );
	param->setDoubleType( OFX::eDoubleTypePlain );
	param->setParent( *group );
	page->addChild( *param );
	return param;
}

void defineToggle( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page,
                   OFX::GroupParamDescriptor* group, const char* name, const char* label, const char* hint,
                   bool value )
{
	OFX::BooleanParamDescriptor* param = desc.defineBooleanParam( name );
	param->setLabels( label, label, label );
	param->setHint( hint );
	param->setDefault( value );
	param->setParent( *group );
	page->addChild( *param );
}

void defineChoice( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page,
                   OFX::GroupParamDescriptor* group, const char* name, const char* label, const char* hint,
                   const char* const* options, int count, int value )
{
	OFX::ChoiceParamDescriptor* param = desc.defineChoiceParam( name );
	param->setLabels( label, label, label );
	param->setHint( hint );
	for( int i = 0; i < count; ++i )
		param->appendOption( options[ i ] );
	param->setDefault( value );
	param->setParent( *group );
	page->addChild( *param );
}

void defineMultiple( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page,
                     OFX::GroupParamDescriptor* group, const char* name, const char* label, const char* hint,
                     float value )
{
	OFX::IntParamDescriptor* param = desc.defineIntParam( name );
	param->setLabels( label, label, label );
	param->setHint( hint );
	param->setRange( 1, wipe::kMultipleMax );
	param->setDisplayRange( 1, wipe::kMultipleMax );
	param->setDefault( wipe::MultipleFromParam( value ) );
	param->setParent( *group );
	page->addChild( *param );
}

mDeclarePluginFactory( WipePluginFactory, {}, {} );
} // namespace

void WipePluginFactory::describe( OFX::ImageEffectDescriptor& desc )
{
	desc.setLabels( kPluginName, kPluginName, kPluginName );
	desc.setPluginGrouping( kPluginGrouping );
	desc.setPluginDescription( kPluginDescription );

	// Transition first: it is what this is. General as well, for the hosts that
	// have no transitions -- see the top of this file.
	desc.addSupportedContext( OFX::eContextTransition );
	desc.addSupportedContext( OFX::eContextGeneral );

	desc.addSupportedBitDepth( OFX::eBitDepthUByte );
	desc.addSupportedBitDepth( OFX::eBitDepthUShort );
	desc.addSupportedBitDepth( OFX::eBitDepthFloat );

	desc.setSingleInstance( false );
	desc.setHostFrameThreading( false );
	desc.setSupportsMultiResolution( true );
	// The waveform and the Area law's level both belong to the whole picture.
	desc.setSupportsTiles( false );
	// Every frame is a function of its own two inputs, its own parameters and
	// its own time -- no history, so no temporal access and no state.
	desc.setTemporalClipAccess( false );
	desc.setRenderTwiceAlways( false );
	desc.setSupportsMultipleClipPARs( false );
	desc.setRenderThreadSafety( OFX::eRenderFullySafe );
}

void WipePluginFactory::describeInContext( OFX::ImageEffectDescriptor& desc, OFX::ContextEnum )
{
	// The Transition context's mandated clips, by the spec's names. The General
	// context takes the same two, so the render does not care which it is in.
	for( const char* name : { kOfxImageEffectTransitionSourceFromClipName, kOfxImageEffectTransitionSourceToClipName } )
	{
		OFX::ClipDescriptor* clip = desc.defineClip( name );
		clip->addSupportedComponent( OFX::ePixelComponentRGBA );
		clip->addSupportedComponent( OFX::ePixelComponentRGB );
		clip->setTemporalClipAccess( false );
		clip->setSupportsTiles( false );
	}

	OFX::ClipDescriptor* dstClip = desc.defineClip( kOfxImageEffectOutputClipName );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGB );
	dstClip->setSupportsTiles( false );

	// Same names, ranges, defaults and groups as the FFGL build wherever the
	// control carries over, read from the same HostValues defaults, so the two
	// panels read alike and one guide covers both. The order is the FFGL
	// build's too, Aspect Comp first included: there it is first because
	// Arena hides a mixer's first parameter, and here it costs nothing to keep
	// one order.
	OFX::PageParamDescriptor* page = desc.definePageParam( "Controls" );
	const wipe::HostValues defaults;

	//---------------------------------------------------------------- Pattern
	OFX::GroupParamDescriptor* patternGroup = defineGroup( desc, page, "patternGroup", "Pattern" );

	defineToggle( desc, page, patternGroup, kParamAspectComp, "Aspect Comp",
	              "Correct the generator for the picture's shape, so a Circle is round and a Box square "
	              "on screen. Off, the pattern is drawn in the unit square and stretched: a 16:9 circle "
	              "is an ellipse, as an uncompensated generator drew it.",
	              defaults.aspectComp > 0.5f );

	defineChoice( desc, page, patternGroup, kParamPattern, "Pattern",
	              "The waveform the comparator reads. Horizontal and Vertical are the ramps; Box is the "
	              "larger of the two, Diamond their sum, Circle the two parabolas added; Clock is the angle "
	              "from twelve; Matrix switches a 32 x 18 grid of cells one at a time.",
	              wipe::kPatternNames, wipe::PAT_COUNT, static_cast< int >( defaults.pattern ) );

	defineToggle( desc, page, patternGroup, kParamReverse, "Reverse",
	              "Mirror the waveform within its own range: a Box closes instead of opening, a ramp runs "
	              "the other way, the Clock sweeps anticlockwise. Not a host's own reverse, which runs "
	              "the whole transition backwards. (The Resolume build's Flip-Flop toggles this on "
	              "alternate transitions; a timeline transition has no previous one, so it is not here.)",
	              defaults.reverse > 0.5f );

	//------------------------------------------------------------------ Fader
	OFX::GroupParamDescriptor* faderGroup = defineGroup( desc, page, "faderGroup", "Fader" );

	// The spec's mandated parameter. In a Transition context the host owns it
	// and animates it 0 -> 1 across the transition; in the General context it
	// is an ordinary control to keyframe. It is the Resolume build's Opacity.
	OFX::DoubleParamDescriptor* transitionParam =
		defineSlider( desc, page, faderGroup, kParamTransition, "Transition",
		              "How far through the wipe: 0 is SourceFrom alone, 1 is SourceTo alone, and at "
		              "exactly 0 and 1 the input is passed through untouched. A transition drives this "
		              "itself.",
		              defaults.position );
	transitionParam->setDoubleType( OFX::eDoubleTypeScale );

	defineChoice( desc, page, faderGroup, kParamLaw, "Law",
	              "Edge is the hardware's: the comparator's level moves in a straight line with the "
	              "fader, so a Box at half way has revealed less than half the picture. Area chooses the "
	              "level so the area of SourceTo on screen IS the fader, for every pattern, soft edge "
	              "included.",
	              wipe::kLawNames, wipe::LAW_COUNT, static_cast< int >( defaults.law ) );

	//------------------------------------------------------------------- Edge
	OFX::GroupParamDescriptor* edgeGroup = defineGroup( desc, page, "edgeGroup", "Edge" );

	defineSlider( desc, page, edgeGroup, kParamSoftness, "Softness",
	              "The soft edge's width, 0 to 64 output pixels measured on the Horizontal wipe. Every "
	              "other pattern's edge follows its own waveform's slope from there, so a Circle's edge "
	              "is softer when the circle is small.",
	              defaults.softness );
	defineSlider( desc, page, edgeGroup, kParamBorderWidth, "Border Width",
	              "A band of solid colour at the edge, 0 to 64 pixels: a second comparator above the "
	              "first. 0 is no border at all.",
	              defaults.borderWidth );
	defineSlider( desc, page, edgeGroup, kParamBorderSoftness, "Border Softness",
	              "The border's outer edge, 0 to 64 pixels. Only does anything with a Border Width.",
	              defaults.borderSoftness );

	OFX::RGBParamDescriptor* colourParam = desc.defineRGBParam( kParamBorderColour );
	colourParam->setLabels( "Border Colour", "Border Colour", "Border Colour" );
	colourParam->setHint( "The border's colour, painted opaque. The Resolume build declares it as "
	                      "Border Red, Border Green and Border Blue." );
	colourParam->setDefault( defaults.borderRed, defaults.borderGreen, defaults.borderBlue );
	colourParam->setParent( *edgeGroup );
	page->addChild( *colourParam );

	//------------------------------------------------------------- Positioner
	OFX::GroupParamDescriptor* positionerGroup = defineGroup( desc, page, "positionerGroup", "Positioner" );

	defineSlider( desc, page, positionerGroup, kParamCentreX, "Centre X",
	              "Where the pattern is centred, across the picture. Moves Box, Diamond, Circle and "
	              "Clock; the ramps and the Matrix ignore it, as the hardware's did.",
	              defaults.centreX );
	defineSlider( desc, page, positionerGroup, kParamCentreY, "Centre Y",
	              "Where the pattern is centred, up the picture. 0, 0 is the bottom-left corner.",
	              defaults.centreY );
	defineSlider( desc, page, positionerGroup, kParamRotation, "Rotation",
	              "Turns every pattern but the Matrix, 0 to 360 degrees.", defaults.rotation );
	defineSlider( desc, page, positionerGroup, kParamAspect, "Aspect",
	              "The pattern's own width against its height, 0.25 to 4 geometrically; the middle is 1. "
	              "Higher is narrower.",
	              defaults.aspect );

	//------------------------------------------------------------- Modulation
	OFX::GroupParamDescriptor* modulationGroup = defineGroup( desc, page, "modulationGroup", "Modulation" );

	defineSlider( desc, page, modulationGroup, kParamModAmount, "Mod Amount",
	              "A sine added to the waveform, so the edge wobbles: 0 to 64 pixels, measured as "
	              "Softness is. 0 is off.",
	              defaults.modAmount );
	defineSlider( desc, page, modulationGroup, kParamModFrequency, "Mod Frequency",
	              "How many waves fit in the picture, 0.5 to 32 geometrically.", defaults.modFrequency );
	defineSlider( desc, page, modulationGroup, kParamModSpeed, "Mod Speed",
	              "How fast the wobble travels along the edge, 0 to 4 cycles a second of timeline time.",
	              defaults.modSpeed );
	defineMultiple( desc, page, modulationGroup, kParamMultipleH, "Multiple H",
	                "Run the waveforms 1 to 8 times over across the picture, so the pattern repeats in "
	                "cells (per picture height of width while Aspect Comp is on, which keeps cells square).",
	                defaults.multipleH );
	defineMultiple( desc, page, modulationGroup, kParamMultipleV, "Multiple V",
	                "Run the waveforms 1 to 8 times over up the picture.", defaults.multipleV );

	// The Stoatworks About block: a read-only credit line and one push button
	// per link, in a group that starts folded. Last, so it sits under the
	// effect's own controls.
	stoatworks::about::ofx::describe( desc, page );
}

OFX::ImageEffect* WipePluginFactory::createInstance( OfxImageEffectHandle handle, OFX::ContextEnum )
{
	return new WipeOFXPlugin( handle );
}

void OFX::Plugin::getPluginIDs( OFX::PluginFactoryArray& ids )
{
	// Deliberately leaked: a by-value static would register an exit-time
	// destructor inside this module, and a host that dlclose()s the bundle
	// before process exit then jumps through a dangling pointer.
	static WipePluginFactory* factory =
		new WipePluginFactory( kPluginIdentifier, PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR );
	ids.push_back( factory );
}
