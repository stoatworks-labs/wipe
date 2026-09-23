#pragma once

#include <FFGLSDK.h>

//AFTER the SDK: this header names FFUInt32 and does not pull the SDK in
//itself, so an include placed above it fails with "unknown type name" errors
//that point at the About block rather than at the include order.
#include "StoatworksAboutParams.h"
#include "Timing.h"
#include "Waveform.h"

#include <string>

/**
    Wipe -- a vision mixer's analogue pattern generator, as an FFGL **mixer**
    for Resolume.

    A 1970s vision mixer did not store its wipes as pictures. It made them
    live from a few waveform generators running at line and field rate --
    ramps and parabolas -- combined in a small analogue matrix and compared
    against the fader's level. Where the waveform was below the level you saw
    B; above it, A. Every pattern here is a formula in those waveforms, and
    everything else -- softness, the border, the modulation -- falls out of
    the comparator and the waveform's slope.

    **This is the fleet's second FF_MIXER.** Read the mixer section of
    genlock's AGENTS.md before changing `ProcessOpenGL`: what is written there
    was measured, and several things the SDK's headers imply are not true.
*/
class Wipe : public CFFGLPlugin
{
public:
	Wipe();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	FFResult SetTime( double time ) override;

	/// None of these change a pixel. They exist so that ONE session in front
	/// of Resolume leaves a log that answers the things about mixers this
	/// repo has never been able to measure.
	void SetHostInfo( const char* hostname, const char* version ) override;
	void SetBeatInfo( float bpm, float barPhase ) override;
	void SetSampleRate( unsigned int sampleRate ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// instantiateGL pushes every declared default back through the setters
	/// and deletes the whole instance if one fails, and CFFGLPlugin's
	/// SetTextParameter is a stub that returns exactly that failure.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	/// Optional, not required -- see genlock's AGENTS.md. A four-character
	/// label for a host with no room for "SW Wipe".
	const char* GetShortName() override
	{
		return "Wipe";
	}

	/// What the last rendered frame actually used, for the harness: the
	/// derived frame, the level, and the comparators in W units. A check
	/// that disagrees with the picture can then say WHICH side is wrong.
	struct State
	{
		wipe::Frame frame;
		wipe::Derived derived;
		double level      = 0.0;
		double softW      = 0.0;
		double borderW    = 0.0;
		double borderSoftW = 0.0;
		double modW       = 0.0;
		double modPhase   = 0.0;
		bool effectiveReverse = false;
		bool flipped      = false;
		int transitions   = 0;
		double elapsedSeconds = 0.0;
	};
	const State& StateForTest() const
	{
		return lastState;
	}

	/// Clock test hook. The offline harness DECLARES its unit rather than
	/// leaving the calibration to infer one.
	void SetClockScaleForTest( double scale )
	{
		clock.SetScaleForTest( scale );
	}

	/// In the order the host shows them.
	enum ParamID : FFUInt32
	{
		//Pattern
		PT_PATTERN,
		PT_REVERSE,
		PT_FLIPFLOP,
		PT_ASPECT_COMP,

		//Fader
		PT_POSITION,
		PT_LAW,

		//Edge
		PT_SOFTNESS,
		PT_BORDER_WIDTH,
		PT_BORDER_SOFT,
		PT_BORDER_R,
		PT_BORDER_G,
		PT_BORDER_B,

		//Positioner
		PT_CENTRE_X,
		PT_CENTRE_Y,
		PT_ROTATION,
		PT_ASPECT,

		//Modulation
		PT_MOD_AMOUNT,
		PT_MOD_FREQ,
		PT_MOD_SPEED,
		PT_MULT_H,
		PT_MULT_V,

		//About. Last in the enum so nothing before it ever moves.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

private:
	ffglex::FFGLShader shader;
	ffglex::FFGLScreenQuad quad;

	wipe::timing::Clock clock;
	State lastState;

	float params[ PT_COUNT ] = {};

	/// Flip-Flop: which end the fader last rested at (-1: neither yet), and
	/// whether the alternate transition is the current one.
	int lastEnd  = -1;
	bool flipped = false;
	int transitions = 0;

	/// The Area law's solve is cached: it costs real CPU time and most
	/// frames change nothing.
	struct AreaCache
	{
		bool valid = false;
		wipe::Frame frame;
		double position = -1.0;
		double softW    = -1.0;
		double level    = 0.0;
	} areaCache;

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
