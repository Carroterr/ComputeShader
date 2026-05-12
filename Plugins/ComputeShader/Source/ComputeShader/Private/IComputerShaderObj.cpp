#include "IComputerShaderObj.h"

#include "IComputerShader.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/CriticalSection.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/ThreadSafeBool.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "RenderGraphUtils.h"

class FIComputerShader;

namespace
{
	constexpr int32 GLineDrawDescFloatCount = 4;
	constexpr int32 GBucketRangeUintCount = 2;
	constexpr int32 GLegacySourceCount = 5000;
	constexpr float GLegacySinAmplitude = 1000.0f;
	constexpr float GBinningExpand = 2.0f;

	struct FReducedWavePoint
	{
		int32 SourceIndex = 0;
		float X = 0.0f;
		float Y = 0.0f;
	};

	struct FCurveSegment
	{
		float X0 = 0.0f;
		float Y0 = 0.0f;
		float X1 = 0.0f;
		float Y1 = 0.0f;
		uint32 CurveIndex = 0;
	};

	struct FBinnedCurveSegment
	{
		FCurveSegment Segment;
		int32 StartBucket = 0;
		int32 EndBucket = 0;
	};

	// Returns a fallback color when the user did not configure one for this curve.
	FLinearColor GetDefaultCurveColor(int32 CurveIndex)
	{
		static const FLinearColor Palette[] =
		{
			FLinearColor(0.0f, 1.0f, 1.0f, 1.0f),
			FLinearColor(1.0f, 0.9f, 0.1f, 1.0f),
			FLinearColor(0.2f, 1.0f, 0.35f, 1.0f),
			FLinearColor(1.0f, 0.25f, 0.25f, 1.0f),
			FLinearColor(0.9f, 0.35f, 1.0f, 1.0f),
			FLinearColor(1.0f, 1.0f, 1.0f, 1.0f),
			FLinearColor(1.0f, 0.55f, 0.15f, 1.0f),
			FLinearColor(0.25f, 0.55f, 1.0f, 1.0f)
		};

		return Palette[CurveIndex % UE_ARRAY_COUNT(Palette)];
	}

	// Reads Config.CurveColors[CurveIndex], falling back to the default palette.
	FLinearColor GetCurveColor(const TArray<FLinearColor>& CurveColors, int32 CurveIndex)
	{
		return CurveColors.IsValidIndex(CurveIndex) ? CurveColors[CurveIndex] : GetDefaultCurveColor(CurveIndex);
	}

	// Builds the compact color table uploaded to the shader, one color per curve.
	void BuildCurveColors(const FIComputerCurveRenderConfig& Config, TArray<FLinearColor>& OutCurveColors)
	{
		const int32 CurveCount = FMath::Max(1, Config.CurveCount);
		OutCurveColors.Reset(CurveCount);

		for (int32 CurveIndex = 0; CurveIndex < CurveCount; ++CurveIndex)
		{
			OutCurveColors.Add(GetCurveColor(Config.CurveColors, CurveIndex));
		}
	}

	// Stores render size and curve count for the shader. Index 2 is reserved (legacy stride field, unused).
	void UpdateLineDrawDesc(TArray<float>& OutLineDrawDesc, int32 Width, int32 Height, int32 CurveCount)
	{
		OutLineDrawDesc.SetNumZeroed(GLineDrawDescFloatCount);
		OutLineDrawDesc[0] = static_cast<float>(Width);
		OutLineDrawDesc[1] = static_cast<float>(Height);
		OutLineDrawDesc[2] = 0.0f;
		OutLineDrawDesc[3] = static_cast<float>(FMath::Max(1, CurveCount));
	}

	// Creates safe non-empty buffers for initialization and invalid input cases.
	void ResetCurveBuffers(int32 Width, int32 Height, const FIComputerCurveRenderConfig& Config,
	                       TArray<float>& OutLineDrawDesc, TArray<FCurveSegmentGPU>& OutLineData,
	                       TArray<uint32>& OutBucketRanges, TArray<FLinearColor>& OutCurveColors)
	{
		const int32 SafeWidth = FMath::Max(1, Width);
		const int32 SafeHeight = FMath::Max(1, Height);
		const int32 CurveCount = FMath::Max(1, Config.CurveCount);

		UpdateLineDrawDesc(OutLineDrawDesc, SafeWidth, SafeHeight, CurveCount);

		OutLineData.Reset(1);
		OutLineData.AddZeroed(1);

		OutBucketRanges.SetNumZeroed(SafeWidth * GBucketRangeUintCount);
		BuildCurveColors(Config, OutCurveColors);
	}

