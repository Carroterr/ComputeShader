#include "IComputerShaderObj.h"

#include "IComputerShader.h"
#include "Engine/TextureRenderTarget2D.h"
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

	// Maps a raw sample index into screen-space x so the shader only sees draw-ready line segments.
	float SourceIndexToScreenX(int32 SourceIndex, int32 SourceCount, int32 Width)
	{
		if (Width <= 1 || SourceCount <= 1)
		{
			return 0.5f;
		}

		return static_cast<float>(SourceIndex) * static_cast<float>(Width - 1) / static_cast<float>(SourceCount - 1) +
			0.5f;
	}

	// M4 can select the same source point more than once, so keep only one point per source index.
	void AddPointUnique(TArray<FReducedWavePoint>& Points, const FReducedWavePoint& Point)
	{
		for (const FReducedWavePoint& ExistingPoint : Points)
		{
			if (ExistingPoint.SourceIndex == Point.SourceIndex)
			{
				return;
			}
		}

		Points.Add(Point);
	}

	// Duplicates a segment into every x bucket it can affect, including a small AA expansion.
	void AddSegmentToBuckets(TArray<TArray<FCurveSegment>>& BucketSegments, int32 Width, uint32 CurveIndex,
	                         const FReducedWavePoint& Start, const FReducedWavePoint& End)
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
			BucketSegments[BucketIndex].Add(Segment);
		}
	}

	// Reduces one curve with M4 sampling and emits screen-space segments into x buckets.
	template <typename SampleFuncType>
	void AddCurveToBuckets(int32 CurveIndex, int32 SampleCount, int32 Width, float BaseLine, float ValueScale,
	                       SampleFuncType GetSample, TArray<TArray<FCurveSegment>>& BucketSegments)
	{
		if (SampleCount < 2 || Width <= 0)
		{
			return;
		}

		auto MakePoint = [SampleCount, Width, BaseLine, ValueScale, &GetSample](int32 SourceIndex)
		{
			const float SampleValue = GetSample(SourceIndex);
			return FReducedWavePoint{
				SourceIndex,
				SourceIndexToScreenX(SourceIndex, SampleCount, Width),
				BaseLine + SampleValue * ValueScale + 0.5f
			};
		};

		bool bHasPreviousPoint = false;
		FReducedWavePoint PreviousPoint;

		for (int32 BucketIndex = 0; BucketIndex < Width; ++BucketIndex)
		{
			const int32 BucketStart = FMath::FloorToInt(static_cast<float>(BucketIndex) * SampleCount / Width);
			const int32 BucketEnd = FMath::Max(
				BucketStart + 1,
				FMath::FloorToInt(static_cast<float>(BucketIndex + 1) * SampleCount / Width)
			);
			const int32 BucketLast = FMath::Min(BucketEnd - 1, SampleCount - 1);

			int32 MinIndex = BucketStart;
			int32 MaxIndex = BucketStart;
			float MinValue = GetSample(BucketStart);
			float MaxValue = MinValue;

			for (int32 SourceIndex = BucketStart + 1; SourceIndex < BucketEnd; ++SourceIndex)
			{
				const float Value = GetSample(SourceIndex);
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

			TArray<FReducedWavePoint> BucketPoints;
			BucketPoints.Reserve(4);
			AddPointUnique(BucketPoints, MakePoint(BucketStart));
			AddPointUnique(BucketPoints, MakePoint(MinIndex));
			AddPointUnique(BucketPoints, MakePoint(MaxIndex));
			AddPointUnique(BucketPoints, MakePoint(BucketLast));

			BucketPoints.Sort([](const FReducedWavePoint& Left, const FReducedWavePoint& Right)
			{
				return Left.SourceIndex < Right.SourceIndex;
			});

			if (BucketPoints.Num() == 0)
			{
				continue;
			}

			if (bHasPreviousPoint)
			{
				AddSegmentToBuckets(BucketSegments, Width, static_cast<uint32>(CurveIndex), PreviousPoint,
				                    BucketPoints[0]);
			}

			for (int32 PointIndex = 0; PointIndex + 1 < BucketPoints.Num(); ++PointIndex)
			{
				AddSegmentToBuckets(BucketSegments, Width, static_cast<uint32>(CurveIndex), BucketPoints[PointIndex],
				                    BucketPoints[PointIndex + 1]);
			}

			PreviousPoint = BucketPoints.Last();
			bHasPreviousPoint = true;
		}
	}

	// Flattens per-column buckets into GPU buffers: LineData (struct per segment) plus BucketRanges offset/count pairs.
	void FlattenBuckets(const TArray<TArray<FCurveSegment>>& BucketSegments, TArray<uint32>& OutBucketRanges,
	                    TArray<FCurveSegmentGPU>& OutLineData)
	{
		const int32 Width = BucketSegments.Num();
		int32 TotalSegmentCount = 0;
		for (const TArray<FCurveSegment>& Bucket : BucketSegments)
		{
			TotalSegmentCount += Bucket.Num();
		}

		OutBucketRanges.SetNumZeroed(Width * GBucketRangeUintCount);
		OutLineData.Reset(FMath::Max(1, TotalSegmentCount));

		for (int32 BucketIndex = 0; BucketIndex < Width; ++BucketIndex)
		{
			const TArray<FCurveSegment>& Bucket = BucketSegments[BucketIndex];
			const uint32 SegmentOffset = static_cast<uint32>(OutLineData.Num());

			OutBucketRanges[BucketIndex * GBucketRangeUintCount] = SegmentOffset;
			OutBucketRanges[BucketIndex * GBucketRangeUintCount + 1] = static_cast<uint32>(Bucket.Num());

			for (const FCurveSegment& Segment : Bucket)
			{
				FCurveSegmentGPU& Out = OutLineData.AddDefaulted_GetRef();
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

		if (OutLineData.Num() == 0)
		{
			OutLineData.AddZeroed(1);
		}
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

TArray<FCurveSegmentGPU>& UIComputerShaderObj::GetWritableLineDataBuffer()
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
			TRACE_CPUPROFILER_EVENT_SCOPE_STR("UIComputerShaderObj::ProcessCurveData.WaitForLineDataUploadBuffer");
			LineDataUploadFences[WaitBufferIndex].Wait();
		}

		LineDataWriteBufferIndex = WaitBufferIndex;
	}

	return EnsureBuffer(LineDataBuffers[LineDataWriteBufferIndex]);
}

void UIComputerShaderObj::MarkLineDataReadyForUpload()
{
	bHasPendingLineDataUpload = true;
	LineDataReadyBufferIndex = LineDataWriteBufferIndex;
	LineDataWriteBufferIndex = (LineDataWriteBufferIndex + 1) % LineDataUploadBufferCount;
}

void UIComputerShaderObj::CreateRenderTarget(int32 Width, int32 Height)
{
	RenderTarget = NewObject<UTextureRenderTarget2D>(this);

	RenderTarget->bCanCreateUAV = true;
	RenderTarget->InitCustomFormat(Width, Height, PF_FloatRGBA, false);
	RenderTarget->UpdateResourceImmediate(true);

	FIComputerCurveRenderConfig DefaultConfig;
	TArray<FCurveSegmentGPU>& WritableLineData = GetWritableLineDataBuffer();
	ResetCurveBuffers(Width, Height, DefaultConfig, LineDrawDesc, WritableLineData, BucketRanges, CurveColors);
	MarkLineDataReadyForUpload();
}

UTextureRenderTarget2D* UIComputerShaderObj::GetRenderTarget() const
{
	return RenderTarget;
}

void UIComputerShaderObj::Execute()
{
	UploadProcessedCurveDataToGPU();
}

void UIComputerShaderObj::UploadProcessedCurveDataToGPU()
{
	TRACE_CPUPROFILER_EVENT_SCOPE_STR("UIComputerShaderObj::UploadProcessedCurveDataToGPU");

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
			FIComputerCurveRenderConfig DefaultConfig;
			UpdateLineDrawDesc(LineDrawDescCopy, Width, Height, DefaultConfig.CurveCount);
		}
		LineDrawDescCopy[0] = static_cast<float>(Width);
		LineDrawDescCopy[1] = static_cast<float>(Height);
		LineDrawDescCopy[2] = 0.0f;
	}

	TArray<uint32> BucketRangesCopy;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE_STR("UIComputerShaderObj::UploadProcessedCurveDataToGPU.CopyBucketRanges");
		BucketRangesCopy = BucketRanges;
		const int32 ExpectedBucketRangeCount = FMath::Max(1, Width) * GBucketRangeUintCount;
		if (BucketRangesCopy.Num() != ExpectedBucketRangeCount)
		{
			BucketRangesCopy.SetNumZeroed(ExpectedBucketRangeCount);
		}
	}

	TArray<FLinearColor> CurveColorsCopy;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE_STR("UIComputerShaderObj::UploadProcessedCurveDataToGPU.CopyCurveColors");
		CurveColorsCopy = CurveColors;
		if (CurveColorsCopy.Num() == 0)
		{
			CurveColorsCopy.Add(GetDefaultCurveColor(0));
		}
		LineDrawDescCopy[3] = static_cast<float>(FMath::Max(1, CurveColorsCopy.Num()));
	}

	// Stage 2: copy processed CPU buffers to the render thread, upload them with RDG, then dispatch.
	{
		TRACE_CPUPROFILER_EVENT_SCOPE_STR("UIComputerShaderObj::UploadProcessedCurveDataToGPU.EnqueueRenderCommand");
		ENQUEUE_RENDER_COMMAND(ExecuteIComputerShader)(
			[RenderTargetResource, Width, Height, LineDrawDescCopy = MoveTemp(LineDrawDescCopy),
					LineDataUploadBuffer = MoveTemp(LineDataUploadBuffer),
					BucketRangesCopy = MoveTemp(BucketRangesCopy),
					CurveColorsCopy = MoveTemp(CurveColorsCopy)](
				FRHICommandListImmediate& RHICmdList)
			{
				TRACE_CPUPROFILER_EVENT_SCOPE_STR("UIComputerShaderObj::UploadProcessedCurveDataToGPU_RenderThread");

				FRDGBuilder GraphBuilder(RHICmdList);

				TShaderMapRef<FIComputerShader> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

				FIComputerShader::FParameters* PassParameters = nullptr;
				{
					TRACE_CPUPROFILER_EVENT_SCOPE_STR("UIComputerShaderObj::UploadProcessedCurveDataToGPU_RenderThread.AllocParameters");
					PassParameters = GraphBuilder.AllocParameters<FIComputerShader::FParameters>();
				}

				{
					TRACE_CPUPROFILER_EVENT_SCOPE_STR("UIComputerShaderObj::UploadProcessedCurveDataToGPU_RenderThread.RegisterTarget");
					FRDGTextureRef TargetTexture = RegisterExternalTexture(
						GraphBuilder,
						RenderTargetResource->GetRenderTargetTexture(),
						TEXT("IComputerShader_RenderTarget")
					);

					PassParameters->RenderTarget = GraphBuilder.CreateUAV(TargetTexture);
				}

				{
					TRACE_CPUPROFILER_EVENT_SCOPE_STR("UIComputerShaderObj::UploadProcessedCurveDataToGPU_RenderThread.UploadLineDrawDesc");
					FRDGBufferRef LineDrawDescBuffer = CreateUploadBuffer(GraphBuilder, TEXT("LineDrawDescBuffer"),
					                                                      sizeof(float), LineDrawDescCopy.Num(),
					                                                      LineDrawDescCopy.GetData(),
					                                                      sizeof(float) * LineDrawDescCopy.Num());
					PassParameters->LineDrawDesc = GraphBuilder.CreateSRV(
						FRDGBufferSRVDesc(LineDrawDescBuffer, PF_R32_FLOAT));
				}

				{
					TRACE_CPUPROFILER_EVENT_SCOPE_STR("UIComputerShaderObj::UploadProcessedCurveDataToGPU_RenderThread.UploadLineData");
					const TArray<FCurveSegmentGPU>& LineDataUpload = *LineDataUploadBuffer;
					FRDGBufferRef LineDataBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("LineDataBuffer"),
					                                                      sizeof(FCurveSegmentGPU), LineDataUpload.Num(),
					                                                      LineDataUpload.GetData(),
					                                                      sizeof(FCurveSegmentGPU) * LineDataUpload.Num());
					PassParameters->LineData = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(LineDataBuffer));
				}

				{
					TRACE_CPUPROFILER_EVENT_SCOPE_STR("UIComputerShaderObj::UploadProcessedCurveDataToGPU_RenderThread.UploadBucketRanges");
					FRDGBufferRef BucketRangesBuffer = CreateUploadBuffer(GraphBuilder, TEXT("BucketRangesBuffer"),
					                                                      sizeof(uint32), BucketRangesCopy.Num(),
					                                                      BucketRangesCopy.GetData(),
					                                                      sizeof(uint32) * BucketRangesCopy.Num());
					PassParameters->BucketRanges = GraphBuilder.CreateSRV(
						FRDGBufferSRVDesc(BucketRangesBuffer, PF_R32_UINT));
				}

				{
					TRACE_CPUPROFILER_EVENT_SCOPE_STR("UIComputerShaderObj::UploadProcessedCurveDataToGPU_RenderThread.UploadCurveColors");
					FRDGBufferRef CurveColorsBuffer = CreateUploadBuffer(GraphBuilder, TEXT("CurveColorsBuffer"),
					                                                     sizeof(FLinearColor), CurveColorsCopy.Num(),
					                                                     CurveColorsCopy.GetData(),
					                                                     sizeof(FLinearColor) * CurveColorsCopy.Num());
					PassParameters->CurveColors = GraphBuilder.CreateSRV(
						FRDGBufferSRVDesc(CurveColorsBuffer, PF_A32B32G32R32F));
				}

				FIntVector GroupCount(0, 0, 0);
				{
					TRACE_CPUPROFILER_EVENT_SCOPE_STR("UIComputerShaderObj::UploadProcessedCurveDataToGPU_RenderThread.GetGroupCount");
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
					TRACE_CPUPROFILER_EVENT_SCOPE_STR("UIComputerShaderObj::UploadProcessedCurveDataToGPU_RenderThread.AddDispatchPass");
					GraphBuilder.AddPass(
						RDG_EVENT_NAME("UIComputerShaderObj::UploadProcessedCurveDataToGPU.Dispatch"),
						PassParameters,
						ERDGPassFlags::AsyncCompute,
						[ComputeShader, PassParameters, GroupCount](FRHIComputeCommandList& RHICmdList)
						{
							TRACE_CPUPROFILER_EVENT_SCOPE_STR("UIComputerShaderObj::UploadProcessedCurveDataToGPU_RenderThread.DispatchRHI");
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
					TRACE_CPUPROFILER_EVENT_SCOPE_STR("UIComputerShaderObj::UploadProcessedCurveDataToGPU_RenderThread.GraphExecute");
					GraphBuilder.Execute();
				}
			}
		);
		LineDataUploadFences[LineDataUploadBufferIndex].BeginFence();
	}
}

