// Copyright Epic Games, Inc. All Rights Reserved.

#include "Serialization/EditorPackageLoader.h"
#include "Serialization/AsyncPackageLoader.h"
#include "Serialization/AsyncLoadingThread.h"
#include "Serialization/AsyncLoading2.h"
#include "UObject/UObjectThreadContext.h"
#include "Misc/PackageName.h"
#include "IO/IoDispatcher.h"

DEFINE_LOG_CATEGORY_STATIC(LogEditorPackageLoader, Log, All);

#if WITH_IOSTORE_IN_EDITOR

class FEditorPackageLoader final
	: public IAsyncPackageLoader
{
public:
	FEditorPackageLoader(FIoDispatcher& InIoDispatcher, IEDLBootNotificationManager& InEDLBootNotificationManager)
	{
		UncookedPackageLoader.Reset(new FAsyncLoadingThread(/** ThreadIndex = */ 0, InEDLBootNotificationManager));
		CookedPackageLoader.Reset(MakeAsyncPackageLoader2(InIoDispatcher, UncookedPackageLoader.Get()));
	}

	virtual ~FEditorPackageLoader () { }

	virtual void InitializeLoading() override
	{
		if (FIoDispatcher::Get().DoesChunkExist(CreateIoChunkId(0, 0, EIoChunkType::ScriptObjects)))
		{
			UE_LOG(LogEditorPackageLoader, Log, TEXT("Initializing Zen loader for cooked packages in editor startup"));
			CookedPackageLoader->InitializeLoading();
			bHasInitializedCookedPackageLoader = true;
		}
		UncookedPackageLoader->InitializeLoading();
	}

	virtual void ShutdownLoading() override
	{
		CookedPackageLoader->ShutdownLoading();
		UncookedPackageLoader->ShutdownLoading();
	}

	virtual void StartThread() override
	{
		if (!bHasInitializedCookedPackageLoader)
		{
			UE_LOG(LogEditorPackageLoader, Log, TEXT("Initializing Zen loader for cooked packages in editor after startup"));
			CookedPackageLoader->InitializeLoading();
			bHasInitializedCookedPackageLoader = true;
		}
		CookedPackageLoader->StartThread();
		UncookedPackageLoader->StartThread();
	}

	virtual bool ShouldAlwaysLoadPackageAsync(const FPackagePath& InPackagePath) override
	{
		// Use the old loader if an uncooked package exists on disk
		const bool bDoesUncookedPackageExist = FPackageName::DoesPackageExistEx(InPackagePath, FPackageName::EPackageLocationFilter::FileSystem) != FPackageName::EPackageLocationFilter::None;
		return !bDoesUncookedPackageExist;
	}

	virtual int32 LoadPackage(
		const FPackagePath& PackagePath,
		FName CustomPackageName,
		FLoadPackageAsyncDelegate InCompletionDelegate,
		EPackageFlags InPackageFlags,
		int32 InPIEInstanceID,
		int32 InPackagePriority,
		const FLinkerInstancingContext* InstancingContext) override
	{
		// Use the old loader if an uncooked package exists on disk
		if (!bHasInitializedCookedPackageLoader ||
			FPackageName::DoesPackageExistEx(PackagePath, FPackageName::EPackageLocationFilter::FileSystem) != FPackageName::EPackageLocationFilter::None)
		{
			UE_LOG(LogEditorPackageLoader, Verbose, TEXT("Loading uncooked package '%s' from filesystem"), *PackagePath.GetDebugName());
			return UncookedPackageLoader->LoadPackage(PackagePath, CustomPackageName, InCompletionDelegate, InPackageFlags, InPIEInstanceID, InPackagePriority, InstancingContext);
		}
		else
		{
			UE_LOG(LogEditorPackageLoader, Verbose, TEXT("Loading cooked package '%s' from I/O Store"), *PackagePath.GetDebugName());
			return CookedPackageLoader->LoadPackage(PackagePath, CustomPackageName, InCompletionDelegate, InPackageFlags, InPIEInstanceID, InPackagePriority, InstancingContext);
		}
	}

	virtual EAsyncPackageState::Type ProcessLoading(bool bUseTimeLimit, bool bUseFullTimeLimit, double TimeLimit) override
	{
		EAsyncPackageState::Type CookedLoadingState = CookedPackageLoader->ProcessLoading(bUseTimeLimit, bUseFullTimeLimit, TimeLimit);
		EAsyncPackageState::Type UncookedLoadingState = UncookedPackageLoader->ProcessLoading(bUseTimeLimit, bUseFullTimeLimit, TimeLimit);

		return CookedLoadingState == EAsyncPackageState::Complete && UncookedLoadingState == EAsyncPackageState::Complete
			? EAsyncPackageState::Complete
			: EAsyncPackageState::TimeOut;
	}

	virtual EAsyncPackageState::Type ProcessLoadingUntilComplete(TFunctionRef<bool()> CompletionPredicate, double TimeLimit) override
	{
		const EAsyncPackageState::Type LoadingState = CookedPackageLoader->ProcessLoadingUntilComplete(CompletionPredicate, TimeLimit);
		if (LoadingState != EAsyncPackageState::Complete)
		{
			return LoadingState;
		}
		else if (CompletionPredicate())
		{
			return EAsyncPackageState::Complete;
		}
		else
		{
			return UncookedPackageLoader->ProcessLoadingUntilComplete(CompletionPredicate, TimeLimit);
		}
	}

	virtual void CancelLoading() override
	{
		CookedPackageLoader->CancelLoading();
		UncookedPackageLoader->CancelLoading();
	}

	virtual void SuspendLoading() override
	{
		CookedPackageLoader->SuspendLoading();
		UncookedPackageLoader->SuspendLoading();
	}

	virtual void ResumeLoading() override
	{
		CookedPackageLoader->ResumeLoading();
		UncookedPackageLoader->ResumeLoading();
	}

	virtual void FlushLoading(int32 PackageId) override
	{
		CookedPackageLoader->FlushLoading(PackageId);
		UncookedPackageLoader->FlushLoading(PackageId);
	}

	virtual int32 GetNumQueuedPackages() override
	{
		return CookedPackageLoader->GetNumQueuedPackages() + UncookedPackageLoader->GetNumQueuedPackages();
	}

	virtual int32 GetNumAsyncPackages() override
	{
		return CookedPackageLoader->GetNumAsyncPackages() + UncookedPackageLoader->GetNumAsyncPackages();
	}

	virtual float GetAsyncLoadPercentage(const FName& PackageName) override
	{
		float Percentage = CookedPackageLoader->GetAsyncLoadPercentage(PackageName);
		if (Percentage < 0.0f)
		{
			Percentage = UncookedPackageLoader->GetAsyncLoadPercentage(PackageName);
		}

		return Percentage;
	}

	virtual bool IsAsyncLoadingSuspended() override
	{
		return CookedPackageLoader->IsAsyncLoadingSuspended() || UncookedPackageLoader->IsAsyncLoadingSuspended();
	}

	virtual bool IsInAsyncLoadThread() override
	{
		return CookedPackageLoader->IsInAsyncLoadThread() || UncookedPackageLoader->IsInAsyncLoadThread();
	}

	virtual bool IsMultithreaded() override
	{
		check(CookedPackageLoader->IsMultithreaded() == UncookedPackageLoader->IsMultithreaded());
		return CookedPackageLoader->IsMultithreaded();
	}

	virtual bool IsAsyncLoadingPackages() override
	{
		return CookedPackageLoader->IsAsyncLoadingPackages() || UncookedPackageLoader->IsAsyncLoadingPackages();
	}

	virtual void NotifyConstructedDuringAsyncLoading(UObject* Object, bool bSubObject) override
	{
		FUObjectThreadContext& ThreadContext = FUObjectThreadContext::Get();

		if (ThreadContext.AsyncPackageLoader)
		{
			ThreadContext.AsyncPackageLoader->NotifyConstructedDuringAsyncLoading(Object, bSubObject);
		}
	}

	virtual void NotifyUnreachableObjects(const TArrayView<FUObjectItem*>& UnreachableObjects) override
	{
		// Only used in the new loader
		CookedPackageLoader->NotifyUnreachableObjects(UnreachableObjects);
	}

	virtual void FireCompletedCompiledInImport(void* AsyncPackage, FPackageIndex Import) override
	{
		// Only used in the old EDL loader which is not enabled in editor builds
	}

private:
	TUniquePtr<IAsyncPackageLoader> CookedPackageLoader;
	TUniquePtr<IAsyncPackageLoader> UncookedPackageLoader;
	bool bHasInitializedCookedPackageLoader = false;
};

TUniquePtr<IAsyncPackageLoader> MakeEditorPackageLoader(FIoDispatcher& InIoDispatcher, IEDLBootNotificationManager& InEDLBootNotificationManager)
{
	return TUniquePtr<IAsyncPackageLoader>(new FEditorPackageLoader(InIoDispatcher, InEDLBootNotificationManager));
}

#endif // WITH_IOSTORE_IN_EDITOR
