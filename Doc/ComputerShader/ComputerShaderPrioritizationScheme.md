# IComputerShader 绘制效率优化方案

## 一、修改概览

| 文件 | 改动项 | 对应优化 |
|------|--------|----------|
| `IComputerShader.h` | 线程组尺寸、Shader 参数类型、新增 `FCurveSegmentGPU` 结构体 | #1 线程组重排、#3 单次取段 |
| `IComputerShader.usf` | 入口函数、`AccumulateSegmentDistance`、线段结构体定义 | #1 线程组重排、#2 删 X 包围盒、#3 单次取段 |
| `IComputerShaderObj.h` | `LineData` 成员类型变更、新增 `IComputerShader.h` 引用；新增 `LineData` 三缓冲、上传 fence 和待上传标记 | #3 单次取段、#4 消除 Game Thread 大拷贝 |
| `IComputerShaderObj.cpp` | `CreateUploadBuffer` → `CreateStructuredBuffer`、`FlattenBuckets` 重写、常量和辅助函数适配；上传时捕获 `TSharedPtr` 缓冲而不是复制整段 `LineData` | #3 单次取段、#4 消除 Game Thread 大拷贝 |

---

## 二、优化项详解

### #1 线程组重排：`[8,8,1]` → `[1,64,1]`（最大单点收益）

**修改点：**
- `IComputerShader.h` 第 28-30 行：`ThreadGroupSizeX=8 → 1`, `ThreadGroupSizeY=8 → 64`
- `IComputerShader.usf` 第 52 行：`numthreads(THREADS_X, THREADS_Y, THREADS_Z)` 通过宏自动同步新值

**原理：**

原始 8×8 线程组导致严重的 warp divergence：

```
8×8 线程组：
  warp 内 32 条线程分属 4 个不同 x 列
  → 4 种不同的 BucketRanges[x]
  → 4 种不同的 SegmentCount
  → for 循环步数不统一，慢的线程拖累整 warp
  → 每条线程独立从内存抓 LineData，无法合并访存
```

修改为 1×64 竖条组：

```
1×64 线程组：
  warp 内 32 条线程共享同 1 个 x 列
  → 同一个 BucketRanges[x]，SegmentCount 是常量
  → for 循环步数完全一致，零 divergence
  → LineData[SegmentOffset + i] 是同一地址 → 广播到全 warp
  → 同一条 LineData 缓存行被 32 条线程复用，L1 hit → 1
```

**关键设计决策：** 线程组的 x 维度设为 1，使得每个 warp/线程组专注于单个屏幕列（x bucket），与数据按 x 列分桶的架构完美对齐。

**预期收益：** 3-5× 吞吐提升，视每条 x 列的线段数量而定。

---

### #2 删除 X 方向包围盒测试

**修改点：**
- `IComputerShader.usf` 第 19-44 行：`AccumulateSegmentDistance` 删掉 `Pixel.x` 对 `MinX/MaxX` 的比较，只保留 Y 方向剔除

**原理：**

线段分桶（CPU 端 `AddSegmentToBuckets`）已经保证当前 x 列只包含与此列相关的线段。`GBinningExpand=2.0` 的安全余量已覆盖 AA 膨胀需求。

```hlsl
// 原代码：4 次比较 + 2 次 min + 2 次 max（每条线段）
float MinX = min(SegmentStart.x, SegmentEnd.x) - BoundsExpand;
float MaxX = max(SegmentStart.x, SegmentEnd.x) + BoundsExpand;
float MinY = min(SegmentStart.y, SegmentEnd.y) - BoundsExpand;
float MaxY = max(SegmentStart.y, SegmentEnd.y) + BoundsExpand;
if (Pixel.x < MinX || Pixel.x > MaxX || Pixel.y < MinY || Pixel.y > MaxY)

// 改后：X 方向由 bucket 预筛，仅 2 次 min/max + 2 次比较
float MinY = min(SegmentStart.y, SegmentEnd.y) - BoundsExpand;
float MaxY = max(SegmentStart.y, SegmentEnd.y) + BoundsExpand;
if (Pixel.y < MinY || Pixel.y > MaxY)
```

