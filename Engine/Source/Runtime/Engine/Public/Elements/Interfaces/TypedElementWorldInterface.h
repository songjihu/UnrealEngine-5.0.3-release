// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Elements/Framework/TypedElementHandle.h"
#include "UObject/Interface.h"

#include "TypedElementWorldInterface.generated.h"

class ULevel;
class UWorld;
class UTypedElementSelectionSet;
struct FCollisionShape;

UENUM()
enum class ETypedElementWorldType : uint8
{
	Game,
	Editor,
};

USTRUCT(BlueprintType)
struct FTypedElementDeletionOptions
{
	GENERATED_BODY()

public:
	FTypedElementDeletionOptions& SetVerifyDeletionCanHappen(const bool InVerifyDeletionCanHappen) { bVerifyDeletionCanHappen = InVerifyDeletionCanHappen; return *this; }
	bool VerifyDeletionCanHappen() const { return bVerifyDeletionCanHappen; }

	FTypedElementDeletionOptions& SetWarnAboutReferences(const bool InWarnAboutReferences) { bWarnAboutReferences = InWarnAboutReferences; return *this; }
	bool WarnAboutReferences() const { return bWarnAboutReferences; }

	FTypedElementDeletionOptions& SetWarnAboutSoftReferences(const bool InWarnAboutSoftReferences) { bWarnAboutSoftReferences = InWarnAboutSoftReferences; return *this; }
	bool WarnAboutSoftReferences() const { return bWarnAboutSoftReferences; }
	
private:
	UPROPERTY(BlueprintReadWrite, Category="TypedElementInterfaces|World|DeletionOptions", meta=(AllowPrivateAccess=true))
	bool bVerifyDeletionCanHappen = false;

	UPROPERTY(BlueprintReadWrite, Category="TypedElementInterfaces|World|DeletionOptions", meta=(AllowPrivateAccess=true))
	bool bWarnAboutReferences = true;

	UPROPERTY(BlueprintReadWrite, Category="TypedElementInterfaces|World|DeletionOptions", meta=(AllowPrivateAccess=true))
	bool bWarnAboutSoftReferences = true;
};

UINTERFACE(MinimalAPI, BlueprintType, meta = (CannotImplementInterfaceInBlueprint))
class UTypedElementWorldInterface : public UInterface
{
	GENERATED_BODY()
};

class ENGINE_API ITypedElementWorldInterface
{
	GENERATED_BODY()

public:
	/**
	 * Is this element considered a template within its world (eg, a CDO or archetype).
	 */
	virtual bool IsTemplateElement(const FTypedElementHandle& InElementHandle) { return false; }

	/**
	 * Can this element actually be edited in the world?
	 */
	virtual bool CanEditElement(const FTypedElementHandle& InElementHandle) { return true; }

	/**
	 * Get the owner level associated with this element, if any.
	 */
	virtual ULevel* GetOwnerLevel(const FTypedElementHandle& InElementHandle) { return nullptr; }

	/**
	 * Get the owner world associated with this element, if any.
	 */
	virtual UWorld* GetOwnerWorld(const FTypedElementHandle& InElementHandle) { return nullptr; }

	/**
	 * Get the bounds of this element, if any.
	 */
	virtual bool GetBounds(const FTypedElementHandle& InElementHandle, FBoxSphereBounds& OutBounds) { return false; }

	/**
	 * Can the given element be moved within the world?
	 */
	virtual bool CanMoveElement(const FTypedElementHandle& InElementHandle, const ETypedElementWorldType InWorldType) { return false; }

	/**
	 * Get the transform of this element within its owner world, if any.
	 */
	virtual bool GetWorldTransform(const FTypedElementHandle& InElementHandle, FTransform& OutTransform) { return false; }
	
	/**
	 * Attempt to set the transform of this element within its owner world.
	 */
	virtual bool SetWorldTransform(const FTypedElementHandle& InElementHandle, const FTransform& InTransform) { return false; }

	/**
	 * Get the transform of this element relative to its parent, if any.
	 */
	virtual bool GetRelativeTransform(const FTypedElementHandle& InElementHandle, FTransform& OutTransform) { return GetWorldTransform(InElementHandle, OutTransform); }
	
