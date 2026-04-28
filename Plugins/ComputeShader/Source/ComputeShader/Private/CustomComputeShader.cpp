#include "CustomComputeShader.h"

#include "Engine/TextureRenderTarget2D.h"

/*
 * 本文件是 Blueprint 侧与底层 Compute Shader 的“执行桥梁”。
 *
 * 关系概览：
 * 1) CustomComputeShader.h
 *    - 定义了 FCustomComputeShader（Shader 参数结构、编译宏等）
 *    - 定义了 UCustomShader（蓝图可调用封装）
 * 2) SimpleComputeShader.usf
 *    - 真正运行在 GPU 上的 HLSL 入口函数 SimpleComputeShader
 * 3) 本 cpp
 *    - 把 UCustomShader 的蓝图调用，转换成 RDG Pass + Dispatch
 *
 * 执行链路（从 BP 到 GPU）：
 * BP 调用 ExecuteComputeShader ->
 *   ENQUEUE_RENDER_COMMAND 切到渲染线程 ->
 *   RDG 构建参数和资源 ->
 *   AddPass + Dispatch ->
 *   GraphBuilder.Execute() 提交到 GPU
 */

// 统计分组：用于在 Unreal Insights / Stat 命令中观察该 Shader 相关耗时。
DECLARE_STATS_GROUP(TEXT("SimpleComputeShader"), STATGROUP_SimpleComputeShader, STATCAT_Advanced);

// CPU 周期统计点：包围 ExecuteComputeShader 中的关键构建逻辑。
DECLARE_CYCLE_STAT(TEXT("SimpleComputeShader Execute"), STAT_SimpleComputeShader_Execute, STATGROUP_SimpleComputeShader);

// 把 C++ Shader 类型和 usf 入口绑定起来。
// 参数含义：
// 1) FCustomComputeShader：C++ 侧 Shader 类型
// 2) "/ComputeShaderShaders/SimpleComputeShader.usf"：Shader 源文件虚拟路径
// 3) "SimpleComputeShader"：usf 中的入口函数名
// 4) SF_Compute：着色器阶段是 Compute Shader
// 注意：这里的虚拟路径前缀 /ComputeShaderShaders 必须和模块启动时注册的映射一致。
IMPLEMENT_GLOBAL_SHADER(FCustomComputeShader, "/ComputeShaderShaders/SimpleComputeShader.usf", "SimpleComputeShader", SF_Compute);

