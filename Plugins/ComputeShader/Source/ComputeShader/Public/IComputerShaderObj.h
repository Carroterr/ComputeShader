#pragma once

#include "CoreMinimal.h"
#include "IComputerShader.h"
#include "RenderCommandFence.h"
#include "UObject/Object.h"
#include "IComputerShaderObj.generated.h"

class UTextureRenderTarget2D;

USTRUCT(BlueprintType)
struct COMPUTESHADER_API FIComputerCurveRenderConfig
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader")
	int32 CurveCount = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader")
	int32 SampleCount = 5000;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader")
	float BaseLineStart = 512.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader")
	float BaseLineStep = 128.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader")
	float ValueScale = 100.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader")
	TArray<FLinearColor> CurveColors;
};

UCLASS(Blueprintable, BlueprintType)
class COMPUTESHADER_API UIComputerShaderObj : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable)
	void CreateRenderTarget(int32 Width = 1024, int32 Height = 1024);

	UFUNCTION(BlueprintPure)
	UTextureRenderTarget2D* GetRenderTarget() const;
	
	UFUNCTION(BlueprintCallable)
	void Execute();

	// 输入原始曲线数组，做极值采样/分桶，只生成待 GPU 使用的缓存数据。
	UFUNCTION(BlueprintCallable)
	bool ProcessCurveData(const TArray<float>& values, const FIComputerCurveRenderConfig& config);

	// 将 ProcessCurveData 生成的最终结果上传到 GPU，并执行 compute shader。
	UFUNCTION(BlueprintCallable)
	void UploadProcessedCurveDataToGPU();

	UFUNCTION(BlueprintCallable)
	void SetSinWaveData(float offset, float coefficient, float baseLineHeight);

	UFUNCTION(BlueprintCallable)
	void SetMultiSinWaveData(float offset, float coefficient, float curvePhaseStep,
	                         const FIComputerCurveRenderConfig& config);

	UFUNCTION(BlueprintCallable)
	void SetCurveData(const TArray<float>& values, const FIComputerCurveRenderConfig& config);
	
private:
	TArray<FCurveSegmentGPU>& GetWritableLineDataBuffer();
	void MarkLineDataReadyForUpload();

	UPROPERTY(Transient)
	UTextureRenderTarget2D* RenderTarget = nullptr;
	
	enum { LineDataUploadBufferCount = 3 };

	TArray<float> LineDrawDesc;
	TSharedPtr<TArray<FCurveSegmentGPU>, ESPMode::ThreadSafe> LineDataBuffers[LineDataUploadBufferCount];
	FRenderCommandFence LineDataUploadFences[LineDataUploadBufferCount];
	int32 LineDataWriteBufferIndex = 0;
	int32 LineDataReadyBufferIndex = INDEX_NONE;
	bool bHasPendingLineDataUpload = false;
	TArray<uint32> BucketRanges;
	TArray<FLinearColor> CurveColors;
};