**预期收益：** 每条线段减少 50% 的盒测试 ALU 操作。对于每像素要遍历多段的场景，ALU 节省显著。

---

### #3 线段数据从 5 次浮点取数改为单次结构化取数

**修改点：**
- `IComputerShader.h` 第 7-14 行：新增 `FCurveSegmentGPU` 结构体（32B 对齐）
- `IComputerShader.h` 第 21 行：`SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, LineData)` → `SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FCurveSegmentGPU>, LineData)`
- `IComputerShader.usf` 第 7-14 行：新增 `FCurveSegment` 结构体（必须与 C++ 端逐字段一致）
- `IComputerShader.usf` 第 89 行：`LineData[i]` 一行取全段 → 读取 `Segment.Endpoints.xy/zw` 和 `Segment.CurveIndex`
- `IComputerShaderObj.h` 第 70 行：`TArray<float> LineData` → `TArray<FCurveSegmentGPU> LineData`
- `IComputerShaderObj.cpp` 第 232-268 行：`FlattenBuckets` 直接写入 `FCurveSegmentGPU` 而非 5 个独立 float
- `IComputerShaderObj.cpp` 第 377-380 行：上传走 `CreateStructuredBuffer`（而非 `CreateUploadBuffer`）

**原理：**

原始数据布局：每条线段 5 个独立 float（x0, y0, x1, y1, curveIndex），shader 端分 5 次 `Buffer<float>` 取数：

```hlsl
// 原：5 次内存取数
float2 SegmentStart = float2(LineData[BaseIndex],     LineData[BaseIndex + 1u]);
float2 SegmentEnd   = float2(LineData[BaseIndex + 2u], LineData[BaseIndex + 3u]);
uint CurveIndex     = min(uint(max(LineData[BaseIndex + 4u], 0.0f)), CurveCount - 1u);
```

新数据布局：打包为 32B 对齐结构体，shader 端单次取数：

```hlsl
// 新：1 次内存取数
FCurveSegment Segment = LineData[SegmentOffset + SegmentIndex];
float2 SegmentStart = Segment.Endpoints.xy;
float2 SegmentEnd   = Segment.Endpoints.zw;
uint CurveIndex     = min(Segment.CurveIndex, CurveCount - 1u);
```

内存访问带宽降低到原来的 ~1/5。

**结构体内存布局（32 字节）：**

| 偏移 | 字段 | 类型 | 大小 |
|------|------|------|------|
| 0 | Endpoints (x0,y0,x1,y1) | float4 | 16B |
| 16 | CurveIndex | uint | 4B |
| 20 | _Pad0 | uint | 4B |
| 24 | _Pad1 | uint | 4B |
| 28 | _Pad2 | uint | 4B |

Padding 保证 32B 对齐（GPU 缓存行友好），同时让结构化 buffer 的 stride 固定。

**关键修复：** 初始实现使用 `CreateUploadBuffer` 创建 buffer，但该函数在 UE 5.1 中生成的是 `EBufferUsageFlags::VertexBuffer` 类型，与 shader 端 `StructuredBuffer<>` 声明不匹配，导致 D3D12 RHI 在创建 SRV 时除零崩溃。修正为 `CreateStructuredBuffer`，产生正确的 `StructuredBuffer` 类型 buffer，SRV 创建不再出错。

**预期收益：** 内存带宽 ~5× 降低；配合 #1 的 warp 广播效果，同一段数据被 64 条线程复用，缓存命中率大幅提升。

---

### #4 实时大数据流：消除 `CopyLineData` 的 Game Thread 大拷贝

**背景：**

实际运行时，`ProcessCurveData` 会面对大量定长曲线数据，并且数据每帧都在变化。此时不能依赖“BeginPlay 只上传一次”的静态数据策略，瓶颈会集中在每帧数据传输链路上。

Unreal Insights 拆分后发现主要耗时来自：

```text
UIComputerShaderObj::UploadProcessedCurveDataToGPU.CopyLineData
UIComputerShaderObj::UploadProcessedCurveDataToGPU_RenderThread.UploadLineData
```