	/**
	 * Attempt to set the transform of this element relative to its parent.
	 */
	virtual bool SetRelativeTransform(const FTypedElementHandle& InElementHandle, const FTransform& InTransform) { return SetWorldTransform(InElementHandle, InTransform); }

	/**
	 * Get the local space offset of this element that should be added to its pivot location, if any.
	 */
	virtual bool GetPivotOffset(const FTypedElementHandle& InElementHandle, FVector& OutPivotOffset) { return false; }

	/**
	 * Attempt to set the local space offset of this element that should be added to its pivot location.
	 */
	virtual bool SetPivotOffset(const FTypedElementHandle& InElementHandle, const FVector& InPivotOffset) { return false; }

	/**
	 * Notify that this element is about to be moved.
	 */
	virtual void NotifyMovementStarted(const FTypedElementHandle& InElementHandle) {}

	/**
	 * Notify that this element is currently being moved.
	 */
	virtual void NotifyMovementOngoing(const FTypedElementHandle& InElementHandle) {}

	/**
	 * Notify that this element is done being moved.
	 */
	virtual void NotifyMovementEnded(const FTypedElementHandle& InElementHandle) {}

	/**
	 * Attempt to find a suitable (non-intersecting) transform for the given element at the given point.
	 */
	virtual bool FindSuitableTransformAtPoint(const FTypedElementHandle& InElementHandle, const FTransform& InPotentialTransform, FTransform& OutSuitableTransform)
	{
		OutSuitableTransform = InPotentialTransform;
		return true;
	}

	/**
	 * Attempt to find a suitable (non-intersecting) transform for the given element along the given path.
	 */
	virtual bool FindSuitableTransformAlongPath(const FTypedElementHandle& InElementHandle, const FVector& InPathStart, const FVector& InPathEnd, const FCollisionShape& InTestShape, TArrayView<const FTypedElementHandle> InElementsToIgnore, FTransform& OutSuitableTransform)
	{
		return false;
	}

	/**
	 * Can the given element be deleted?
	 */
	virtual bool CanDeleteElement(const FTypedElementHandle& InElementHandle) { return false; }

	/**
	 * Delete the given element.
	 * @note Default version calls DeleteElements with a single element.
	 */
	virtual bool DeleteElement(const FTypedElementHandle& InElementHandle, UWorld* InWorld, UTypedElementSelectionSet* InSelectionSet, const FTypedElementDeletionOptions& InDeletionOptions)
	{
		return DeleteElements(MakeArrayView(&InElementHandle, 1), InWorld, InSelectionSet, InDeletionOptions);
	}

	/**
	 * Delete the given set of elements.
	 * @note If you want to delete an array of elements that are potentially different types, you probably want to use the higher-level UTypedElementCommonActions::DeleteNormalizedElements function instead.
	 */
	virtual bool DeleteElements(TArrayView<const FTypedElementHandle> InElementHandles, UWorld* InWorld, UTypedElementSelectionSet* InSelectionSet, const FTypedElementDeletionOptions& InDeletionOptions) { return false; }

	/**
	 * Can the given element be duplicated?
	 */
	virtual bool CanDuplicateElement(const FTypedElementHandle& InElementHandle) { return false; }

	/**
	 * Duplicate the given element.
	 * @note Default version calls DuplicateElements with a single element.
	 */
	virtual FTypedElementHandle DuplicateElement(const FTypedElementHandle& InElementHandle, UWorld* InWorld, const FVector& InLocationOffset)
	{
		TArray<FTypedElementHandle> NewElements;
		DuplicateElements(MakeArrayView(&InElementHandle, 1), InWorld, InLocationOffset, NewElements);
		return NewElements.Num() > 0 ? MoveTemp(NewElements[0]) : FTypedElementHandle();
	}

	/**
	 * Duplicate the given set of elements.
	 * @note If you want to duplicate an array of elements that are potentially different types, you probably want to use the higher-level UTypedElementCommonActions::DuplicateNormalizedElements function instead.
	 */
	virtual void DuplicateElements(TArrayView<const FTypedElementHandle> InElementHandles, UWorld* InWorld, const FVector& InLocationOffset, TArray<FTypedElementHandle>& OutNewElements)
	{
	}


	/**
	 * Script Api
	 */

	/**
	 * Is this element considered a template within its world (eg, a CDO or archetype).
	 */
	UFUNCTION(BlueprintCallable, Category="TypedElementInterfaces|World")
	virtual bool IsTemplateElement(const FScriptTypedElementHandle& InElementHandle);

