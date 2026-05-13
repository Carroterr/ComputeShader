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

#include <atomic>

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
	                                                     int32 Width, int32 Height)
	{
		FIComputerProcessedCurveData ProcessedData;
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
	FComputerCurveProcessWorker(const TArray<TArray<float>>* InValuesPtr,
	                            const FIComputerCurveRenderConfig* InConfigPtr,
	                            FCriticalSection* InSourceCriticalSection,
	                            int32 InWidth, int32 InHeight)
		: ValuesPtr(InValuesPtr)
		  , ConfigPtr(InConfigPtr)
		  , Width(InWidth)
		  , Height(InHeight)
		  , SourceCriticalSection(InSourceCriticalSection)
	{
		// 后台线程只负责 CPU 侧预处理：把原始采样转换成 shader 可直接消费的线段/bucket buffer。
		WorkEvent = FPlatformProcess::GetSynchEventFromPool(false);
		Thread = FRunnableThread::Create(this, TEXT("IComputerShader_CurveProcessWorker"), 0, TPri_BelowNormal);
	}

	virtual ~FComputerCurveProcessWorker() override
	{
		Shutdown();
	}

	virtual uint32 Run() override
	{
		uint64 LastProcessedRequestCount = 0;

		while (!bStopRequested)
		{
			WorkEvent->Wait();
			if (bStopRequested)
			{
				break;
			}

			const uint64 CurrentRequestCount = WorkRequestCounter.load(std::memory_order_relaxed);
			if (CurrentRequestCount == LastProcessedRequestCount)
			{
				continue;
			}
			LastProcessedRequestCount = CurrentRequestCount;

			TArray<TArray<float>> ValuesSnapshot;
			FIComputerCurveRenderConfig ConfigSnapshot;
			bool bHasInput = false;

			if (ValuesPtr && ConfigPtr && SourceCriticalSection)
			{
				TRACE_CPUPROFILER_EVENT_SCOPE_STR("CurveProcessWorker::SnapshotInput");
				// 只在拷贝输入时加锁；真正耗时的 M4 降采样/分桶在锁外做，避免卡住 game thread 写入新波形。
				FScopeLock Lock(SourceCriticalSection);
				if (ValuesPtr->Num() > 0)
				{
					ValuesSnapshot = *ValuesPtr;
					ConfigSnapshot = *ConfigPtr;
					bHasInput = true;
				}
			}
			if (bHasInput)
			{
				// BuildProcessedCurveData 是纯 CPU 工作：降采样、生成线段、按屏幕 x 分桶，不接触 UObject/RHI。
				FIComputerProcessedCurveData Result = BuildProcessedCurveData(
					ValuesSnapshot,
					ConfigSnapshot,
					Width,
					Height
				);

				{
					// 只保留最新一帧结果；如果 game thread 来不及上传，旧结果会被新结果覆盖。
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

	void RequestWork()
	{
		WorkRequestCounter.fetch_add(1, std::memory_order_relaxed);
		if (WorkEvent)
		{
			WorkEvent->Trigger();
		}
	}

	bool DequeueResult(FIComputerProcessedCurveData& OutResult)
	{
		FScopeLock Lock(&ResultCriticalSection);
		if (!bHasCompletedResult)
		{
			return false;
		}

		// game thread 在 UploadProcessedCurveDataToGPU 开头取走结果，然后再进入渲染线程上传。
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
	const TArray<TArray<float>>* ValuesPtr = nullptr;
	const FIComputerCurveRenderConfig* ConfigPtr = nullptr;
	int32 Width = 0;
	int32 Height = 0;

	FRunnableThread* Thread = nullptr;
	FEvent* WorkEvent = nullptr;
	FThreadSafeBool bStopRequested = false;
	std::atomic<uint64> WorkRequestCounter{0};

	FCriticalSection ResultCriticalSection;
	FIComputerProcessedCurveData CompletedResult;
	FCriticalSection* SourceCriticalSection = nullptr;
	bool bHasCompletedResult = false;
};

AIComputerShaderObj::AIComputerShaderObj()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
}

TArray<FCurveSegmentGPU>& AIComputerShaderObj::GetWritableLineDataBuffer()
{
	// LineDataBuffers 是按需创建的三缓冲槽。每个槽保存一整帧 LineData，
	// 后续可能被 render command 通过 SharedPtr 持有，所以不能随便覆盖。
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
		// 可写条件有两个：
		// 1. 不是 ready 槽，避免覆盖“已经写完但还没上传”的数据。
		// 2. fence 已完成，说明 render thread 不再读取这个槽。
		return BufferIndex != LineDataReadyBufferIndex && LineDataUploadFences[BufferIndex].IsFenceComplete();
	};

	// 优先沿用当前 WriteIndex；如果它还被 ready 或 render thread 占着，就在另外两个槽里找一个空闲槽。
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

	// 三个槽都不可写时，当前实现选择等待一个非 ready 槽完成，保证数据正确但可能造成 game thread 卡顿。
	// 如果要进一步优化实时波形，可以把这里改成“拿不到可写槽就丢掉本帧结果”。
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
			LineDataUploadFences[WaitBufferIndex].Wait();
		}

		LineDataWriteBufferIndex = WaitBufferIndex;
	}

	return EnsureBuffer(LineDataBuffers[LineDataWriteBufferIndex]);
}

void AIComputerShaderObj::MarkLineDataReadyForUpload()
{
	// 当前 WriteIndex 已经填入一帧完整 LineData：
	// - 先把它发布成 ready 槽，等待 UploadProcessedCurveDataToGPU 取走。
	// - 再把 WriteIndex 推到下一个槽，为下一帧 CPU 结果预留写入位置。
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

	if (ProcessedData.Width != Width || ProcessedData.Height != Height)
	{
		return false;
	}

	ApplyProcessedCurveData(MoveTemp(ProcessedData));
	return true;
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
}

void AIComputerShaderObj::BeginPlay()
{
	Super::BeginPlay();

	if (!RenderTarget)
	{
		CreateRenderTarget(RenderTargetWidth, RenderTargetHeight);
	}

	SimulatedRunningPhase = SimulatedSinOffset;
	SetMultiSinWaveData(SimulatedRunningPhase, SimulatedSinCoefficient, SimulatedCurvePhaseStep);

	WorkerWidth = RenderTarget ? RenderTarget->SizeX : RenderTargetWidth;
	WorkerHeight = RenderTarget ? RenderTarget->SizeY : RenderTargetHeight;

	if (!CurveProcessWorker)
	{
		CurveProcessWorker = new FComputerCurveProcessWorker(&SimulatedCurveValues, &RenderConfig,
		                                                     &SimulatedCurveValuesCriticalSection, WorkerWidth,
		                                                     WorkerHeight);
	}
	CurveProcessWorker->RequestWork();
}

void AIComputerShaderObj::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	UploadTickAccumulatorSeconds += DeltaSeconds;

	//30hz运行速度
	if (UploadTickAccumulatorSeconds >= 0.033f)
	{
		UploadTickAccumulatorSeconds -= 0.033f;
		// SetMultiSinWaveData 的 offset 是弧度；持续推进相位后，CPU 采样数据会变化，后台 worker 会处理出新的线段。
		SimulatedRunningPhase = FMath::Fmod(SimulatedRunningPhase + DeltaSeconds * SimulatedScrollSpeed, 2.0f * PI);
		SetMultiSinWaveData(SimulatedRunningPhase, SimulatedSinCoefficient, SimulatedCurvePhaseStep);

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
	{
		FScopeLock Lock(&SimulatedCurveValuesCriticalSection);
		RenderConfig = InConfig;
	}

	if (CurveProcessWorker)
	{
		CurveProcessWorker->RequestWork();
	}
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

	TryApplyWorkerCurveProcessResult(Width, Height);

	TSharedPtr<TArray<FCurveSegmentGPU>, ESPMode::ThreadSafe> LineDataUploadBuffer;
	int32 LineDataUploadBufferIndex = INDEX_NONE;
	{
		// 这里消费 ready 槽：如果没有待上传 LineData，或者 ready 槽还没创建，就没有必要 dispatch。
		if (!bHasPendingLineDataUpload || LineDataReadyBufferIndex == INDEX_NONE ||
			!LineDataBuffers[LineDataReadyBufferIndex].IsValid())
		{
			return;
		}

		// 不复制 LineData 大数组，只拿 ready 槽的 SharedPtr。
		// 这个 SharedPtr 会被 render command 捕获，保证 render thread 执行前数组不会被释放。
		LineDataUploadBufferIndex = LineDataReadyBufferIndex;
		LineDataUploadBuffer = LineDataBuffers[LineDataUploadBufferIndex];
		if (LineDataUploadBuffer->Num() == 0)
		{
			LineDataUploadBuffer->AddZeroed(1);
		}

		// ready 槽已经交给本次上传流程；从 game thread 视角看，当前没有新的 ready 槽。
		// 但这个槽还不能马上复用，复用要等下面 BeginFence 对应的 fence 完成。
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
		// LineDrawDesc/BucketRanges/CurveColors 都已经复制成局部变量并 move 捕获。
		// LineDataUploadBuffer 则是三缓冲槽的 SharedPtr，避免在 game thread 再深拷贝大数组。
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
					TRACE_CPUPROFILER_EVENT_SCOPE_STR(
						"AIComputerShaderObj::UploadProcessedCurveDataToGPU_RenderThread.UploadLineData");
					// render thread 在这里读取被捕获的三缓冲槽，并把线段数据上传成 RDG StructuredBuffer。
					// 直到对应 fence 完成前，game thread 都不能覆盖这个槽。
					const TArray<FCurveSegmentGPU>& LineDataUpload = *LineDataUploadBuffer;
					FRDGBufferRef LineDataBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("LineDataBuffer"),
					                                                      sizeof(FCurveSegmentGPU),
					                                                      LineDataUpload.Num(),
					                                                      LineDataUpload.GetData(),
					                                                      sizeof(FCurveSegmentGPU) * LineDataUpload.
					                                                      Num());
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
		// 标记本次 render command 对该槽的使用范围。之后 IsFenceComplete() 返回 true，
		// 才表示这个 LineDataBuffers[LineDataUploadBufferIndex] 可以重新作为 write 槽。
		LineDataUploadFences[LineDataUploadBufferIndex].BeginFence();
	}
}

