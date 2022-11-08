// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "RemoteExecutionSettings.generated.h"

UCLASS(config = EditorSettings)
class URemoteExecutionSettings : public UObject
{
	GENERATED_UCLASS_BODY()

public:
	/** The remote executor we prefer to use. */
	UPROPERTY(Config, EditAnywhere, Category = "Remote Execution", meta = (DisplayName = "Preferred Executor", ConfigRestartRequired = true))
	FString PreferredRemoteExecutor;
};
