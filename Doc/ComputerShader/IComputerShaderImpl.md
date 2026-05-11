# IComputerShader 绘线实现逻辑

本文说明 `UIComputerShaderObj`、`FIComputerShader` 和 `IComputerShader.usf` 这一套绘线管线的调用流程、CPU 侧极值采样逻辑、GPU 数据布局，以及 shader 最终如何把多条曲线画到 `UTextureRenderTarget2D`。

## 总体流程

当前实现把绘线拆成两个阶段：

1. CPU 侧处理原始数据
   - 入口：`UIComputerShaderObj::ProcessCurveData`
   - 输入：原始采样数组 `values` 和 `FIComputerCurveRenderConfig`
   - 输出：缓存到对象成员里的最终绘制数据：
     - `LineDrawDesc`
     - `LineData`
     - `BucketRanges`
     - `CurveColors`

2. GPU 侧上传并执行 compute shader
   - 入口：`UIComputerShaderObj::UploadProcessedCurveDataToGPU`
   - 行为：把 CPU 已处理好的缓存数据复制到渲染线程，通过 RDG upload buffer 绑定给 shader，然后 dispatch `IComputerShader`
   - 输出：shader 写入 `RenderTarget`

UMG 推荐调用顺序：

```text
CreateRenderTarget
ProcessCurveData 或 SetMultiSinWaveData
UploadProcessedCurveDataToGPU
GetRenderTarget
```

旧接口 `Execute` 仍保留，它现在只是转调 `UploadProcessedCurveDataToGPU`。旧接口 `SetCurveData` 也保留，它现在只是转调 `ProcessCurveData`。

## UMG 模拟多条 Sin 曲线

如果只是模拟多条 sin 线，可以直接调用：

```text
SetMultiSinWaveData(offset, coefficient, curvePhaseStep, config)
UploadProcessedCurveDataToGPU()
```

`SetMultiSinWaveData` 内部会生成一个扁平数组，然后调用 `ProcessCurveData`。

配置示例：

```text
CurveCount = 3
SampleCount = 5000
BaseLineStart = 200
BaseLineStep = 250
ValueScale = 80
CurveColors = [Red, Green, Yellow]
```

含义：

- `CurveCount`：曲线数量。
- `SampleCount`：每条曲线的原始采样点数量。
- `BaseLineStart`：第 0 条曲线的基线高度。
- `BaseLineStep`：相邻曲线基线之间的高度差。
- `ValueScale`：原始值到屏幕 y 偏移的缩放系数。
- `CurveColors[Index]`：第 `Index` 组数据的曲线颜色。
- `curvePhaseStep`：每条模拟 sin 曲线的相位差，避免多条线完全重叠。

每条曲线的屏幕基线为：

```text
BaseLine = BaseLineStart + CurveIndex * BaseLineStep
```

每个采样点的屏幕 y 坐标为：

```text
ScreenY = BaseLine + SampleValue * ValueScale + 0.5
```

## 真实数据输入格式

真实数据使用一个扁平 `TArray<float>` 输入，所有曲线的采样长度必须相同。

数组布局固定为：

```text
values[CurveIndex * SampleCount + SampleIndex]
```

例如 `CurveCount = 3`、`SampleCount = 5000` 时：

```text
第 0 条曲线: values[0 ... 4999]
第 1 条曲线: values[5000 ... 9999]
第 2 条曲线: values[10000 ... 14999]
```

如果 `values.Num() < CurveCount * SampleCount`，`ProcessCurveData` 会重置成一套安全空缓存并返回 `false`，避免 shader 读取空 buffer 或越界。

## CPU 侧极值采样

原始曲线可能有 5000 个甚至更多采样点，而 render target 宽度可能只有 1024。不能直接把所有点逐像素画，也不能简单平均降采样，否则尖峰会被抹掉。

当前实现使用 M4 思路按屏幕 x bucket 降采样。对每条曲线、每个屏幕 x 列：

1. 计算这个屏幕 x bucket 对应的原始采样范围。
2. 在这个范围内保留 4 类点：
   - `first`：范围内第一个采样点
   - `min`：范围内最小值点
   - `max`：范围内最大值点
   - `last`：范围内最后一个采样点
3. 对这 4 个点按原始 `SourceIndex` 去重并排序。
4. 按原始采样顺序连接成线段。
5. 把线段写入它横向覆盖到的所有 x bucket。

这样做的目的：

- `first/last` 保证曲线走势连续。
- `min/max` 保留局部极值和尖峰。
- 按 `SourceIndex` 排序可以避免 min/max 出现顺序反了导致折线走错方向。

## Bucket 分桶逻辑

shader 是按像素执行的。如果每个像素都扫描所有曲线的所有线段，成本会非常高。

所以 CPU 侧先把线段按屏幕 x 列分桶：

```text
BucketSegments[ScreenX] = 当前 x 列附近可能影响这个像素列的线段列表
```

一条线段可能横跨多个 x 列，所以 `AddSegmentToBuckets` 会把它复制到它覆盖到的所有 bucket 中，并额外加一点 `GBinningExpand`，让抗锯齿边缘附近的像素也能找到这条线段。

这一步完成后，shader 处理某个像素时只需要读取：

```text
BucketRanges[PixelCoord.x]
```

然后扫描这一列相关的少量线段。

## FlattenBuckets 输出的数据

`FlattenBuckets` 会把 CPU 侧的二维 bucket 列表压平成 GPU 容易读取的两个 buffer。

### LineData

`LineData` 是所有线段的扁平 float 数组。每条线段占 5 个 float：

