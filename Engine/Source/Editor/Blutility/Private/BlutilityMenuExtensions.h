// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "UObject/ObjectMacros.h"
#include "BlutilityMenuExtensions.generated.h"

struct FAssetData;
class FMenuBuilder;

// Blutility Menu extension helpers
class FBlutilityMenuExtensions
{
public:
	/** Helper function to get all Blutility classes derived from the specified class name */
	static void GetBlutilityClasses(TArray<FAssetData>& OutAssets, const FName& InClassName);

	/** Helper function that populates a menu based on the exposed functions in a set of Blutility objects */
	static void CreateAssetBlutilityActionsMenu(FMenuBuilder& MenuBuilder, TMap<class IEditorUtilityExtension*, TSet<int32>> Utils, const TArray<FAssetData> SelectedSupportedAssets);
	static void CreateActorBlutilityActionsMenu(FMenuBuilder& MenuBuilder, TMap<class IEditorUtilityExtension*, TSet<int32>> Utils, const TArray<AActor*> SelectedSupportedActors);
	
protected:
	// Helper struct to track the util to call a function on
	struct FFunctionAndUtil
	{
		FFunctionAndUtil(UFunction* InFunction, IEditorUtilityExtension* InUtil, TSet<int32>& InSelection)
			: Function(InFunction)
			, Util(InUtil) 
			, SelectionIndices(InSelection)
		{}

		bool operator==(const FFunctionAndUtil& InFunction) const
		{
			return InFunction.Function == Function && InFunction.Util == Util;
		}

		UFunction* Function;
		IEditorUtilityExtension* Util;
		/** Indices to original object selection array which are supported by this utility */
		TSet<int32> SelectionIndices;
	};

protected:
	template<typename SelectionType>
	static void CreateBlutilityActionsMenu(FMenuBuilder& MenuBuilder, TMap<class IEditorUtilityExtension*, TSet<int32>> Utils, const FText& MenuLabel, const FText& MenuToolTip, TFunction<bool(const FProperty * Property)> IsValidPropertyType, const TArray<SelectionType> Selection, const FName& IconName = "GraphEditor.Event_16x");

	static void OpenEditorForUtility(const FFunctionAndUtil& FunctionAndUtil);
	static void ExtractFunctions(TMap<class IEditorUtilityExtension*, TSet<int32>>& Utils, TMap<FString, TArray<FFunctionAndUtil>>& OutCategoryFunctions);
};

UINTERFACE(BlueprintType)
class UEditorUtilityExtension : public UInterface
{
	GENERATED_UINTERFACE_BODY()
};

class IEditorUtilityExtension
{
	GENERATED_IINTERFACE_BODY()
};