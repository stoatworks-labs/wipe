#pragma once

/**
    The host's clock, and the one phase Wipe takes from it.

    A wipe is static: the fader decides the picture, and nothing here moves
    on its own except the **modulator**, whose sine slides along the edge at
    `Mod Speed`. That one phase is enough to inherit both of the fleet's
    clock lessons, so they are inherited whole:

    - **Resolume hands over milliseconds, not seconds.** The FFGL header never
      says which, and hosts disagree. `Clock` settles the unit by watching the
      host's clock against a real one for a few frames -- the ratio names the
      unit -- exactly as tinsel and genlock do.
    - **A float cannot hold the host's clock.** Resolume counts milliseconds
      from the start of the session, and past about 4.99e8 ms -- a little under
      six days -- a 32-bit float can no longer represent consecutive
      milliseconds at all. Anything computed from an absolute host time in
      float stops moving. So time here is kept **frame-relative**: the first
      settled reading becomes the epoch, everything downstream is `now -
      epoch`, and the reduction to a phase happens in **double** before any
      float ever sees it.

    What the shader is handed is never a time. It is a phase already reduced
    into [0, 1), so the float it arrives in has its full precision available
    for the part that matters.
*/
namespace wipe::timing
{

/// The host's clock, in seconds, with its unit worked out by observation.
class Clock
{
public:
	/// Called from the plugin's SetTime with whatever the host said.
	void Observe( double hostTime );

	/// Advance to this frame. Must be called once per ProcessOpenGL, after
	/// Observe. Returns seconds since the epoch.
	double Tick();

	/// Seconds since the epoch, as of the last Tick.
	double Elapsed() const
	{
		return elapsed;
	}

	/// The multiplier taking the host's unit to seconds: 1.0 for seconds,
	/// 0.001 for milliseconds, 0 while undecided. Declared rather than
	/// inferred by the offline harness, which knows it sends seconds.
	void SetScaleForTest( double scale )
	{
		scale_ = scale;
	}

	double Scale() const
	{
		return scale_;
	}

	/// Whether the host has ever called SetTime, and what it last said. Only
	/// the diagnostics log reads these. Resolume Arena 7.27.1 drives a MIXER's
	/// clock every frame, in milliseconds (measured on genlock); whether it
	/// does the same for this one is what the log would confirm.
	bool Observed() const
	{
		return raw_ >= 0.0;
	}

	double Raw() const
	{
		return raw_;
	}

	void Votes( int& seconds, int& millis ) const
	{
		seconds = secondsVotes_;
		millis  = millisVotes_;
	}

private:
	double raw_        = -1.0;///< the host's last reading, in the host's unit
	double lastRaw_    = -1.0;
	double lastWall_   = -1.0;
	double wallStart_  = -1.0;
	double scale_      = 0.0;
	double epoch_      = 0.0;
	bool epochSet_     = false;
	int secondsVotes_  = 0;
	int millisVotes_   = 0;
	double elapsed     = 0.0;
};

/// The modulator's phase, in cycles, reduced into [0, 1). Reduced in double,
/// from an elapsed time that is already frame-relative.
double ModPhase( double elapsedSeconds, double cyclesPerSecond );

/// A positive remainder. `fmod` keeps the sign of the numerator, so a
/// negative rate -- or a host that scrubs backwards -- would otherwise put
/// the phase outside its own period.
double PositiveMod( double value, double period );

} // namespace wipe::timing