	// M4 can select the same source point more than once, so keep only one point per source index.
	void AddPointUniqueSorted(TArray<FReducedWavePoint, TInlineAllocator<4>>& Points, const FReducedWavePoint& Point)
	{
		for (int32 PointIndex = 0; PointIndex < Points.Num(); ++PointIndex)
		{
			const FReducedWavePoint& ExistingPoint = Points[PointIndex];
			if (ExistingPoint.SourceIndex == Point.SourceIndex)
			{
				return;
			}

			if (Point.SourceIndex < ExistingPoint.SourceIndex)
			{
				Points.Insert(Point, PointIndex);
				return;
			}
		}

		Points.Add(Point);
	}

	// Records a segment and the x bucket range it can affect, including a small AA expansion.
	void AddSegmentToBins(TArray<FBinnedCurveSegment>& BinnedSegments, TArray<int32>& BucketCounts, int32 Width,
	                      uint32 CurveIndex, const FReducedWavePoint& Start, const FReducedWavePoint& End)
	{
		if (Start.SourceIndex == End.SourceIndex || Width <= 0)
		{
			return;
		}

		const int32 StartBucket = FMath::Clamp(FMath::FloorToInt(FMath::Min(Start.X, End.X) - GBinningExpand), 0,
		                                       Width - 1);
		const int32 EndBucket = FMath::Clamp(FMath::FloorToInt(FMath::Max(Start.X, End.X) + GBinningExpand), 0,
		                                     Width - 1);

		const FCurveSegment Segment{Start.X, Start.Y, End.X, End.Y, CurveIndex};
		for (int32 BucketIndex = StartBucket; BucketIndex <= EndBucket; ++BucketIndex)
		{
			++BucketCounts[BucketIndex];
		}

		BinnedSegments.Add(FBinnedCurveSegment{Segment, StartBucket, EndBucket});
	}

	// Reduces one curve with M4 sampling and emits screen-space segments into x buckets.
	template <typename SampleFuncType>
	void AddCurveToBins(int32 CurveIndex, int32 SampleCount, int32 Width, float BaseLine, float ValueScale,
	                    SampleFuncType GetSample, TArray<FBinnedCurveSegment>& BinnedSegments,
	                    TArray<int32>& BucketCounts)
	{
		if (SampleCount < 2 || Width <= 0)
		{
			return;
		}

		const float SourceToScreenScale = Width > 1
			                                  ? static_cast<float>(Width - 1) / static_cast<float>(SampleCount - 1)
			                                  : 0.0f;
		auto MakePoint = [SourceToScreenScale, BaseLine, ValueScale](int32 SourceIndex, float SampleValue)
		{
			return FReducedWavePoint{
				SourceIndex,
				static_cast<float>(SourceIndex) * SourceToScreenScale + 0.5f,
				BaseLine + SampleValue * ValueScale + 0.5f
			};
		};

		bool bHasPreviousPoint = false;
		FReducedWavePoint PreviousPoint;

		for (int32 BucketIndex = 0; BucketIndex < Width; ++BucketIndex)
		{
			const int32 BucketStart = static_cast<int32>(
				static_cast<int64>(BucketIndex) * static_cast<int64>(SampleCount) / static_cast<int64>(Width));
			const int32 BucketEnd = FMath::Max(
				BucketStart + 1,
				static_cast<int32>(
					static_cast<int64>(BucketIndex + 1) * static_cast<int64>(SampleCount) / static_cast<int64>(Width))
			);
			const int32 BucketLast = FMath::Min(BucketEnd - 1, SampleCount - 1);

			int32 MinIndex = BucketStart;
			int32 MaxIndex = BucketStart;
			const float StartValue = GetSample(BucketStart);
			float LastValue = StartValue;
			float MinValue = StartValue;
			float MaxValue = StartValue;

			for (int32 SourceIndex = BucketStart + 1; SourceIndex < BucketEnd; ++SourceIndex)
			{
				const float Value = GetSample(SourceIndex);
				if (SourceIndex == BucketLast)
				{
					LastValue = Value;
				}

				if (Value < MinValue)
				{
					MinValue = Value;
					MinIndex = SourceIndex;
				}

				if (Value > MaxValue)
				{
					MaxValue = Value;
					MaxIndex = SourceIndex;
				}
			}

			TArray<FReducedWavePoint, TInlineAllocator<4>> BucketPoints;
			AddPointUniqueSorted(BucketPoints, MakePoint(BucketStart, StartValue));
			AddPointUniqueSorted(BucketPoints, MakePoint(MinIndex, MinValue));
			AddPointUniqueSorted(BucketPoints, MakePoint(MaxIndex, MaxValue));
			AddPointUniqueSorted(BucketPoints, MakePoint(BucketLast, LastValue));

			if (BucketPoints.Num() == 0)
			{
				continue;
			}

			if (bHasPreviousPoint)
			{
				AddSegmentToBins(BinnedSegments, BucketCounts, Width, static_cast<uint32>(CurveIndex), PreviousPoint,
				                 BucketPoints[0]);
			}

			for (int32 PointIndex = 0; PointIndex + 1 < BucketPoints.Num(); ++PointIndex)
			{
				AddSegmentToBins(BinnedSegments, BucketCounts, Width, static_cast<uint32>(CurveIndex),
				                 BucketPoints[PointIndex], BucketPoints[PointIndex + 1]);
			}

			PreviousPoint = BucketPoints.Last();
			bHasPreviousPoint = true;
		}
	}

