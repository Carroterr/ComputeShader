#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"

class COMPUTESHADER_API FIComputerShader : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FIComputerShader);
	SHADER_USE_PARAMETER_STRUCT(FIComputerShader, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, RenderTarget)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, DrawDesc)
	END_SHADER_PARAMETER_STRUCT()

	static constexpr int32 ThreadGroupSizeX = 8;
	static constexpr int32 ThreadGroupSizeY = 8;
	static constexpr int32 ThreadGroupSizeZ = 1;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

	static void ModifyCompilationEnvironment(
		const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		OutEnvironment.SetDefine(TEXT("THREADS_X"), ThreadGroupSizeX);
		OutEnvironment.SetDefine(TEXT("THREADS_Y"), ThreadGroupSizeY);
		OutEnvironment.SetDefine(TEXT("THREADS_Z"), ThreadGroupSizeZ);
	}
};
