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

	TArray<float> DrawDesc;
	DrawDesc.Add(static_cast<float>(Width));
	DrawDesc.Add(static_cast<float>(Height));

	ENQUEUE_RENDER_COMMAND(ExecuteMyFirstComputeShader)(
		[RenderTargetResource, Width, Height, DrawDesc](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);

			TShaderMapRef<FIComputerShader> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

			FIComputerShader::FParameters* PassParameters =
				GraphBuilder.AllocParameters<FIComputerShader::FParameters>();

			FRDGTextureRef TargetTexture = RegisterExternalTexture(
				GraphBuilder,
				RenderTargetResource->GetRenderTargetTexture(),
				TEXT("MyFirstComputeShader_RenderTarget")
			);

			PassParameters->RenderTarget = GraphBuilder.CreateUAV(TargetTexture);

			FRDGBufferRef DrawDescBuffer = CreateUploadBuffer(
				GraphBuilder,
				TEXT("MyFirstComputeShader_DrawDesc"),
				sizeof(float),
				DrawDesc.Num(),
				DrawDesc.GetData(),
				sizeof(float) * DrawDesc.Num()
			);

			PassParameters->DrawDesc =
				GraphBuilder.CreateSRV(FRDGBufferSRVDesc(DrawDescBuffer, PF_R32_FLOAT));

			const FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(
				FIntVector(Width, Height, 1),
				FIntVector(
					FIComputerShader::ThreadGroupSizeX,
					FIComputerShader::ThreadGroupSizeY,
					FIComputerShader::ThreadGroupSizeZ
				)
			);

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("MyFirstComputeShader"),
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
	for (int32 i = 0; i < 1024; ++i)
	{
		float Value = FMath::Sin(FMath::DegreesToRadians(i * coefficient) + offset) * 300.0f + 500.0f;
		//.LineData[i] = Value;
	}
}
