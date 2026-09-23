#include "Waveform.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace wipe
{
namespace
{
constexpr double kTwoPi = 6.283185307179586;

struct Pt
{
	double x, y;
};
using Poly = std::vector< Pt >;

double cross( Pt a, Pt b )
{
	return a.x * b.y - a.y * b.x;
}

double dot( Pt a, Pt b )
{
	return a.x * b.x + a.y * b.y;
}

/// Shoelace. Positive for a counter-clockwise polygon.
double polyArea( const Poly& p )
{
	double sum = 0.0;
	for( size_t i = 0, n = p.size(); i < n; ++i )
		sum += cross( p[ i ], p[ ( i + 1 ) % n ] );
	return 0.5 * sum;
}

/// Sutherland-Hodgman against one half-plane: keeps the part where
/// a*x + b*y <= c. Convex in, convex out.
Poly clipHalf( const Poly& in, double a, double b, double c )
{
	Poly out;
	const size_t n = in.size();
	if( n == 0 )
		return out;
	out.reserve( n + 2 );
	for( size_t i = 0; i < n; ++i )
	{
		const Pt cur  = in[ i ];
		const Pt prev = in[ ( i + n - 1 ) % n ];
		const double fc = a * cur.x + b * cur.y - c;
		const double fp = a * prev.x + b * prev.y - c;
		const bool inC = fc <= 0.0;
		const bool inP = fp <= 0.0;
		if( inC != inP )
		{
			const double t = fp / ( fp - fc );
			out.push_back( { prev.x + t * ( cur.x - prev.x ), prev.y + t * ( cur.y - prev.y ) } );
		}
		if( inC )
			out.push_back( cur );
	}
	return out;
}

/// Convex polygon (CCW) intersected with the disc of radius r about the
/// origin, exactly: every edge contributes the signed area between the
/// origin and its clipped self -- a triangle where the edge is inside the
/// disc, a sector where it is outside. Contributions from edges on the far
/// side of the origin cancel the way the shoelace formula's do.
double edgeDiscContribution( Pt a, Pt b, double r )
{
	const Pt d      = { b.x - a.x, b.y - a.y };
	const double A  = dot( d, d );
	const double B  = 2.0 * dot( a, d );
	const double C  = dot( a, a ) - r * r;
	double ts[ 4 ]  = { 0.0, 1.0, 1.0, 1.0 };
	int count       = 1;
	if( A > 0.0 )
	{
		const double disc = B * B - 4.0 * A * C;
		if( disc > 0.0 )
		{
			const double s  = std::sqrt( disc );
			const double t0 = ( -B - s ) / ( 2.0 * A );
			const double t1 = ( -B + s ) / ( 2.0 * A );
			if( t0 > 0.0 && t0 < 1.0 )
				ts[ count++ ] = t0;
			if( t1 > 0.0 && t1 < 1.0 )
				ts[ count++ ] = t1;
		}
	}
	ts[ count++ ] = 1.0;

	double sum = 0.0;
	for( int i = 0; i + 1 < count; ++i )
	{
		const Pt p0 = { a.x + ts[ i ] * d.x, a.y + ts[ i ] * d.y };
		const Pt p1 = { a.x + ts[ i + 1 ] * d.x, a.y + ts[ i + 1 ] * d.y };
		const double tm = 0.5 * ( ts[ i ] + ts[ i + 1 ] );
		const Pt m      = { a.x + tm * d.x, a.y + tm * d.y };
		if( dot( m, m ) < r * r )
			sum += 0.5 * cross( p0, p1 );
		else
			sum += 0.5 * r * r * std::atan2( cross( p0, p1 ), dot( p0, p1 ) );
	}
	return sum;
}

double discPolyArea( const Poly& p, Pt centre, double r )
{
	if( !( r > 0.0 ) || p.size() < 3 )
		return 0.0;
	double sum = 0.0;
	for( size_t i = 0, n = p.size(); i < n; ++i )
	{
		const Pt a = { p[ i ].x - centre.x, p[ i ].y - centre.y };
		const Pt b = { p[ ( i + 1 ) % n ].x - centre.x, p[ ( i + 1 ) % n ].y - centre.y };
		sum += edgeDiscContribution( a, b, r );
	}
	return std::max( 0.0, sum );
}

//---------------------------------------------------------------------------
// The frame.
//---------------------------------------------------------------------------
bool usesPositioner( int pattern )
{
	return pattern == PAT_BOX || pattern == PAT_DIAMOND || pattern == PAT_CIRCLE || pattern == PAT_CLOCK;
}

Pt toFrame( const Derived& d, double u, double v )
{
	const double dx = ( u - d.centreX ) * d.scaleX;
	const double dy = v - d.centreY;
	return { d.cosR * dx - d.sinR * dy, d.sinR * dx + d.cosR * dy };
}

/// The picture, as a polygon in q. Counter-clockwise because the map's
/// determinant (ScaleX) is positive.
Poly picturePoly( const Derived& d )
{
	return { toFrame( d, 0.0, 0.0 ), toFrame( d, 1.0, 0.0 ), toFrame( d, 1.0, 1.0 ), toFrame( d, 0.0, 1.0 ) };
}

double normOf( int pattern, double x, double y )
{
	switch( pattern )
	{
	case PAT_BOX: return std::max( std::fabs( x ), std::fabs( y ) );
	case PAT_DIAMOND: return std::fabs( x ) + std::fabs( y );
	default: return std::sqrt( x * x + y * y );
	}
}

Poly boundsOf( const Poly& p, double& xmin, double& xmax, double& ymin, double& ymax )
{
	xmin = ymin = 1e300;
	xmax = ymax = -1e300;
	for( const Pt& v : p )
	{
		xmin = std::min( xmin, v.x );
		xmax = std::max( xmax, v.x );
		ymin = std::min( ymin, v.y );
		ymax = std::max( ymax, v.y );
	}
	return p;
}

/// The shape { norm( q - centre ) < size } for the closed patterns, as a
/// convex polygon (box, diamond), intersected with `region`.
double shapeArea( int pattern, const Poly& region, Pt centre, double size )
{
	if( !( size > 0.0 ) || region.size() < 3 )
		return 0.0;
	if( pattern == PAT_CIRCLE )
		return discPolyArea( region, centre, size );

	Poly p = region;
	if( pattern == PAT_BOX )
	{
		p = clipHalf( p, 1.0, 0.0, centre.x + size );
		p = clipHalf( p, -1.0, 0.0, -( centre.x - size ) );
		p = clipHalf( p, 0.0, 1.0, centre.y + size );
		p = clipHalf( p, 0.0, -1.0, -( centre.y - size ) );
	}
	else//diamond: |x| + |y| < size, four half-planes
	{
		p = clipHalf( p, 1.0, 1.0, centre.x + centre.y + size );
		p = clipHalf( p, 1.0, -1.0, centre.x - centre.y + size );
		p = clipHalf( p, -1.0, 1.0, -centre.x + centre.y + size );
		p = clipHalf( p, -1.0, -1.0, -centre.x - centre.y + size );
	}
	return polyArea( p );
}

/// The wedge of angles [a0, a1) clockwise from twelve, a1 - a0 <= pi,
/// intersected with `region` (which must be about the wedge's apex at the
/// origin).
double wedgeArea( const Poly& region, double a0, double a1 )
{
	const Pt d0 = { std::sin( a0 ), std::cos( a0 ) };
	const Pt d1 = { std::sin( a1 ), std::cos( a1 ) };
	//inside: cross( d0, q ) <= 0  i.e.  d0.x*q.y - d0.y*q.x <= 0
	Poly p = clipHalf( region, -d0.y, d0.x, 0.0 );
	//and     cross( d1, q ) >= 0  i.e. -d1.x*q.y + d1.y*q.x <= 0
	p = clipHalf( p, d1.y, -d1.x, 0.0 );
	return polyArea( p );
}

//---------------------------------------------------------------------------
// The geometry that does not depend on the level, built once per solve: the
// picture in q, its bounds, and for the lattice patterns every cell that
// meets the picture, already clipped to it. The solve evaluates the area a
// few hundred times and the lattice can hold a hundred cells, so clipping
// them on every evaluation was most of the cost.
//---------------------------------------------------------------------------
struct Prepared
{
	Poly picture;
	double qxmin = 0, qxmax = 0, qymin = 0, qymax = 0;
	struct Cell
	{
		Poly region;
		Pt centre;
	};
	std::vector< Cell > cells;
};

Prepared prepare( const Frame& frame, const Derived& d )
{
	Prepared p;
	p.picture = picturePoly( d );
	boundsOf( p.picture, p.qxmin, p.qxmax, p.qymin, p.qymax );
	const bool lattice = ( frame.pattern == PAT_BOX || frame.pattern == PAT_DIAMOND || frame.pattern == PAT_CIRCLE )
	                     && ( frame.multipleH > 1 || frame.multipleV > 1 );
	if( lattice )
	{
		const double nh = frame.multipleH, nv = frame.multipleV;
		const int imin  = static_cast< int >( std::floor( p.qxmin * nh ) ) - 1;
		const int imax  = static_cast< int >( std::ceil( p.qxmax * nh ) ) + 1;
		const int jmin  = static_cast< int >( std::floor( p.qymin * nv ) ) - 1;
		const int jmax  = static_cast< int >( std::ceil( p.qymax * nv ) ) + 1;
		for( int j = jmin; j <= jmax; ++j )
			for( int i = imin; i <= imax; ++i )
			{
				Poly cell = clipHalf( p.picture, 1.0, 0.0, ( i + 0.5 ) / nh );
				cell      = clipHalf( cell, -1.0, 0.0, -( i - 0.5 ) / nh );
				cell      = clipHalf( cell, 0.0, 1.0, ( j + 0.5 ) / nv );
				cell      = clipHalf( cell, 0.0, -1.0, -( j - 0.5 ) / nv );
				if( cell.size() >= 3 && polyArea( cell ) > 0.0 )
					p.cells.push_back( { cell, { i / nh, j / nv } } );
			}
	}
	return p;
}

//---------------------------------------------------------------------------
// The hard area, in the un-reversed waveform.
//---------------------------------------------------------------------------
double hardAreaRaw( const Frame& frame, const Derived& d, const Prepared& pre, double level )
{
	if( level <= d.wMin )
		return 0.0;
	if( level >= d.wMax )
		return 1.0;

	const Poly& picture = pre.picture;
	const double detInv = 1.0 / d.scaleX;//q area to picture area
	const double qxmin = pre.qxmin, qxmax = pre.qxmax, qymin = pre.qymin, qymax = pre.qymax;

	switch( frame.pattern )
	{
	case PAT_HORIZONTAL:
	case PAT_VERTICAL:
	{
		const bool horizontal = frame.pattern == PAT_HORIZONTAL;
		const int n           = horizontal ? frame.multipleH : frame.multipleV;
		const double a = horizontal ? 1.0 : 0.0, b = horizontal ? 0.0 : 1.0;
		if( n == 1 )
			return polyArea( clipHalf( picture, a, b, level - 0.5 ) ) * detInv;

		//Strips: W = fract( n * ( q + 0.5 ) ) < level.
		const double lo  = ( horizontal ? qxmin : qymin ) + 0.5;
		const double hi  = ( horizontal ? qxmax : qymax ) + 0.5;
		const int kmin   = static_cast< int >( std::floor( lo * n ) ) - 1;
		const int kmax   = static_cast< int >( std::ceil( hi * n ) ) + 1;
		const double l   = std::clamp( level, 0.0, 1.0 );
		double sum       = 0.0;
		for( int k = kmin; k <= kmax; ++k )
		{
			const double s0 = static_cast< double >( k ) / n - 0.5;
			const double s1 = ( static_cast< double >( k ) + l ) / n - 0.5;
			Poly p          = clipHalf( picture, -a, -b, -s0 );
			p               = clipHalf( p, a, b, s1 );
			sum += polyArea( p );
		}
		return sum * detInv;
	}

	case PAT_BOX:
	case PAT_DIAMOND:
	case PAT_CIRCLE:
	{
		//W = norm / R (box, diamond) or norm^2 / R^2 (circle): the level set
		//is the shape of "size" level*R or R*sqrt(level).
		const double size = frame.pattern == PAT_CIRCLE ? d.norm * std::sqrt( std::max( 0.0, level ) ) : d.norm * level;
		if( frame.multipleH == 1 && frame.multipleV == 1 )
			return shapeArea( frame.pattern, picture, { 0.0, 0.0 }, size ) * detInv;

		//Each copy inside its own cell (the wrap keeps it there), and each
		//cell already clipped to the picture.
		double sum = 0.0;
		for( const Prepared::Cell& cell : pre.cells )
			sum += shapeArea( frame.pattern, cell.region, cell.centre, size );
		( void )qymin;
		( void )qymax;
		return sum * detInv;
	}

	case PAT_CLOCK:
	{
		const int n     = frame.multipleH;
		const double l  = std::clamp( level, 0.0, 1.0 );
		const double w  = kTwoPi * l / n;//each wedge's width
		double sum      = 0.0;
		for( int k = 0; k < n; ++k )
		{
			const double a0 = kTwoPi * k / n;
			if( w <= 3.141592653589793 )
				sum += wedgeArea( picture, a0, a0 + w );
			else//only n == 1: the complement is the convex one
				sum += polyArea( picture ) - wedgeArea( picture, a0 + w, a0 + kTwoPi );
		}
		return sum * detInv;
	}

	case PAT_MATRIX:
	{
		const double count = static_cast< double >( d.matrixCols ) * d.matrixRows;
		//W = ( rank + 0.5 ) / count < level  <=>  rank < level*count - 0.5
		const double below = std::ceil( level * count - 0.5 );
		return std::clamp( below, 0.0, count ) / count;
	}

	default:
		return 0.0;
	}
}

double hardArea( const Frame& frame, const Derived& d, const Prepared& pre, double level )
{
	if( frame.reverse )
		return 1.0 - hardAreaRaw( frame, d, pre, d.wMin + d.wMax - level );
	return hardAreaRaw( frame, d, pre, level );
}

//---------------------------------------------------------------------------
// Adaptive Simpson. The area function is continuous with kinks (a shape's
// corner reaching the picture's edge), so a fixed-order rule would carry an
// O(h^2) error at each kink; bisecting until the local estimate settles
// puts the fine steps only where the kinks are.
//---------------------------------------------------------------------------
struct AreaFn
{
	const Frame& frame;
	const Derived& d;
	const Prepared& pre;
	double operator()( double l ) const
	{
		return hardArea( frame, d, pre, l );
	}
};

double simpsonRec( const AreaFn& f, double a, double b, double fa, double fm, double fb, double whole, double eps, int depth )
{
	const double m  = 0.5 * ( a + b );
	const double lm = 0.5 * ( a + m ), rm = 0.5 * ( m + b );
	const double flm = f( lm ), frm = f( rm );
	const double left  = ( m - a ) / 6.0 * ( fa + 4.0 * flm + fm );
	const double right = ( b - m ) / 6.0 * ( fm + 4.0 * frm + fb );
	const double delta = left + right - whole;
	if( depth <= 0 || std::fabs( delta ) <= 15.0 * eps )
		return left + right + delta / 15.0;
	return simpsonRec( f, a, m, fa, flm, fm, left, 0.5 * eps, depth - 1 )
	       + simpsonRec( f, m, b, fm, frm, fb, right, 0.5 * eps, depth - 1 );
}

double simpson( const AreaFn& f, double a, double b, double eps )
{
	const double fa = f( a ), fb = f( b ), fm = f( 0.5 * ( a + b ) );
	const double whole = ( b - a ) / 6.0 * ( fa + 4.0 * fm + fb );
	return simpsonRec( f, a, b, fa, fm, fb, whole, eps, 24 );
}

//---------------------------------------------------------------------------
// The matrix's order: a Feistel network over the next power of two, walked
// until it lands inside the count. Identical, integer for integer, to the
// GLSL in Shaders.cpp.
//---------------------------------------------------------------------------
uint32_t mix32( uint32_t x )
{
	//PCG output permutation on a Weyl-stepped input: cheap, integer, and
	//the same on every GPU.
	x ^= x >> 16;
	x *= 0x7FEB352Du;
	x ^= x >> 15;
	x *= 0x846CA68Bu;
	x ^= x >> 16;
	return x;
}

uint32_t feistel( uint32_t x, uint32_t halfBits )
{
	const uint32_t mask = ( 1u << halfBits ) - 1u;
	uint32_t l = x >> halfBits;
	uint32_t r = x & mask;
	for( uint32_t round = 0; round < 4u; ++round )
	{
		const uint32_t f = mix32( r + round * 0x9E3779B9u ) & mask;
		const uint32_t t = r;
		r                = l ^ f;
		l                = t;
	}
	return ( l << halfBits ) | r;
}
} // namespace

unsigned MatrixRank( unsigned index, unsigned count )
{
	if( count < 2 )
		return 0;
	//The smallest EVEN number of bits that holds count, so the two Feistel
	//halves are equal.
	uint32_t bits = 2;
	while( ( 1u << bits ) < count )
		bits += 2;
	uint32_t x = index % count;
	for( int walk = 0; walk < 64; ++walk )
	{
		x = feistel( x, bits / 2 );
		if( x < count )
			return x;
	}
	return index % count;//unreachable in practice: the walk lands in a handful of steps
}

double MatrixThreshold( unsigned index, unsigned count )
{
	return ( static_cast< double >( MatrixRank( index, count ) ) + 0.5 ) / static_cast< double >( count );
}

//---------------------------------------------------------------------------
Derived Derive( const Frame& frame )
{
	Derived d;
	const bool positioned = usesPositioner( frame.pattern );
	d.centreX = positioned ? std::clamp( frame.centreX, 0.0, 1.0 ) : 0.5;
	d.centreY = positioned ? std::clamp( frame.centreY, 0.0, 1.0 ) : 0.5;
	const double pictureAspect = static_cast< double >( frame.outW ) / std::max( 1, frame.outH );
	d.scaleX = ( frame.aspectComp ? pictureAspect : 1.0 ) * frame.aspect;
	d.cosR   = std::cos( frame.rotation );
	d.sinR   = std::sin( frame.rotation );
	d.matrixCols = kMatrixBaseCols * frame.multipleH;
	d.matrixRows = kMatrixBaseRows * frame.multipleV;

	const Poly picture = picturePoly( d );

	//The gradient of q.x and q.y in output pixels, for the reference slope.
	const double gxx = d.cosR * d.scaleX / frame.outW, gxy = -d.sinR / frame.outH;//grad of q.x
	const double gyx = d.sinR * d.scaleX / frame.outW, gyy = d.cosR / frame.outH;//grad of q.y
	const double gradQx = std::sqrt( gxx * gxx + gxy * gxy );
	const double gradQy = std::sqrt( gyx * gyx + gyy * gyy );

	switch( frame.pattern )
	{
	case PAT_HORIZONTAL:
	case PAT_VERTICAL:
	{
		const bool horizontal = frame.pattern == PAT_HORIZONTAL;
		const int n           = horizontal ? frame.multipleH : frame.multipleV;
		if( n == 1 )
		{
			d.wMin = 1e300;
			d.wMax = -1e300;
			for( const Pt& v : picture )
			{
				const double w = ( horizontal ? v.x : v.y ) + 0.5;
				d.wMin         = std::min( d.wMin, w );
				d.wMax         = std::max( d.wMax, w );
			}
		}
		else
		{
			d.wMin = 0.0;
			d.wMax = 1.0;
		}
		d.unitsPerPixel = ( horizontal ? gradQx : gradQy ) * n;
		d.norm          = 1.0;
		break;
	}

	case PAT_BOX:
	case PAT_DIAMOND:
	case PAT_CIRCLE:
	{
		if( frame.multipleH == 1 && frame.multipleV == 1 )
		{
			d.norm = 0.0;
			for( const Pt& v : picture )
				d.norm = std::max( d.norm, normOf( frame.pattern, v.x, v.y ) );
		}
		else
			d.norm = normOf( frame.pattern, 0.5 / frame.multipleH, 0.5 / frame.multipleV );
		d.norm = std::max( d.norm, 1e-9 );
		d.wMin = 0.0;
		d.wMax = 1.0;
		if( frame.pattern == PAT_BOX )
			d.unitsPerPixel = gradQx / d.norm;//a vertical side of the box
		else if( frame.pattern == PAT_DIAMOND )
			d.unitsPerPixel = std::sqrt( ( gxx + gyx ) * ( gxx + gyx ) + ( gxy + gyy ) * ( gxy + gyy ) ) / d.norm;
		else//circle: the parabola's slope where the level is 0.5, along q.x
			d.unitsPerPixel = std::sqrt( 2.0 ) * gradQx / d.norm;
		break;
	}

	case PAT_CLOCK:
		d.norm          = 1.0;
		d.wMin          = 0.0;
		d.wMax          = 1.0;
		//The angle's slope a quarter of a picture height from the centre,
		//on the twelve o'clock ray: N / ( 2 pi r ) per unit of q.x.
		d.unitsPerPixel = 2.0 * frame.multipleH * gradQx / 3.141592653589793;
		break;

	default://matrix
		d.norm          = 1.0;
		d.wMin          = 0.0;
		d.wMax          = 1.0;
		d.unitsPerPixel = 1.0 / frame.outW;
		break;
	}
	return d;
}

double EdgeLevel( const Derived& d, double position )
{
	return d.wMin + std::clamp( position, 0.0, 1.0 ) * ( d.wMax - d.wMin );
}

namespace
{
double softArea( const Frame& frame, const Derived& d, const Prepared& pre, double level, double softW )
{
	if( !( softW > 0.0 ) )
		return hardArea( frame, d, pre, level );

	if( frame.pattern == PAT_MATRIX )
	{
		//Every cell's key in closed form: the thresholds are the ranks, so
		//the sum runs over ranks and the permutation never enters.
		const unsigned count = static_cast< unsigned >( d.matrixCols ) * static_cast< unsigned >( d.matrixRows );
		const double lv      = frame.reverse ? d.wMin + d.wMax - level : level;
		double sum           = 0.0;
		for( unsigned r = 0; r < count; ++r )
		{
			const double w = ( r + 0.5 ) / count;
			sum += std::clamp( 0.5 + ( lv - w ) / softW, 0.0, 1.0 );
		}
		const double area = sum / count;
		return frame.reverse ? 1.0 - area : area;
	}

	const AreaFn f { frame, d, pre };
	const double a = level - 0.5 * softW, b = level + 0.5 * softW;
	//1e-10 on the band's integral is 1e-10/softW on the mean, under 1e-8
	//at the softest setting: a tenth of a pixel of area at 4K.
	return simpson( f, a, b, 1e-10 ) / softW;
}
} // namespace

double HardArea( const Frame& frame, const Derived& d, double level )
{
	return hardArea( frame, d, prepare( frame, d ), level );
}

double SoftArea( const Frame& frame, const Derived& d, double level, double softW )
{
	return softArea( frame, d, prepare( frame, d ), level, softW );
}

double AreaLevel( const Frame& frame, const Derived& d, double position, double softW )
{
	const Prepared pre = prepare( frame, d );
	const double p     = std::clamp( position, 0.0, 1.0 );
	double lo          = d.wMin - 0.5 * std::max( softW, 0.0 );
	double hi          = d.wMax + 0.5 * std::max( softW, 0.0 );
	//softArea is 0 at lo and 1 at hi and monotone between: bisect.
	for( int i = 0; i < 60 && ( hi - lo ) > kAreaSolveTolerance; ++i )
	{
		const double mid = 0.5 * ( lo + hi );
		if( softArea( frame, d, pre, mid, softW ) < p )
			lo = mid;
		else
			hi = mid;
	}
	return 0.5 * ( lo + hi );
}

} // namespace wipe
