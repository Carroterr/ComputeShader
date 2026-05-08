#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"

class COMPUTESHADER_API FIComputerShader : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FIComputerShader);
	SHADER_USE_PARAMETER_STRUCT(FIComputerShader, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters,)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, RenderTarget)
		// 输入 SRV：线条绘制描述数据（宽高、偏移等，具体解释由 usf 侧决定）。
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, LineDrawDesc)

		// 输入 SRV：每个采样点的数据（本示例中是波形数据）。
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, LineData)

		// 每个屏幕 x bucket 的线段 offset/count。
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, BucketRanges)

		// 每条曲线一组颜色，CurveColors[CurveIndex] 对应一组数据。
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, CurveColors)
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