	// Builds final GPU buffers directly from binned segments.
	// BucketRanges[x * 2 + 0/1] records each column's offset/count in OutLineData.
	void BuildBucketRangesAndLineData(const TArray<FBinnedCurveSegment>& BinnedSegments,
	                                  const TArray<int32>& BucketCounts, TArray<uint32>& OutBucketRanges,
	                                  TArray<FCurveSegmentGPU>& OutLineData)
	{
		const int32 Width = BucketCounts.Num();
		int32 TotalSegmentCount = 0;

		OutBucketRanges.SetNumUninitialized(Width * GBucketRangeUintCount);
		TArray<uint32> BucketWriteOffsets;
		BucketWriteOffsets.SetNumUninitialized(Width);

		for (int32 BucketIndex = 0; BucketIndex < Width; ++BucketIndex)
		{
			const uint32 SegmentOffset = static_cast<uint32>(TotalSegmentCount);
			const int32 BucketSegmentCount = BucketCounts[BucketIndex];

			OutBucketRanges[BucketIndex * GBucketRangeUintCount] = SegmentOffset;
			OutBucketRanges[BucketIndex * GBucketRangeUintCount + 1] = static_cast<uint32>(BucketSegmentCount);
			BucketWriteOffsets[BucketIndex] = SegmentOffset;
			TotalSegmentCount += BucketSegmentCount;
		}

		if (TotalSegmentCount == 0)
		{
			OutLineData.Reset(1);
			OutLineData.AddZeroed(1);
			return;
		}

		OutLineData.SetNumUninitialized(TotalSegmentCount);

		for (const FBinnedCurveSegment& BinnedSegment : BinnedSegments)
		{
			const FCurveSegment& Segment = BinnedSegment.Segment;
			for (int32 BucketIndex = BinnedSegment.StartBucket; BucketIndex <= BinnedSegment.EndBucket; ++BucketIndex)
			{
				FCurveSegmentGPU& Out = OutLineData[BucketWriteOffsets[BucketIndex]++];
				Out.X0 = Segment.X0;
				Out.Y0 = Segment.Y0;
				Out.X1 = Segment.X1;
				Out.Y1 = Segment.Y1;
				Out.CurveIndex = Segment.CurveIndex;
				Out._Pad0 = 0;
				Out._Pad1 = 0;
				Out._Pad2 = 0;
			}
		}
	}

