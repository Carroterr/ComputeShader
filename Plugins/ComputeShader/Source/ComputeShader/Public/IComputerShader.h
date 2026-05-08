#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"

// GPU 端线段布局，必须和 .usf 里的 FCurveSegment 完全一致。
// 32 字节对齐，单次取数即可拿到一条线段的全部数据。
struct FCurveSegmentGPU
{
	float X0;
	float Y0;
	float X1;
	float Y1;
	uint32 CurveIndex;
	uint32 _Pad0;
	uint32 _Pad1;
	uint32 _Pad2;
};

class COMPUTESHADER_API FIComputerShader : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FIComputerShader);
	SHADER_USE_PARAMETER_STRUCT(FIComputerShader, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters,)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, RenderTarget)
		// 输入 SRV：线条绘制描述数据（宽高、偏移等，具体解释由 usf 侧决定）。
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, LineDrawDesc)

		// 输入 SRV：每条线段打包成 FCurveSegmentGPU，单次取数完成。
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FCurveSegmentGPU>, LineData)

		// 每个屏幕 x bucket 的线段 offset/count。
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, BucketRanges)

		// 每条曲线一组颜色，CurveColors[CurveIndex] 对应一组数据。
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, CurveColors)
	END_SHADER_PARAMETER_STRUCT()

	// 竖条线程组：整 warp 共享同一个 x bucket，循环长度一致，无 divergence。
	static constexpr int32 ThreadGroupSizeX = 1;
	static constexpr int32 ThreadGroupSizeY = 64;
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
