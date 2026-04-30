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
	
	UFUNCTION(BlueprintCallable)
	void Execute();

	UFUNCTION(BlueprintCallable)
	void SetSinWaveData(float offset, float coefficient);
	
private:
	UPROPERTY(Transient)
	UTextureRenderTarget2D* RenderTarget = nullptr;
};
