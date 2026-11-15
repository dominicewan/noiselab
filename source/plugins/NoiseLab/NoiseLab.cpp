#include "NoiseLab.h"
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <algorithm>
using namespace ffglex;

enum ParamID : FFUInt32
{
	// Noise
	PT_TYPE,
	PT_SEED,
	PT_SCALE,
	PT_STRETCH,
	PT_ROTATION,
	// Detail
	PT_OCTAVES,
	PT_LACUNARITY,
	PT_GAIN,
	// Motion
	PT_SPEED,
	PT_LOOP,
	PT_PHASE,
	PT_SCROLL_ANGLE,
	PT_SCROLL_SPEED,
	// Warp
	PT_WARP,
	PT_WARP_SCALE,
	// Shape
	PT_LOW_CLIP,
	PT_HIGH_CLIP,
	PT_CONTRAST,
	PT_ABSOLUTE,
	PT_CYCLES,
	PT_INVERT,
	// Output
	PT_MODE,
	PT_CHANNELS,
	PT_ALPHA_NOISE,
	// Relief
	PT_HEIGHT,
	PT_LIGHT_ANGLE,
	PT_SPECULAR,
	PT_TINT,
	// Gradient
	PT_STOPS,
	PT_INTERP,
	PT_SHIFT,
	PT_STOP_FIRST,// each stop is five parameters, see StopField
	PT_STOP_END = PT_STOP_FIRST + NoiseLab::MAX_STOPS * 5,
	// Performance
	PT_SLOPE_QUALITY = PT_STOP_END,

	PT_COUNT
};
// The five parameters of one gradient stop. Hue, saturation, brightness and alpha in a row are
// shown by Resolume as a single colour picker, named after the hue parameter.
enum StopField : FFUInt32
{
	SF_HUE,
	SF_SAT,
	SF_BRI,
	SF_ALPHA,
	SF_POS,
};
static unsigned int StopParam( int stop, StopField field )
{
	return PT_STOP_FIRST + static_cast< unsigned int >( stop ) * 5 + field;
}
enum OutputMode
{
	MODE_GRADIENT,
	MODE_GREYSCALE,
	MODE_NORMAL,
	MODE_PLANAR,
};
static_assert( PT_COUNT == NoiseLab::NUM_PARAMS, "NUM_PARAMS in NoiseLab.h must match the ParamID list" );

// --- Slider curves ---
// Wide-range parameters use a 0..1 (or -1..1) slider with a curve, so there is fine control at
// the subtle end and room for extremes at the other. Each has an inverse, used for the defaults.

// Scale: 0..1 -> 0.1x..256x, exponential (equal knob travel = equal zoom ratio).
static const float SCALE_MIN = 0.1f, SCALE_MAX = 256.0f;
static float ScaleFromSlider( float v )
{
	return SCALE_MIN * std::pow( SCALE_MAX / SCALE_MIN, v );
}
// Warp Scale: 0..1 -> 0.05x..16x, exponential.
static const float WARP_SCALE_MIN = 0.05f, WARP_SCALE_MAX = 16.0f;
static float WarpScaleFromSlider( float v )
{
	return WARP_SCALE_MIN * std::pow( WARP_SCALE_MAX / WARP_SCALE_MIN, v );
}
// Lacunarity: 0..1 -> 0.25..16, exponential. The centre of the slider is exactly 2.
static const float LACUNARITY_MIN = 0.25f, LACUNARITY_MAX = 16.0f;
static float LacunarityFromSlider( float v )
{
	return LACUNARITY_MIN * std::pow( LACUNARITY_MAX / LACUNARITY_MIN, v );
}
// Inverse of the three exponential curves above.
static float SliderFromExp( float value, float minValue, float maxValue )
{
	return std::log( value / minValue ) / std::log( maxValue / minValue );
}
// Evolve Speed: -1..1 -> -16..16 units per second on a cubic curve. Half the slider is still
// only +-2, so slow drifts stay easy to set, and zero is exactly zero.
static const float SPEED_MAX = 16.0f;
static float SpeedFromSlider( float v )
{
	return SPEED_MAX * v * v * v;
}
static float SliderFromSpeed( float speed )
{
	return std::cbrt( speed / SPEED_MAX );
}
// Warp: 0..1 -> 0..8 on a squared curve (the old maximum of 1 is now at about 35%).
static const float WARP_MAX = 8.0f;
static float WarpFromSlider( float v )
{
	return WARP_MAX * v * v;
}
// Stretch: -1..1 -> 1/64x..64x on one axis and the inverse on the other, so the extremes are
// close to straight lines.
static const double STRETCH_MAX = 64.0;

// Relief Height: 0..1 -> 0..4 on a squared curve.
static const float HEIGHT_MAX = 4.0f;
static float HeightFromSlider( float v )
{
	return HEIGHT_MAX * v * v;
}

// Defaults, in ParamID order.
static void FillDefaults( float* d )
{
	d[ PT_TYPE ]         = 1.0f;// FBM
	d[ PT_SEED ]         = 0.0f;
	d[ PT_SCALE ]        = SliderFromExp( 4.0f, SCALE_MIN, SCALE_MAX );
	d[ PT_STRETCH ]      = 0.0f;
	d[ PT_ROTATION ]     = 0.0f;
	d[ PT_OCTAVES ]      = 4.0f;
	d[ PT_LACUNARITY ]   = SliderFromExp( 2.0f, LACUNARITY_MIN, LACUNARITY_MAX );
	d[ PT_GAIN ]         = 0.5f;
	d[ PT_SPEED ]        = SliderFromSpeed( 0.25f );
	d[ PT_LOOP ]         = 0.0f;
	d[ PT_PHASE ]        = 0.0f;
	d[ PT_SCROLL_ANGLE ] = 0.0f;
	d[ PT_SCROLL_SPEED ] = 0.0f;
	d[ PT_WARP ]         = 0.0f;
	d[ PT_WARP_SCALE ]   = SliderFromExp( 1.0f, WARP_SCALE_MIN, WARP_SCALE_MAX );
	d[ PT_LOW_CLIP ]     = 0.0f;
	d[ PT_HIGH_CLIP ]    = 1.0f;
	d[ PT_CONTRAST ]     = 0.0f;
	d[ PT_ABSOLUTE ]     = 0.0f;
	d[ PT_CYCLES ]       = 1.0f;
	d[ PT_INVERT ]       = 0.0f;

	d[ PT_MODE ]        = static_cast< float >( MODE_GRADIENT );
	d[ PT_CHANNELS ]    = 0.0f;
	d[ PT_ALPHA_NOISE ] = 0.0f;

	d[ PT_HEIGHT ]      = 0.35f;
	d[ PT_LIGHT_ANGLE ] = 0.0f;
	d[ PT_SPECULAR ]    = 0.5f;
	d[ PT_TINT ]        = 0.0f;

	d[ PT_STOPS ]  = 2.0f;
	d[ PT_INTERP ] = 1.0f;// Linear, so the default black to white gradient equals Greyscale
	d[ PT_SHIFT ]  = 0.0f;
	// Stops 1 and 2 are black and white at the ends. Stops 3 to 6 only appear when Stops is
	// raised; they start with distinct colours at spread-out positions so each one is visible
	// the moment it is enabled.
	static const float stopDefaults[ NoiseLab::MAX_STOPS ][ 5 ] = {
		// hue   sat   bri   alpha pos
		{ 0.00f, 0.0f, 0.0f, 1.0f, 0.00f },
		{ 0.00f, 0.0f, 1.0f, 1.0f, 1.00f },
		{ 0.07f, 1.0f, 1.0f, 1.0f, 0.50f },
		{ 0.62f, 1.0f, 0.6f, 1.0f, 0.25f },
		{ 0.88f, 1.0f, 1.0f, 1.0f, 0.75f },
		{ 0.50f, 1.0f, 1.0f, 1.0f, 0.90f },
	};
	for( int s = 0; s < NoiseLab::MAX_STOPS; ++s )
		for( int f = 0; f < 5; ++f )
			d[ PT_STOP_FIRST + s * 5 + f ] = stopDefaults[ s ][ f ];

	d[ PT_SLOPE_QUALITY ] = 0.0f;// Fast
}

