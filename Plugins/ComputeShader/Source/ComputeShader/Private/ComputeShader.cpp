// Copyright Epic Games, Inc. All Rights Reserved.

#include "ComputeShader.h"

#include "Interfaces/IPluginManager.h"

#define LOCTEXT_NAMESPACE "FComputeShaderModule"

void FComputeShaderModule::StartupModule()
{
	// 模块加载后执行。插件名是 "ComputeShader"，这里通过 IPluginManager 拿到插件根目录。
	// 目标是拼出 "<PluginRoot>/Shaders" 的真实磁盘路径。

	FString PluginShaderDir = FPaths::Combine(IPluginManager::Get().FindPlugin(TEXT("ComputeShader"))->GetBaseDir(), TEXT("Shaders"));

	// 建立“虚拟路径 -> 真实目录”的映射：
	// 之后 C++ 里只要引用 /ComputeShaderShaders/*.usf，编译器就会在 PluginShaderDir 中查找。
	// 这也是 CustomComputeShader.cpp 里 IMPLEMENT_GLOBAL_SHADER 能找到 usf 的关键。
	AddShaderSourceDirectoryMapping(TEXT("/ComputeShaderShaders"), PluginShaderDir);
}

void FComputeShaderModule::ShutdownModule()
{
	// 模块卸载前回调。
	// 当前示例未分配需要手动释放的全局资源，因此这里保持空实现。
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FComputeShaderModule, ComputeShader)
