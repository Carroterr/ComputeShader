### 官方支持文档
[Adding Global Shaders to Unreal Engine | Unreal Engine 5.7 Documentation | Epic Developer Community](https://dev.epicgames.com/documentation/unreal-engine/adding-global-shaders-to-unreal-engine)


### 如何新建一个ComputerShader
1. 在../Shader 文件夹中新建一个.usf的shader文件
2. **在 C++ 里声明并注册这个 Compute Shader 类型**。
它的职责只是描述 shader：
`1. 源码文件在哪里 2. 入口函数叫什么 3. 需要哪些参数 4. numthreads 的宏是多少`
告诉 Unreal：
> 有一个全局 Compute Shader，它的源码文件是 MyFirstComputeShader.usf，入口函数叫 MyFirstComputeShader，它需要一个输出 RenderTarget UAV。

~~~ C++
#pragma once  
  
#include "CoreMinimal.h"  
#include "GlobalShader.h"  
#include "ShaderParameterStruct.h"  
  
class COMPUTESHADER_API FIComputerShader : public FGlobalShader  
{  
public:  
    DECLARE_GLOBAL_SHADER(FIComputerShader);  
    SHADER_USE_PARAMETER_STRUCT(FIComputerShader, FGlobalShader);  
  
    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )  
       SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, RenderTarget)  
       SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, DrawDesc)  
    END_SHADER_PARAMETER_STRUCT()  
  
    static constexpr int32 ThreadGroupSizeX = 8;  
    static constexpr int32 ThreadGroupSizeY = 8;  
    static constexpr int32 ThreadGroupSizeZ = 1;  
  
    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)  
    {       return true;  
    }  
    static void ModifyCompilationEnvironment(  
       const FGlobalShaderPermutationParameters& Parameters,  
       FShaderCompilerEnvironment& OutEnvironment)  
    {       FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);  
  
       OutEnvironment.SetDefine(TEXT("THREADS_X"), ThreadGroupSizeX);  
       OutEnvironment.SetDefine(TEXT("THREADS_Y"), ThreadGroupSizeY);  
       OutEnvironment.SetDefine(TEXT("THREADS_Z"), ThreadGroupSizeZ);  
    }};

~~~

然后在 CustomComputeShader.cpp 里注册它：
~~~ C++
#include "IComputerShader.h"  
  
IMPLEMENT_GLOBAL_SHADER(  
    FIComputerShader,  
    "/ComputeShaderShaders/CurvePlotting_MXAAShader.usf",  
    "IComputerShader",  
    SF_Compute  
);
~~~

3. 第三步：**新建一个 Blueprint 可调用的 UObject 类**
~~~ C++
#pragma once  
  
#include "CoreMinimal.h"  
#include "UObject/Object.h"  
#include "IComputerShaderObj.generated.h"  
  
class UTextureRenderTarget2D;  
  
UCLASS(Blueprintable, BlueprintType)  
class COMPUTESHADER_API UIComputerShaderObj : public UObject  
{  
    GENERATED_BODY()  
  
public:  
    UFUNCTION(BlueprintCallable)  
    void CreateRenderTarget(int32 Width = 1024, int32 Height = 1024);  
  
    UFUNCTION(BlueprintPure)  
    UTextureRenderTarget2D* GetRenderTarget() const;  
  
private:  
    UPROPERTY(Transient)  
    UTextureRenderTarget2D* RenderTarget = nullptr;  
};
~~~

~~~ c++
#include "IComputerShaderObj.h"  
  
#include "Engine/TextureRenderTarget2D.h"  
  
void UIComputerShaderObj::CreateRenderTarget(int32 Width, int32 Height)  
{  
    RenderTarget = NewObject<UTextureRenderTarget2D>(this);  
  
    RenderTarget->bCanCreateUAV = true;  
    RenderTarget->InitCustomFormat(Width, Height, PF_FloatRGBA, false);  
    RenderTarget->UpdateResourceImmediate(true);  
}  
  
UTextureRenderTarget2D* UIComputerShaderObj::GetRenderTarget() const  
{  
    return RenderTarget;  
}
~~~


4. 第四步：**在 UMyFirstComputeShaderObject 里新增 Execute()，真正 Dispatch Compute Shader。**
这一步会把前面三块连起来：
~~~
UMyFirstComputeShaderObject
        ↓
UTextureRenderTarget2D
        ↓
FMyFirstComputeShader
        ↓
MyFirstComputeShader.usf

~~~