// Which parameters get the ~50 ms low-pass so stepped MIDI/FFT input glides instead of jumping.
// Wrapping parameters (rotation, phase, angle), switches and speeds (already integrated) do not.
static bool IsSmoothed( unsigned int index )
{
	switch( index )
	{
	case PT_SCALE:
	case PT_STRETCH:
	case PT_OCTAVES:
	case PT_LACUNARITY:
	case PT_GAIN:
	case PT_WARP:
	case PT_WARP_SCALE:
	case PT_LOW_CLIP:
	case PT_HIGH_CLIP:
	case PT_CONTRAST:
	case PT_CYCLES:
	case PT_HEIGHT:
	case PT_SPECULAR:
		return true;
	default:
		break;
	}
	// Gradient stop positions glide too; colours follow the picker directly.
	if( index >= PT_STOP_FIRST && index < PT_STOP_END )
		return ( index - PT_STOP_FIRST ) % 5 == SF_POS;
	return false;
}

// The evolution axis of the noise repeats every EVOLVE_WRAP units, so the integrated phase can be
// wrapped there without a visible jump (keeps float precision healthy on long gigs).
// LOOP_LENGTH is how many noise units one trip of Loop Phase 0..1 travels. EVOLVE_WRAP must be a
// multiple of LOOP_LENGTH.
static const int EVOLVE_WRAP = 256;
static const int LOOP_LENGTH = 4;

// The noise lattice is addressed with 32 bit integers, so it repeats every 2^32 cells. Scroll
// positions are wrapped there, which is seamless.
static const double LATTICE_WRAP = 4294967296.0;

static const double PI = 3.14159265358979323846;

// Split a scroll position into the whole lattice cell and the fraction inside it. Sending these
// separately keeps the noise sharp however far it has scrolled: a single float would run out of
// precision and start to jitter after a few minutes of fast scrolling.
static void SplitOffset( double value, GLint& cell, GLfloat& frac )
{
	const double whole = std::floor( value );
	frac               = static_cast< GLfloat >( value - whole );
	cell               = static_cast< GLint >( static_cast< uint32_t >( static_cast< int64_t >( whole ) ) );
}
static double WrapLattice( double value )
{
	return std::fmod( value, LATTICE_WRAP );
}

static CFFGLPluginInfo PluginInfo(
	PluginFactory< NoiseLab >,          // Create method
	"NZLB",                             // Plugin unique ID (4 chars). Change it if Resolume shows stale parameters.
	"NoiseLab",                         // Plugin name
	2,                                  // API major version number
	1,                                  // API minor version number
	0,                                  // Plugin major version number
	4,                                  // Plugin minor version number
	FF_SOURCE,                          // Plugin type
	"Procedural noise generator",       // Plugin description
	"NoiseLab step 3: gradient and output modes"// About
);

static const char vertexShaderCode[] = R"(#version 410 core
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV;
}
)";

static const char fragmentShaderCode[] = R"(#version 410 core
// --- Coordinates ---
uniform float uAspect;       // output width / height
uniform vec2 uRow0;          // rows of the 2x2 matrix taking centred screen coords to pattern coords
uniform vec2 uRow1;          // (rotation, stretch and scale folded together on the CPU)

// --- Detail ---
uniform float uOctaves;      // 1..8, fractional
uniform float uGain;         // amplitude ratio between octaves
uniform float uOctFreq[ 8 ]; // frequency of each octave (lacunarity ^ octave)
uniform ivec2 uOctCell[ 8 ]; // scroll position of each octave: whole lattice cells...
uniform vec2 uOctFrac[ 8 ];  // ...and the fraction inside the cell

// --- Motion ---
uniform float uEvolve;       // position on the evolution axis
uniform int uPeriod;         // evolution axis repeats after this many units
uniform int uSeed;
uniform int uType;           // 0 = Perlin (single octave), 1 = FBM

// --- Warp ---
uniform float uWarp;         // strength, 0..8
uniform float uWarpScale;    // warp feature size relative to the noise
uniform ivec2 uWarpCell;     // scroll position of the warp field
uniform vec2 uWarpFrac;

// --- Shape ---
uniform float uLowClip;
uniform float uClipRange;    // high clip - low clip, never zero (negative = inverted)
uniform float uContrast;     // multiplier around mid grey
uniform float uCycles;
uniform int uAbsolute;
uniform int uInvert;

// --- Output ---
uniform float uResY;         // output height in pixels
uniform int uMode;           // 0 = gradient, 1 = greyscale, 2 = normal map, 3 = planar
uniform int uChannels;       // normal map layout: 0 = red+green (standard), 1 = red+blue
uniform int uAlphaNoise;     // multiply alpha by the noise value

// --- Relief ---
uniform float uRelief;       // surface height, already divided by Scale
uniform vec2 uLightDir;      // direction the key light comes from, on screen
uniform float uSpecular;
uniform int uTint;           // planar: colour the surface with the gradient
uniform int uSmoothSlope;    // 0 = screen-space derivatives (free), 1 = two extra noise samples

// --- Gradient ---
uniform int uStopCount;      // 2..6, sorted by position on the CPU
uniform float uStopPos[ 6 ];
uniform vec4 uStopCol[ 6 ];  // straight (not premultiplied) RGBA
uniform int uInterp;         // 0 = smooth, 1 = linear, 2 = stepped
uniform float uShift;        // rotates the noise through the gradient, 0..1 wraps