	FIComputerProcessedCurveData BuildProcessedCurveData(const TArray<TArray<float>>& Values,
	                                                    const FIComputerCurveRenderConfig& RenderConfig,
	                                                    int32 Width, int32 Height, int32 Generation)
	{
		FIComputerProcessedCurveData ProcessedData;
		ProcessedData.Generation = Generation;
		ProcessedData.Width = Width;
		ProcessedData.Height = Height;

		const int32 CurveCount = Values.Num();
		const int32 SampleCount = FMath::Max(2, RenderConfig.SampleCount);

		if (CurveCount <= 0)
		{
			FIComputerCurveRenderConfig ConfigForFrame = RenderConfig;
			ConfigForFrame.CurveCount = 1;
			ResetCurveBuffers(Width, Height, ConfigForFrame, ProcessedData.LineDrawDesc, ProcessedData.LineData,
			                  ProcessedData.BucketRanges, ProcessedData.CurveColors);
			return ProcessedData;
		}

		for (const TArray<float>& CurveSamples : Values)
		{
			if (CurveSamples.Num() < SampleCount)
			{
				FIComputerCurveRenderConfig ConfigForFrame = RenderConfig;
				ConfigForFrame.CurveCount = CurveCount;
				ResetCurveBuffers(Width, Height, ConfigForFrame, ProcessedData.LineDrawDesc, ProcessedData.LineData,
				                  ProcessedData.BucketRanges, ProcessedData.CurveColors);
				return ProcessedData;
			}
		}

		TRACE_CPUPROFILER_EVENT_SCOPE_STR("AIComputerShaderObj::ProcessCurveData");

		const int32 SafeCurveCount = FMath::Max(1, CurveCount);
		const int32 SafeSampleCount = FMath::Max(2, SampleCount);

		FIComputerCurveRenderConfig ConfigForFrame = RenderConfig;
		ConfigForFrame.CurveCount = SafeCurveCount;
		ConfigForFrame.SampleCount = SafeSampleCount;

		UpdateLineDrawDesc(ProcessedData.LineDrawDesc, Width, Height, SafeCurveCount);
		BuildCurveColors(ConfigForFrame, ProcessedData.CurveColors);

		TArray<FBinnedCurveSegment> BinnedSegments;
		TArray<int32> BucketCounts;
		{
			TRACE_CPUPROFILER_EVENT_SCOPE_STR("AIComputerShaderObj::ProcessCurveData.InitBuckets");
			const int64 EstimatedSegmentCount = static_cast<int64>(SafeCurveCount) * static_cast<int64>(Width) * 4;
			BinnedSegments.Reserve(static_cast<int32>(FMath::Min<int64>(EstimatedSegmentCount, MAX_int32)));
			BucketCounts.SetNumZeroed(FMath::Max(1, Width));
		}

		{
			TRACE_CPUPROFILER_EVENT_SCOPE_STR("AIComputerShaderObj::ProcessCurveData.BuildBuckets");
			for (int32 CurveIndex = 0; CurveIndex < SafeCurveCount; ++CurveIndex)
			{
				const float BaseLine = ConfigForFrame.BaseLineStart +
					static_cast<float>(CurveIndex) * ConfigForFrame.BaseLineStep;

				const TArray<float>& CurveSamples = Values[CurveIndex];
				auto GetCurveSample = [&CurveSamples](int32 SourceIndex)
				{
					return CurveSamples[SourceIndex];
				};

				AddCurveToBins(CurveIndex, SafeSampleCount, Width, BaseLine, ConfigForFrame.ValueScale,
				               GetCurveSample, BinnedSegments, BucketCounts);
			}
		}

		{
			TRACE_CPUPROFILER_EVENT_SCOPE_STR("AIComputerShaderObj::ProcessCurveData.BuildGPUData");
			BuildBucketRangesAndLineData(BinnedSegments, BucketCounts, ProcessedData.BucketRanges,
			                             ProcessedData.LineData);
		}

		ProcessedData.bAcceptedInput = true;
		return ProcessedData;
	}

	// Legacy SetSinWaveData defaults: one 5000-sample curve, caller-provided baseline, default color.
	FIComputerCurveRenderConfig MakeLegacyConfig(float BaseLineHeight)
	{
		FIComputerCurveRenderConfig Config;
		Config.CurveCount = 1;
		Config.SampleCount = GLegacySourceCount;
		Config.BaseLineStart = BaseLineHeight;
		Config.BaseLineStep = 0.0f;
		Config.ValueScale = GLegacySinAmplitude;
		Config.CurveColors.Add(GetDefaultCurveColor(0));
		return Config;
	}
}

class FComputerCurveProcessWorker final : public FRunnable
{
public:
	FComputerCurveProcessWorker()
	{
		WorkEvent = FPlatformProcess::GetSynchEventFromPool(false);
		Thread = FRunnableThread::Create(this, TEXT("IComputerShader_CurveProcessWorker"), 0, TPri_BelowNormal);
	}

	virtual ~FComputerCurveProcessWorker() override
	{
		Shutdown();
	}

	virtual uint32 Run() override
	{
		while (!bStopRequested)
		{
			WorkEvent->Wait();

			while (!bStopRequested)
			{
				FRequest Request;
				{
					FScopeLock Lock(&RequestCriticalSection);
					if (!bHasPendingRequest)
					{
						break;
					}

					Request = MoveTemp(PendingRequest);
					bHasPendingRequest = false;
				}

				FIComputerProcessedCurveData Result = BuildProcessedCurveData(
					Request.Values,
					Request.Config,
					Request.Width,
					Request.Height,
					Request.Generation
				);

				{
					FScopeLock Lock(&ResultCriticalSection);
					CompletedResult = MoveTemp(Result);
					bHasCompletedResult = true;
				}
			}
		}

		return 0;
	}

	virtual void Stop() override
	{
		bStopRequested = true;
		if (WorkEvent)
		{
			WorkEvent->Trigger();
		}
	}

