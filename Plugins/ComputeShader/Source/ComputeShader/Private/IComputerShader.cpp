#include "IComputerShader.h"

IMPLEMENT_GLOBAL_SHADER(
	FIComputerShader,
	"/ComputeShaderShaders/CurvePlotting_MXAAShader.usf",
	"IComputerShader",
	SF_Compute
);