in vec2 uv;
out vec4 fragColor;

// Integer hash (PCG). Integer maths gives identical noise on every GPU, unlike the
// classic fract(sin()) trick which differs between vendors.
uint pcg( uint v )
{
	uint s = v * 747796405u + 2891336453u;
	uint w = ( ( s >> ( ( s >> 28u ) + 4u ) ) ^ s ) * 277803737u;
	return ( w >> 22u ) ^ w;
}
uint hash3( ivec3 p, uint seed )
{
	return pcg( uint( p.x ) + pcg( uint( p.y ) + pcg( uint( p.z ) + seed ) ) );
}

// Dot product of the offset with one of 12 fixed gradient directions (Perlin's improved noise).
float gradDot( uint h, vec3 d )
{
	h &= 15u;
	float a = h < 8u ? d.x : d.y;
	float b = h < 4u ? d.y : ( ( h == 12u || h == 14u ) ? d.x : d.z );
	return ( ( h & 1u ) == 0u ? a : -a ) + ( ( h & 2u ) == 0u ? b : -b );
}

// Modulo that is safe for negative numbers (GLSL's % is undefined there).
int wrapInt( int a, int n )
{
	return a - n * int( floor( float( a ) / float( n ) ) );
}

// 3D gradient noise in roughly -1..1.
//  p       position; xy is small (screen sized), the scroll lives in 'cell'
//  cell    whole lattice cells to shift xy by
//  periodZ the noise repeats along z after this many units, which makes looping exact and free
float pnoise( vec3 p, ivec2 cell, int periodZ, uint seed )
{
	vec3 pf = floor( p );
	vec3 f  = p - pf;
	vec3 w  = f * f * f * ( f * ( f * 6.0 - 15.0 ) + 10.0 );

	ivec2 i = ivec2( pf.xy ) + cell;
	int iz  = int( pf.z );
	int z0  = wrapInt( iz, periodZ );
	int z1  = wrapInt( iz + 1, periodZ );

	float n000 = gradDot( hash3( ivec3( i.x,     i.y,     z0 ), seed ), f );
	float n100 = gradDot( hash3( ivec3( i.x + 1, i.y,     z0 ), seed ), f - vec3( 1.0, 0.0, 0.0 ) );
	float n010 = gradDot( hash3( ivec3( i.x,     i.y + 1, z0 ), seed ), f - vec3( 0.0, 1.0, 0.0 ) );
	float n110 = gradDot( hash3( ivec3( i.x + 1, i.y + 1, z0 ), seed ), f - vec3( 1.0, 1.0, 0.0 ) );
	float n001 = gradDot( hash3( ivec3( i.x,     i.y,     z1 ), seed ), f - vec3( 0.0, 0.0, 1.0 ) );
	float n101 = gradDot( hash3( ivec3( i.x + 1, i.y,     z1 ), seed ), f - vec3( 1.0, 0.0, 1.0 ) );
	float n011 = gradDot( hash3( ivec3( i.x,     i.y + 1, z1 ), seed ), f - vec3( 0.0, 1.0, 1.0 ) );
	float n111 = gradDot( hash3( ivec3( i.x + 1, i.y + 1, z1 ), seed ), f - vec3( 1.0, 1.0, 1.0 ) );

	float nx00 = mix( n000, n100, w.x );
	float nx10 = mix( n010, n110, w.x );
	float nx01 = mix( n001, n101, w.x );
	float nx11 = mix( n011, n111, w.x );
	return mix( mix( nx00, nx10, w.y ), mix( nx01, nx11, w.y ), w.z );
}

// Domain warp: how far to push the sampling position, in pattern units. Two octaves of a
// separate noise per axis is enough to look organic and keeps the cost down (4 noise calls).
vec2 warpOffset( vec2 p, uint seed )
{
	if( uWarp <= 0.0 )
		return vec2( 0.0 );

	vec2 q  = p * uWarpScale + uWarpFrac;
	vec2 q2 = q * 2.0;
	ivec2 c2 = uWarpCell * 2;
	float x = pnoise( vec3( q, uEvolve ), uWarpCell, uPeriod, seed + 101u )
	        + 0.5 * pnoise( vec3( q2, uEvolve * 2.0 ), c2, uPeriod * 2, seed + 102u );
	float y = pnoise( vec3( q, uEvolve ), uWarpCell, uPeriod, seed + 201u )
	        + 0.5 * pnoise( vec3( q2, uEvolve * 2.0 ), c2, uPeriod * 2, seed + 202u );
	// Dividing by the warp scale keeps the look self-similar as Warp Scale changes.
	return vec2( x, y ) * ( uWarp * 2.0 / uWarpScale );
}

// Fractal sum in roughly -1..1. The top octave fades in with the fractional part of the octave
// count so modulating Octaves never pops, and the result is normalised by its expected
// strength so brightness and contrast hold steady as Octaves and Gain change.
float fractal( vec2 p, float octaves, uint seed )
{
	float sum   = 0.0;
	float power = 0.0;
	float amp   = 1.0;
	float zMul  = 1.0;
	int period  = uPeriod;
	for( int i = 0; i < 8; ++i )
	{
		float weight = clamp( octaves - float( i ), 0.0, 1.0 );
		if( weight <= 0.0 )
			break;
		float a = weight * amp;
		vec2 q  = p * uOctFreq[ i ] + uOctFrac[ i ];
		sum   += a * pnoise( vec3( q, uEvolve * zMul ), uOctCell[ i ], period, seed + uint( i ) );
		power += a * a;
		amp    *= uGain;
		zMul   *= 2.0;
		period *= 2;
	}
	return sum / sqrt( power );
}

// Output shaping, in Cinema 4D's order: absolute, cycles, clips, contrast, then invert.
float shape( float v )
{
	if( uAbsolute != 0 )
		v = abs( v * 2.0 - 1.0 );// fold about mid grey

	// Repeat the 0..1 range uCycles times (sawtooth), which gives contour-like banding.
	float c = v * uCycles;
	v = min( c, uCycles - 0.0001 );
	v = v - floor( v );

	v = clamp( ( v - uLowClip ) / uClipRange, 0.0, 1.0 );
	v = clamp( 0.5 + ( v - 0.5 ) * uContrast, 0.0, 1.0 );

	if( uInvert != 0 )
		v = 1.0 - v;
	return v;
}

// The finished 0..1 noise value at a screen position. Everything (warp, fractal, shaping) is in
// here so the relief modes can sample neighbouring positions.
float heightAt( vec2 s )
{
	vec2 p = vec2( dot( uRow0, s ), dot( uRow1, s ) );

	uint seed = uint( uSeed ) * 7919u;
	p += warpOffset( p, seed );

	float octaves = uType == 0 ? 1.0 : uOctaves;
	float n = fractal( p, octaves, seed );

	return shape( clamp( 0.5 + 0.6 * n, 0.0, 1.0 ) );
}

