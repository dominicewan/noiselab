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

	PT_COUNT
};
static_assert( PT_COUNT == NoiseLab::NUM_PARAMS, "NUM_PARAMS in NoiseLab.h must match the ParamID list" );

// Defaults, in ParamID order.
static const float PARAM_DEFAULTS[ NoiseLab::NUM_PARAMS ] = {
	1.0f, // Type: FBM
	0.0f, // Seed
	0.5f, // Scale slider (4x)
	0.0f, // Stretch
	0.0f, // Rotation
	4.0f, // Octaves
	2.0f, // Lacunarity
	0.5f, // Gain
	0.25f,// Evolve Speed
	0.0f, // Loop
	0.0f, // Loop Phase
	0.0f, // Scroll Angle
	0.0f, // Scroll Speed
	0.0f, // Warp
	0.5f, // Warp Scale slider (1x)
	0.0f, // Low Clip
	1.0f, // High Clip
	0.0f, // Contrast
	0.0f, // Absolute
	1.0f, // Cycles
	0.0f, // Invert
};

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
		return true;
	default:
		return false;
	}
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

static float ScaleFromSlider( float v )
{
	return 0.25f * std::pow( 256.0f, v );// 0..1 -> 0.25x..64x
}
static float WarpScaleFromSlider( float v )
{
	return 0.25f * std::pow( 16.0f, v );// 0..1 -> 0.25x..4x
}

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
	2,                                  // Plugin minor version number
	FF_SOURCE,                          // Plugin type
	"Procedural noise generator",       // Plugin description
	"NoiseLab step 2: shaping, scroll and warp"// About
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
uniform float uWarp;         // 0..1
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

void main()
{
	// Centre the pattern and correct for aspect so cells stay square at any output size.
	vec2 s = ( uv - 0.5 ) * vec2( uAspect, 1.0 );
	vec2 p = vec2( dot( uRow0, s ), dot( uRow1, s ) );

	uint seed = uint( uSeed ) * 7919u;
	p += warpOffset( p, seed );

	float octaves = uType == 0 ? 1.0 : uOctaves;
	float n = fractal( p, octaves, seed );

	float v = clamp( 0.5 + 0.6 * n, 0.0, 1.0 );
	v = shape( v );
	fragColor = vec4( v, v, v, 1.0 );
}
)";

NoiseLab::NoiseLab()
{
	for( int i = 0; i < NUM_PARAMS; ++i )
	{
		params[ i ]   = PARAM_DEFAULTS[ i ];
		smoothed[ i ] = PARAM_DEFAULTS[ i ];
	}

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
	SetParamRange( PT_LACUNARITY, 1.5f, 4.0f );
	SetParamGroup( PT_LACUNARITY, "Detail" );

	SetParamInfof( PT_GAIN, "Gain", FF_TYPE_STANDARD );
	SetParamRange( PT_GAIN, 0.2f, 0.8f );
	SetParamGroup( PT_GAIN, "Detail" );

	// --- Motion ---
	SetParamInfof( PT_SPEED, "Evolve Speed", FF_TYPE_STANDARD );
	SetParamRange( PT_SPEED, -2.0f, 2.0f );
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
	const double stretch  = std::pow( 4.0, static_cast< double >( smoothed[ PT_STRETCH ] ) );// >1 squashes x, stretches y
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
	const double lacunarity  = std::min( std::max( smoothed[ PT_LACUNARITY ], 1.5f ), 4.0f );
	const float gain         = std::min( std::max( smoothed[ PT_GAIN ], 0.2f ), 0.8f );
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
	double frequency = 1.0;
	for( int i = 0; i < MAX_OCTAVES; ++i )
	{
		octaveOffset[ i ].x = WrapLattice( octaveOffset[ i ].x + stepX * frequency );
		octaveOffset[ i ].y = WrapLattice( octaveOffset[ i ].y + stepY * frequency );
		octFreq[ i ]        = static_cast< GLfloat >( frequency );
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
	evolvePhase += dt * static_cast< double >( smoothed[ PT_SPEED ] );
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

	glUniform1f( loc.warp, std::min( std::max( smoothed[ PT_WARP ], 0.0f ), 1.0f ) );
	glUniform1f( loc.warpScale, static_cast< GLfloat >( warpScale ) );
	glUniform2iv( loc.warpCell, 1, warpCell );
	glUniform2fv( loc.warpFrac, 1, warpFrac );

	glUniform1f( loc.lowClip, lowClip );
	glUniform1f( loc.clipRange, clipRange );
	glUniform1f( loc.contrast, contrast );
	glUniform1f( loc.cycles, cycles );
	glUniform1i( loc.absolute, smoothed[ PT_ABSOLUTE ] > 0.5f ? 1 : 0 );
	glUniform1i( loc.invert, smoothed[ PT_INVERT ] > 0.5f ? 1 : 0 );

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
	return FF_SUCCESS;
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
	case PT_SEED:
		snprintf( displayBuffer, sizeof( displayBuffer ), "%d", static_cast< int >( std::floor( params[ index ] + 0.5f ) ) );
		return displayBuffer;
	case PT_TYPE:
	case PT_LOOP:
	case PT_ABSOLUTE:
	case PT_INVERT:
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