void UIComputerShaderObj::SetSinWaveData(float offset, float coefficient, float baseLineHeight)
{
	SetMultiSinWaveData(offset, coefficient, 0.0f, MakeLegacyConfig(baseLineHeight));
}

void UIComputerShaderObj::SetMultiSinWaveData(float offset, float coefficient, float curvePhaseStep,
                                              const FIComputerCurveRenderConfig& config)
{
	const int32 CurveCount = FMath::Max(1, config.CurveCount);
	const int32 SampleCount = FMath::Max(2, config.SampleCount);

	TArray<float> Values;
	Values.SetNumUninitialized(CurveCount * SampleCount);

	for (int32 CurveIndex = 0; CurveIndex < CurveCount; ++CurveIndex)
	{
		const float CurveOffset = offset + static_cast<float>(CurveIndex) * curvePhaseStep;
		for (int32 SourceIndex = 0; SourceIndex < SampleCount; ++SourceIndex)
		{
			Values[CurveIndex * SampleCount + SourceIndex] =
				FMath::Sin(FMath::DegreesToRadians(SourceIndex * coefficient) + CurveOffset);
		}
	}

	ProcessCurveData(Values, config);
}

void UIComputerShaderObj::SetCurveData(const TArray<float>& values, const FIComputerCurveRenderConfig& config)
{
	ProcessCurveData(values, config);
}