void UCustomShader::ExecuteComputeShader(const TArray<float>& InValue)
{
	// 蓝图传入的绘制描述参数（例如宽高等）拷贝到成员变量，
	// 后续会上传到 GPU 并绑定到 usf 的 Buffer<float> LineDrawDesc。
	LineDrawDesc = InValue;

	// Compute Shader 的调度必须在渲染线程上进行，不能直接在游戏线程执行。
	// 因此把后续所有 RHI/RDG 操作封装成命令，排队到渲染线程执行。
	ENQUEUE_RENDER_COMMAND(SceneDrawCompletion)(
		[&](FRHICommandListImmediate& RHICmdList)
		{
			// 创建 RDG 构建器：本次 pass 用到的纹理/缓冲/依赖关系都在这里描述。
			// 只有最终调用 Execute()，这些描述才会真正提交执行。
			FRDGBuilder GraphBuilder(RHICmdList);
			{
				SCOPE_CYCLE_COUNTER(STAT_SimpleComputeShader_Execute);
				// GPU 统计与事件标记，便于在分析工具中定位本 Pass。
				DECLARE_GPU_STAT(SimpleComputeShader)
				RDG_EVENT_SCOPE(GraphBuilder, "SimpleComputeShader");
				RDG_GPU_STAT_SCOPE(GraphBuilder, SimpleComputeShader);

				// Shader 变体参数。当前工程只定义了一个 TEST 维度，默认构造即可。
				typename FCustomComputeShader::FPermutationDomain PermutationVector;

				// 从全局 ShaderMap 中拿到本平台/特性级别的 Compute Shader 实例。
				TShaderMapRef<FCustomComputeShader> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel), PermutationVector);

				// 运行时安全检查：如果 Shader 未成功编译/加载，则本次不派发。
				bool bIsShaderValid = ComputeShader.IsValid();

				if (bIsShaderValid)
				{
					// 分配并填写 Shader 参数结构体。
					// 字段名必须与 FCustomComputeShader::FParameters 定义一致。
					FCustomComputeShader::FParameters* PassParameters = GraphBuilder.AllocParameters<FCustomComputeShader::FParameters>();

					// 把 UTextureRenderTarget2D 对应的外部 RHI 纹理注册到 RDG。
					// 这样 RDG Pass 才能把它当作图内资源使用。
					FRDGTextureRef TargetTexture = RegisterExternalTexture(GraphBuilder, RenderTarget->GetRenderTargetResource()->GetRenderTargetTexture(),
					                                                       TEXT("SimpleComputeShader_RT"));

					// 创建 UAV（可写视图）并绑定到 Shader 参数 RenderTarget。
					// 对应 usf 中：RWTexture2D<float4> RenderTarget;
					PassParameters->RenderTarget = GraphBuilder.CreateUAV(TargetTexture);

					// 创建上传缓冲，把 CPU 内存中的 LineDrawDesc 数据送到 GPU。
					// 该缓冲随后以 SRV 只读方式绑定到 Shader。
					FRDGBufferRef LineDrawDescBuffer = CreateUploadBuffer(GraphBuilder, TEXT("LineDrawDescBuffer"), sizeof(float), LineDrawDesc.Num(), LineDrawDesc.GetData(),
					                                                      sizeof(float) * LineDrawDesc.Num());

					// 绑定 LineDrawDesc 的 SRV，格式是 PF_R32_FLOAT（每元素一个 float）。
					PassParameters->LineDrawDesc = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(LineDrawDescBuffer, PF_R32_FLOAT));

					// 同理：上传波形采样数据 LineData。
					FRDGBufferRef LineDataBuffer = CreateUploadBuffer(GraphBuilder, TEXT("LineDataBuffer"), sizeof(float), LineData.Num(), LineData.GetData(),
					                                                  sizeof(int) * LineData.Num());

					// 绑定 LineData 的 SRV，对应 usf 中：Buffer<float> LineData;
					PassParameters->LineData = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(LineDataBuffer, PF_R32_FLOAT));

					// 根据渲染目标尺寸计算 Dispatch 的线程组数量。
					// 这里使用 UE 推荐的 kGolden2DGroupSize；它应与 usf 的 numthreads 宏协同工作，
					// 保证覆盖整张目标纹理，并由 shader 内边界判断处理越界线程。
					auto GroupCount = FComputeShaderUtils::GetGroupCount(FIntVector(RenderTarget->SizeX, RenderTarget->SizeY, 1), FComputeShaderUtils::kGolden2DGroupSize);
					GraphBuilder.AddPass(
						RDG_EVENT_NAME("SimpleComputeShader"),
						PassParameters,
						// 该 Pass 允许在异步计算队列执行（具体是否异步由平台/RHI 决定）。
						ERDGPassFlags::AsyncCompute,
						[&PassParameters, ComputeShader, GroupCount](FRHIComputeCommandList& RHICmdList)
						{
							// 真正发起一次 Compute Shader Dispatch。
							FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, *PassParameters, GroupCount);
						});
				}
			}
			// 执行 RDG：把上面声明的资源和 Pass 真正提交给 RHI/GPU。
			GraphBuilder.Execute();
		});
}

void UCustomShader::CreateRenderTarget()
{
	// 创建运行时渲染目标，用于承接 Compute Shader 输出。
	RenderTarget = NewObject<UTextureRenderTarget2D>();

	// 必须允许创建 UAV，否则无法在 Compute Shader 中作为 RWTexture2D 写入。
	RenderTarget->bCanCreateUAV = true;

	// 1024x1024 + PF_FloatRGBA：
	// - 分辨率决定输出纹理大小
	// - PF_FloatRGBA 与 usf 中 RWTexture2D<float4> 对应
	RenderTarget->InitCustomFormat(1024, 1024, PF_FloatRGBA, false);

	// 立即创建底层 RHI 资源，确保后续渲染线程可访问。
	RenderTarget->UpdateResourceImmediate(true);

	// 预留/初始化绘制描述数据（示例中先放 6 个 float，实际含义由 usf 读取约定决定）。
	LineDrawDesc.AddZeroed(6);

	LineData.SetNum(1024);
}

void UCustomShader::SetSinWaveData(float offset, float coefficient)
{
	for (int32 i = 0; i < 1024; ++i)
	{
		int Value = static_cast<int>(FMath::Sin(FMath::DegreesToRadians(i * coefficient) + offset) * 300 + 500);
		LineData[i] = Value;
	}
}

UTextureRenderTarget2D* UCustomShader::GetRenderTarget()
{
	// 提供给 BP/UMG 侧读取本次 Compute Shader 结果纹理。
	return RenderTarget;
}