// Multi-stop gradient lookup. Stops arrive sorted, so walking them in order and blending toward
// each one in turn leaves exactly the two neighbours of v contributing.
vec4 gradient( float v )
{
	float t = uShift > 0.0 ? fract( v + uShift ) : v;
	vec4 c = uStopCol[ 0 ];
	for( int i = 1; i < uStopCount; ++i )
	{
		float a = uStopPos[ i - 1 ];
		float b = uStopPos[ i ];
		float k;
		if( uInterp == 2 )
			k = step( 0.5 * ( a + b ), t );// flat bands that switch halfway between stops
		else
		{
			k = clamp( ( t - a ) / max( b - a, 0.0001 ), 0.0, 1.0 );
			if( uInterp == 0 )
				k = k * k * ( 3.0 - 2.0 * k );
		}
		c = mix( c, uStopCol[ i ], k );
	}
	return c;
}

void main()
{
	// Centre the pattern and correct for aspect so cells stay square at any output size.
	vec2 s = ( uv - 0.5 ) * vec2( uAspect, 1.0 );
	float v = heightAt( s );

	// Slope of the noise in value per screen height. The derivative functions compare this pixel
	// with its neighbours in the 2x2 block the GPU shades together: free, but slightly blocky.
	vec2 slope = vec2( dFdx( v ), dFdy( v ) ) * uResY;

	vec4 col;
	if( uMode == 1 )
	{
		col = vec4( v, v, v, 1.0 );
	}
	else if( uMode == 0 )
	{
		col = gradient( v );
	}
	else
	{
		if( uSmoothSlope != 0 )
		{
			// Sample one pixel right and one pixel up instead. Three times the noise cost.
			float e = 1.0 / uResY;
			slope = ( vec2( heightAt( s + vec2( e, 0.0 ) ), heightAt( s + vec2( 0.0, e ) ) ) - v ) / e;
		}
		// Surface normal of the noise treated as a height field. z points at the viewer.
		vec3 n = normalize( vec3( -slope * uRelief, 1.0 ) );

		if( uMode == 2 )
		{
			vec3 e = n * 0.5 + 0.5;// 0.5 = flat
			col = uChannels == 0 ? vec4( e, 1.0 ) : vec4( e.x, 0.0, e.y, 1.0 );
		}
		else
		{
			// Planar: warm key light, cool fill from the opposite side, tight highlight.
			vec4 base  = uTint != 0 ? gradient( v ) : vec4( 0.85, 0.85, 0.85, 1.0 );
			vec3 key   = normalize( vec3( uLightDir, 0.75 ) );
			vec3 fill  = normalize( vec3( -uLightDir, 0.45 ) );
			vec3 light = vec3( 0.06 )
			           + max( dot( n, key ), 0.0 ) * vec3( 1.0, 0.93, 0.82 )
			           + max( dot( n, fill ), 0.0 ) * 0.45 * vec3( 0.6, 0.78, 1.0 );
			vec3 lit = base.rgb * light * ( 0.6 + 0.4 * v );// darker in the valleys
			vec3 halfway = normalize( key + vec3( 0.0, 0.0, 1.0 ) );
			lit += uSpecular * pow( max( dot( n, halfway ), 0.0 ), 48.0 ) * vec3( 1.0, 0.97, 0.9 );
			col = vec4( lit, base.a );
		}
	}

	float alpha = clamp( col.a, 0.0, 1.0 );
	if( uAlphaNoise != 0 )
		alpha *= v;
	// FFGL plugins must output premultiplied colour.
	fragColor = vec4( clamp( col.rgb, 0.0, 1.0 ) * alpha, alpha );
}
)";

