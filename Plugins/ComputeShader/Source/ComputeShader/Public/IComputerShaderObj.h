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
	int32 CurveCount = 100;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader")
	int32 SampleCount = 5000;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader")
	float BaseLineStart = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader")
	float BaseLineStep = 10.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader")
	float ValueScale = 20.f;

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

	// 临时正弦波数据源模拟入口：生成一份二维 raw samples 后提交给 CPU 预处理管线。
	void SetMultiSinWaveData(float offset, float coefficient, float curvePhaseStep);

private:
	TArray<FCurveSegmentGPU>* TryGetWritableLineDataBuffer();
	void MarkLineDataReadyForUpload();
	void ResetCurveDataToSafeBuffers(int32 Width, int32 Height, int32 CurveCount);
	bool ApplyProcessedCurveData(FIComputerProcessedCurveData&& ProcessedData);
	bool TryApplyWorkerCurveProcessResult(int32 Width, int32 Height);
	bool ProcessCurveDataOnGameThread(const TArray<TArray<float>>& Values,
	                                  const FIComputerCurveRenderConfig& ConfigSnapshot);
	void QueueCurveDataForWorker(TArray<TArray<float>>&& Values,
	                             const FIComputerCurveRenderConfig& ConfigSnapshot);
	void QueueSimulatedCurveDataForWorker();
	void ShutdownCurveProcessWorker();

	UPROPERTY(Transient)
	UTextureRenderTarget2D* RenderTarget = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader", meta = (AllowPrivateAccess = "true"))
	FIComputerCurveRenderConfig RenderConfig;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader|RenderTarget", meta = (AllowPrivateAccess = "true", ClampMin = "1"))
	int32 RenderTargetWidth = 1024;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader|RenderTarget", meta = (AllowPrivateAccess = "true", ClampMin = "1"))
	int32 RenderTargetHeight = 1024;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader|Simulation", meta = (AllowPrivateAccess = "true"))
	float SimulatedSinOffset = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader|Simulation", meta = (AllowPrivateAccess = "true"))
	float SimulatedSinCoefficient = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader|Simulation", meta = (AllowPrivateAccess = "true"))
	float SimulatedCurvePhaseStep = 0.0f;

	// 每秒推进的相位（弧度），让 sin 波形横向滚动；设为 0 可暂停模拟波形动画。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader|Simulation", meta = (AllowPrivateAccess = "true"))
	float SimulatedScrollSpeed = 30.0f;

	enum { LineDataUploadBufferCount = 3 };

	TArray<float> LineDrawDesc;
	float SimulatedRunningPhase = 0.0f;
	bool bUseSimulatedCurveData = true;
	TArray<TArray<float>> CachedExternalCurveSamples;
	FComputerCurveProcessWorker* CurveProcessWorker = nullptr;
	int32 WorkerWidth = 0;
	int32 WorkerHeight = 0;
	float UploadTickAccumulatorSeconds = 0.0f;
	uint64 NextCurveProcessRequestId = 0;
	uint64 LastAcceptedCurveProcessRequestId = 0;

	// LineData 三缓冲只保护“线段大数组”的生命周期：
	// 1. game thread 把 worker 结果写入 LineDataBuffers[LineDataWriteBufferIndex]。
	// 2. 写完后把该槽标记成 LineDataReadyBufferIndex，等待上传。
	// 3. UploadProcessedCurveDataToGPU 将 ready 槽的 SharedPtr 捕获进 render command。
	// 4. render thread 上传完成前，这个槽不能被 game thread 覆盖，复用安全性由对应 fence 判断。
	TSharedPtr<TArray<FCurveSegmentGPU>, ESPMode::ThreadSafe> LineDataBuffers[LineDataUploadBufferCount];
	FRenderCommandFence LineDataUploadFences[LineDataUploadBufferCount];

	// 下一次 CPU 预处理结果优先写入的槽。
	int32 LineDataWriteBufferIndex = 0;
	// 已写完、还没被 UploadProcessedCurveDataToGPU 取走的槽；INDEX_NONE 表示没有待上传 LineData。
	int32 LineDataReadyBufferIndex = INDEX_NONE;
	// 和 LineDataReadyBufferIndex 一起表示“有一帧 LineData 等待进入 render command”。
	bool bHasPendingLineDataUpload = false;

	// 这些 buffer 在上传前会复制到局部变量并 move 捕获进 render command，所以不需要三缓冲。
	TArray<uint32> BucketRanges;
	TArray<FLinearColor> CurveColors;
};
