#include "ExtremeValueSampleActor.h"

#include "ExtremeValueSampleShader.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/CriticalSection.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/ThreadSafeBool.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/RealtimeGPUProfiler.h"
#include "RenderGraphEvent.h"
#include "RenderGraphUtils.h"

class FExtremeValueSampleShader;

DECLARE_GPU_STAT_NAMED(ExtremeValueSampleShaderDispatch, TEXT("ExtremeValueSampleShader Dispatch"));

namespace
{
	constexpr int32 GLineDrawDescFloatCount = 4;
	constexpr int32 GBucketRangeUintCount = 2;
	constexpr int32 GLegacySourceCount = 5000;
	constexpr float GLegacySinAmplitude = 1000.0f;
	constexpr float GBinningExpand = 2.0f;
	constexpr int32 GTileHeight = 64;

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

	struct FExtremeValueCurveProcessInput
	{
		uint64 RequestId = 0;
		FExtremeValueCurveRenderConfig RenderConfig;
		TArray<TArray<float>> Values;
	};

	struct FSineSampleGenerator
	{
		explicit FSineSampleGenerator(float InOffset, float InCoefficientDegrees)
			: Offset(InOffset)
			  , CoefficientDegrees(InCoefficientDegrees)
		{
			const float StepRadians = FMath::DegreesToRadians(InCoefficientDegrees);
			FMath::SinCos(&StepSin, &StepCos, StepRadians);
			FMath::SinCos(&CurrentSin, &CurrentCos, Offset);
		}

		float GetSample(int32 SourceIndex)
		{
			if (SourceIndex == CachedIndex)
			{
				return CurrentSin;
			}

			if (SourceIndex == CachedIndex + 1)
			{
				Advance();
				return CurrentSin;
			}

			const float Angle = Offset + FMath::DegreesToRadians(static_cast<float>(SourceIndex) * CoefficientDegrees);
			FMath::SinCos(&CurrentSin, &CurrentCos, Angle);
			CachedIndex = SourceIndex;
			return CurrentSin;
		}

	private:
		void Advance()
		{
			const float NextSin = CurrentSin * StepCos + CurrentCos * StepSin;
			const float NextCos = CurrentCos * StepCos - CurrentSin * StepSin;
			CurrentSin = NextSin;
			CurrentCos = NextCos;
			++CachedIndex;

			if ((CachedIndex & 1023) == 0)
			{
				const float InvLength = FMath::InvSqrt(FMath::Max(CurrentSin * CurrentSin + CurrentCos * CurrentCos, SMALL_NUMBER));
				CurrentSin *= InvLength;
				CurrentCos *= InvLength;
			}
		}

		float Offset = 0.0f;
		float CoefficientDegrees = 0.0f;
		float StepSin = 0.0f;
		float StepCos = 1.0f;
		float CurrentSin = 0.0f;
		float CurrentCos = 1.0f;
		int32 CachedIndex = 0;
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
	void BuildCurveColors(const FExtremeValueCurveRenderConfig& Config, TArray<FLinearColor>& OutCurveColors)
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
		OutLineDrawDesc[2] = static_cast<float>(FMath::DivideAndRoundUp(FMath::Max(1, Height), GTileHeight));
		OutLineDrawDesc[3] = static_cast<float>(FMath::Max(1, CurveCount));
	}

