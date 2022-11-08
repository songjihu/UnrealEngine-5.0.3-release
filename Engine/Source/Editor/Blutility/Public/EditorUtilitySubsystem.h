// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "EditorSubsystem.h"

#include "UObject/ObjectMacros.h"
#include "UObject/UObjectGlobals.h"
#include "Templates/SubclassOf.h"
#include "Containers/Queue.h"
#include "Containers/Ticker.h"

#include "EditorUtilitySubsystem.generated.h"

class SWindow;
class UEditorUtilityWidget;
class UEditorUtilityTask;

/** Delegate for a PIE event exposed via Editor Utility (begin, end, pause/resume, etc) */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnEditorUtilityPIEEvent, const bool, bIsSimulating);

UCLASS(config = EditorPerProjectUserSettings)
class BLUTILITY_API UEditorUtilitySubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	UEditorUtilitySubsystem();

	virtual void Initialize(FSubsystemCollectionBase& Collection);
	virtual void Deinitialize();

	static void AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector);

	void MainFrameCreationFinished(TSharedPtr<SWindow> InRootWindow, bool bIsNewProjectWindow);
	void HandleStartup();

	UPROPERTY(config)
	TArray<FSoftObjectPath> LoadedUIs;

	UPROPERTY(config)
	TArray<FSoftObjectPath> StartupObjects;

	TMap<FName, UEditorUtilityWidgetBlueprint*> RegisteredTabs;

	// Allow startup object to be garbage collected
	UFUNCTION(BlueprintCallable, Category = "Development|Editor")
	void ReleaseInstanceOfAsset(UObject* Asset);

	UFUNCTION(BlueprintCallable, Category = "Development|Editor")
	bool TryRun(UObject* Asset);

	UFUNCTION(BlueprintCallable, Category = "Development|Editor")
	bool CanRun(UObject* Asset) const;

	UFUNCTION(BlueprintCallable, Category = "Development|Editor")
	UEditorUtilityWidget* SpawnAndRegisterTabAndGetID(class UEditorUtilityWidgetBlueprint* InBlueprint, FName& NewTabID);

	UFUNCTION(BlueprintCallable, Category = "Development|Editor")
	UEditorUtilityWidget* SpawnAndRegisterTab(class UEditorUtilityWidgetBlueprint* InBlueprint);

	UFUNCTION(BlueprintCallable, Category = "Development|Editor")
	void RegisterTabAndGetID(class UEditorUtilityWidgetBlueprint* InBlueprint, FName& NewTabID);

	/** Given an ID for a tab, try to find a tab spawner that matches, and then spawn a tab. Returns true if it was able to find a matching tab spawner */
	UFUNCTION(BlueprintCallable, Category = "Development|Editor")
	bool SpawnRegisteredTabByID(FName NewTabID);

	/** Given an ID for a tab, try to find an existing tab. Returns true if it found a tab. */
	UFUNCTION(BlueprintCallable, Category = "Development|Editor")
	bool DoesTabExist(FName NewTabID);

	/** Given an ID for a tab, try to find and close an existing tab. Returns true if it found a tab to close. */
	UFUNCTION(BlueprintCallable, Category = "Development|Editor")
	bool CloseTabByID(FName NewTabID);

	/** Given an editor utility widget blueprint, get the widget it creates. This will return a null pointer if the widget is not currently in a tab.*/
	UFUNCTION(BlueprintCallable, Category = "Development|Editor")
	UEditorUtilityWidget* FindUtilityWidgetFromBlueprint(class UEditorUtilityWidgetBlueprint* InBlueprint);

	/**  */
	UFUNCTION(BlueprintCallable, Category = "Development|Editor")
	void RegisterAndExecuteTask(UEditorUtilityTask* NewTask, UEditorUtilityTask* OptionalParentTask = nullptr);

	void RemoveTaskFromActiveList(UEditorUtilityTask* Task);

	void RegisterReferencedObject(UObject* ObjectToReference);
	void UnregisterReferencedObject(UObject* ObjectToReference);

	/** Expose Begin PIE to blueprints.*/
	UPROPERTY(BlueprintAssignable)
	FOnEditorUtilityPIEEvent OnBeginPIE;

	/** Expose End PIE to blueprints.*/
	UPROPERTY(BlueprintAssignable)
	FOnEditorUtilityPIEEvent OnEndPIE;

protected:
	UEditorUtilityTask* GetActiveTask() { return ActiveTaskStack.Num() > 0 ? ActiveTaskStack[ActiveTaskStack.Num() - 1] : nullptr; };

	void StartTask(UEditorUtilityTask* Task);

	bool Tick(float DeltaTime);

	void ProcessRunTaskCommands();

	void RunTaskCommand(const TArray<FString>& Params, UWorld* InWorld, FOutputDevice& Ar);
	void CancelAllTasksCommand(const TArray<FString>& Params, UWorld* InWorld, FOutputDevice& Ar);

	UClass* FindClassByName(const FString& RawTargetName);
	UClass* FindBlueprintClass(const FString& TargetNameRaw);

	/** Called when Play in Editor begins. */
	void HandleOnBeginPIE(const bool bIsSimulating);

	/** Called when Play in Editor stops. */
	void HandleOnEndPIE(const bool bIsSimulating);

private:
	IConsoleObject* RunTaskCommandObject = nullptr;
	IConsoleObject* CancelAllTasksCommandObject = nullptr;
	
	UPROPERTY()
	TMap<TObjectPtr<UObject> /*Asset*/, TObjectPtr<UObject> /*Instance*/> ObjectInstances;

	TQueue< TArray<FString> > RunTaskCommandBuffer;

	/** AddReferencedObjects is used to report these references to GC. */
	TMap<TObjectPtr<UEditorUtilityTask>, TArray<TObjectPtr<UEditorUtilityTask>>> PendingTasks;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UEditorUtilityTask>> ActiveTaskStack;

	FTSTicker::FDelegateHandle TickerHandle;

	/** List of objects that are being kept alive by this subsystem. */
	UPROPERTY()
	TSet<TObjectPtr<UObject>> ReferencedObjects;
};