含义分别是：

```text
CopyLineData:
  Game Thread 将巨大的 LineData TArray 完整复制一份给 render command。

UploadLineData:
  Render Thread 将这份 LineData 创建/上传成 GPU StructuredBuffer。
```

其中 `CopyLineData` 是纯 CPU 侧重复拷贝，应该优先消掉；`UploadLineData` 是每帧真实上传大 buffer，仍会保留，后续需要更深层的数据布局或 GPU 缓存方案优化。

**修改点：**

- `IComputerShaderObj.h`
  - `LineData` 从单个 `TArray<FCurveSegmentGPU>` 改为三份 `TSharedPtr<TArray<FCurveSegmentGPU>, ESPMode::ThreadSafe>` 缓冲。
  - 新增 `FRenderCommandFence LineDataUploadFences[3]`，避免 Game Thread 写入仍被 Render Thread 使用的缓冲。
  - 新增 `LineDataWriteBufferIndex`、`LineDataReadyBufferIndex`、`bHasPendingLineDataUpload`，跟踪当前写入缓冲和待上传缓冲。

- `IComputerShaderObj.cpp`
  - 新增 `GetWritableLineDataBuffer()`：选择一个当前可写的 `LineData` 缓冲；如果三缓冲都被占用，则在 `ProcessCurveData.WaitForLineDataUploadBuffer` 中等待 fence。
  - 新增 `MarkLineDataReadyForUpload()`：`ProcessCurveData` 完成后标记本帧 `LineData` 可上传。
  - `ProcessCurveData` / `CreateRenderTarget` 写入 `GetWritableLineDataBuffer()` 返回的缓冲，而不是写入单个成员 `LineData`。
  - `UploadProcessedCurveDataToGPU` 不再执行 `TArray<FCurveSegmentGPU> LineDataCopy = LineData`。
  - render command lambda 捕获 `TSharedPtr`，只复制智能指针控制信息，不复制大数组内容。
  - render command 入队后对对应缓冲调用 `BeginFence()`，用于判断该缓冲何时可重新写入。

**优化前：**

```cpp
TArray<FCurveSegmentGPU> LineDataCopy = LineData;

ENQUEUE_RENDER_COMMAND(ExecuteIComputerShader)(
    [LineDataCopy = MoveTemp(LineDataCopy)](...)
    {
        CreateStructuredBuffer(..., LineDataCopy.GetData(), ...);
    }
);
```

问题是 `LineDataCopy = LineData` 会完整复制所有线段。数据量越大，Game Thread 越容易被这一步拖住。

**优化后：**

```cpp
TSharedPtr<TArray<FCurveSegmentGPU>, ESPMode::ThreadSafe> LineDataUploadBuffer;
LineDataUploadBuffer = LineDataBuffers[LineDataReadyBufferIndex];

ENQUEUE_RENDER_COMMAND(ExecuteIComputerShader)(
    [LineDataUploadBuffer = MoveTemp(LineDataUploadBuffer)](...)
    {
        const TArray<FCurveSegmentGPU>& LineDataUpload = *LineDataUploadBuffer;
        CreateStructuredBuffer(..., LineDataUpload.GetData(), ...);
    }
);
```

render command 持有 shared pointer，所以 Game Thread 可以继续写另一份缓冲，不需要为 lambda 再复制一份巨大的 `TArray`。

**三缓冲时序：**

```text
Frame N:
  ProcessCurveData 写 Buffer A
  Upload 入队，Render Thread 读取 Buffer A，BeginFence(A)

Frame N+1:
  ProcessCurveData 写 Buffer B
  Upload 入队，Render Thread 读取 Buffer B，BeginFence(B)

Frame N+2:
  ProcessCurveData 写 Buffer C
  Upload 入队，Render Thread 读取 Buffer C，BeginFence(C)

Frame N+3:
  优先复用已经 fence complete 的 Buffer A
```

如果 Render Thread 跟不上，三份缓冲都还没释放，`GetWritableLineDataBuffer()` 会等待某个 fence，并在 Insights 中显示：

