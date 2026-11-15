#pragma once
#include <FFGLSDK.h>
#include <chrono>

// NoiseLab - procedural noise source for Resolume (FFGL 2.x).
// Step 1: Perlin + FBM, scale, octaves, integrated evolution and seamless loop phase.
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

private:
	// Values exactly as the host hands them to us.
	float typeParam    = 1.0f; // option value: 0 = Perlin, 1 = FBM
	float scaleParam   = 0.5f; // 0..1 slider, mapped exponentially to 0.25x..64x
	float octavesParam = 4.0f; // 1..8, fractional
	float speedParam   = 0.25f;// -2..2 noise units per second
	float loopParam    = 0.0f; // boolean
	float phaseParam   = 0.0f; // 0..1, wraps seamlessly when Loop is on

	// Per-frame state. Nothing here allocates.
	bool firstFrame      = true;
	float smoothScale    = 4.0f;
	float smoothOctaves  = 4.0f;
	double evolvePhase   = 0.0;// integrated evolution position, in noise units
	std::chrono::steady_clock::time_point lastFrameTime;

	char displayBuffer[ 16 ] = {};

	ffglex::FFGLShader shader;  //!< Compiles and links the noise shader.
	ffglex::FFGLScreenQuad quad;//!< Full screen quad the shader is drawn on.
};