	void Enqueue(TArray<TArray<float>>&& Values, const FIComputerCurveRenderConfig& Config, int32 Width, int32 Height,
	             int32 Generation)
	{
		{
			FScopeLock Lock(&RequestCriticalSection);
			PendingRequest.Values = MoveTemp(Values);
			PendingRequest.Config = Config;
			PendingRequest.Width = Width;
			PendingRequest.Height = Height;
			PendingRequest.Generation = Generation;
			bHasPendingRequest = true;
		}

		WorkEvent->Trigger();
	}

	bool DequeueResult(FIComputerProcessedCurveData& OutResult)
	{
		FScopeLock Lock(&ResultCriticalSection);
		if (!bHasCompletedResult)
		{
			return false;
		}

		OutResult = MoveTemp(CompletedResult);
		bHasCompletedResult = false;
		return true;
	}

	void Shutdown()
	{
		Stop();

		if (Thread)
		{
			Thread->WaitForCompletion();
			delete Thread;
			Thread = nullptr;
		}

		if (WorkEvent)
		{
			FPlatformProcess::ReturnSynchEventToPool(WorkEvent);
			WorkEvent = nullptr;
		}
	}

private:
	struct FRequest
	{
		TArray<TArray<float>> Values;
		FIComputerCurveRenderConfig Config;
		int32 Width = 0;
		int32 Height = 0;
		int32 Generation = 0;
	};

	FRunnableThread* Thread = nullptr;
	FEvent* WorkEvent = nullptr;
	FThreadSafeBool bStopRequested = false;

	FCriticalSection RequestCriticalSection;
	FRequest PendingRequest;
	bool bHasPendingRequest = false;

	FCriticalSection ResultCriticalSection;
	FIComputerProcessedCurveData CompletedResult;
	bool bHasCompletedResult = false;
};

AIComputerShaderObj::AIComputerShaderObj()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
}

TArray<FCurveSegmentGPU>& AIComputerShaderObj::GetWritableLineDataBuffer()
{
	auto EnsureBuffer = [](TSharedPtr<TArray<FCurveSegmentGPU>, ESPMode::ThreadSafe>& Buffer)
		-> TArray<FCurveSegmentGPU>&
	{
		if (!Buffer.IsValid())
		{
			Buffer = MakeShared<TArray<FCurveSegmentGPU>, ESPMode::ThreadSafe>();
		}

		return *Buffer;
	};

	auto IsBufferWritable = [this](int32 BufferIndex)
	{
		return BufferIndex != LineDataReadyBufferIndex && LineDataUploadFences[BufferIndex].IsFenceComplete();
	};

	if (!IsBufferWritable(LineDataWriteBufferIndex))
	{
		for (int32 Offset = 1; Offset < LineDataUploadBufferCount; ++Offset)
		{
			const int32 CandidateIndex = (LineDataWriteBufferIndex + Offset) % LineDataUploadBufferCount;
			if (IsBufferWritable(CandidateIndex))
			{
				LineDataWriteBufferIndex = CandidateIndex;
				break;
			}
		}
	}

	if (!IsBufferWritable(LineDataWriteBufferIndex))
	{
		int32 WaitBufferIndex = INDEX_NONE;
		for (int32 BufferIndex = 0; BufferIndex < LineDataUploadBufferCount; ++BufferIndex)
		{
			if (BufferIndex != LineDataReadyBufferIndex)
			{
				WaitBufferIndex = BufferIndex;
				break;
			}
		}

		if (WaitBufferIndex == INDEX_NONE)
		{
			WaitBufferIndex = LineDataWriteBufferIndex;
		}

		{
			TRACE_CPUPROFILER_EVENT_SCOPE_STR("AIComputerShaderObj::ProcessCurveData.WaitForLineDataUploadBuffer");
			LineDataUploadFences[WaitBufferIndex].Wait();
		}

		LineDataWriteBufferIndex = WaitBufferIndex;
	}

	return EnsureBuffer(LineDataBuffers[LineDataWriteBufferIndex]);
}

void AIComputerShaderObj::MarkLineDataReadyForUpload()
{
	bHasPendingLineDataUpload = true;
	LineDataReadyBufferIndex = LineDataWriteBufferIndex;
	LineDataWriteBufferIndex = (LineDataWriteBufferIndex + 1) % LineDataUploadBufferCount;
}

void AIComputerShaderObj::ResetCurveDataToSafeBuffers(int32 Width, int32 Height, int32 CurveCount)
{
	FIComputerCurveRenderConfig ConfigForFrame = RenderConfig;
	ConfigForFrame.CurveCount = FMath::Max(1, CurveCount);

	TArray<FCurveSegmentGPU>& WritableLineData = GetWritableLineDataBuffer();
	ResetCurveBuffers(Width, Height, ConfigForFrame, LineDrawDesc, WritableLineData, BucketRanges, CurveColors);
	MarkLineDataReadyForUpload();
}

