// Copyright Epic Games, Inc. All Rights Reserved.


#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "Materials/MaterialExpression.h"
#include "MaterialExpressionCustomOutput.generated.h"

UCLASS(abstract,collapsecategories, hidecategories = Object, MinimalAPI)
class UMaterialExpressionCustomOutput : public UMaterialExpression
{
	GENERATED_UCLASS_BODY()

	// Override to enable multiple outputs
	virtual int32 GetNumOutputs() const { return 1; };
	virtual FString GetFunctionName() const PURE_VIRTUAL(UMaterialExpressionCustomOutput::GetFunctionName, return TEXT("GetCustomOutput"););
	virtual FString GetDisplayName() const { return GetFunctionName(); }

#if WITH_EDITOR
	// Allow custom outputs to generate their own source code
	virtual bool HasCustomSourceOutput() { return false; }
	virtual bool AllowMultipleCustomOutputs() { return false; }
	virtual bool NeedsCustomOutputDefines() { return true; }
	virtual bool ShouldCompileBeforeAttributes() { return false; }
	virtual EShaderFrequency GetShaderFrequency() { return SF_Pixel; }
#endif
};



