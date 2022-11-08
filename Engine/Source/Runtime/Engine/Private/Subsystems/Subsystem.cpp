// Copyright Epic Games, Inc. All Rights Reserved.

#include "Subsystems/Subsystem.h"
#include "Subsystems/SubsystemCollection.h"
#include "Engine/Engine.h"

USubsystem::USubsystem()
{

}

int32 USubsystem::GetFunctionCallspace(UFunction* Function, FFrame* Stack)
{
	return GEngine->GetGlobalFunctionCallspace(Function, this, Stack);
}

UDynamicSubsystem::UDynamicSubsystem()
	: USubsystem()
{

}