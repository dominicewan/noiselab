#pragma once
#include <FFGLSDK.h>
#include <chrono>

// NoiseLab - procedural noise source for Resolume (FFGL 2.x).
// Step 4a: 16 noise types with shaping, scroll and warp, a multi-stop gradient, and four
// output modes (gradient, greyscale, normal map, planar relief).
class NoiseLab : public CFFGLPlugin
{
public:
	NoiseLab();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	char* GetParameterDisplay( unsigned int index ) override;

	static const int MAX_OCTAVES = 8;
	static const int MAX_STOPS   = 6;
	static const int STOP_SLOTS  = MAX_STOPS + 2;// room for the two extra stops Gradient Loop: Wrap adds
	static const int NUM_PARAMS  = 67;

private:
	struct Vec2d
	{
		double x = 0.0;
		double y = 0.0;
	};

	float params[ NUM_PARAMS ];  // values exactly as the host hands them to us
	float smoothed[ NUM_PARAMS ];// low-pass filtered copies used for rendering

	// Per-frame state. Nothing here allocates.
	bool firstFrame    = true;
	double evolvePhase = 0.0;          // integrated evolution position, in noise units
	double warpPhase   = 0.0;          // the warp field's own evolution position
	Vec2d octaveOffset[ MAX_OCTAVES ]; // integrated scroll position of each octave, in that octave's cells
	Vec2d warpOffset;                  // integrated scroll position of the warp field, in its cells
	std::chrono::steady_clock::time_point lastFrameTime;

	char displayBuffer[ 16 ] = {};

	int GetMode() const;
	int GetStopCount() const;
	void UpdateVisibility( bool raiseEvent );// hide parameters that don't apply to the current mode / stop count

	ffglex::FFGLShader shader;  //!< Compiles and links the noise shader.
	ffglex::FFGLScreenQuad quad;//!< Full screen quad the shader is drawn on.

	// Uniform locations, looked up once in InitGL so the render call does no string work.
	struct Locations
	{
		GLint aspect = -1, row0 = -1, row1 = -1;
		GLint octaves = -1, gain = -1, octRow0 = -1, octRow1 = -1, octCell = -1, octFrac = -1;
		GLint evolve = -1, period = -1, seed = -1, type = -1;
		GLint warp = -1, warpScale = -1, warpCell = -1, warpFrac = -1, warpEvolve = -1, gradLoop = -1;
		GLint lowClip = -1, clipRange = -1, contrast = -1, cycles = -1, absolute = -1, invert = -1;
		GLint resY = -1, mode = -1, channels = -1, alphaNoise = -1;
		GLint relief = -1, lightDir = -1, specular = -1, tint = -1, smoothSlope = -1;
		GLint stopCount = -1, stopPos = -1, stopCol = -1, interp = -1, shift = -1;
	} loc;
};