```text
x0, y0, x1, y1, curveIndex
```

含义：

- `x0, y0`：线段起点，屏幕空间坐标。
- `x1, y1`：线段终点，屏幕空间坐标。
- `curveIndex`：这条线段属于第几条曲线，用来读取 `CurveColors[curveIndex]`。

### BucketRanges

`BucketRanges` 是每个屏幕 x bucket 的线段范围。每个 bucket 占 2 个 `uint32`：

```text
segmentOffset, segmentCount
```

含义：

- `segmentOffset`：当前 bucket 的第一条线段在 `LineData` 中的线段索引。
- `segmentCount`：当前 bucket 有多少条线段。

注意这里的 `segmentOffset` 是“线段索引”，不是 float 索引。shader 读取时会再乘以 `SegmentFloatCount`：

```hlsl
BaseIndex = (SegmentOffset + SegmentIndex) * SegmentFloatCount
```

### LineDrawDesc

`LineDrawDesc` 存 shader 需要的基础描述信息：

```text
LineDrawDesc[0] = Width
LineDrawDesc[1] = Height
LineDrawDesc[2] = SegmentFloatCount
LineDrawDesc[3] = CurveCount
```

### CurveColors

`CurveColors` 是每条曲线一个颜色：

```text
CurveColors[CurveIndex] = 当前曲线颜色
```

如果用户没有给够颜色，CPU 会使用默认调色板补齐。

## GPU 上传流程

`UploadProcessedCurveDataToGPU` 在 Game Thread 调用，但真正的 RDG 操作在 Render Thread 执行。

流程如下：

1. 从 `RenderTarget` 获取 `FTextureRenderTargetResource`。
2. 复制 CPU 缓存：
   - `LineDrawDescCopy`
   - `LineDataCopy`
   - `BucketRangesCopy`
   - `CurveColorsCopy`
3. `ENQUEUE_RENDER_COMMAND` 进入渲染线程。
4. 创建 `FRDGBuilder`。
5. `RegisterExternalTexture` 把 `UTextureRenderTarget2D` 注册成 RDG texture。
6. 为 render target 创建 UAV：

```cpp
PassParameters->RenderTarget = GraphBuilder.CreateUAV(TargetTexture);
```

7. 用 `CreateUploadBuffer` 上传 CPU 数组，并创建 SRV：

```cpp
LineDrawDesc -> Buffer<float>
LineData     -> Buffer<float>
BucketRanges -> Buffer<uint>
CurveColors  -> Buffer<float4>
```

8. 计算 dispatch group count。
9. 添加 RDG compute pass，调用 `FComputeShaderUtils::Dispatch`。

## Shader 绘线原理

`IComputerShader.usf` 中每个 compute shader thread 负责一个像素：

```hlsl
uint2 PixelCoord = DispatchThreadId.xy;
```

shader 首先根据像素 x 坐标读取当前 bucket：

```hlsl
uint RangeBase = PixelCoord.x * 2u;
uint SegmentOffset = BucketRanges[RangeBase];
uint SegmentCount = BucketRanges[RangeBase + 1u];
```

然后遍历当前 bucket 中的线段：

```hlsl
for (uint SegmentIndex = 0u; SegmentIndex < SegmentCount; ++SegmentIndex)
```

对每条线段：

1. 从 `LineData` 取出 `x0,y0,x1,y1,curveIndex`。
2. 用 `DistanceToSegment` 计算当前像素中心点到线段的最短距离。
3. 记录距离当前像素最近的线段和它的 `curveIndex`。

距离计算思路：

```text
像素点投影到线段上
投影参数 clamp 到 [0, 1]
计算像素点到最近投影点的距离
```

最后用 `smoothstep` 做线宽和抗锯齿：

```hlsl
Alpha = 1.0f - smoothstep(HalfWidth, HalfWidth + AntiAliasWidth, MinDistance);
```

再根据最近线段所属曲线取颜色：

```hlsl
LineColor = CurveColors[BestCurveIndex];
```

最终写入 render target：

```hlsl
RenderTarget[PixelCoord] = lerp(BackgroundColor, LineColor, Alpha * LineColor.a);
```

## 多条曲线重叠时的规则

当前 shader 不做复杂颜色混合。一个像素如果同时靠近多条曲线，会选择距离该像素最近的那条线段，并使用那条线段所属曲线的颜色。

这意味着：

- 曲线交叉处不会叠色。
- 哪条线更靠近当前像素中心，就显示哪条线的颜色。
- 如果两条线距离完全一样，先扫描到的线段会保留。

## 关键成员变量

`UIComputerShaderObj` 内部缓存的是处理后的最终绘制数据：

```cpp
TArray<float> LineDrawDesc;
TArray<float> LineData;
TArray<uint32> BucketRanges;
TArray<FLinearColor> CurveColors;
```

这些数组就是 `ProcessCurveData` 的处理结果。它们不会立刻进入 GPU，只有调用 `UploadProcessedCurveDataToGPU` 后才会上传。

## 设计取舍

当前实现把“数据处理”和“GPU 执行”拆开，是为了让 UMG 或后续真实数据源可以主动控制刷新时机：

- 数据变了：调用 `ProcessCurveData`。
- 需要刷新 render target：调用 `UploadProcessedCurveDataToGPU`。
- 多次 UI 操作只改数据参数时，可以先处理，最后再统一上传。

CPU 侧做极值采样和 bucket 分桶，GPU 侧只负责像素级距离判断和着色。这样 shader 逻辑比较稳定，后续接入真实数据源时主要改输入数组和配置，不需要改 `.usf`。
