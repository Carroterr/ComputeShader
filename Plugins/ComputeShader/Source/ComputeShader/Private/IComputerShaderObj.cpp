#include "IComputerShaderObj.h"

#include "IComputerShader.h"
#include "Engine/TextureRenderTarget2D.h"
#include "RenderGraphUtils.h"

class FIComputerShader;

namespace
{
	constexpr int32 GSegmentFloatCount = 4;
	constexpr int32 GSegmentsPerBucket = 32;
	constexpr float GInvalidSegmentX = -1.0f;
	constexpr float GBinningExpand = 2.0f;

	struct FReducedWavePoint
	{
		int32 SourceIndex = 0;
		float X = 0.0f;
		float Y = 0.0f;
	};
}

void UIComputerShaderObj::CreateRenderTarget(int32 Width, int32 Height)
{
	RenderTarget = NewObject<UTextureRenderTarget2D>(this);

	RenderTarget->bCanCreateUAV = true;
	RenderTarget->InitCustomFormat(Width, Height, PF_FloatRGBA, false);
	RenderTarget->UpdateResourceImmediate(true);

	LineDrawDesc.SetNumZeroed(3);
	LineDrawDesc[0] = static_cast<float>(Width); // texture width
	LineDrawDesc[1] = static_cast<float>(Height); // texture height
	LineDrawDesc[2] = static_cast<float>(GSegmentsPerBucket);
	LineData.Init(GInvalidSegmentX, Width * GSegmentsPerBucket * GSegmentFloatCount);
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
	LineDrawDescCopy.Add(static_cast<float>(GSegmentsPerBucket));

	TArray<float> LineDataCopy = LineData;
	const int32 ExpectedLineDataCount = Width * GSegmentsPerBucket * GSegmentFloatCount;
	if (LineDataCopy.Num() != ExpectedLineDataCount)
	{
		LineDataCopy.Init(GInvalidSegmentX, ExpectedLineDataCount);
	}

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
	const int32 Height = RenderTarget ? RenderTarget->SizeY : 1024;

	// 临时模拟一条 5000 点的源曲线。后续接真实数据源时，
	// SourceCount 应改成当前曲线真实采样点数量。
	const int32 SourceCount = 5000;

	// LineData 不再存“每列 min/max”，而是存已经整理好的线段：
	// 每条线段 4 个 float：x0, y0, x1, y1。
	// 为了让 shader 只检查当前像素列附近的线段，这里按屏幕 x bucket 预分桶。
	// 空槽用 GInvalidSegmentX 标记，shader 读到 x0 < 0 时跳过。
	LineData.Init(GInvalidSegmentX, Width * GSegmentsPerBucket * GSegmentFloatCount);

	// 记录每个屏幕 x bucket 已经塞了多少条线段，避免写超过固定槽位。
	TArray<int32> SegmentCounts;
	SegmentCounts.Init(0, Width);

	// 生成临时 Sin 源数据。真实数据接入时，可以替换成 RawValues[SourceIndex]。
	auto MakeSample = [offset, coefficient](int32 SourceIndex)
	{
		return FMath::Sin(FMath::DegreesToRadians(SourceIndex * coefficient) + offset) * 300.0f + 500.0f;
	};

	// 把源数据 index 映射到屏幕空间 x。这里直接生成屏幕坐标，
	// shader 就不需要知道原始数据长度、降采样率或当前视口范围。
	auto SourceIndexToScreenX = [Width, SourceCount](int32 SourceIndex)
	{
		if (Width <= 1)
		{
			return 0.5f;
		}

		return static_cast<float>(SourceIndex) * static_cast<float>(Width - 1) / static_cast<float>(SourceCount - 1) + 0.5f;
	};

	// 把数据值映射到屏幕空间 y。当前临时 Sin 已经近似是屏幕坐标，
	// 所以这里只做 clamp；接真实数据时这里应接入 y 轴缩放/偏移逻辑。
	auto ValueToScreenY = [Height](float Value)
	{
		return FMath::Clamp(Value, 0.0f, static_cast<float>(Height - 1)) + 0.5f;
	};

	// 构造带源 index 的绘制点。SourceIndex 用于排序和去重，
	// X/Y 是最终传给 shader 的屏幕空间坐标。
	auto MakePoint = [&MakeSample, &SourceIndexToScreenX, &ValueToScreenY](int32 SourceIndex)
	{
		return FReducedWavePoint{
			SourceIndex,
			SourceIndexToScreenX(SourceIndex),
			ValueToScreenY(MakeSample(SourceIndex))
		};
	};

	// first/min/max/last 可能会指向同一个源采样点，
	// 例如 bucket 很窄或极值刚好出现在首尾，所以需要按 SourceIndex 去重。
	auto AddPointUnique = [](TArray<FReducedWavePoint>& Points, const FReducedWavePoint& Point)
	{
		for (const FReducedWavePoint& ExistingPoint : Points)
		{
			if (ExistingPoint.SourceIndex == Point.SourceIndex)
			{
				return;
			}
		}

		Points.Add(Point);
	};

	// 把一条已确定的线段写入它横向覆盖到的屏幕 x bucket。
	// shader 每个像素只读取自己 x 列的线段槽，因此这里要把跨多列的斜线
	// 复制到所有可能受影响的 bucket 中。
	auto AddSegmentToBuckets = [this, &SegmentCounts, Width](const FReducedWavePoint& Start, const FReducedWavePoint& End)
	{
		if (Start.SourceIndex == End.SourceIndex)
		{
			return;
		}

		// 根据线段横向范围加一点扩展，保证抗锯齿宽度范围内的像素也能读到该线段。
		const int32 StartBucket = FMath::Clamp(FMath::FloorToInt(FMath::Min(Start.X, End.X) - GBinningExpand), 0, Width - 1);
		const int32 EndBucket = FMath::Clamp(FMath::FloorToInt(FMath::Max(Start.X, End.X) + GBinningExpand), 0, Width - 1);

		for (int32 BucketIndex = StartBucket; BucketIndex <= EndBucket; ++BucketIndex)
		{
			int32& SegmentCount = SegmentCounts[BucketIndex];
			if (SegmentCount >= GSegmentsPerBucket)
			{
				continue;
			}

			// 当前 bucket 内按固定步长写入 x0, y0, x1, y1。
			const int32 DataIndex = (BucketIndex * GSegmentsPerBucket + SegmentCount) * GSegmentFloatCount;
			LineData[DataIndex] = Start.X;
			LineData[DataIndex + 1] = Start.Y;
			LineData[DataIndex + 2] = End.X;
			LineData[DataIndex + 3] = End.Y;
			++SegmentCount;
		}
	};

	// 记录上一个 bucket 的最后一个点，用来补：
	// previous bucket last -> current bucket first
	// 这就是避免 bucket 之间断线的关键。
	bool bHasPreviousPoint = false;
	FReducedWavePoint PreviousPoint;

	for (int32 BucketIndex = 0; BucketIndex < Width; ++BucketIndex)
	{
		// 当前屏幕 x bucket 对应的源数据 index 范围：[BucketStart, BucketEnd)。
		// 不同源数据长度下，只需要改 SourceCount，这个映射仍然成立。
		const int32 BucketStart = FMath::FloorToInt(static_cast<float>(BucketIndex) * SourceCount / Width);
		const int32 BucketEnd = FMath::Max(
			BucketStart + 1,
			FMath::FloorToInt(static_cast<float>(BucketIndex + 1) * SourceCount / Width)
		);
		const int32 BucketLast = FMath::Min(BucketEnd - 1, SourceCount - 1);

		int32 MinIndex = BucketStart;
		int32 MaxIndex = BucketStart;
		float MinValue = MakeSample(BucketStart);
		float MaxValue = MinValue;

		// 在当前 bucket 覆盖的源数据范围内找极小值和极大值。
		// 这样高频尖峰不会因为平均降采样被抹掉。
		for (int32 SourceIndex = BucketStart + 1; SourceIndex < BucketEnd; ++SourceIndex)
		{
			const float Value = MakeSample(SourceIndex);
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

		// M4 降采样思路：每个 bucket 保留 first/min/max/last。
		// first/last 保证连续走势，min/max 保留极值尖峰。
		TArray<FReducedWavePoint> BucketPoints;
		BucketPoints.Reserve(4);
		AddPointUnique(BucketPoints, MakePoint(BucketStart));
		AddPointUnique(BucketPoints, MakePoint(MinIndex));
		AddPointUnique(BucketPoints, MakePoint(MaxIndex));
		AddPointUnique(BucketPoints, MakePoint(BucketLast));

		// min 和 max 谁先出现是不确定的，必须按源数据顺序排序。
		// 这样生成的折线方向才符合真实采样顺序。
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
			// 补上两个相邻 bucket 之间的连接段，避免出现：
			// bucket0 内部线段结束后，bucket1 起点之前断开的情况。
			AddSegmentToBuckets(PreviousPoint, BucketPoints[0]);
		}

		// 当前 bucket 内部按 source index 顺序连接。
		for (int32 PointIndex = 0; PointIndex + 1 < BucketPoints.Num(); ++PointIndex)
		{
			AddSegmentToBuckets(BucketPoints[PointIndex], BucketPoints[PointIndex + 1]);
		}

		// 留给下一个 bucket 做跨 bucket 连接。
		PreviousPoint = BucketPoints.Last();
		bHasPreviousPoint = true;
	}
}