	/**
	 * Can this element actually be edited in the world?
	 */
	UFUNCTION(BlueprintCallable, Category="TypedElementInterfaces|World")
	virtual bool CanEditElement(const FScriptTypedElementHandle& InElementHandle);

	/**
	 * Get the owner level associated with this element, if any.
	 */
	UFUNCTION(BlueprintCallable, Category="TypedElementInterfaces|World")
	virtual ULevel* GetOwnerLevel(const FScriptTypedElementHandle& InElementHandle);

	/**
	 * Get the owner world associated with this element, if any.
	 */
	UFUNCTION(BlueprintCallable, Category="TypedElementInterfaces|World")
	virtual UWorld* GetOwnerWorld(const FScriptTypedElementHandle& InElementHandle);

	/**
	 * Get the bounds of this element, if any.
	 */
	UFUNCTION(BlueprintCallable, Category="TypedElementInterfaces|World")
	virtual bool GetBounds(const FScriptTypedElementHandle& InElementHandle, FBoxSphereBounds& OutBounds);

	/**
	 * Can the given element be moved within the world?
	 */
	UFUNCTION(BlueprintCallable, Category="TypedElementInterfaces|World")
	virtual bool CanMoveElement(const FScriptTypedElementHandle& InElementHandle, const ETypedElementWorldType InWorldType);

	/**
	 * Get the transform of this element within its owner world, if any.
	 */
	UFUNCTION(BlueprintCallable, Category="TypedElementInterfaces|World")
	virtual bool GetWorldTransform(const FScriptTypedElementHandle& InElementHandle, FTransform& OutTransform);
	
	/**
	 * Attempt to set the transform of this element within its owner world.
	 */
	UFUNCTION(BlueprintCallable, Category="TypedElementInterfaces|World")
	virtual bool SetWorldTransform(const FScriptTypedElementHandle& InElementHandle, const FTransform& InTransform);

	/**
	 * Get the transform of this element relative to its parent, if any.
	 */
	UFUNCTION(BlueprintCallable, Category="TypedElementInterfaces|World")
	virtual bool GetRelativeTransform(const FScriptTypedElementHandle& InElementHandle, FTransform& OutTransform);
	
	/**
	 * Attempt to set the transform of this element relative to its parent.
	 */
	UFUNCTION(BlueprintCallable, Category="TypedElementInterfaces|World")
	virtual bool SetRelativeTransform(const FScriptTypedElementHandle& InElementHandle, const FTransform& InTransform);

	/**
	 * Get the local space offset of this element that should be added to its pivot location, if any.
	 */
	UFUNCTION(BlueprintCallable, Category="TypedElementInterfaces|World")
	virtual bool GetPivotOffset(const FScriptTypedElementHandle& InElementHandle, FVector& OutPivotOffset);

	/**
	 * Attempt to set the local space offset of this element that should be added to its pivot location.
	 */
	UFUNCTION(BlueprintCallable, Category="TypedElementInterfaces|World")
	virtual bool SetPivotOffset(const FScriptTypedElementHandle& InElementHandle, const FVector& InPivotOffset);

	/**
	 * Notify that this element is about to be moved.
	 */
	UFUNCTION(BlueprintCallable, Category="TypedElementInterfaces|World")
	virtual void NotifyMovementStarted(const FScriptTypedElementHandle& InElementHandle);

	/**
	 * Notify that this element is currently being moved.
	 */
	UFUNCTION(BlueprintCallable, Category="TypedElementInterfaces|World")
	virtual void NotifyMovementOngoing(const FScriptTypedElementHandle& InElementHandle);

	/**
	 * Notify that this element is done being moved.
	 */
	UFUNCTION(BlueprintCallable, Category="TypedElementInterfaces|World")
	virtual void NotifyMovementEnded(const FScriptTypedElementHandle& InElementHandle);

	/**
	 * Can the given element be deleted?
	 */
	UFUNCTION(BlueprintCallable, Category="TypedElementInterfaces|World")
	virtual bool CanDeleteElement(const FScriptTypedElementHandle& InElementHandle);