void AIComputerShaderObj::ApplyProcessedCurveData(FIComputerProcessedCurveData&& ProcessedData)
{
	LineDrawDesc = MoveTemp(ProcessedData.LineDrawDesc);
	BucketRanges = MoveTemp(ProcessedData.BucketRanges);
	CurveColors = MoveTemp(ProcessedData.CurveColors);

	TArray<FCurveSegmentGPU>& WritableLineData = GetWritableLineDataBuffer();
	WritableLineData = MoveTemp(ProcessedData.LineData);
	if (WritableLineData.Num() == 0)
	{
		WritableLineData.AddZeroed(1);
	}

	LastAppliedCurveProcessGeneration = FMath::Max(LastAppliedCurveProcessGeneration, ProcessedData.Generation);
	MarkLineDataReadyForUpload();
}

bool AIComputerShaderObj::TryApplyWorkerCurveProcessResult(int32 Width, int32 Height)
{
	if (!CurveProcessWorker)
	{
		return false;
	}

	FIComputerProcessedCurveData ProcessedData;
	if (!CurveProcessWorker->DequeueResult(ProcessedData))
	{
		return false;
	}

	if (ProcessedData.Generation <= LastAppliedCurveProcessGeneration)
	{
		return false;
	}

	if (ProcessedData.Width != Width || ProcessedData.Height != Height)
	{
		bCurveProcessRequestPending = true;
		return false;
	}

	ApplyProcessedCurveData(MoveTemp(ProcessedData));
	return true;
}

void AIComputerShaderObj::RequestWorkerCurveProcess(int32 Width, int32 Height)
{
	if (!bCurveProcessRequestPending)
	{
		return;
	}

	if (!CurveProcessWorker)
	{
		CurveProcessWorker = new FComputerCurveProcessWorker();
	}

	TArray<TArray<float>> ValuesSnapshot = SimulatedCurveValues;
	FIComputerCurveRenderConfig ConfigSnapshot = RenderConfig;
	const int32 Generation = ++CurveProcessGeneration;
	bCurveProcessRequestPending = false;

	CurveProcessWorker->Enqueue(MoveTemp(ValuesSnapshot), ConfigSnapshot, Width, Height, Generation);
}

void AIComputerShaderObj::ShutdownCurveProcessWorker()
{
	if (CurveProcessWorker)
	{
		delete CurveProcessWorker;
		CurveProcessWorker = nullptr;
	}
}

void AIComputerShaderObj::CreateRenderTarget(int32 Width, int32 Height)
{
	RenderTargetWidth = FMath::Max(1, Width);
	RenderTargetHeight = FMath::Max(1, Height);

	RenderTarget = NewObject<UTextureRenderTarget2D>(this);

	RenderTarget->bCanCreateUAV = true;
	RenderTarget->InitCustomFormat(RenderTargetWidth, RenderTargetHeight, PF_FloatRGBA, false);
	RenderTarget->UpdateResourceImmediate(true);

	ResetCurveDataToSafeBuffers(RenderTargetWidth, RenderTargetHeight, RenderConfig.CurveCount);
	bCurveProcessRequestPending = true;
}

void AIComputerShaderObj::BeginPlay()
{
	Super::BeginPlay();

	if (!CurveProcessWorker)
	{
		CurveProcessWorker = new FComputerCurveProcessWorker();
	}

	if (bCreateRenderTargetOnBeginPlay && !RenderTarget)
	{
		CreateRenderTarget(RenderTargetWidth, RenderTargetHeight);
	}
}

void AIComputerShaderObj::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (bUploadEveryTick)
	{
		UploadProcessedCurveDataToGPU();
	}
}

void AIComputerShaderObj::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ShutdownCurveProcessWorker();
	Super::EndPlay(EndPlayReason);
}

void AIComputerShaderObj::BeginDestroy()
{
	ShutdownCurveProcessWorker();
	Super::BeginDestroy();
}

UTextureRenderTarget2D* AIComputerShaderObj::GetRenderTarget() const
{
	return RenderTarget;
}

void AIComputerShaderObj::Execute()
{
	UploadProcessedCurveDataToGPU();
}

void AIComputerShaderObj::SetRenderConfig(const FIComputerCurveRenderConfig& InConfig)
{
	RenderConfig = InConfig;
	bCurveProcessRequestPending = true;
}

FIComputerCurveRenderConfig AIComputerShaderObj::GetRenderConfig() const
{
	return RenderConfig;
}

