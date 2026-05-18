# Compute Shader 性能优化

## 背景

`IComputerShader Dispatch` 在 100 条曲线、1024×1024 分辨率下 GPU 耗时 ~6ms（stat gpu），目标降至 <2ms。

瓶颈：每个像素遍历其 x-bucket 内所有 segments（100 曲线 × ~3-4 segments/bucket = 300-400 次距离计算）。

---

## 优化 1：Y-Tile 二维分桶

### 原理

原方案只按 X 列分桶，每个像素遍历该列所有 segments。新方案将 Y 方向按 64px 切分为 tile，形成 (x, yTile) 二维 cell。每个 cell 只存储 Y 范围与该 tile 重叠的 segments。

1024×1024 纹理：1024 列 × 16 tiles = 16,384 cells。每像素遍历量从 ~300-400 降至 ~20-40。

### 改动

**Shader (`IComputerShader.usf`)**

```hlsl
#define TILE_HEIGHT 64

uint TileCountY = max(uint(LineDrawDesc[2]), 1u);  // 从 LineDrawDesc[2] 读取
uint YTile = PixelCoord.y / TILE_HEIGHT;
uint CellIndex = PixelCoord.x * TileCountY + YTile;
uint RangeBase = CellIndex * 2u;
```

**C++ (`IComputerShaderObj.cpp`)**

- `GTileHeight = 64` 常量
- `UpdateLineDrawDesc`: index[2] 写入 `TileCountY = ceil(Height / 64)`
- `BuildBucketRangesAndLineData`: 接收 Height 参数，按 (x, yTile) 二维分桶
  - 第一遍：统计每个 cell 的 segment 数量
  - 计算 offset 前缀和
  - 第二遍：scatter segments 到对应 cells
- `ResetCurveBuffers` / upload 验证：BucketRanges 大小 = `Width × TileCountY × 2`

### 内存影响

- BucketRanges: 2KB → 128KB（可接受）
- LineData: segments 跨 tile 复制，总量约 ×1.5

### 预期收益

GPU dispatch 耗时降低 ~8-10x（每像素循环次数从 300+ 降至 20-40）。

---

## 优化 2：降低 Dispatch 频率（30Hz）

### 原理

原方案每帧（60Hz）都执行 `UploadProcessedCurveDataToGPU()`。波形曲线 30Hz 刷新在视觉上几乎无差异，直接减半 GPU 开销。

### 改动

**C++ (`IComputerShaderObj.cpp` — `Tick`)**

```cpp
// 数据生成仍每帧运行（保持相位平滑推进）
SimulatedRunningPhase = FMath::Fmod(...);
SetMultiSinWaveData(...);

// GPU dispatch 限制为 30Hz
if (UploadTickAccumulatorSeconds >= 0.033f)
{
    UploadTickAccumulatorSeconds -= 0.033f;
    UploadProcessedCurveDataToGPU();
}
```

### 预期收益

GPU 时间直接减半（每秒 60 次 dispatch → 30 次）。