	/**
	 * Delete the given element.
	 * @note Default version calls DeleteElements with a single element.
	 */
	UFUNCTION(BlueprintCallable, Category="TypedElementInterfaces|World")
	virtual bool DeleteElement(const FScriptTypedElementHandle& InElementHandle, UWorld* InWorld, UTypedElementSelectionSet* InSelectionSet, const FTypedElementDeletionOptions& InDeletionOptions);

	/**
	 * Can the given element be duplicated?
	 */
	UFUNCTION(BlueprintCallable, Category="TypedElementInterfaces|World")
	virtual bool CanDuplicateElement(const FScriptTypedElementHandle& InElementHandle);

	/**
	 * Duplicate the given element.
	 * @note Default version calls DuplicateElements with a single element.
	 */
	UFUNCTION(BlueprintCallable, Category="TypedElementInterfaces|World")
	virtual FScriptTypedElementHandle DuplicateElement(const FScriptTypedElementHandle& InElementHandle, UWorld* InWorld, const FVector& InLocationOffset);

private:
	/**
	 * Return the registry associated with this interface implementation
	 */
	virtual class UTypedElementRegistry& GetRegistry() const;
};

template <>
struct TTypedElement<ITypedElementWorldInterface> : public TTypedElementBase<ITypedElementWorldInterface>
{
	bool IsTemplateElement() const { return InterfacePtr->IsTemplateElement(*this); }
	bool CanEditElement() const { return InterfacePtr->CanEditElement(*this); }
	ULevel* GetOwnerLevel() const { return InterfacePtr->GetOwnerLevel(*this); }
	UWorld* GetOwnerWorld() const { return InterfacePtr->GetOwnerWorld(*this); }
	bool GetBounds(FBoxSphereBounds& OutBounds) const { return InterfacePtr->GetBounds(*this, OutBounds); }
	bool CanMoveElement(const ETypedElementWorldType InWorldType) const { return InterfacePtr->CanMoveElement(*this, InWorldType); }
	bool GetWorldTransform(FTransform& OutTransform) const { return InterfacePtr->GetWorldTransform(*this, OutTransform); }
	bool SetWorldTransform(const FTransform& InTransform) const { return InterfacePtr->SetWorldTransform(*this, InTransform); }
	bool GetRelativeTransform(FTransform& OutTransform) const { return InterfacePtr->GetRelativeTransform(*this, OutTransform); }
	bool SetRelativeTransform(const FTransform& InTransform) const { return InterfacePtr->SetRelativeTransform(*this, InTransform); }
	bool GetPivotOffset(FVector& OutPivotOffset) const { return InterfacePtr->GetPivotOffset(*this, OutPivotOffset); }
	bool SetPivotOffset(const FVector& InPivotOffset) const { return InterfacePtr->SetPivotOffset(*this, InPivotOffset); }
	void NotifyMovementStarted() const { InterfacePtr->NotifyMovementStarted(*this); }
	void NotifyMovementOngoing() const { InterfacePtr->NotifyMovementOngoing(*this); }
	void NotifyMovementEnded() const { InterfacePtr->NotifyMovementEnded(*this); }
	bool FindSuitableTransformAtPoint(const FTransform& InPotentialTransform, FTransform& OutSuitableTransform) const { return InterfacePtr->FindSuitableTransformAtPoint(*this, InPotentialTransform, OutSuitableTransform); }
	bool FindSuitableTransformAlongPath(const FVector& InPathStart, const FVector& InPathEnd, const FCollisionShape& InTestShape, TArrayView<const FTypedElementHandle> InElementsToIgnore, FTransform& OutSuitableTransform) const { return InterfacePtr->FindSuitableTransformAlongPath(*this, InPathStart, InPathEnd, InTestShape, InElementsToIgnore, OutSuitableTransform); }
	bool CanDeleteElement() const { return InterfacePtr->CanDeleteElement(*this); }
	bool DeleteElement(UWorld* InWorld, UTypedElementSelectionSet* InSelectionSet, const FTypedElementDeletionOptions& InDeletionOptions) const { return InterfacePtr->DeleteElement(*this, InWorld, InSelectionSet, InDeletionOptions); }
	bool CanDuplicateElement() const { return InterfacePtr->CanDuplicateElement(*this); }
	FTypedElementHandle DuplicateElement(UWorld* InWorld, const FVector& InLocationOffset) const { return InterfacePtr->DuplicateElement(*this, InWorld, InLocationOffset); }
};