void AIComputerShaderObj::UploadProcessedCurveDataToGPU()
{
	TRACE_CPUPROFILER_EVENT_SCOPE_STR("AIComputerShaderObj::UploadProcessedCurveDataToGPU");

	if (!RenderTarget)
	{
		return;
	}

	FTextureRenderTargetResource* RenderTargetResource = nullptr;
	int32 Width = 0;
	int32 Height = 0;

	{
		RenderTargetResource = RenderTarget->GameThread_GetRenderTargetResource();
		Width = RenderTarget->SizeX;
		Height = RenderTarget->SizeY;
	}

	bCurveProcessRequestPending = true;
	TryApplyWorkerCurveProcessResult(Width, Height);
	RequestWorkerCurveProcess(Width, Height);

	TSharedPtr<TArray<FCurveSegmentGPU>, ESPMode::ThreadSafe> LineDataUploadBuffer;
	int32 LineDataUploadBufferIndex = INDEX_NONE;
	{
		if (!bHasPendingLineDataUpload || LineDataReadyBufferIndex == INDEX_NONE ||
			!LineDataBuffers[LineDataReadyBufferIndex].IsValid())
		{
			return;
		}

		LineDataUploadBufferIndex = LineDataReadyBufferIndex;
		LineDataUploadBuffer = LineDataBuffers[LineDataUploadBufferIndex];
		if (LineDataUploadBuffer->Num() == 0)
		{
			LineDataUploadBuffer->AddZeroed(1);
		}

		bHasPendingLineDataUpload = false;
		LineDataReadyBufferIndex = INDEX_NONE;
	}

	TArray<float> LineDrawDescCopy;
	{
		LineDrawDescCopy = LineDrawDesc;
		if (LineDrawDescCopy.Num() != GLineDrawDescFloatCount)
		{
			UpdateLineDrawDesc(LineDrawDescCopy, Width, Height, RenderConfig.CurveCount);
		}
		LineDrawDescCopy[0] = static_cast<float>(Width);
		LineDrawDescCopy[1] = static_cast<float>(Height);
		LineDrawDescCopy[2] = 0.0f;
	}

	TArray<uint32> BucketRangesCopy;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE_STR("AIComputerShaderObj::UploadProcessedCurveDataToGPU.CopyBucketRanges");
		BucketRangesCopy = BucketRanges;
		const int32 ExpectedBucketRangeCount = FMath::Max(1, Width) * GBucketRangeUintCount;
		if (BucketRangesCopy.Num() != ExpectedBucketRangeCount)
		{
			BucketRangesCopy.SetNumZeroed(ExpectedBucketRangeCount);
		}
	}

	TArray<FLinearColor> CurveColorsCopy;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE_STR("AIComputerShaderObj::UploadProcessedCurveDataToGPU.CopyCurveColors");
		CurveColorsCopy = CurveColors;
		if (CurveColorsCopy.Num() == 0)
		{
			CurveColorsCopy.Add(GetDefaultCurveColor(0));
		}
		LineDrawDescCopy[3] = static_cast<float>(FMath::Max(1, CurveColorsCopy.Num()));
	}

	// Stage 2: copy processed CPU buffers to the render thread, upload them with RDG, then dispatch.
	{
		ENQUEUE_RENDER_COMMAND(ExecuteIComputerShader)(
			[RenderTargetResource, Width, Height, LineDrawDescCopy = MoveTemp(LineDrawDescCopy),
					LineDataUploadBuffer = MoveTemp(LineDataUploadBuffer),
					BucketRangesCopy = MoveTemp(BucketRangesCopy),
					CurveColorsCopy = MoveTemp(CurveColorsCopy)](
				FRHICommandListImmediate& RHICmdList)
			{
				TRACE_CPUPROFILER_EVENT_SCOPE_STR("AIComputerShaderObj::UploadProcessedCurveDataToGPU_RenderThread");

				FRDGBuilder GraphBuilder(RHICmdList);

				TShaderMapRef<FIComputerShader> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

				FIComputerShader::FParameters* PassParameters = nullptr;
				{
					PassParameters = GraphBuilder.AllocParameters<FIComputerShader::FParameters>();
				}

				{
					FRDGTextureRef TargetTexture = RegisterExternalTexture(
						GraphBuilder,
						RenderTargetResource->GetRenderTargetTexture(),
						TEXT("IComputerShader_RenderTarget")
					);

					PassParameters->RenderTarget = GraphBuilder.CreateUAV(TargetTexture);
				}
				{
					FRDGBufferRef LineDrawDescBuffer = CreateUploadBuffer(GraphBuilder, TEXT("LineDrawDescBuffer"),
					                                                      sizeof(float), LineDrawDescCopy.Num(),
					                                                      LineDrawDescCopy.GetData(),
					                                                      sizeof(float) * LineDrawDescCopy.Num());
					PassParameters->LineDrawDesc = GraphBuilder.CreateSRV(
						FRDGBufferSRVDesc(LineDrawDescBuffer, PF_R32_FLOAT));
				}

				{
					TRACE_CPUPROFILER_EVENT_SCOPE_STR("AIComputerShaderObj::UploadProcessedCurveDataToGPU_RenderThread.UploadLineData");
					const TArray<FCurveSegmentGPU>& LineDataUpload = *LineDataUploadBuffer;
					FRDGBufferRef LineDataBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("LineDataBuffer"),
					                                                      sizeof(FCurveSegmentGPU), LineDataUpload.Num(),
					                                                      LineDataUpload.GetData(),
					                                                      sizeof(FCurveSegmentGPU) * LineDataUpload.Num());
					PassParameters->LineData = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(LineDataBuffer));
				}

				{
					FRDGBufferRef BucketRangesBuffer = CreateUploadBuffer(GraphBuilder, TEXT("BucketRangesBuffer"),
					                                                      sizeof(uint32), BucketRangesCopy.Num(),
					                                                      BucketRangesCopy.GetData(),
					                                                      sizeof(uint32) * BucketRangesCopy.Num());
					PassParameters->BucketRanges = GraphBuilder.CreateSRV(
						FRDGBufferSRVDesc(BucketRangesBuffer, PF_R32_UINT));
				}

				{
					FRDGBufferRef CurveColorsBuffer = CreateUploadBuffer(GraphBuilder, TEXT("CurveColorsBuffer"),
					                                                     sizeof(FLinearColor), CurveColorsCopy.Num(),
					                                                     CurveColorsCopy.GetData(),
					                                                     sizeof(FLinearColor) * CurveColorsCopy.Num());
					PassParameters->CurveColors = GraphBuilder.CreateSRV(
						FRDGBufferSRVDesc(CurveColorsBuffer, PF_A32B32G32R32F));
				}

				FIntVector GroupCount(0, 0, 0);
				{
					GroupCount = FComputeShaderUtils::GetGroupCount(
						FIntVector(Width, Height, 1),
						FIntVector(
							FIComputerShader::ThreadGroupSizeX,
							FIComputerShader::ThreadGroupSizeY,
							FIComputerShader::ThreadGroupSizeZ
						)
					);
				}

				{
					GraphBuilder.AddPass(
						RDG_EVENT_NAME("AIComputerShaderObj::UploadProcessedCurveDataToGPU.Dispatch"),
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
				}

				{
					GraphBuilder.Execute();
				}
			}
		);
		LineDataUploadFences[LineDataUploadBufferIndex].BeginFence();
	}
}

