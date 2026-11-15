#include "NoiseLab.h"
#include <cmath>
#include <cstdio>
#include <algorithm>
using namespace ffglex;

enum ParamID : FFUInt32
{
	PT_TYPE,
	PT_SCALE,
	PT_OCTAVES,

	PT_SPEED,
	PT_LOOP,
	PT_PHASE,
};

// The evolution axis of the noise repeats every EVOLVE_WRAP units, so the integrated phase can be
// wrapped there without a visible jump (keeps float precision healthy on long gigs).
// LOOP_LENGTH is how many noise units one trip of Loop Phase 0..1 travels. EVOLVE_WRAP must be a
// multiple of LOOP_LENGTH.
static const int EVOLVE_WRAP = 256;
static const int LOOP_LENGTH = 4;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< NoiseLab >,          // Create method
	"NZLB",                             // Plugin unique ID (4 chars). Change it if Resolume shows stale parameters.
	"NoiseLab",                         // Plugin name
	2,                                  // API major version number
	1,                                  // API minor version number
	0,                                  // Plugin major version number
	1,                                  // Plugin minor version number
	FF_SOURCE,                          // Plugin type
	"Procedural noise generator",       // Plugin description
	"NoiseLab step 1: Perlin and FBM"   // About
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

//NOISELAB_FRAGMENT_BEGIN
static const char fragmentShaderCode[] = R"(#version 410 core
uniform vec2 uResolution; // output size in pixels
uniform float uScale;     // noise cells across the output height
uniform float uOctaves;   // 1..8, fractional
uniform float uEvolve;    // position on the evolution axis
uniform int uPeriod;      // evolution axis repeats after this many units
uniform int uType;        // 0 = Perlin, 1 = FBM

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
uint hash3( ivec3 p )
{
	return pcg( uint( p.x ) + pcg( uint( p.y ) + pcg( uint( p.z ) ) ) );
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

// 3D gradient noise in roughly -1..1, periodic along z with period periodZ.
// The periodic z axis is what makes looping exact and free.
float pnoise( vec3 p, int periodZ )
{
	vec3 pf = floor( p );
	ivec3 i = ivec3( pf );
	vec3 f  = p - pf;
	vec3 w  = f * f * f * ( f * ( f * 6.0 - 15.0 ) + 10.0 );

	int z0 = wrapInt( i.z, periodZ );
	int z1 = wrapInt( i.z + 1, periodZ );

	float n000 = gradDot( hash3( ivec3( i.x,     i.y,     z0 ) ), f );
	float n100 = gradDot( hash3( ivec3( i.x + 1, i.y,     z0 ) ), f - vec3( 1.0, 0.0, 0.0 ) );
	float n010 = gradDot( hash3( ivec3( i.x,     i.y + 1, z0 ) ), f - vec3( 0.0, 1.0, 0.0 ) );
	float n110 = gradDot( hash3( ivec3( i.x + 1, i.y + 1, z0 ) ), f - vec3( 1.0, 1.0, 0.0 ) );
	float n001 = gradDot( hash3( ivec3( i.x,     i.y,     z1 ) ), f - vec3( 0.0, 0.0, 1.0 ) );
	float n101 = gradDot( hash3( ivec3( i.x + 1, i.y,     z1 ) ), f - vec3( 1.0, 0.0, 1.0 ) );
	float n011 = gradDot( hash3( ivec3( i.x,     i.y + 1, z1 ) ), f - vec3( 0.0, 1.0, 1.0 ) );
	float n111 = gradDot( hash3( ivec3( i.x + 1, i.y + 1, z1 ) ), f - vec3( 1.0, 1.0, 1.0 ) );

	float nx00 = mix( n000, n100, w.x );
	float nx10 = mix( n010, n110, w.x );
	float nx01 = mix( n001, n101, w.x );
	float nx11 = mix( n011, n111, w.x );
	return mix( mix( nx00, nx10, w.y ), mix( nx01, nx11, w.y ), w.z );
}

// Fractal sum. The top octave fades in with the fractional part of uOctaves so
// modulating Octaves never pops.
float fbm( vec2 p, float z, int periodZ )
{
	float sum  = 0.0;
	float norm = 0.0;
	float amp  = 0.5;
	float freq = 1.0;
	int period = periodZ;
	for( int i = 0; i < 8; ++i )
	{
		float weight = clamp( uOctaves - float( i ), 0.0, 1.0 );
		if( weight <= 0.0 )
			break;
		// Offset each octave so their lattices never line up.
		vec2 q = p * freq + vec2( 17.3, 9.1 ) * float( i );
		sum  += weight * amp * pnoise( vec3( q, z * freq ), period );
		norm += weight * amp;
		amp    *= 0.5;
		freq   *= 2.0;
		period *= 2;
	}
	return sum / norm;
}

void main()
{
	// Centre the pattern and correct for aspect so cells stay square at any output size.
	float aspect = uResolution.x / max( uResolution.y, 1.0 );
	vec2 p = ( uv - 0.5 ) * vec2( aspect, 1.0 ) * uScale;

	float n;
	if( uType == 0 )
		n = pnoise( vec3( p, uEvolve ), uPeriod ) * 1.4;
	else
		n = fbm( p, uEvolve, uPeriod ) * 1.9;

	float v = clamp( 0.5 + 0.5 * n, 0.0, 1.0 );
	fragColor = vec4( v, v, v, 1.0 );
}
)";
//NOISELAB_FRAGMENT_END