```text
UIComputerShaderObj::ProcessCurveData.WaitForLineDataUploadBuffer
```

这个事件如果明显变长，说明瓶颈已经从“Game Thread 复制大数组”转移到“Render Thread/GPU 上传节奏跟不上”。

**预期收益：**

- `UIComputerShaderObj::UploadProcessedCurveDataToGPU.CopyLineData` 应消失。
- Game Thread 少一次完整 `LineData` 大拷贝。
- 大数组容量可在三份缓冲中复用，减少每帧分配和释放压力。
- `UploadLineData` 仍然存在，因为数据仍需每帧上传到 GPU。

**限制：**

这个优化只解决 CPU 侧重复拷贝，不解决每帧上传数据体积本身。如果 `UploadLineData` 仍然很高，需要继续做下面的结构性优化。

**后续优化方向：**

1. **GPU 持久 buffer**
   - 将 `LineData` 上传到持久 GPU buffer。
   - 数据变化时用更新/拷贝路径刷新，而不是每帧创建新的 RDG structured buffer。
   - 适合数据大但更新频率或更新范围可控的情况。

2. **上传原始数据，让 GPU 做分桶**
   - CPU 每帧只上传原始 `float` 样本。
   - GPU pass 1 做 min/max、M4 采样、bucket 构建。
   - GPU pass 2 绘制到 RenderTarget。
   - 适合原始数据比展开后的 `FCurveSegmentGPU` 小很多的情况。

3. **滚动窗口 / ring buffer 增量上传**
   - 如果曲线是示波器式滚动数据，每帧只新增少量 sample。
   - GPU 侧保存固定长度环形 buffer。
   - 每帧只上传新增 sample 和 `HeadIndex`。
   - 适合定长数组持续滚动更新的场景，收益通常最大。

4. **压缩上传格式**
   - 将 y 值量化为 `uint16` / `half`。
   - 或将线段端点改成局部坐标/短整型。
   - 适合视觉精度允许少量量化误差的 UI 曲线。

---

## 三、不变项

以下流水线或数据路径未做修改，不影响功能：

| 项目 | 说明 |
|------|------|
| `SimpleComputeShader.usf` / `CustomComputeShader.cpp` | 独立流水线，未触及 |
| `LineDrawDesc` / `BucketRanges` / `CurveColors` 的上传方式 | 仍使用 `CreateUploadBuffer`，与 shader 端 `Buffer<>` 声明匹配 |
| CPU 端 M4 采样 + x-bucket 分桶逻辑 | `AddCurveToBuckets` / `AddPointUnique` 等核心算法不变；只是最终写入可复用的三缓冲 `LineData` |
| `FIComputerCurveRenderConfig` 结构体 | API 签名不变 |
| 线程组总数 | `ThreadGroupSizeX × ThreadGroupSizeY = 64` 不变，一个组仍处理 64 个像素 |
| `GBinningExpand = 2.0` | AA 安全余量不变 |
| `HalfWidth = 0.5`, `AntiAliasWidth = 1.0` | 线宽和反锯齿参数不变 |

---

## 四、验证清单

- [ ] C++ 编译通过（UBT 重新编译 `ComputeShader` 模块）
- [ ] Shader 编译通过（编辑器 `Ctrl+Shift+.` 或自动热重载）
- [ ] `FCurveSegmentGPU` sizeof 确认为 32（`static_assert` 可选）
- [ ] 运行时不再崩溃（无除零/无 SRV 创建失败）
- [ ] 渲染结果与修改前视觉一致
- [ ] 性能对比（RenderDoc / PIX / UE GPU Profiler）
- [ ] Unreal Insights 中 `CopyLineData` 消失或接近 0
- [ ] Unreal Insights 中观察 `ProcessCurveData.WaitForLineDataUploadBuffer`，确认三缓冲没有频繁等待
- [ ] Unreal Insights 中继续观察 `UploadLineData`，判断是否需要进入 GPU 持久 buffer / 原始数据上传 / ring buffer 方案
