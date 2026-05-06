#include "IComputerShader.h"

IMPLEMENT_GLOBAL_SHADER(
	FIComputerShader,
	"/ComputeShaderShaders/IComputerShader.usf",
	"IComputerShader",
	SF_Compute
);