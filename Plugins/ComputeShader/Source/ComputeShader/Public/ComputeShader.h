// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

/**
 * ComputeShader 插件模块入口。
 *
 * 这个类本身不负责执行计算着色器，而是负责“初始化运行环境”：
 * 1. 在模块启动时把虚拟着色器路径映射到插件 Shaders 目录；
 * 2. 让引擎可以通过 /ComputeShaderShaders/*.usf 找到项目内的 HLSL 文件。
 *
 * 注意：如果这个映射没有建立成功，后续 IMPLEMENT_GLOBAL_SHADER 指向的
 * /ComputeShaderShaders/SimpleComputeShader.usf 将无法被编译/加载。
 */
class FComputeShaderModule : public IModuleInterface
{
public:

	/**
	 * 模块加载后调用（触发时机由 .uplugin 的 LoadingPhase 决定）。
	 * 这里完成 Shader 目录映射，是整个插件可用的前置条件。
	 */
	virtual void StartupModule() override;

	/**
	 * 模块卸载前调用。
	 * 当前示例没有额外资源释放逻辑，保留该函数用于后续扩展。
	 */
	virtual void ShutdownModule() override;
};
