#pragma once

#include "CoreMinimal.h"

#include "GlobalShader.h"
#include "PixelShaderUtils.h"

#include "CustomComputeShader.generated.h"

/**
 * 全局计算着色器（Global Shader）对应的 C++ 封装。
 *
 * 这个类的职责是告诉引擎三件事：
 * 1. 这支 Shader 需要哪些参数（FParameters）；
 * 2. 允许编译哪些变体（ShouldCompilePermutation）；
 * 3. 编译时要注入哪些宏（ModifyCompilationEnvironment）。
 *
 * 注意：真正触发 Dispatch 的代码在 UCustomShader::ExecuteComputeShader 中，
 * 这里更像“着色器描述/绑定协议”，不是业务入口。
 */
class COMPUTESHADER_API FCustomComputeShader : public FGlobalShader
{
public:
	// 声明这是一个可被全局 Shader 系统管理的类型。
	DECLARE_GLOBAL_SHADER(FCustomComputeShader);

	// 声明本 Shader 使用参数结构体传参（推荐做法，便于 RDG 绑定）。
	SHADER_USE_PARAMETER_STRUCT(FCustomComputeShader, FGlobalShader);

	// 示例 Permutation 维度：定义一个名为 TEST 的 int 变体开关（当前只有 1 个取值）。
	class FCustomComputeShader_Perm_TEST : SHADER_PERMUTATION_INT("TEST", 1);

	using FPermutationDomain = TShaderPermutationDomain<FCustomComputeShader_Perm_TEST>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters,)

		// 输出 UAV：Compute Shader 会把结果写入这个 RenderTarget。
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, RenderTarget)

		// 输入 SRV：线条绘制描述数据（宽高、偏移等，具体解释由 usf 侧决定）。
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, LineDrawDesc)

		// 输入 SRV：每个采样点的数据（本示例中是波形数据）。
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, LineData)

	END_SHADER_PARAMETER_STRUCT()

public:
	/**
	 * 判断某个 Shader 变体是否需要编译。
	 * 当前示例返回 true，表示所有平台/特性级别都尝试编译。
	 * 后续可在这里加平台过滤（例如仅限 SM6 + DX12）。
	 */
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		const FPermutationDomain PermutationVector(Parameters.PermutationId);

		return true;
	}

	/**
	 * 设置编译环境宏。
	 * 这些宏会在 .usf 中用于 [numthreads(THREADS_X, THREADS_Y, THREADS_Z)]。
	 * 如果你改了这里的线程组尺寸，需要同步考虑 C++ 侧 group count 计算策略。
	 */
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		const FPermutationDomain PermutationVector(Parameters.PermutationId);

		// 供 usf 中 numthreads 使用的宏定义。
		OutEnvironment.SetDefine(TEXT("THREADS_X"), 32);
		OutEnvironment.SetDefine(TEXT("THREADS_Y"), 32);
		OutEnvironment.SetDefine(TEXT("THREADS_Z"), 1);
	}
};

UCLASS(Blueprintable, BlueprintType)
class COMPUTESHADER_API UCustomShader : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable)
	void ExecuteComputeShader(const TArray<float>& InValue);

	UFUNCTION(BlueprintCallable)
	void CreateRenderTarget();

	UFUNCTION(BlueprintCallable)
	void SetSinWaveData(float offset, float coefficient);

	UFUNCTION(BlueprintPure)
	UTextureRenderTarget2D* GetRenderTarget();

private:
	UTextureRenderTarget2D* RenderTarget;

	TArray<float> LineDrawDesc;
	TArray<int> LineData;
};
