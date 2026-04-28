# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Unreal Engine **5.1** project (`ComputeShaderTest.uproject`) demonstrating a custom HLSL compute shader that renders a waveform into a `UTextureRenderTarget2D`. The game module itself (`Source/ComputeShaderTest`) is near-empty scaffolding — all meaningful code lives in the **`ComputeShader` plugin** under `Plugins/ComputeShader`.

Default RHI is **DX12 / SM6** (see `Config/DefaultEngine.ini`). `r.ShaderDevelopmentMode=1` is enabled, so `.usf` edits recompile live via the in-editor shader recompile (`Ctrl+Shift+.` / `recompileshaders changed`) without a full C++ rebuild.

## Build / Run

No CLI build scripts are committed. Typical UE workflows:
- Open `ComputeShaderTest.uproject` in the editor (it offers to rebuild out-of-date binaries on launch), **or**
- Build from `ComputeShaderTest.sln` in Visual Studio (configuration `Development Editor | Win64`), **or**
- Command line via UBT, e.g.:
  - `"<UE_5.1>/Engine/Build/BatchFiles/Build.bat" ComputeShaderTestEditor Win64 Development -Project="D:/GameDevelop/Package/Unreal/ComputeShaderTest/ComputeShaderTest.uproject" -WaitMutex`

Targets are defined in `Source/ComputeShaderTest.Target.cs` (Game) and `ComputeShaderTestEditor.Target.cs` (Editor). There is no test suite in this project.

## Architecture — where the work actually happens

The compute shader pipeline is split across four files. You cannot understand any one of them in isolation:

1. **`Plugins/ComputeShader/Shaders/SimpleComputeShader.usf`** — HLSL entry point `SimpleComputeShader`, thread group `[THREADS_X, THREADS_Y, THREADS_Z]` (defines injected from C++). Writes a waveform to `RWTexture2D<float4> RenderTarget` using `LineDrawDesc` (dimensions) and `LineData` (per-x sample values).

2. **`Plugins/ComputeShader/Source/ComputeShader/Private/ComputeShader.cpp`** — module startup registers the virtual shader path **`/ComputeShaderShaders` → `Plugins/ComputeShader/Shaders`** via `AddShaderSourceDirectoryMapping`. The plugin must load at `PostConfigInit` (set in `ComputeShader.uplugin`) so this mapping exists before shader compilation. **Any new `.usf` file must go under that directory and be referenced as `/ComputeShaderShaders/Foo.usf`.**

3. **`Public/CustomComputeShader.h`** — declares:
   - `FCustomComputeShader : FGlobalShader` with `SHADER_USE_PARAMETER_STRUCT`, RDG parameter struct (`RenderTarget` UAV + two float SRV buffers), a permutation domain (`TEST` int), and a `ModifyCompilationEnvironment` that injects `THREADS_X/Y/Z` macros read by the `.usf`.
   - `UCustomShader : UObject` — the Blueprint-facing wrapper exposing `CreateRenderTarget`, `ExecuteComputeShader(TArray<float>)`, and `GetRenderTarget()`.

4. **`Private/CustomComputeShader.cpp`** — `IMPLEMENT_GLOBAL_SHADER(FCustomComputeShader, "/ComputeShaderShaders/SimpleComputeShader.usf", "SimpleComputeShader", SF_Compute)`. `ExecuteComputeShader` runs on the render thread via `ENQUEUE_RENDER_COMMAND`, builds an RDG graph, registers the render target as an external RDG texture, uploads `LineDrawDesc` / `LineData` with `CreateUploadBuffer`, computes group count via `FComputeShaderUtils::GetGroupCount(..., kGolden2DGroupSize)`, and dispatches with `ERDGPassFlags::AsyncCompute`.

Key cross-file invariants:
- The second argument to `IMPLEMENT_GLOBAL_SHADER` (`/ComputeShaderShaders/...`) **must** match the mapping added in `FComputeShaderModule::StartupModule`.
- `THREADS_X/Y/Z` are defined by `ModifyCompilationEnvironment` in C++ and consumed by `[numthreads(...)]` in the `.usf` — changing thread-group size requires editing both.
- The render target is created with `bCanCreateUAV = true` and `PF_FloatRGBA`; the UAV binding in the shader (`RWTexture2D<float4>`) depends on this.
- Plugin `Build.cs` privately depends on `Renderer`, `RenderCore`, `RHI`, `Projects` — these are required for `FGlobalShader`, RDG, and `IPluginManager`.

## Blueprint integration

`UCustomShader` is `Blueprintable / BlueprintType`. Content assets under `Content/Widgets`, `Content/Objects`, and `Content/Maps` (e.g. `Test.Test` — the default map) drive the demo; the `BlueprintCallable` trio (`CreateRenderTarget` → `ExecuteComputeShader` → `GetRenderTarget`) is the expected usage from BP/UMG. There is no C++ actor or game-mode logic beyond the empty `AComputeShaderTestGameModeBase`.

## Gotchas

- `Source/ComputeShaderTest/ComputeShaderTest.h` is essentially empty — do not expect a module header with declarations; the primary game module is implemented via `IMPLEMENT_PRIMARY_GAME_MODULE` in the `.cpp`.
- `CustomComputeShader.h` `#include`s `CustomComputeShader.generated.h` because of `UCustomShader`; if you add new `UCLASS`/`UFUNCTION` declarations, UHT must regenerate — trigger a rebuild (editor prompt or UBT) rather than only recompiling shaders.
- There is commented-out legacy code at the bottom of `SimpleComputeShader.usf` showing a prior sine-wave path that used a different parameter layout — ignore it unless you intentionally revive that mode.