void AIComputerShaderObj::SetSinWaveData(float offset, float coefficient, float baseLineHeight)
{
	SetRenderConfig(MakeLegacyConfig(baseLineHeight));
	SetMultiSinWaveData(offset, coefficient, 0.0f);
}

void AIComputerShaderObj::SetMultiSinWaveData(float offset, float coefficient, float curvePhaseStep)
{
	const int32 CurveCount = FMath::Max(1, RenderConfig.CurveCount);
	const int32 SampleCount = FMath::Max(2, RenderConfig.SampleCount);
	
	SimulatedCurveValues.SetNum(CurveCount, false);

	for (int32 CurveIndex = 0; CurveIndex < CurveCount; ++CurveIndex)
	{
		TArray<float>& CurveSamples = SimulatedCurveValues[CurveIndex];
		CurveSamples.SetNumUninitialized(SampleCount, false);

		const float CurveOffset = offset + static_cast<float>(CurveIndex) * curvePhaseStep;
		for (int32 SourceIndex = 0; SourceIndex < SampleCount; ++SourceIndex)
		{
			CurveSamples[SourceIndex] = FMath::Sin(FMath::DegreesToRadians(SourceIndex * coefficient) + CurveOffset);
		}
	}

	bCurveProcessRequestPending = true;
}

bool AIComputerShaderObj::ProcessCurveData(const TArray<TArray<float>>& Values)
{
	const int32 Width = RenderTarget ? RenderTarget->SizeX : 1024;
	const int32 Height = RenderTarget ? RenderTarget->SizeY : 1024;
	FIComputerProcessedCurveData ProcessedData = BuildProcessedCurveData(
		Values,
		RenderConfig,
		Width,
		Height,
		++CurveProcessGeneration
	);
	const bool bAcceptedInput = ProcessedData.bAcceptedInput;
	ApplyProcessedCurveData(MoveTemp(ProcessedData));
	return bAcceptedInput;
}