NoiseLab::NoiseLab()
{
	FillDefaults( params );
	for( int i = 0; i < NUM_PARAMS; ++i )
		smoothed[ i ] = params[ i ];

	// A source has no inputs.
	SetMinInputs( 0 );
	SetMaxInputs( 0 );

	// Consecutive parameters with the same group name share a collapsible section in Resolume.
	// --- Noise ---
	SetOptionParamInfo( PT_TYPE, "Type", 2, params[ PT_TYPE ] );
	SetParamElementInfo( PT_TYPE, 0, "Perlin", 0.0f );
	SetParamElementInfo( PT_TYPE, 1, "FBM", 1.0f );
	SetParamGroup( PT_TYPE, "Noise" );

	SetParamInfof( PT_SEED, "Seed", FF_TYPE_INTEGER );
	SetParamRange( PT_SEED, 0.0f, 100.0f );
	SetParamGroup( PT_SEED, "Noise" );

	SetParamInfof( PT_SCALE, "Scale", FF_TYPE_STANDARD );
	SetParamGroup( PT_SCALE, "Noise" );

	SetParamInfof( PT_STRETCH, "Stretch", FF_TYPE_STANDARD );
	SetParamRange( PT_STRETCH, -1.0f, 1.0f );
	SetParamGroup( PT_STRETCH, "Noise" );

	SetParamInfof( PT_ROTATION, "Rotation", FF_TYPE_STANDARD );
	SetParamRange( PT_ROTATION, 0.0f, 360.0f );
	SetParamGroup( PT_ROTATION, "Noise" );

	// --- Detail ---
	SetParamInfof( PT_OCTAVES, "Octaves", FF_TYPE_STANDARD );
	SetParamRange( PT_OCTAVES, 1.0f, static_cast< float >( MAX_OCTAVES ) );
	SetParamGroup( PT_OCTAVES, "Detail" );

	SetParamInfof( PT_LACUNARITY, "Lacunarity", FF_TYPE_STANDARD );
	SetParamGroup( PT_LACUNARITY, "Detail" );

	SetParamInfof( PT_GAIN, "Gain", FF_TYPE_STANDARD );
	SetParamGroup( PT_GAIN, "Detail" );

	// --- Motion ---
	SetParamInfof( PT_SPEED, "Evolve Speed", FF_TYPE_STANDARD );
	SetParamRange( PT_SPEED, -1.0f, 1.0f );
	SetParamGroup( PT_SPEED, "Motion" );

	SetParamInfo( PT_LOOP, "Loop", FF_TYPE_BOOLEAN, false );
	SetParamGroup( PT_LOOP, "Motion" );

	SetParamInfof( PT_PHASE, "Loop Phase", FF_TYPE_STANDARD );
	SetParamGroup( PT_PHASE, "Motion" );

	SetParamInfof( PT_SCROLL_ANGLE, "Scroll Angle", FF_TYPE_STANDARD );
	SetParamRange( PT_SCROLL_ANGLE, 0.0f, 360.0f );
	SetParamGroup( PT_SCROLL_ANGLE, "Motion" );

	SetParamInfof( PT_SCROLL_SPEED, "Scroll Speed", FF_TYPE_STANDARD );
	SetParamRange( PT_SCROLL_SPEED, 0.0f, 2.0f );
	SetParamGroup( PT_SCROLL_SPEED, "Motion" );

	// --- Warp ---
	SetParamInfof( PT_WARP, "Warp", FF_TYPE_STANDARD );
	SetParamGroup( PT_WARP, "Warp" );

	SetParamInfof( PT_WARP_SCALE, "Warp Scale", FF_TYPE_STANDARD );
	SetParamGroup( PT_WARP_SCALE, "Warp" );

	// --- Shape ---
	SetParamInfof( PT_LOW_CLIP, "Low Clip", FF_TYPE_STANDARD );
	SetParamGroup( PT_LOW_CLIP, "Shape" );

	SetParamInfof( PT_HIGH_CLIP, "High Clip", FF_TYPE_STANDARD );
	SetParamGroup( PT_HIGH_CLIP, "Shape" );

	SetParamInfof( PT_CONTRAST, "Contrast", FF_TYPE_STANDARD );
	SetParamRange( PT_CONTRAST, -1.0f, 1.0f );
	SetParamGroup( PT_CONTRAST, "Shape" );

	SetParamInfo( PT_ABSOLUTE, "Absolute", FF_TYPE_BOOLEAN, false );
	SetParamGroup( PT_ABSOLUTE, "Shape" );

	SetParamInfof( PT_CYCLES, "Cycles", FF_TYPE_STANDARD );
	SetParamRange( PT_CYCLES, 1.0f, 8.0f );
	SetParamGroup( PT_CYCLES, "Shape" );

	SetParamInfo( PT_INVERT, "Invert", FF_TYPE_BOOLEAN, false );
	SetParamGroup( PT_INVERT, "Shape" );

	// --- Output ---
	SetOptionParamInfo( PT_MODE, "Output Mode", 4, params[ PT_MODE ] );
	SetParamElementInfo( PT_MODE, 0, "Gradient", static_cast< float >( MODE_GRADIENT ) );
	SetParamElementInfo( PT_MODE, 1, "Greyscale", static_cast< float >( MODE_GREYSCALE ) );
	SetParamElementInfo( PT_MODE, 2, "Normal Map", static_cast< float >( MODE_NORMAL ) );
	SetParamElementInfo( PT_MODE, 3, "Planar", static_cast< float >( MODE_PLANAR ) );
	SetParamGroup( PT_MODE, "Output" );

	SetOptionParamInfo( PT_CHANNELS, "Channels", 2, params[ PT_CHANNELS ] );
	SetParamElementInfo( PT_CHANNELS, 0, "Red+Green", 0.0f );
	SetParamElementInfo( PT_CHANNELS, 1, "Red+Blue", 1.0f );
	SetParamGroup( PT_CHANNELS, "Output" );

	SetParamInfo( PT_ALPHA_NOISE, "Alpha From Noise", FF_TYPE_BOOLEAN, false );
	SetParamGroup( PT_ALPHA_NOISE, "Output" );

	// --- Relief ---
	SetParamInfof( PT_HEIGHT, "Height", FF_TYPE_STANDARD );
	SetParamGroup( PT_HEIGHT, "Relief" );

	SetParamInfof( PT_LIGHT_ANGLE, "Light Angle", FF_TYPE_STANDARD );
	SetParamRange( PT_LIGHT_ANGLE, 0.0f, 360.0f );
	SetParamGroup( PT_LIGHT_ANGLE, "Relief" );

	SetParamInfof( PT_SPECULAR, "Specular", FF_TYPE_STANDARD );
	SetParamGroup( PT_SPECULAR, "Relief" );

	SetParamInfo( PT_TINT, "Tint By Gradient", FF_TYPE_BOOLEAN, false );
	SetParamGroup( PT_TINT, "Relief" );

	// --- Gradient ---
	SetParamInfof( PT_STOPS, "Stops", FF_TYPE_INTEGER );
	SetParamRange( PT_STOPS, 2.0f, static_cast< float >( MAX_STOPS ) );
	SetParamGroup( PT_STOPS, "Gradient" );

	SetOptionParamInfo( PT_INTERP, "Blend", 3, params[ PT_INTERP ] );
	SetParamElementInfo( PT_INTERP, 0, "Smooth", 0.0f );
	SetParamElementInfo( PT_INTERP, 1, "Linear", 1.0f );
	SetParamElementInfo( PT_INTERP, 2, "Stepped", 2.0f );
	SetParamGroup( PT_INTERP, "Gradient" );

	SetParamInfof( PT_SHIFT, "Gradient Shift", FF_TYPE_STANDARD );
	SetParamGroup( PT_SHIFT, "Gradient" );

	for( int s = 0; s < MAX_STOPS; ++s )
	{
		// FFGL parameter names are limited to 16 characters.
		char name[ 17 ];
		snprintf( name, sizeof( name ), "Stop %d", s + 1 );
		SetParamInfof( StopParam( s, SF_HUE ), name, FF_TYPE_HUE );
		snprintf( name, sizeof( name ), "Stop %d Sat", s + 1 );
		SetParamInfof( StopParam( s, SF_SAT ), name, FF_TYPE_SATURATION );
		snprintf( name, sizeof( name ), "Stop %d Bri", s + 1 );
		SetParamInfof( StopParam( s, SF_BRI ), name, FF_TYPE_BRIGHTNESS );
		snprintf( name, sizeof( name ), "Stop %d Alpha", s + 1 );
		SetParamInfof( StopParam( s, SF_ALPHA ), name, FF_TYPE_ALPHA );
		snprintf( name, sizeof( name ), "Stop %d Pos", s + 1 );
		SetParamInfof( StopParam( s, SF_POS ), name, FF_TYPE_STANDARD );
		for( int f = 0; f < 5; ++f )
			SetParamGroup( PT_STOP_FIRST + s * 5 + f, "Gradient" );
	}

	// --- Performance ---
	SetOptionParamInfo( PT_SLOPE_QUALITY, "Slope Quality", 2, params[ PT_SLOPE_QUALITY ] );
	SetParamElementInfo( PT_SLOPE_QUALITY, 0, "Fast", 0.0f );
	SetParamElementInfo( PT_SLOPE_QUALITY, 1, "Smooth", 1.0f );
	SetParamGroup( PT_SLOPE_QUALITY, "Performance" );

	// Hide whatever doesn't apply to the default mode. No event needed during construction.
	UpdateVisibility( false );

	FFGLLog::LogToHost( "NoiseLab: created" );
}