~~~ c++
void UMyFirstComputeShaderObject::Execute()
{
	if (!RenderTarget)
	{
		return;
	}

	FTextureRenderTargetResource* RenderTargetResource = RenderTarget->GameThread_GetRenderTargetResource();

	const int32 Width = RenderTarget->SizeX;
	const int32 Height = RenderTarget->SizeY;

	TArray<float> DrawDesc;
	DrawDesc.Add(static_cast<float>(Width));
	DrawDesc.Add(static_cast<float>(Height));

	ENQUEUE_RENDER_COMMAND(ExecuteMyFirstComputeShader)(
		[RenderTargetResource, Width, Height, DrawDesc](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);

			TShaderMapRef<FMyFirstComputeShader> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

			FMyFirstComputeShader::FParameters* PassParameters =
				GraphBuilder.AllocParameters<FMyFirstComputeShader::FParameters>();

			FRDGTextureRef TargetTexture = RegisterExternalTexture(
				GraphBuilder,
				RenderTargetResource->GetRenderTargetTexture(),
				TEXT("MyFirstComputeShader_RenderTarget")
			);

			PassParameters->RenderTarget = GraphBuilder.CreateUAV(TargetTexture);

			FRDGBufferRef DrawDescBuffer = CreateUploadBuffer(
				GraphBuilder,
				TEXT("MyFirstComputeShader_DrawDesc"),
				sizeof(float),
				DrawDesc.Num(),
				DrawDesc.GetData(),
				sizeof(float) * DrawDesc.Num()
			);

			PassParameters->DrawDesc =
				GraphBuilder.CreateSRV(FRDGBufferSRVDesc(DrawDescBuffer, PF_R32_FLOAT));

			const FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(
				FIntVector(Width, Height, 1),
				FIntVector(
					FMyFirstComputeShader::ThreadGroupSizeX,
					FMyFirstComputeShader::ThreadGroupSizeY,
					FMyFirstComputeShader::ThreadGroupSizeZ
				)
			);

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("MyFirstComputeShader"),
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

~~~

这一段做了几件事：
`1. 取得 UTextureRenderTarget2D 的 RenderTargetResource 2. 准备 DrawDesc，告诉 shader 纹理宽高 3. 切到渲染线程 4. 创建 RDG GraphBuilder 5. 找到 FMyFirstComputeShader 6. 把 RenderTarget 绑定成 RWTexture2D UAV 7. 上传 DrawDesc buffer 8. 计算 Dispatch 线程组数量 9. Dispatch shader`

蓝图侧调用顺序就是：
`CreateRenderTarget Execute GetRenderTarget`

### shader中的数据来源
1. 在继承自FGlobalShader的类定义参数结构体
``` C++
BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )  
  
    // 输出 UAV：Compute Shader 会把结果写入这个 RenderTarget。  
    SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, RenderTarget)  
  
    // 输入 SRV：线条绘制描述数据（宽高、偏移等，具体解释由 usf 侧决定）。  
    SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, LineDrawDesc)  
  
    // 输入 SRV：每个采样点的数据（本示例中是波形数据）。  
    SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, LineData)  
  
END_SHADER_PARAMETER_STRUCT()
```

### ComputerShader入口
``` c++
IMPLEMENT_GLOBAL_SHADER(
    FCustomComputeShader,
    "/ComputeShaderShaders/SimpleComputeShader.usf",
    "SimpleComputeShader",
    SF_Compute
);
```
就是告诉 Unreal：

> 编译 /ComputeShaderShaders/SimpleComputeShader.usf 这个文件时，把里面名叫 SimpleComputeShader 的函数当作 Compute Shader 入口。


~~~ C++
[numthreads(THREADS_X, THREADS_Y, THREADS_Z)]  
void SimpleComputeShader(  
    uint3 DispatchThreadId : SV_DispatchThreadID,  
    uint GroupIndex : SV_GroupIndex )  
{}
~~~

	对于函数入口来说[numthreads(THREADS_X, THREADS_Y, THREADS_Z)]是必要的，这行代码地意思是：告诉GPU每一个线程组里有多少个线程，THREADS_X, THREADS_Y, THREADS_Z代表X,Y,Z三个方向的线程数
	
	这里有个关键点：像素数量大于“单个线程组的线程数”没关系，因为你可以 dispatch 很多个线程组。
	[numthreads] 只决定：
	一个线程组里有多少线程，而不是总线程数。
	总线程数由：线程组数量 * 每组线程数量决定。

	比如你要写一张 8192 x 8192 的超大贴图，使用：`[numthreads(16, 16, 1)]`
	每组是：`16 * 16 = 256 个线程`
	那 C++ 侧 dispatch：`GroupCountX = ceil(8192 / 16) = 512 GroupCountY = ceil(8192 / 16) = 512`
	也就是启动：`512 * 512 = 262144 个线程组 每组 256 个线程 总线程 = 67,108,864 个线程`
	刚好覆盖：`8192 * 8192 = 67,108,864 个像素`


所以函数名不是固定必须叫 SimpleComputeShader，但 **HLSL 里的函数名必须和 C++ 这里写的入口名一致**。

执行链路：
蓝图调用 ExecuteComputeShader
    ↓
C++ 进入 UCustomShader::ExecuteComputeShader
    ↓
ENQUEUE_RENDER_COMMAND 把任务丢到渲染线程
    ↓
RDG 创建 Pass 和参数
    ↓
FComputeShaderUtils::Dispatch(...)
    ↓
RHI 发起 GPU Dispatch
    ↓
GPU 执行 usf 里的 SimpleComputeShader


### 重新编译着色器
recompileshaders changed