void AIComputerShaderObj::SetMultiSinWaveData(float offset, float coefficient, float curvePhaseStep)
{
	{
		FScopeLock Lock(&SimulatedCurveValuesCriticalSection);

		// 记录最近一次模拟参数，Tick 推进相位时会沿用它们继续生成后续帧。
		SimulatedRunningPhase = offset;
		SimulatedSinCoefficient = coefficient;
		SimulatedCurvePhaseStep = curvePhaseStep;

		const int32 CurveCount = FMath::Max(1, RenderConfig.CurveCount);
		const int32 SampleCount = FMath::Max(2, RenderConfig.SampleCount);

		// 临时 sin 模拟数据直接复用 SimulatedCurveValues 的已有容量，避免每帧创建/搬移 NewCurveValues。
		// 这里必须持锁写完整帧，防止 worker snapshot 到写了一半的曲线数据。
		SimulatedCurveValues.SetNum(CurveCount, false);

		for (int32 CurveIndex = 0; CurveIndex < CurveCount; ++CurveIndex)
		{
			TArray<float>& CurveSamples = SimulatedCurveValues[CurveIndex];
			CurveSamples.SetNumUninitialized(SampleCount, false);

			const float CurveOffset = offset + static_cast<float>(CurveIndex) * curvePhaseStep;
			for (int32 SourceIndex = 0; SourceIndex < SampleCount; ++SourceIndex)
			{
				CurveSamples[SourceIndex] =
					FMath::Sin(FMath::DegreesToRadians(SourceIndex * coefficient) + CurveOffset);
			}
		}
	}

	if (CurveProcessWorker)
	{
		CurveProcessWorker->RequestWork();
	}
}

bool AIComputerShaderObj::ProcessCurveData(const TArray<TArray<float>>& Values)
{
	const int32 Width = RenderTarget ? RenderTarget->SizeX : 1024;
	const int32 Height = RenderTarget ? RenderTarget->SizeY : 1024;
	FIComputerProcessedCurveData ProcessedData = BuildProcessedCurveData(
		Values,
		RenderConfig,
		Width,
		Height
	);
	const bool bAcceptedInput = ProcessedData.bAcceptedInput;
	ApplyProcessedCurveData(MoveTemp(ProcessedData));
	return bAcceptedInput;
}