NoiseLab::NoiseLab()
{
	// A source has no inputs.
	SetMinInputs( 0 );
	SetMaxInputs( 0 );

	// --- Noise ---
	SetOptionParamInfo( PT_TYPE, "Type", 2, typeParam );
	SetParamElementInfo( PT_TYPE, 0, "Perlin", 0.0f );
	SetParamElementInfo( PT_TYPE, 1, "FBM", 1.0f );
	SetParamGroup( PT_TYPE, "Noise" );

	SetParamInfof( PT_SCALE, "Scale", FF_TYPE_STANDARD );
	SetParamGroup( PT_SCALE, "Noise" );

	SetParamInfof( PT_OCTAVES, "Octaves", FF_TYPE_STANDARD );
	SetParamRange( PT_OCTAVES, 1.0f, 8.0f );
	SetParamGroup( PT_OCTAVES, "Noise" );

	// --- Motion ---
	SetParamInfof( PT_SPEED, "Evolve Speed", FF_TYPE_STANDARD );
	SetParamRange( PT_SPEED, -2.0f, 2.0f );
	SetParamGroup( PT_SPEED, "Motion" );

	SetParamInfo( PT_LOOP, "Loop", FF_TYPE_BOOLEAN, false );
	SetParamGroup( PT_LOOP, "Motion" );

	SetParamInfof( PT_PHASE, "Loop Phase", FF_TYPE_STANDARD );
	SetParamGroup( PT_PHASE, "Motion" );

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

	// Slider 0..1 -> 0.25x..64x. Exponential so equal knob travel is an equal zoom ratio.
	const float targetScale   = 0.25f * std::pow( 256.0f, scaleParam );
	const float targetOctaves = std::min( std::max( octavesParam, 1.0f ), 8.0f );

	// Light smoothing (about 50 ms) so stepped MIDI/FFT input zooms instead of jumping.
	if( firstFrame )
	{
		smoothScale   = targetScale;
		smoothOctaves = targetOctaves;
		firstFrame    = false;
	}
	else
	{
		const float blend = 1.0f - std::exp( -static_cast< float >( dt ) / 0.05f );
		smoothScale   += ( targetScale - smoothScale ) * blend;
		smoothOctaves += ( targetOctaves - smoothOctaves ) * blend;
	}

	// Integrate speed into a position. Because we accumulate, changing Evolve Speed only changes
	// how fast we move from here on, it never teleports the pattern.
	evolvePhase += dt * static_cast< double >( speedParam );
	evolvePhase  = std::fmod( evolvePhase, static_cast< double >( EVOLVE_WRAP ) );
	if( evolvePhase < 0.0 )
		evolvePhase += EVOLVE_WRAP;

	const bool looping = loopParam > 0.5f;
	double evolve      = evolvePhase;
	int period         = EVOLVE_WRAP;
	if( looping )
	{
		// Loop Phase 0..1 is one full trip round a short periodic axis. Evolve Speed still runs,
		// so the loop can free-run, be scrubbed, or both.
		evolve = std::fmod( evolvePhase + static_cast< double >( phaseParam ) * LOOP_LENGTH, static_cast< double >( LOOP_LENGTH ) );
		period = LOOP_LENGTH;
	}

	//FFGL requires us to leave the context in a default state on return, so use this scoped binding to help us do that.
	ScopedShaderBinding shaderBinding( shader.GetGLID() );
	shader.Set( "uResolution", static_cast< float >( currentViewport.width ), static_cast< float >( currentViewport.height ) );
	shader.Set( "uScale", smoothScale );
	shader.Set( "uOctaves", smoothOctaves );
	shader.Set( "uEvolve", static_cast< float >( evolve ) );
	shader.Set( "uPeriod", period );
	shader.Set( "uType", typeParam > 0.5f ? 1 : 0 );

	quad.Draw();

	return FF_SUCCESS;
}

FFResult NoiseLab::DeInitGL()
{
	shader.FreeGLResources();
	quad.Release();
	return FF_SUCCESS;
}

FFResult NoiseLab::SetFloatParameter( unsigned int index, float value )
{
	switch( index )
	{
	case PT_TYPE:
		typeParam = value;
		break;
	case PT_SCALE:
		scaleParam = value;
		break;
	case PT_OCTAVES:
		octavesParam = value;
		break;
	case PT_SPEED:
		speedParam = value;
		break;
	case PT_LOOP:
		loopParam = value;
		break;
	case PT_PHASE:
		phaseParam = value;
		break;
	default:
		return FF_FAIL;
	}

	return FF_SUCCESS;
}

float NoiseLab::GetFloatParameter( unsigned int index )
{
	switch( index )
	{
	case PT_TYPE:
		return typeParam;
	case PT_SCALE:
		return scaleParam;
	case PT_OCTAVES:
		return octavesParam;
	case PT_SPEED:
		return speedParam;
	case PT_LOOP:
		return loopParam;
	case PT_PHASE:
		return phaseParam;
	}

	return 0.0f;
}

char* NoiseLab::GetParameterDisplay( unsigned int index )
{
	// Show the real zoom factor for Scale instead of the raw 0..1 slider value.
	if( index == PT_SCALE )
	{
		snprintf( displayBuffer, sizeof( displayBuffer ), "%.2fx", 0.25f * std::pow( 256.0f, scaleParam ) );
		return displayBuffer;
	}
	return CFFGLPlugin::GetParameterDisplay( index );
}