bool UIComputerShaderObj::ProcessCurveData(const TArray<float>& values, const FIComputerCurveRenderConfig& config)
{
	TRACE_CPUPROFILER_EVENT_SCOPE_STR("UIComputerShaderObj::ProcessCurveData");

	const int32 Width = RenderTarget ? RenderTarget->SizeX : 1024;
	const int32 Height = RenderTarget ? RenderTarget->SizeY : 1024;
	const int32 CurveCount = FMath::Max(1, config.CurveCount);
	const int32 SampleCount = FMath::Max(2, config.SampleCount);
	const int64 RequiredValueCount = static_cast<int64>(CurveCount) * static_cast<int64>(SampleCount);

	if (values.Num() < RequiredValueCount)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE_STR("UIComputerShaderObj::ProcessCurveData.ResetInvalidInput");
		TArray<FCurveSegmentGPU>& WritableLineData = GetWritableLineDataBuffer();
		ResetCurveBuffers(Width, Height, config, LineDrawDesc, WritableLineData, BucketRanges, CurveColors);
		MarkLineDataReadyForUpload();
		return false;
	}

	{
		TRACE_CPUPROFILER_EVENT_SCOPE_STR("UIComputerShaderObj::ProcessCurveData.UpdateDescriptors");
		UpdateLineDrawDesc(LineDrawDesc, Width, Height, CurveCount);
		BuildCurveColors(config, CurveColors);
	}

	// Stage 1: convert raw samples into draw-ready screen-space segments grouped by x bucket.
	TArray<TArray<FCurveSegment>> BucketSegments;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE_STR("UIComputerShaderObj::ProcessCurveData.InitBucketSegments");
		BucketSegments.SetNum(FMath::Max(1, Width));
	}

	{
		TRACE_CPUPROFILER_EVENT_SCOPE_STR("UIComputerShaderObj::ProcessCurveData.BuildBuckets");
		for (int32 CurveIndex = 0; CurveIndex < CurveCount; ++CurveIndex)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE_STR("UIComputerShaderObj::ProcessCurveData.BuildBuckets.Curve");
			const float BaseLine = config.BaseLineStart + static_cast<float>(CurveIndex) * config.BaseLineStep;
			const int32 CurveValueOffset = CurveIndex * SampleCount;

			auto GetSample = [&values, CurveValueOffset](int32 SourceIndex)
			{
				return values[CurveValueOffset + SourceIndex];
			};

			AddCurveToBuckets(CurveIndex, SampleCount, Width, BaseLine, config.ValueScale, GetSample, BucketSegments);
		}
	}

	{
		TRACE_CPUPROFILER_EVENT_SCOPE_STR("UIComputerShaderObj::ProcessCurveData.FlattenBuckets");
		TArray<FCurveSegmentGPU>& WritableLineData = GetWritableLineDataBuffer();
		FlattenBuckets(BucketSegments, BucketRanges, WritableLineData);
		MarkLineDataReadyForUpload();
	}

	return true;
}