FFResult NoiseLab::InitGL( const FFGLViewportStruct* vp )
{
	// All GL resources are created here, never in ProcessOpenGL.
	if( !shader.Compile( vertexShaderCode, fragmentShaderCode ) )
	{
		FFGLLog::LogToHost( "NoiseLab: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}
	if( !quad.Initialise() )
	{
		FFGLLog::LogToHost( "NoiseLab: quad failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	loc.aspect    = shader.FindUniform( "uAspect" );
	loc.row0      = shader.FindUniform( "uRow0" );
	loc.row1      = shader.FindUniform( "uRow1" );
	loc.octaves   = shader.FindUniform( "uOctaves" );
	loc.gain      = shader.FindUniform( "uGain" );
	loc.octFreq   = shader.FindUniform( "uOctFreq" );
	loc.octCell   = shader.FindUniform( "uOctCell" );
	loc.octFrac   = shader.FindUniform( "uOctFrac" );
	loc.evolve    = shader.FindUniform( "uEvolve" );
	loc.period    = shader.FindUniform( "uPeriod" );
	loc.seed      = shader.FindUniform( "uSeed" );
	loc.type      = shader.FindUniform( "uType" );
	loc.warp      = shader.FindUniform( "uWarp" );
	loc.warpScale = shader.FindUniform( "uWarpScale" );
	loc.warpCell  = shader.FindUniform( "uWarpCell" );
	loc.warpFrac  = shader.FindUniform( "uWarpFrac" );
	loc.lowClip   = shader.FindUniform( "uLowClip" );
	loc.clipRange = shader.FindUniform( "uClipRange" );
	loc.contrast  = shader.FindUniform( "uContrast" );
	loc.cycles    = shader.FindUniform( "uCycles" );
	loc.absolute  = shader.FindUniform( "uAbsolute" );
	loc.invert    = shader.FindUniform( "uInvert" );
	loc.resY        = shader.FindUniform( "uResY" );
	loc.mode        = shader.FindUniform( "uMode" );
	loc.channels    = shader.FindUniform( "uChannels" );
	loc.alphaNoise  = shader.FindUniform( "uAlphaNoise" );
	loc.relief      = shader.FindUniform( "uRelief" );
	loc.lightDir    = shader.FindUniform( "uLightDir" );
	loc.specular    = shader.FindUniform( "uSpecular" );
	loc.tint        = shader.FindUniform( "uTint" );
	loc.smoothSlope = shader.FindUniform( "uSmoothSlope" );
	loc.stopCount   = shader.FindUniform( "uStopCount" );
	loc.stopPos     = shader.FindUniform( "uStopPos" );
	loc.stopCol     = shader.FindUniform( "uStopCol" );
	loc.interp      = shader.FindUniform( "uInterp" );
	loc.shift       = shader.FindUniform( "uShift" );

	firstFrame = true;

	//Use base-class init as success result so that it retains the viewport.
	return CFFGLPlugin::InitGL( vp );
}

FFResult NoiseLab::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	// Frame delta from a monotonic clock, clamped so a stall (or the first frame) can't make the
	// pattern leap.
	const auto now = std::chrono::steady_clock::now();
	double dt      = 0.0;
	if( !firstFrame )
		dt = std::chrono::duration< double >( now - lastFrameTime ).count();
	lastFrameTime = now;
	dt            = std::min( std::max( dt, 0.0 ), 0.1 );

	// Low-pass the continuous parameters. Everything else follows the host value directly.
	const float blend = firstFrame ? 1.0f : 1.0f - std::exp( -static_cast< float >( dt ) / 0.05f );
	for( unsigned int i = 0; i < NUM_PARAMS; ++i )
	{
		if( IsSmoothed( i ) )
			smoothed[ i ] += ( params[ i ] - smoothed[ i ] ) * blend;
		else
			smoothed[ i ] = params[ i ];
	}
	firstFrame = false;

	// --- Coordinates: fold rotation, stretch and scale into one 2x2 matrix ---
	const double scale    = ScaleFromSlider( smoothed[ PT_SCALE ] );
	const double stretch  = std::pow( STRETCH_MAX, static_cast< double >( std::min( std::max( smoothed[ PT_STRETCH ], -1.0f ), 1.0f ) ) );// >1 squashes x, stretches y
	const double rotation = smoothed[ PT_ROTATION ] * PI / 180.0;
	const double cosR     = std::cos( rotation );
	const double sinR     = std::sin( rotation );
	// pattern = Stretch * Rotate * screen * scale
	const double m00 = stretch * cosR * scale;
	const double m01 = -stretch * sinR * scale;
	const double m10 = sinR * scale / stretch;
	const double m11 = cosR * scale / stretch;

	// --- Detail ---
	const float octaves      = std::min( std::max( smoothed[ PT_OCTAVES ], 1.0f ), static_cast< float >( MAX_OCTAVES ) );
	const double lacunarity  = LacunarityFromSlider( std::min( std::max( smoothed[ PT_LACUNARITY ], 0.0f ), 1.0f ) );
	const float gain         = std::min( std::max( smoothed[ PT_GAIN ], 0.0f ), 1.0f );
	const double warpScale   = WarpScaleFromSlider( smoothed[ PT_WARP_SCALE ] );

	// --- Scroll: integrate a position, like evolution, so speed changes never teleport ---
	// Speed is in output heights per second, so it looks the same at any Scale. The step is
	// converted to pattern units with the same matrix the pixels use, and each octave keeps its
	// own running position so changing Lacunarity or Scale zooms about the screen centre.
	const double scrollAngle = smoothed[ PT_SCROLL_ANGLE ] * PI / 180.0;
	const double scrollStep  = static_cast< double >( smoothed[ PT_SCROLL_SPEED ] ) * dt;
	const double screenX     = std::cos( scrollAngle ) * scrollStep;
	const double screenY     = std::sin( scrollAngle ) * scrollStep;
	// Moving the picture one way means moving the sample position the other way.
	const double stepX = -( m00 * screenX + m01 * screenY );
	const double stepY = -( m10 * screenX + m11 * screenY );

	GLfloat octFreq[ MAX_OCTAVES ];
	GLint octCell[ MAX_OCTAVES * 2 ];
	GLfloat octFrac[ MAX_OCTAVES * 2 ];
	// Extreme Lacunarity and Scale multiply up fast (16^7 is 268 million). Past a few million
	// cells across the screen the shader's floats can no longer address the lattice, so cap each
	// octave there. Visually nothing is lost: that is already far finer than a pixel, i.e. static.
	const double maxFrequency = 4194304.0 / ( 4.0 * scale * std::max( stretch, 1.0 / stretch ) );
	double frequency          = 1.0;
	for( int i = 0; i < MAX_OCTAVES; ++i )
	{
		const double f      = std::min( frequency, maxFrequency );
		octaveOffset[ i ].x = WrapLattice( octaveOffset[ i ].x + stepX * f );
		octaveOffset[ i ].y = WrapLattice( octaveOffset[ i ].y + stepY * f );
		octFreq[ i ]        = static_cast< GLfloat >( f );
		SplitOffset( octaveOffset[ i ].x, octCell[ i * 2 ], octFrac[ i * 2 ] );
		SplitOffset( octaveOffset[ i ].y, octCell[ i * 2 + 1 ], octFrac[ i * 2 + 1 ] );
		frequency *= lacunarity;
	}

	warpOffset.x = WrapLattice( warpOffset.x + stepX * warpScale );
	warpOffset.y = WrapLattice( warpOffset.y + stepY * warpScale );
	GLint warpCell[ 2 ];
	GLfloat warpFrac[ 2 ];
	SplitOffset( warpOffset.x, warpCell[ 0 ], warpFrac[ 0 ] );
	SplitOffset( warpOffset.y, warpCell[ 1 ], warpFrac[ 1 ] );

	// --- Evolution: integrate speed into a position ---
	evolvePhase += dt * static_cast< double >( SpeedFromSlider( std::min( std::max( smoothed[ PT_SPEED ], -1.0f ), 1.0f ) ) );
	evolvePhase  = std::fmod( evolvePhase, static_cast< double >( EVOLVE_WRAP ) );
	if( evolvePhase < 0.0 )
		evolvePhase += EVOLVE_WRAP;

	double evolve = evolvePhase;
	int period    = EVOLVE_WRAP;
	if( smoothed[ PT_LOOP ] > 0.5f )
	{
		// Loop Phase 0..1 is one full trip round a short periodic axis. Evolve Speed still runs,
		// so the loop can free-run, be scrubbed, or both.
		evolve = std::fmod( evolvePhase + static_cast< double >( smoothed[ PT_PHASE ] ) * LOOP_LENGTH, static_cast< double >( LOOP_LENGTH ) );
		period = LOOP_LENGTH;
	}

	// --- Shape ---
	// Keep the clips at least 1% apart so the divide in the shader is always safe. If High is
	// dragged below Low the range goes negative, which simply inverts the picture.
	const float lowClip = smoothed[ PT_LOW_CLIP ];
	float clipRange     = smoothed[ PT_HIGH_CLIP ] - lowClip;
	if( std::fabs( clipRange ) < 0.01f )
		clipRange = clipRange < 0.0f ? -0.01f : 0.01f;
	// -1..1 -> 1/8x..8x around mid grey. Never reaches zero, so the frame can't go fully flat.
	const float contrast = std::pow( 2.0f, 3.0f * smoothed[ PT_CONTRAST ] );
	const float cycles   = std::min( std::max( smoothed[ PT_CYCLES ], 1.0f ), 8.0f );

	// --- Relief ---
	// Dividing by Scale keeps the surface looking equally deep at any zoom.
	const float relief      = HeightFromSlider( std::min( std::max( smoothed[ PT_HEIGHT ], 0.0f ), 1.0f ) ) / static_cast< float >( scale );
	const double lightAngle = smoothed[ PT_LIGHT_ANGLE ] * PI / 180.0;// 0 = lit from the top, clockwise

	// --- Gradient: collect the active stops and sort them by position ---
	// At most six stops, so a simple insertion sort on the stack is all that's needed.
	const int stopCount = GetStopCount();
	GLfloat stopPos[ MAX_STOPS ];
	GLfloat stopCol[ MAX_STOPS * 4 ];
	int filled = 0;
	for( int s = 0; s < stopCount; ++s )
	{
		const float pos = std::min( std::max( smoothed[ StopParam( s, SF_POS ) ], 0.0f ), 1.0f );
		float hue       = smoothed[ StopParam( s, SF_HUE ) ];
		if( hue >= 1.0f )
			hue = 0.0f;// hue is a circle; the conversion expects [0, 1)
		float rgba[ 4 ];
		HSVtoRGB( hue, smoothed[ StopParam( s, SF_SAT ) ], smoothed[ StopParam( s, SF_BRI ) ], rgba[ 0 ], rgba[ 1 ], rgba[ 2 ] );
		rgba[ 3 ] = smoothed[ StopParam( s, SF_ALPHA ) ];

		int slot = filled;
		while( slot > 0 && stopPos[ slot - 1 ] > pos )
		{
			stopPos[ slot ] = stopPos[ slot - 1 ];
			for( int c = 0; c < 4; ++c )
				stopCol[ slot * 4 + c ] = stopCol[ ( slot - 1 ) * 4 + c ];
			--slot;
		}
		stopPos[ slot ] = pos;
		for( int c = 0; c < 4; ++c )
			stopCol[ slot * 4 + c ] = rgba[ c ];
		++filled;
	}
	// Pad the unused slots with the last stop so the arrays are always fully defined.
	for( int s = filled; s < MAX_STOPS; ++s )
	{
		stopPos[ s ] = stopPos[ filled - 1 ];
		for( int c = 0; c < 4; ++c )
			stopCol[ s * 4 + c ] = stopCol[ ( filled - 1 ) * 4 + c ];
	}

	const float aspect = currentViewport.height > 0 ? static_cast< float >( currentViewport.width ) / static_cast< float >( currentViewport.height ) : 1.0f;

	//FFGL requires us to leave the context in a default state on return, so use this scoped binding to help us do that.
	ScopedShaderBinding shaderBinding( shader.GetGLID() );
	glUniform1f( loc.aspect, aspect );
	glUniform2f( loc.row0, static_cast< GLfloat >( m00 ), static_cast< GLfloat >( m01 ) );
	glUniform2f( loc.row1, static_cast< GLfloat >( m10 ), static_cast< GLfloat >( m11 ) );

	glUniform1f( loc.octaves, octaves );
	glUniform1f( loc.gain, gain );
	glUniform1fv( loc.octFreq, MAX_OCTAVES, octFreq );
	glUniform2iv( loc.octCell, MAX_OCTAVES, octCell );
	glUniform2fv( loc.octFrac, MAX_OCTAVES, octFrac );

	glUniform1f( loc.evolve, static_cast< GLfloat >( evolve ) );
	glUniform1i( loc.period, period );
	glUniform1i( loc.seed, static_cast< GLint >( std::floor( smoothed[ PT_SEED ] + 0.5f ) ) );
	glUniform1i( loc.type, smoothed[ PT_TYPE ] > 0.5f ? 1 : 0 );

	glUniform1f( loc.warp, WarpFromSlider( std::min( std::max( smoothed[ PT_WARP ], 0.0f ), 1.0f ) ) );
	glUniform1f( loc.warpScale, static_cast< GLfloat >( warpScale ) );
	glUniform2iv( loc.warpCell, 1, warpCell );
	glUniform2fv( loc.warpFrac, 1, warpFrac );

	glUniform1f( loc.lowClip, lowClip );
	glUniform1f( loc.clipRange, clipRange );
	glUniform1f( loc.contrast, contrast );
	glUniform1f( loc.cycles, cycles );
	glUniform1i( loc.absolute, smoothed[ PT_ABSOLUTE ] > 0.5f ? 1 : 0 );
	glUniform1i( loc.invert, smoothed[ PT_INVERT ] > 0.5f ? 1 : 0 );

	glUniform1f( loc.resY, static_cast< GLfloat >( std::max< FFUInt32 >( currentViewport.height, 1 ) ) );
	glUniform1i( loc.mode, GetMode() );
	glUniform1i( loc.channels, smoothed[ PT_CHANNELS ] > 0.5f ? 1 : 0 );
	glUniform1i( loc.alphaNoise, smoothed[ PT_ALPHA_NOISE ] > 0.5f ? 1 : 0 );

	glUniform1f( loc.relief, relief );
	glUniform2f( loc.lightDir, static_cast< GLfloat >( std::sin( lightAngle ) ), static_cast< GLfloat >( std::cos( lightAngle ) ) );
	glUniform1f( loc.specular, std::min( std::max( smoothed[ PT_SPECULAR ], 0.0f ), 1.0f ) );
	glUniform1i( loc.tint, smoothed[ PT_TINT ] > 0.5f ? 1 : 0 );
	glUniform1i( loc.smoothSlope, smoothed[ PT_SLOPE_QUALITY ] > 0.5f ? 1 : 0 );

	glUniform1i( loc.stopCount, stopCount );
	glUniform1fv( loc.stopPos, MAX_STOPS, stopPos );
	glUniform4fv( loc.stopCol, MAX_STOPS, stopCol );
	glUniform1i( loc.interp, static_cast< GLint >( std::floor( smoothed[ PT_INTERP ] + 0.5f ) ) );
	glUniform1f( loc.shift, smoothed[ PT_SHIFT ] - std::floor( smoothed[ PT_SHIFT ] ) );

	quad.Draw();

	return FF_SUCCESS;
}

FFResult NoiseLab::DeInitGL()
{
	shader.FreeGLResources();
	quad.Release();
	loc = Locations();
	return FF_SUCCESS;
}

FFResult NoiseLab::SetFloatParameter( unsigned int index, float value )
{
	if( index >= NUM_PARAMS )
		return FF_FAIL;

	params[ index ] = value;

	// These two decide which other parameters are relevant, so refresh what Resolume shows.
	if( index == PT_MODE || index == PT_STOPS )
		UpdateVisibility( true );

	return FF_SUCCESS;
}

int NoiseLab::GetMode() const
{
	const int mode = static_cast< int >( std::floor( params[ PT_MODE ] + 0.5f ) );
	return std::min( std::max( mode, static_cast< int >( MODE_GRADIENT ) ), static_cast< int >( MODE_PLANAR ) );
}
int NoiseLab::GetStopCount() const
{
	const int count = static_cast< int >( std::floor( params[ PT_STOPS ] + 0.5f ) );
	return std::min( std::max( count, 2 ), static_cast< int >( MAX_STOPS ) );
}

void NoiseLab::UpdateVisibility( bool raiseEvent )
{
	const int mode      = GetMode();
	const bool relief   = mode == MODE_NORMAL || mode == MODE_PLANAR;
	const bool planar   = mode == MODE_PLANAR;
	const int stopCount = GetStopCount();

	SetParamVisibility( PT_CHANNELS, mode == MODE_NORMAL, raiseEvent );
	SetParamVisibility( PT_HEIGHT, relief, raiseEvent );
	SetParamVisibility( PT_SLOPE_QUALITY, relief, raiseEvent );
	SetParamVisibility( PT_LIGHT_ANGLE, planar, raiseEvent );
	SetParamVisibility( PT_SPECULAR, planar, raiseEvent );
	SetParamVisibility( PT_TINT, planar, raiseEvent );

	for( int s = 0; s < MAX_STOPS; ++s )
	{
		// The colour picker (hue, saturation, brightness, alpha) is shown or hidden as one through
		// its hue parameter; the position slider is separate.
		SetParamVisibility( StopParam( s, SF_HUE ), s < stopCount, raiseEvent );
		SetParamVisibility( StopParam( s, SF_POS ), s < stopCount, raiseEvent );
	}
}

float NoiseLab::GetFloatParameter( unsigned int index )
{
	if( index >= NUM_PARAMS )
		return 0.0f;

	return params[ index ];
}

char* NoiseLab::GetParameterDisplay( unsigned int index )
{
	// Show friendlier values than the raw slider position.
	switch( index )
	{
	case PT_SCALE:
		snprintf( displayBuffer, sizeof( displayBuffer ), "%.2fx", ScaleFromSlider( params[ index ] ) );
		return displayBuffer;
	case PT_WARP_SCALE:
		snprintf( displayBuffer, sizeof( displayBuffer ), "%.2fx", WarpScaleFromSlider( params[ index ] ) );
		return displayBuffer;
	case PT_LACUNARITY:
		snprintf( displayBuffer, sizeof( displayBuffer ), "%.2f", LacunarityFromSlider( params[ index ] ) );
		return displayBuffer;
	case PT_SPEED:
		snprintf( displayBuffer, sizeof( displayBuffer ), "%.3f", SpeedFromSlider( params[ index ] ) );
		return displayBuffer;
	case PT_WARP:
		snprintf( displayBuffer, sizeof( displayBuffer ), "%.2f", WarpFromSlider( params[ index ] ) );
		return displayBuffer;
	case PT_HEIGHT:
		snprintf( displayBuffer, sizeof( displayBuffer ), "%.2f", HeightFromSlider( params[ index ] ) );
		return displayBuffer;
	case PT_STOPS:
	case PT_SEED:
		snprintf( displayBuffer, sizeof( displayBuffer ), "%d", static_cast< int >( std::floor( params[ index ] + 0.5f ) ) );
		return displayBuffer;
	case PT_TYPE:
	case PT_LOOP:
	case PT_ABSOLUTE:
	case PT_INVERT:
	case PT_MODE:
	case PT_CHANNELS:
	case PT_ALPHA_NOISE:
	case PT_TINT:
	case PT_INTERP:
	case PT_SLOPE_QUALITY:
		return CFFGLPlugin::GetParameterDisplay( index );
	default:
		if( index < NUM_PARAMS )
		{
			snprintf( displayBuffer, sizeof( displayBuffer ), "%.2f", params[ index ] );
			return displayBuffer;
		}
		return CFFGLPlugin::GetParameterDisplay( index );
	}
}
