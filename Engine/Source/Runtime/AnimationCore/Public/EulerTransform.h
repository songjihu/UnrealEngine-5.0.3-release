// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "EulerTransform.generated.h"

UENUM()
enum class EEulerRotationOrder : uint8
{
	XYZ,
	XZY,
	YXZ,
	YZX,
	ZXY,
	ZYX
};

USTRUCT(BlueprintType)
struct ANIMATIONCORE_API FEulerTransform
{
	GENERATED_BODY()

	/**
	 * The identity transformation (Rotation = FRotator::ZeroRotator, Translation = FVector::ZeroVector, Scale = (1,1,1)).
	 */
	static const FEulerTransform Identity;

	FORCEINLINE FEulerTransform()
		: Location(ForceInitToZero)
		, Rotation(ForceInitToZero)
		, Scale(FVector::OneVector)
	{
	}

	FORCEINLINE FEulerTransform(const FVector& InLocation, const FRotator& InRotation, const FVector& InScale)
		: Location(InLocation)
		, Rotation(InRotation)
		, Scale(InScale)
	{
	}

	FORCEINLINE FEulerTransform(const FRotator& InRotation, const FVector& InLocation, const FVector& InScale)
		: Location(InLocation)
		, Rotation(InRotation)
		, Scale(InScale)
	{
	}

	FORCEINLINE FEulerTransform(const FTransform& InTransform)
		: Location(InTransform.GetLocation())
		, Rotation(InTransform.GetRotation().Rotator())
		, Scale(InTransform.GetScale3D())
	{

	}

	FORCEINLINE FEulerTransform& operator =(const FTransform& InTransform)
	{
		FromFTransform(InTransform);
		return *this;
	}

	FORCEINLINE operator FTransform() const
	{
		return ToFTransform();
	}

	/** The translation of this transform */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Transform")
	FVector Location;

	/** The rotation of this transform */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Transform")
	FRotator Rotation;

	/** The scale of this transform */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Transform")
	FVector Scale;

	/** Convert to an FTransform */
	FORCEINLINE FTransform ToFTransform() const
	{
		return FTransform(Rotation.Quaternion(), Location, Scale);
	}

	/** Convert from an FTransform */
	FORCEINLINE void FromFTransform(const FTransform& InTransform)
	{
		Location = InTransform.GetLocation();
		Rotation = InTransform.GetRotation().Rotator();
		Scale = InTransform.GetScale3D();
	}

	FORCEINLINE const FVector& GetLocation() const { return Location; }
	FORCEINLINE FQuat GetRotation() const { return Rotation.Quaternion(); }
	FORCEINLINE const FRotator& Rotator() const { return Rotation; }
	FORCEINLINE const FVector& GetScale3D() const { return Scale; }
	FORCEINLINE void SetLocation(const FVector& InValue) { Location = InValue; }
	FORCEINLINE void SetRotation(const FQuat& InValue) { Rotation = InValue.Rotator(); }
	FORCEINLINE void SetRotator(const FRotator& InValue) { Rotation = InValue; }
	FORCEINLINE void SetScale3D(const FVector& InValue) { Scale = InValue; }
	FORCEINLINE void NormalizeRotation() {}
};

template<> struct TBaseStructure<FEulerTransform>
{
	ANIMATIONCORE_API static UScriptStruct* Get() { return FEulerTransform::StaticStruct(); }
};
