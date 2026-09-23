#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace wipe
{
namespace
{
float clamp01( float value )
{
	return std::min( 1.0f, std::max( 0.0f, value ) );
}

constexpr float kPxMax = 64.0f;
} // namespace

float SoftnessPxFromParam( float value )
{
	return clamp01( value ) * kPxMax;
}

float BorderWidthPxFromParam( float value )
{
	return clamp01( value ) * kPxMax;
}

float BorderSoftnessPxFromParam( float value )
{
	return clamp01( value ) * kPxMax;
}

float RotationRadiansFromParam( float value )
{
	return clamp01( value ) * 6.283185307179586f;
}

float AspectFromParam( float value )
{
	//4^(2t-1): 0.25 at 0, 1 at 0.5, 4 at 1.
	return std::pow( 4.0f, 2.0f * clamp01( value ) - 1.0f );
}

float ModAmountPxFromParam( float value )
{
	return clamp01( value ) * kPxMax;
}

float ModFrequencyFromParam( float value )
{
	return 0.5f * std::pow( 64.0f, clamp01( value ) );
}

float ModSpeedHzFromParam( float value )
{
	return clamp01( value ) * 4.0f;
}

int MultipleFromParam( float value )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), 1, kMultipleMax );
}

float PxParamFor( double px )
{
	return clamp01( static_cast< float >( px / kPxMax ) );
}

} // namespace wipe
