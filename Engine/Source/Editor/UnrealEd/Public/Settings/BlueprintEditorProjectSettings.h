// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "Engine/DeveloperSettings.h"
#include "Components/ChildActorComponent.h"

#include "BlueprintEditorProjectSettings.generated.h"


UCLASS(config=Editor, meta=(DisplayName="Blueprint Project Settings"), defaultconfig)
class UNREALED_API UBlueprintEditorProjectSettings : public UDeveloperSettings
{
	GENERATED_UCLASS_BODY()

public:
	/**
	 * Flag to disable faster compiles for individual blueprints if they have no function signature
	 * changes. This flag is deprecated! In 4.21 there will be no way to force all dependencies to 
	 * compile when no changes are detected. Report any issues immediately.
	 */
	UPROPERTY(EditAnywhere, config, Category=Blueprints, DisplayName = "Force All Dependencies To Recompile (DEPRECATED)")
	uint8 bForceAllDependenciesToRecompile:1;

	/** If enabled, the editor will load packages to look for soft references to actors when deleting/renaming them. This can be slow in large projects so disable this to improve performance but increase the chance of breaking blueprints/sequences that use soft actor references */
	UPROPERTY(EditAnywhere, config, Category=Actors)
	uint8 bValidateUnloadedSoftActorReferences : 1;

	/**
	 * Enable the option to expand child actor components within component tree views (experimental).
	 */
	UPROPERTY(EditAnywhere, config, Category = Experimental)
	uint8 bEnableChildActorExpansionInTreeView : 1;

	/**
	 * Default view mode to use for child actor components in a Blueprint actor's component tree hierarchy (experimental).
	 */
	UPROPERTY(EditAnywhere, config, Category = Experimental, meta = (EditCondition = "bEnableChildActorExpansionInTreeView"))
	EChildActorComponentTreeViewVisualizationMode DefaultChildActorTreeViewMode;

	// The list of namespaces to always expose in any Blueprint (for all users of the game/project). Requires Blueprint namespace features to be enabled in editor preferences.
	UPROPERTY(EditAnywhere, config, Category = Experimental)
	TArray<FString> NamespacesToAlwaysInclude;

	/** 
	 * List of compiler messages that have been suppressed outside of full, interactive editor sessions for 
	 * the current project - useful for silencing warnings that were added to the engine after 
	 * project inception and are going to be addressed as they are found by content authors
	 */
	UPROPERTY(EditAnywhere, config, Category= Blueprints, DisplayName = "Compiler Messages Disabled Except in Editor")
	TArray<FName> DisabledCompilerMessagesExceptEditor;
	
	/** 
	 * List of compiler messages that have been suppressed completely - message suppression is only 
	 * advisable when using blueprints that you cannot update and are raising innocuous warnings. 
	 * If useless messages are being raised prefer to contact support rather than disabling messages
	 */
	UPROPERTY(EditAnywhere, config, Category= Blueprints, DisplayName = "Compiler Messages Disabled Entirely")
	TArray<FName> DisabledCompilerMessages;

	/**
	 * Any blueprint deriving from one of these base classes will be allowed to recompile during Play-in-Editor
	 * (This setting exists both as an editor preference and project setting, and will be allowed if listed in either place) 
	 */
	UPROPERTY(EditAnywhere, config, Category=Play, meta=(AllowAbstract))
	TArray<TSoftClassPtr<UObject>> BaseClassesToAllowRecompilingDuringPlayInEditor;

public:
	// UObject interface
	virtual void PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent) override;
	// End of UObject interface
};
