// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RigVMExecuteContext.h"
#include "RigVMArray.h"
#include "RigVMMemory.h"
#include "Blueprint/BlueprintSupport.h"

#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED

typedef FRigVMFixedArray<FRigVMMemoryHandle> FRigVMMemoryHandleArray;
typedef FRigVMFixedArray<void*> FRigVMUserDataArray;

#else

typedef TArrayView<FRigVMMemoryHandle> FRigVMMemoryHandleArray;
typedef TArrayView<void*> FRigVMUserDataArray;

#endif

typedef void (*FRigVMFunctionPtr)(FRigVMExecuteContext& RigVMExecuteContext, FRigVMMemoryHandleArray RigVMMemoryHandles);

/**
 * The Pin Direction is used to differentiate different kinds of 
 * pins in the data flow graph - inputs, outputs etc.
 */
UENUM(BlueprintType)
enum class ERigVMPinDirection : uint8
{
	Input, // A const input value
	Output, // A mutable output value
	IO, // A mutable input and output value
	Visible, // A const value that cannot be connected to
	Hidden, // A mutable hidden value (used for interal state)
	Invalid // The max value for this enum - used for guarding.
};

/**
 * The FRigVMFunction is used to represent a function pointer generated by UHT
 * for a given name. The name might be something like "FMyStruct::MyVirtualMethod"
 */
struct RIGVM_API FRigVMFunction
{
	const TCHAR* Name;
	UScriptStruct* Struct;
	FRigVMFunctionPtr FunctionPtr;
	int32 Index;
	int32 PrototypeIndex;

	FRigVMFunction()
		: Name(nullptr)
		, Struct(nullptr)
		, FunctionPtr(nullptr)
		, Index(INDEX_NONE)
		, PrototypeIndex(INDEX_NONE)
	{
	}

	FRigVMFunction(const TCHAR* InName, FRigVMFunctionPtr InFunctionPtr, UScriptStruct* InStruct = nullptr, int32 InIndex = INDEX_NONE)
		: Name(InName)
		, Struct(InStruct)
		, FunctionPtr(InFunctionPtr)
		, Index(InIndex)
		, PrototypeIndex(INDEX_NONE)
	{
	}

	FORCEINLINE bool IsValid() const { return Name != nullptr && FunctionPtr != nullptr; }
	FName GetMethodName() const;
	FString GetModuleName() const;
	FString GetModuleRelativeHeaderPath() const;
};
