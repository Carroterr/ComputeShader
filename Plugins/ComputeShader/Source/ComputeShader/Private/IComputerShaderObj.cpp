#include "IComputerShaderObj.h"

#include "IComputerShader.h"
#include "Engine/TextureRenderTarget2D.h"
#include "RenderGraphUtils.h"

class FIComputerShader;

void UIComputerShaderObj::CreateRenderTarget(int32 Width, int32 Height)
{
	RenderTarget = NewObject<UTextureRenderTarget2D>(this);

	RenderTarget->bCanCreateUAV = true;
	RenderTarget->InitCustomFormat(Width, Height, PF_FloatRGBA, false);
	RenderTarget->UpdateResourceImmediate(true);

	// 预留/初始化绘制描述数据（示例中先放 6 个 float，实际含义由 usf 读取约定决定）。
	LineDrawDesc.SetNumZeroed(2);
	LineDrawDesc[0] = static_cast<float>(Width); // texture width
	LineDrawDesc[1] = static_cast<float>(Height); // texture height
	LineData.SetNumZeroed(Width * 2); // 极值采样长度 = texture width * 2
}

UTextureRenderTarget2D* UIComputerShaderObj::GetRenderTarget() const
{
	return RenderTarget;
}

void UIComputerShaderObj::Execute()
{
	if (!RenderTarget)
	{
		return;
	}

	FTextureRenderTargetResource* RenderTargetResource = RenderTarget->GameThread_GetRenderTargetResource();

	const int32 Width = RenderTarget->SizeX;
	const int32 Height = RenderTarget->SizeY;

	TArray<float> LineDrawDescCopy;
	LineDrawDescCopy.Add(static_cast<float>(Width));
	LineDrawDescCopy.Add(static_cast<float>(Height));

	TArray<float> LineDataCopy = LineData;
	LineDataCopy.SetNumZeroed(Width * 2);

	ENQUEUE_RENDER_COMMAND(ExecuteSimpleComputerShader)(
		[RenderTargetResource, Width, Height, LineDrawDescCopy, LineDataCopy](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);

			TShaderMapRef<FIComputerShader> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

			FIComputerShader::FParameters* PassParameters =
				GraphBuilder.AllocParameters<FIComputerShader::FParameters>();

			FRDGTextureRef TargetTexture = RegisterExternalTexture(
				GraphBuilder,
				RenderTargetResource->GetRenderTargetTexture(),
				TEXT("SimpleComputeShader_RenderTarget")
			);

			// 创建上传缓冲，把 CPU 内存中的 LineDrawDesc 数据送到 GPU。
			// 该缓冲随后以 SRV 只读方式绑定到 Shader。

			PassParameters->RenderTarget = GraphBuilder.CreateUAV(TargetTexture);

			FRDGBufferRef LineDrawDescBuffer = CreateUploadBuffer(GraphBuilder, TEXT("LineDrawDescBuffer"),
			                                                      sizeof(float), LineDrawDescCopy.Num(),
			                                                      LineDrawDescCopy.GetData(),
			                                                      sizeof(float) * LineDrawDescCopy.Num());

			// 绑定 LineDrawDesc 的 SRV，格式是 PF_R32_FLOAT（每元素一个 float）。
			PassParameters->LineDrawDesc = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(LineDrawDescBuffer, PF_R32_FLOAT));

			// 同理：上传波形采样数据 LineData。
			FRDGBufferRef LineDataBuffer = CreateUploadBuffer(GraphBuilder, TEXT("LineDataBuffer"), sizeof(float),
			                                                  LineDataCopy.Num(), LineDataCopy.GetData(),
			                                                  sizeof(float) * LineDataCopy.Num());

			// 绑定 LineData 的 SRV，对应 usf 中：Buffer<float> LineData;
			PassParameters->LineData = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(LineDataBuffer, PF_R32_FLOAT));

			const FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(
				FIntVector(Width, Height, 1),
				FIntVector(
					FIComputerShader::ThreadGroupSizeX,
					FIComputerShader::ThreadGroupSizeY,
					FIComputerShader::ThreadGroupSizeZ
				)
			);

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("SimpleComputeShader"),
				PassParameters,
				ERDGPassFlags::AsyncCompute,
				[ComputeShader, PassParameters, GroupCount](FRHIComputeCommandList& RHICmdList)
				{
					FComputeShaderUtils::Dispatch(
						RHICmdList,
						ComputeShader,
						*PassParameters,
						GroupCount
					);
				}
			);

			GraphBuilder.Execute();
		}
	);
}

void UIComputerShaderObj::SetSinWaveData(float offset, float coefficient)
{
	const int32 Width = RenderTarget ? RenderTarget->SizeX : 1024;
	LineData.SetNumZeroed(Width * 2);
    float lastMin = 0;
	for (int32 i = 0; i < 2046; i += 2)
	{
		float Value = FMath::Sin(FMath::DegreesToRadians(i * coefficient) + offset) * 300.0f + 500.0f;
		LineData[i] = lastMin;
		LineData[i + 1] = Value;
		lastMin = LineData[i + 1];
	}
}