	// Creates safe non-empty buffers for initialization and invalid input cases.
	void ResetCurveBuffers(int32 Width, int32 Height, const FExtremeValueCurveRenderConfig& Config,
	                       TArray<float>& OutLineDrawDesc, TArray<FCurveSegmentGPU>& OutLineData,
	                       TArray<uint32>& OutBucketRanges, TArray<FLinearColor>& OutCurveColors)
	{
		const int32 SafeWidth = FMath::Max(1, Width);
		const int32 SafeHeight = FMath::Max(1, Height);
		const int32 CurveCount = FMath::Max(1, Config.CurveCount);

		UpdateLineDrawDesc(OutLineDrawDesc, SafeWidth, SafeHeight, CurveCount);

		OutLineData.Reset(1);
		OutLineData.AddZeroed(1);

		OutBucketRanges.SetNumZeroed(SafeWidth * FMath::DivideAndRoundUp(SafeHeight, GTileHeight) * GBucketRangeUintCount);
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

	// Builds final GPU buffers with 2D (x, yTile) binning.
	// BucketRanges[(x * TileCountY + yTile) * 2 + 0/1] = offset, count in OutLineData.
	void BuildBucketRangesAndLineData(const TArray<FBinnedCurveSegment>& BinnedSegments,
	                                  const TArray<int32>& BucketCounts, int32 Height,
	                                  TArray<uint32>& OutBucketRanges,
	                                  TArray<FCurveSegmentGPU>& OutLineData)
	{
		const int32 Width = BucketCounts.Num();
		const int32 TileCountY = FMath::DivideAndRoundUp(FMath::Max(1, Height), GTileHeight);
		const int32 TotalCells = Width * TileCountY;
		const float TileExpand = GBinningExpand + 1.5f;

		// First pass: count segments per (x, yTile) cell.
		TArray<int32> CellCounts;
		CellCounts.SetNumZeroed(TotalCells);

		for (const FBinnedCurveSegment& BinnedSegment : BinnedSegments)
		{
			const FCurveSegment& Segment = BinnedSegment.Segment;
			const float SegMinY = FMath::Min(Segment.Y0, Segment.Y1) - TileExpand;
			const float SegMaxY = FMath::Max(Segment.Y0, Segment.Y1) + TileExpand;
			const int32 StartTile = FMath::Clamp(FMath::FloorToInt(SegMinY / static_cast<float>(GTileHeight)), 0, TileCountY - 1);
			const int32 EndTile = FMath::Clamp(FMath::FloorToInt(SegMaxY / static_cast<float>(GTileHeight)), 0, TileCountY - 1);

			for (int32 BucketIndex = BinnedSegment.StartBucket; BucketIndex <= BinnedSegment.EndBucket; ++BucketIndex)
			{
				for (int32 TileIndex = StartTile; TileIndex <= EndTile; ++TileIndex)
				{
					++CellCounts[BucketIndex * TileCountY + TileIndex];
				}
			}
		}

		// Build offsets
		OutBucketRanges.SetNumUninitialized(TotalCells * GBucketRangeUintCount);
		TArray<uint32> CellWriteOffsets;
		CellWriteOffsets.SetNumUninitialized(TotalCells);
		int32 TotalSegmentCount = 0;

		for (int32 CellIndex = 0; CellIndex < TotalCells; ++CellIndex)
		{
			OutBucketRanges[CellIndex * GBucketRangeUintCount] = static_cast<uint32>(TotalSegmentCount);
			OutBucketRanges[CellIndex * GBucketRangeUintCount + 1] = static_cast<uint32>(CellCounts[CellIndex]);
			CellWriteOffsets[CellIndex] = static_cast<uint32>(TotalSegmentCount);
			TotalSegmentCount += CellCounts[CellIndex];
		}

		if (TotalSegmentCount == 0)
		{
			OutLineData.Reset(1);
			OutLineData.AddZeroed(1);
			return;
		}

		// Scatter segments into cells
		OutLineData.SetNumUninitialized(TotalSegmentCount);

		for (const FBinnedCurveSegment& BinnedSegment : BinnedSegments)
		{
			const FCurveSegment& Segment = BinnedSegment.Segment;
			const float SegMinY = FMath::Min(Segment.Y0, Segment.Y1) - TileExpand;
			const float SegMaxY = FMath::Max(Segment.Y0, Segment.Y1) + TileExpand;
			const int32 StartTile = FMath::Clamp(FMath::FloorToInt(SegMinY / static_cast<float>(GTileHeight)), 0, TileCountY - 1);
			const int32 EndTile = FMath::Clamp(FMath::FloorToInt(SegMaxY / static_cast<float>(GTileHeight)), 0, TileCountY - 1);

			for (int32 BucketIndex = BinnedSegment.StartBucket; BucketIndex <= BinnedSegment.EndBucket; ++BucketIndex)
			{
				for (int32 TileIndex = StartTile; TileIndex <= EndTile; ++TileIndex)
				{
					const int32 CellIndex = BucketIndex * TileCountY + TileIndex;
					FCurveSegmentGPU& Out = OutLineData[CellWriteOffsets[CellIndex]++];
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
	}

	FExtremeValueProcessedCurveData BuildProcessedCurveData(const TArray<TArray<float>>& Values,
	                                                     const FExtremeValueCurveRenderConfig& RenderConfig,
	                                                     int32 Width, int32 Height)
	{
		FExtremeValueProcessedCurveData ProcessedData;
		ProcessedData.Width = Width;
		ProcessedData.Height = Height;

		const int32 CurveCount = Values.Num();
		const int32 SampleCount = FMath::Max(2, RenderConfig.SampleCount);

		if (CurveCount <= 0)
		{
			FExtremeValueCurveRenderConfig ConfigForFrame = RenderConfig;
			ConfigForFrame.CurveCount = 1;
			ResetCurveBuffers(Width, Height, ConfigForFrame, ProcessedData.LineDrawDesc, ProcessedData.LineData,
			                  ProcessedData.BucketRanges, ProcessedData.CurveColors);
			return ProcessedData;
		}

		for (const TArray<float>& CurveSamples : Values)
		{
			if (CurveSamples.Num() < SampleCount)
			{
				FExtremeValueCurveRenderConfig ConfigForFrame = RenderConfig;
				ConfigForFrame.CurveCount = CurveCount;
				ResetCurveBuffers(Width, Height, ConfigForFrame, ProcessedData.LineDrawDesc, ProcessedData.LineData,
				                  ProcessedData.BucketRanges, ProcessedData.CurveColors);
				return ProcessedData;
			}
		}

		TRACE_CPUPROFILER_EVENT_SCOPE_STR("AExtremeValueSampleActor::ProcessCurveData");

		const int32 SafeCurveCount = FMath::Max(1, CurveCount);
		const int32 SafeSampleCount = FMath::Max(2, SampleCount);

		FExtremeValueCurveRenderConfig ConfigForFrame = RenderConfig;
		ConfigForFrame.CurveCount = SafeCurveCount;
		ConfigForFrame.SampleCount = SafeSampleCount;

		UpdateLineDrawDesc(ProcessedData.LineDrawDesc, Width, Height, SafeCurveCount);
		BuildCurveColors(ConfigForFrame, ProcessedData.CurveColors);

		TArray<FBinnedCurveSegment> BinnedSegments;
		TArray<int32> BucketCounts;
		{
			TRACE_CPUPROFILER_EVENT_SCOPE_STR("AExtremeValueSampleActor::ProcessCurveData.InitBuckets");
			const int64 EstimatedSegmentCount = static_cast<int64>(SafeCurveCount) * static_cast<int64>(Width) * 4;
			BinnedSegments.Reserve(static_cast<int32>(FMath::Min<int64>(EstimatedSegmentCount, MAX_int32)));
			BucketCounts.SetNumZeroed(FMath::Max(1, Width));
		}

		{
			TRACE_CPUPROFILER_EVENT_SCOPE_STR("AExtremeValueSampleActor::ProcessCurveData.BuildBuckets");
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
			TRACE_CPUPROFILER_EVENT_SCOPE_STR("AExtremeValueSampleActor::ProcessCurveData.BuildGPUData");
			BuildBucketRangesAndLineData(BinnedSegments, BucketCounts, Height, ProcessedData.BucketRanges,
			                             ProcessedData.LineData);
		}

		ProcessedData.bAcceptedInput = true;
		return ProcessedData;
	}

	TArray<TArray<float>> BuildSimulatedCurveSamples(const FExtremeValueCurveRenderConfig& RenderConfig,
	                                                 float Offset, float Coefficient, float CurvePhaseStep)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE_STR("AExtremeValueSampleActor::BuildSimulatedCurveSamples");

		const int32 SafeCurveCount = FMath::Max(1, RenderConfig.CurveCount);
		const int32 SafeSampleCount = FMath::Max(2, RenderConfig.SampleCount);

		TArray<TArray<float>> Values;
		Values.SetNum(SafeCurveCount);

		for (int32 CurveIndex = 0; CurveIndex < SafeCurveCount; ++CurveIndex)
		{
			const float CurveOffset = Offset + static_cast<float>(CurveIndex) * CurvePhaseStep;
			FSineSampleGenerator SineGenerator(CurveOffset, Coefficient);
			TArray<float>& CurveSamples = Values[CurveIndex];
			CurveSamples.SetNumUninitialized(SafeSampleCount);
			for (int32 SourceIndex = 0; SourceIndex < SafeSampleCount; ++SourceIndex)
			{
				CurveSamples[SourceIndex] = SineGenerator.GetSample(SourceIndex);
			}
		}

		return Values;
	}

	// Legacy SetSinWaveData defaults: one 5000-sample curve, caller-provided baseline, default color.
	FExtremeValueCurveRenderConfig MakeLegacyConfig(float BaseLineHeight)
	{
		FExtremeValueCurveRenderConfig Config;
		Config.CurveCount = 1;
		Config.SampleCount = GLegacySourceCount;
		Config.BaseLineStart = BaseLineHeight;
		Config.BaseLineStep = 0.0f;
		Config.ValueScale = GLegacySinAmplitude;
		Config.CurveColors.Add(GetDefaultCurveColor(0));
		return Config;
	}
}

class FExtremeValueCurveProcessWorker final : public FRunnable
{
public:
	FExtremeValueCurveProcessWorker(int32 InWidth, int32 InHeight)
		: Width(InWidth)
		  , Height(InHeight)
	{
		// 后台线程只负责 CPU 侧预处理：把外部准备好的 raw samples 转换成 shader 可直接消费的线段/bucket buffer。
		WorkEvent = FPlatformProcess::GetSynchEventFromPool(false);
		Thread = FRunnableThread::Create(this, TEXT("ExtremeValueSampleShader_CurveProcessWorker"), 0, TPri_BelowNormal);
	}

	virtual ~FExtremeValueCurveProcessWorker() override
	{
		Shutdown();
	}

	virtual uint32 Run() override
	{
		while (!bStopRequested)
		{
			WorkEvent->Wait();
			if (bStopRequested)
			{
				break;
			}

			FExtremeValueCurveProcessInput Input;
			bool bHasInput = false;

			{
				TRACE_CPUPROFILER_EVENT_SCOPE_STR("CurveProcessWorker::DequeueRawSamples");
				FScopeLock Lock(&InputCriticalSection);
				if (bHasPendingInput)
				{
					Input = MoveTemp(PendingInput);
					bHasPendingInput = false;
					bHasInput = true;
				}
			}

			if (bHasInput)
			{
				// BuildProcessedCurveData 是纯 CPU 工作：M4 极值采样、生成线段、按屏幕 x/y tile 分桶，不接触 UObject/RHI。
				FExtremeValueProcessedCurveData Result = BuildProcessedCurveData(
					Input.Values,
					Input.RenderConfig,
					Width,
					Height
				);

				{
					// 只保留最新一帧结果；如果 game thread 来不及上传，旧结果会被新结果覆盖。
					FScopeLock Lock(&ResultCriticalSection);
					CompletedResult = MoveTemp(Result);
					CompletedResultRequestId = Input.RequestId;
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

	void RequestWork(FExtremeValueCurveProcessInput&& Input)
	{
		{
			FScopeLock Lock(&InputCriticalSection);
			PendingInput = MoveTemp(Input);
			bHasPendingInput = true;
		}

		if (WorkEvent)
		{
			WorkEvent->Trigger();
		}
	}

	bool DequeueResult(FExtremeValueProcessedCurveData& OutResult, uint64& OutRequestId)
	{
		FScopeLock Lock(&ResultCriticalSection);
		if (!bHasCompletedResult)
		{
			return false;
		}

		// game thread 在 UploadProcessedCurveDataToGPU 开头取走结果，然后再进入渲染线程上传。
		OutResult = MoveTemp(CompletedResult);
		OutRequestId = CompletedResultRequestId;
		bHasCompletedResult = false;
		CompletedResultRequestId = 0;
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
	int32 Width = 0;
	int32 Height = 0;

	FRunnableThread* Thread = nullptr;
	FEvent* WorkEvent = nullptr;
	FThreadSafeBool bStopRequested = false;

	FCriticalSection InputCriticalSection;
	FExtremeValueCurveProcessInput PendingInput;
	bool bHasPendingInput = false;

	FCriticalSection ResultCriticalSection;
	FExtremeValueProcessedCurveData CompletedResult;
	uint64 CompletedResultRequestId = 0;
	bool bHasCompletedResult = false;
};

AExtremeValueSampleActor::AExtremeValueSampleActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
}

TArray<FCurveSegmentGPU>* AExtremeValueSampleActor::TryGetWritableLineDataBuffer()
{
	// LineDataBuffers 是按需创建的三缓冲槽。每个槽保存一整帧 LineData，
	// 后续可能被 render command 通过 SharedPtr 持有，所以不能随便覆盖。
	auto EnsureBuffer = [](TSharedPtr<TArray<FCurveSegmentGPU>, ESPMode::ThreadSafe>& Buffer)
		-> TArray<FCurveSegmentGPU>*
	{
		if (!Buffer.IsValid())
		{
			Buffer = MakeShared<TArray<FCurveSegmentGPU>, ESPMode::ThreadSafe>();
		}

		return Buffer.Get();
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

	// 三个槽都不可写时，丢掉本帧 CPU 结果，避免 Game Thread 等待 render thread fence。
	if (!IsBufferWritable(LineDataWriteBufferIndex))
	{
		TRACE_CPUPROFILER_EVENT_SCOPE_STR("AExtremeValueSampleActor::TryGetWritableLineDataBuffer.DropFrame");
		return nullptr;
	}

	return EnsureBuffer(LineDataBuffers[LineDataWriteBufferIndex]);
}

void AExtremeValueSampleActor::MarkLineDataReadyForUpload()
{
	// 当前 WriteIndex 已经填入一帧完整 LineData：
	// - 先把它发布成 ready 槽，等待 UploadProcessedCurveDataToGPU 取走。
	// - 再把 WriteIndex 推到下一个槽，为下一帧 CPU 结果预留写入位置。
	bHasPendingLineDataUpload = true;
	LineDataReadyBufferIndex = LineDataWriteBufferIndex;
	LineDataWriteBufferIndex = (LineDataWriteBufferIndex + 1) % LineDataUploadBufferCount;
}

void AExtremeValueSampleActor::ResetCurveDataToSafeBuffers(int32 Width, int32 Height, int32 CurveCount)
{
	FExtremeValueCurveRenderConfig ConfigForFrame = RenderConfig;
	ConfigForFrame.CurveCount = FMath::Max(1, CurveCount);

	if (TArray<FCurveSegmentGPU>* WritableLineData = TryGetWritableLineDataBuffer())
	{
		ResetCurveBuffers(Width, Height, ConfigForFrame, LineDrawDesc, *WritableLineData, BucketRanges, CurveColors);
		MarkLineDataReadyForUpload();
	}
}

bool AExtremeValueSampleActor::ApplyProcessedCurveData(FExtremeValueProcessedCurveData&& ProcessedData)
{
	TArray<FCurveSegmentGPU>* WritableLineData = TryGetWritableLineDataBuffer();
	if (!WritableLineData)
	{
		return false;
	}

	LineDrawDesc = MoveTemp(ProcessedData.LineDrawDesc);
	BucketRanges = MoveTemp(ProcessedData.BucketRanges);
	CurveColors = MoveTemp(ProcessedData.CurveColors);

	*WritableLineData = MoveTemp(ProcessedData.LineData);
	if (WritableLineData->Num() == 0)
	{
		WritableLineData->AddZeroed(1);
	}

	MarkLineDataReadyForUpload();
	return true;
}

bool AExtremeValueSampleActor::TryApplyWorkerCurveProcessResult(int32 Width, int32 Height)
{
	if (!CurveProcessWorker)
	{
		return false;
	}

	FExtremeValueProcessedCurveData ProcessedData;
	uint64 ResultRequestId = 0;
	if (!CurveProcessWorker->DequeueResult(ProcessedData, ResultRequestId))
	{
		return false;
	}

	if (ResultRequestId <= LastAcceptedCurveProcessRequestId)
	{
		return false;
	}

	if (ProcessedData.Width != Width || ProcessedData.Height != Height)
	{
		return false;
	}

	const bool bApplied = ApplyProcessedCurveData(MoveTemp(ProcessedData));
	if (bApplied)
	{
		LastAcceptedCurveProcessRequestId = ResultRequestId;
	}
	return bApplied;
}

bool AExtremeValueSampleActor::ProcessCurveDataOnGameThread(const TArray<TArray<float>>& Values,
                                                       const FExtremeValueCurveRenderConfig& ConfigSnapshot)
{
	const int32 Width = RenderTarget ? RenderTarget->SizeX : RenderTargetWidth;
	const int32 Height = RenderTarget ? RenderTarget->SizeY : RenderTargetHeight;
	FExtremeValueProcessedCurveData ProcessedData = BuildProcessedCurveData(
		Values,
		ConfigSnapshot,
		Width,
		Height
	);

	const bool bAcceptedInput = ProcessedData.bAcceptedInput;
	return bAcceptedInput && ApplyProcessedCurveData(MoveTemp(ProcessedData));
}

void AExtremeValueSampleActor::QueueCurveDataForWorker(TArray<TArray<float>>&& Values,
                                                  const FExtremeValueCurveRenderConfig& ConfigSnapshot)
{
	const uint64 RequestId = ++NextCurveProcessRequestId;

	if (!CurveProcessWorker)
	{
		const bool bApplied = ProcessCurveDataOnGameThread(Values, ConfigSnapshot);
		if (bApplied)
		{
			LastAcceptedCurveProcessRequestId = RequestId;
		}
		return;
	}

	FExtremeValueCurveProcessInput Input;
	Input.RequestId = RequestId;
	Input.RenderConfig = ConfigSnapshot;
	Input.Values = MoveTemp(Values);
	CurveProcessWorker->RequestWork(MoveTemp(Input));
}

void AExtremeValueSampleActor::QueueSimulatedCurveDataForWorker()
{
	FExtremeValueCurveRenderConfig ConfigSnapshot = RenderConfig;
	ConfigSnapshot.CurveCount = FMath::Max(1, ConfigSnapshot.CurveCount);
	ConfigSnapshot.SampleCount = FMath::Max(2, ConfigSnapshot.SampleCount);

	TArray<TArray<float>> SimulatedSamples = BuildSimulatedCurveSamples(
		ConfigSnapshot,
		SimulatedRunningPhase,
		SimulatedSinCoefficient,
		SimulatedCurvePhaseStep
	);

	QueueCurveDataForWorker(MoveTemp(SimulatedSamples), ConfigSnapshot);
}

void AExtremeValueSampleActor::ShutdownCurveProcessWorker()
{
	if (CurveProcessWorker)
	{
		delete CurveProcessWorker;
		CurveProcessWorker = nullptr;
	}
}

void AExtremeValueSampleActor::CreateRenderTarget(int32 Width, int32 Height)
{
	RenderTargetWidth = FMath::Max(1, Width);
	RenderTargetHeight = FMath::Max(1, Height);

	RenderTarget = NewObject<UTextureRenderTarget2D>(this);

	RenderTarget->bCanCreateUAV = true;
	RenderTarget->InitCustomFormat(RenderTargetWidth, RenderTargetHeight, PF_FloatRGBA, false);
	RenderTarget->UpdateResourceImmediate(true);

	ResetCurveDataToSafeBuffers(RenderTargetWidth, RenderTargetHeight, RenderConfig.CurveCount);
}

void AExtremeValueSampleActor::BeginPlay()
{
	Super::BeginPlay();

	if (!RenderTarget)
	{
		CreateRenderTarget(RenderTargetWidth, RenderTargetHeight);
	}

	SimulatedRunningPhase = SimulatedSinOffset;

	WorkerWidth = RenderTarget ? RenderTarget->SizeX : RenderTargetWidth;
	WorkerHeight = RenderTarget ? RenderTarget->SizeY : RenderTargetHeight;

	if (!CurveProcessWorker)
	{
		CurveProcessWorker = new FExtremeValueCurveProcessWorker(WorkerWidth, WorkerHeight);
	}

	SetMultiSinWaveData(SimulatedRunningPhase, SimulatedSinCoefficient, SimulatedCurvePhaseStep);
}

void AExtremeValueSampleActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	UploadTickAccumulatorSeconds += DeltaSeconds;

	if (UploadTickAccumulatorSeconds >= 0.033f)
	{
		if (bUseSimulatedCurveData)
		{
			//30hz运行速度
			SimulatedRunningPhase = FMath::Fmod(SimulatedRunningPhase + DeltaSeconds * SimulatedScrollSpeed, 2.0f * PI);
			SetMultiSinWaveData(SimulatedRunningPhase, SimulatedSinCoefficient, SimulatedCurvePhaseStep);
		}
		UploadTickAccumulatorSeconds -= 0.033f;
		UploadProcessedCurveDataToGPU();
	}
}

void AExtremeValueSampleActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ShutdownCurveProcessWorker();
	Super::EndPlay(EndPlayReason);
}

void AExtremeValueSampleActor::BeginDestroy()
{
	ShutdownCurveProcessWorker();
	Super::BeginDestroy();
}

UTextureRenderTarget2D* AExtremeValueSampleActor::GetRenderTarget() const
{
	return RenderTarget;
}

void AExtremeValueSampleActor::Execute()
{
	UploadProcessedCurveDataToGPU();
}

void AExtremeValueSampleActor::SetRenderConfig(const FExtremeValueCurveRenderConfig& InConfig)
{
	RenderConfig = InConfig;

	if (bUseSimulatedCurveData)
	{
		QueueSimulatedCurveDataForWorker();
	}
	else if (CachedExternalCurveSamples.Num() > 0)
	{
		const uint64 RequestId = ++NextCurveProcessRequestId;
		ProcessCurveDataOnGameThread(CachedExternalCurveSamples, RenderConfig);
		LastAcceptedCurveProcessRequestId = RequestId;
	}
}

FExtremeValueCurveRenderConfig AExtremeValueSampleActor::GetRenderConfig() const
{
	return RenderConfig;
}

void AExtremeValueSampleActor::UploadProcessedCurveDataToGPU()
{
	TRACE_CPUPROFILER_EVENT_SCOPE_STR("AExtremeValueSampleActor::UploadProcessedCurveDataToGPU");

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
		LineDrawDescCopy[2] = static_cast<float>(FMath::DivideAndRoundUp(FMath::Max(1, Height), GTileHeight));
	}

	TArray<uint32> BucketRangesCopy;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE_STR("AExtremeValueSampleActor::UploadProcessedCurveDataToGPU.CopyBucketRanges");
		BucketRangesCopy = BucketRanges;
		const int32 TileCountY = FMath::DivideAndRoundUp(FMath::Max(1, Height), GTileHeight);
		const int32 ExpectedBucketRangeCount = FMath::Max(1, Width) * TileCountY * GBucketRangeUintCount;
		if (BucketRangesCopy.Num() != ExpectedBucketRangeCount)
		{
			BucketRangesCopy.SetNumZeroed(ExpectedBucketRangeCount);
		}
	}

	TArray<FLinearColor> CurveColorsCopy;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE_STR("AExtremeValueSampleActor::UploadProcessedCurveDataToGPU.CopyCurveColors");
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
		ENQUEUE_RENDER_COMMAND(ExecuteExtremeValueSampleShader)(
			[RenderTargetResource, Width, Height, LineDrawDescCopy = MoveTemp(LineDrawDescCopy),
				LineDataUploadBuffer = MoveTemp(LineDataUploadBuffer),
				BucketRangesCopy = MoveTemp(BucketRangesCopy),
				CurveColorsCopy = MoveTemp(CurveColorsCopy)](
			FRHICommandListImmediate& RHICmdList)
			{
				TRACE_CPUPROFILER_EVENT_SCOPE_STR("AExtremeValueSampleActor::UploadProcessedCurveDataToGPU_RenderThread");

				FRDGBuilder GraphBuilder(RHICmdList);

				TShaderMapRef<FExtremeValueSampleShader> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

				FExtremeValueSampleShader::FParameters* PassParameters = nullptr;
				{
					PassParameters = GraphBuilder.AllocParameters<FExtremeValueSampleShader::FParameters>();
				}

				{
					FRDGTextureRef TargetTexture = RegisterExternalTexture(
						GraphBuilder,
						RenderTargetResource->GetRenderTargetTexture(),
						TEXT("ExtremeValueSampleShader_RenderTarget")
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
						"AExtremeValueSampleActor::UploadProcessedCurveDataToGPU_RenderThread.UploadLineData");
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
							FExtremeValueSampleShader::ThreadGroupSizeX,
							FExtremeValueSampleShader::ThreadGroupSizeY,
							FExtremeValueSampleShader::ThreadGroupSizeZ
						)
					);
				}

				{
					RDG_EVENT_SCOPE(GraphBuilder, "ExtremeValueSampleShader Dispatch");
					RDG_GPU_STAT_SCOPE(GraphBuilder, ExtremeValueSampleShaderDispatch);
					GraphBuilder.AddPass(
						RDG_EVENT_NAME("ExtremeValueSampleShader Dispatch"),
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

void AExtremeValueSampleActor::SetMultiSinWaveData(float offset, float coefficient, float curvePhaseStep)
{
	// 记录最近一次模拟参数，Tick 推进相位时会沿用它们继续生成后续帧。
	bUseSimulatedCurveData = true;
	CachedExternalCurveSamples.Reset();
	SimulatedRunningPhase = offset;
	SimulatedSinCoefficient = coefficient;
	SimulatedCurvePhaseStep = curvePhaseStep;

	QueueSimulatedCurveDataForWorker();
}

bool AExtremeValueSampleActor::ProcessCurveData(const TArray<TArray<float>>& Values)
{
	bUseSimulatedCurveData = false;
	CachedExternalCurveSamples = Values;

	const uint64 RequestId = ++NextCurveProcessRequestId;

	const bool bApplied = ProcessCurveDataOnGameThread(Values, RenderConfig);
	LastAcceptedCurveProcessRequestId = RequestId;
	return bApplied;
}
