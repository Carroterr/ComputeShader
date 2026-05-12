#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "IComputerShader.h"
#include "RenderCommandFence.h"
#include "IComputerShaderObj.generated.h"

class UTextureRenderTarget2D;
class FComputerCurveProcessWorker;

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

USTRUCT(BlueprintType)
struct COMPUTESHADER_API FIComputerCurveSamples
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader")
	TArray<float> Samples;
};

struct FIComputerProcessedCurveData
{
	int32 Generation = 0;
	int32 Width = 0;
	int32 Height = 0;
	bool bAcceptedInput = false;
	TArray<float> LineDrawDesc;
	TArray<FCurveSegmentGPU> LineData;
	TArray<uint32> BucketRanges;
	TArray<FLinearColor> CurveColors;
};

UCLASS(Blueprintable, BlueprintType)
class COMPUTESHADER_API AIComputerShaderObj : public AActor
{
	GENERATED_BODY()

public:
	AIComputerShaderObj();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void BeginDestroy() override;

	UFUNCTION(BlueprintCallable)
	void CreateRenderTarget(int32 Width = 1024, int32 Height = 1024);

	UFUNCTION(BlueprintPure)
	UTextureRenderTarget2D* GetRenderTarget() const;
	
	UFUNCTION(BlueprintCallable)
	void Execute();

	UFUNCTION(BlueprintCallable)
	void SetRenderConfig(const FIComputerCurveRenderConfig& InConfig);

	UFUNCTION(BlueprintPure)
	FIComputerCurveRenderConfig GetRenderConfig() const;
	
	// C++ 原生入口：UHT 不支持 TArray<TArray<float>> 作为 UFUNCTION 参数，所以蓝图入口使用 FIComputerCurveSamples 包一层。
	bool ProcessCurveData(const TArray<TArray<float>>& Values);

	// 将 ProcessCurveData 生成的最终结果上传到 GPU，并执行 compute shader。
	void UploadProcessedCurveDataToGPU();

	UFUNCTION(BlueprintCallable)
	void SetSinWaveData(float offset, float coefficient, float baseLineHeight);

	// 临时正弦波数据源模拟入口：生成 CurveCount 条曲线，每条曲线包含 SampleCount 个原始样本。
	// 生成结果复用内部 SimulatedCurveValues 缓存，然后走二维数组版本的 ProcessCurveData。
	UFUNCTION(BlueprintCallable)
	void SetMultiSinWaveData(float offset, float coefficient, float curvePhaseStep);
	
private:
	TArray<FCurveSegmentGPU>& GetWritableLineDataBuffer();
	void MarkLineDataReadyForUpload();
	void ResetCurveDataToSafeBuffers(int32 Width, int32 Height, int32 CurveCount);
	void ApplyProcessedCurveData(FIComputerProcessedCurveData&& ProcessedData);
	bool TryApplyWorkerCurveProcessResult(int32 Width, int32 Height);
	void RequestWorkerCurveProcess(int32 Width, int32 Height);
	void ShutdownCurveProcessWorker();

	UPROPERTY(Transient)
	UTextureRenderTarget2D* RenderTarget = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader", meta = (AllowPrivateAccess = "true"))
	FIComputerCurveRenderConfig RenderConfig;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader|Tick", meta = (AllowPrivateAccess = "true"))
	bool bAutoUploadToGPU = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader|Tick", meta = (AllowPrivateAccess = "true", ClampMin = "0.1"))
	float UploadFrequencyHz = 30.0f;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader|RenderTarget", meta = (AllowPrivateAccess = "true", ClampMin = "1"))
	int32 RenderTargetWidth = 1024;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader|RenderTarget", meta = (AllowPrivateAccess = "true", ClampMin = "1"))
	int32 RenderTargetHeight = 1024;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader|RenderTarget", meta = (AllowPrivateAccess = "true"))
	bool bCreateRenderTargetOnBeginPlay = true;
	
	enum { LineDataUploadBufferCount = 3 };

	TArray<float> LineDrawDesc;
	// SetMultiSinWaveData 专用的模拟原始数据缓存，布局为 [CurveIndex][SourceIndex]。
	// 不标记 UPROPERTY：UE 反射不支持 TArray<TArray<float>>，这里也不需要暴露给蓝图。
	TArray<TArray<float>> SimulatedCurveValues;
	FComputerCurveProcessWorker* CurveProcessWorker = nullptr;
	int32 CurveProcessGeneration = 0;
	int32 LastAppliedCurveProcessGeneration = 0;
	float UploadTickAccumulatorSeconds = 0.0f;
	bool bCurveProcessRequestPending = false;
	TSharedPtr<TArray<FCurveSegmentGPU>, ESPMode::ThreadSafe> LineDataBuffers[LineDataUploadBufferCount];
	FRenderCommandFence LineDataUploadFences[LineDataUploadBufferCount];
	int32 LineDataWriteBufferIndex = 0;
	int32 LineDataReadyBufferIndex = INDEX_NONE;
	bool bHasPendingLineDataUpload = false;
	TArray<uint32> BucketRanges;
	TArray<FLinearColor> CurveColors;
};
