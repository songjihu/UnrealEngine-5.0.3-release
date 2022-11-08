// Copyright Epic Games, Inc. All Rights Reserved.

#include "EditorDomain/EditorDomain.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Async/AsyncFileHandleNull.h"
#include "Containers/UnrealString.h"
#include "CoreGlobals.h"
#include "DerivedDataCache.h"
#include "DerivedDataCacheRecord.h"
#include "EditorDomain/EditorDomainArchive.h"
#include "EditorDomain/EditorDomainSave.h"
#include "EditorDomain/EditorDomainUtils.h"
#include "HAL/CriticalSection.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CoreDelegates.h"
#include "Misc/PackagePath.h"
#include "Misc/PackageSegment.h"
#include "Misc/ScopeLock.h"
#include "Serialization/Archive.h"
#include "Serialization/CompactBinary.h"
#include "TargetDomain/TargetDomainUtils.h"
#include "Templates/RefCounting.h"
#include "Templates/UniquePtr.h"
#include "UObject/PackageResourceManagerFile.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

DEFINE_LOG_CATEGORY(LogEditorDomain);

/** Add a hook to the PackageResourceManager's startup delegate to use the EditorDomain as the IPackageResourceManager */
class FEditorDomainRegisterAsPackageResourceManager
{
public:
	FEditorDomainRegisterAsPackageResourceManager()
	{
		IPackageResourceManager::GetSetPackageResourceManagerDelegate().BindStatic(SetPackageResourceManager);
	}

	static IPackageResourceManager* SetPackageResourceManager()
	{
		bool bEditorDomainEnabled = IsEditorDomainEnabled();
		if (GIsEditor)
		{
			UE_LOG(LogEditorDomain, Display, TEXT("EditorDomain is %s"), bEditorDomainEnabled ? TEXT("Enabled") : TEXT("Disabled"));
		}
		if (bEditorDomainEnabled)
		{
			UE::EditorDomain::UtilsInitialize();
			UE::TargetDomain::UtilsInitialize(bEditorDomainEnabled);
			if (bEditorDomainEnabled)
			{
				// Set values for config settings EditorDomain depends on
				GAllowUnversionedContentInEditor = 1;

				// Create the editor domain and return it as the package resource manager
				check(FEditorDomain::RegisteredEditorDomain == nullptr);
				FEditorDomain::RegisteredEditorDomain = new FEditorDomain();
				return FEditorDomain::RegisteredEditorDomain;
			}
		}
		return nullptr;
	}
} GRegisterAsPackageResourceManager;

FEditorDomain* FEditorDomain::RegisteredEditorDomain = nullptr;

FEditorDomain::FEditorDomain()
{
	Locks = TRefCountPtr<FLocks>(new FLocks(*this));
	Workspace.Reset(MakePackageResourceManagerFile());
	GConfig->GetBool(TEXT("CookSettings"), TEXT("EditorDomainExternalSave"), bExternalSave, GEditorIni);
	if (bExternalSave)
	{
		SaveClient.Reset(new FEditorDomainSaveClient());
	}
	bSkipSavesUntilCatalogLoaded = GIsBuildMachine;

	AssetRegistry = IAssetRegistry::Get();
	// We require calling SearchAllAssets, because we rely on being able to call WaitOnAsset
	// without needing to call ScanPathsSynchronous
	AssetRegistry->SearchAllAssets(false /* bSynchronousSearch */);

	bEditorDomainReadEnabled = !FParse::Param(FCommandLine::Get(), TEXT("noeditordomainread"));

	ELoadingPhase::Type CurrentPhase = IPluginManager::Get().GetLastCompletedLoadingPhase();
	if (CurrentPhase == ELoadingPhase::None || CurrentPhase < ELoadingPhase::PostEngineInit)
	{
		FCoreDelegates::OnPostEngineInit.AddRaw(this, &FEditorDomain::OnPostEngineInit);
	}
	else
	{
		OnPostEngineInit();
	}
	FCoreUObjectDelegates::OnEndLoadPackage.AddRaw(this, &FEditorDomain::OnEndLoadPackage);
	UPackage::PackageSavedWithContextEvent.AddRaw(this, &FEditorDomain::OnPackageSavedWithContext);
	AssetRegistry->OnAssetUpdatedOnDisk().AddRaw(this, &FEditorDomain::OnAssetUpdatedOnDisk);
}

FEditorDomain::~FEditorDomain()
{
	TUniquePtr<UE::DerivedData::FRequestOwner> LocalBatchDownloadOwner;
	{
		FScopeLock ScopeLock(&Locks->Lock);
		LocalBatchDownloadOwner = MoveTemp(BatchDownloadOwner);
	}
	// BatchDownloadOwner must be deleted (which calls Cancel) outside of the lock, since its callback takes the lock
	LocalBatchDownloadOwner.Reset();

	FScopeLock ScopeLock(&Locks->Lock);
	// AssetRegistry has already been destructed by this point, do not try to access it.
	// AssetRegistry->OnAssetUpdatedOnDisk().RemoveAll(this);
	UPackage::PackageSavedWithContextEvent.RemoveAll(this);
	FCoreUObjectDelegates::OnEndLoadPackage.RemoveAll(this);
	FCoreDelegates::OnPostEngineInit.RemoveAll(this);
	Locks->Owner = nullptr;
	AssetRegistry = nullptr;
	Workspace.Reset();

	if (RegisteredEditorDomain == this)
	{
		RegisteredEditorDomain = nullptr;
	}
}

FEditorDomain* FEditorDomain::Get()
{
	return RegisteredEditorDomain;
}

bool FEditorDomain::SupportsLocalOnlyPaths()
{
	// Local Only paths are supported by falling back to the WorkspaceDomain
	return true;
}

bool FEditorDomain::SupportsPackageOnlyPaths()
{
	return true;
}

bool FEditorDomain::DoesPackageExist(const FPackagePath& PackagePath, EPackageSegment PackageSegment,
	FPackagePath* OutUpdatedPath)
{
	return Workspace->DoesPackageExist(PackagePath, PackageSegment, OutUpdatedPath);
}

FEditorDomain::FLocks::FLocks(FEditorDomain& InOwner)
	:Owner(&InOwner)
{
}

bool FEditorDomain::TryFindOrAddPackageSource(FName PackageName,
	TRefCountPtr<FPackageSource>& OutSource, UE::EditorDomain::FPackageDigest* OutErrorDigest)
{
	// Called within Locks.Lock
	using namespace UE::EditorDomain;

	// EDITOR_DOMAIN_TODO: Need to delete entries from PackageSources when the assetregistry reports the package is
	// resaved on disk.
	TRefCountPtr<FPackageSource>& PackageSource = PackageSources.FindOrAdd(PackageName);
	if (PackageSource)
	{
		OutSource = PackageSource;
		return true;
	}

	FString ErrorMessage;
	FPackageDigest PackageDigest = CalculatePackageDigest(*AssetRegistry, PackageName);
	switch (PackageDigest.Status)
	{
	case FPackageDigest::EStatus::Successful:
		PackageSource = new FPackageSource();
		PackageSource->Digest = MoveTemp(PackageDigest);
		OutSource = PackageSource;
		if (!bEditorDomainReadEnabled || !EnumHasAnyFlags(PackageDigest.DomainUse, EDomainUse::LoadEnabled))
		{
			PackageSource->Source = EPackageSource::Workspace;
		}
		return true;
	case FPackageDigest::EStatus::DoesNotExistInAssetRegistry:
		OutSource.SafeRelease();
		// Remove the entry in PackageSources that we added; we added it to avoid a double lookup for new packages,
		// but for non-existent packages we want it not to be there to avoid wasting memory on it
		PackageSources.Remove(PackageName);
		if (OutErrorDigest)
		{
			*OutErrorDigest = MoveTemp(PackageDigest);
		}
		return false;
	default:
		UE_LOG(LogEditorDomain, Warning,
			TEXT("Could not load package from EditorDomain; it will be loaded from the WorkspaceDomain: %s."),
			*ErrorMessage)
		PackageSource = new FPackageSource();
		PackageSource->Source = EPackageSource::Workspace;
		OutSource = PackageSource;
		return true;
	}
}

UE::EditorDomain::FPackageDigest FEditorDomain::GetPackageDigest(FName PackageName)
{
	FScopeLock ScopeLock(&Locks->Lock);
	return GetPackageDigest_WithinLock(PackageName);
}

UE::EditorDomain::FPackageDigest FEditorDomain::GetPackageDigest_WithinLock(FName PackageName)
{
	// Called within &Locks->Lock
	using namespace UE::EditorDomain;

	TRefCountPtr<FPackageSource> PackageSource;
	FPackageDigest ErrorDigest;
	if (!TryFindOrAddPackageSource(PackageName, PackageSource, &ErrorDigest))
	{
		return ErrorDigest;
	}
	return PackageSource->Digest;
}

void FEditorDomain::PrecachePackageDigest(FName PackageName)
{
	AssetRegistry->WaitForPackage(PackageName.ToString());
	TOptional<FAssetPackageData> PackageData = AssetRegistry->GetAssetPackageDataCopy(PackageName);
	if (PackageData)
	{
		UE::EditorDomain::PrecacheClassDigests(PackageData->ImportedClasses);
	}
}

TRefCountPtr<FEditorDomain::FPackageSource> FEditorDomain::FindPackageSource(const FPackagePath& PackagePath)
{
	// Called within Locks.Lock
	using namespace UE::EditorDomain;

	FName PackageName = PackagePath.GetPackageFName();
	if (!PackageName.IsNone())
	{
		TRefCountPtr<FPackageSource>* PackageSource = PackageSources.Find(PackageName);
		if (PackageSource)
		{
			return *PackageSource;
		}
	}

	return TRefCountPtr<FPackageSource>();
}

void FEditorDomain::MarkNeedsLoadFromWorkspace(const FPackagePath& PackagePath, TRefCountPtr<FPackageSource>& PackageSource)
{
	PackageSource->Source = FEditorDomain::EPackageSource::Workspace;
	if (bExternalSave)
	{
		SaveClient->RequestSave(PackagePath);
	}
	// Otherwise, we will note the need for save in OnEndLoadPackage

}

int64 FEditorDomain::FileSize(const FPackagePath& PackagePath, EPackageSegment PackageSegment,
	FPackagePath* OutUpdatedPath)
{
	using namespace UE::EditorDomain;
	using namespace UE::DerivedData;

	if (PackageSegment != EPackageSegment::Header)
	{
		return Workspace->FileSize(PackagePath, PackageSegment, OutUpdatedPath);
	}

	TOptional<UE::DerivedData::FRequestOwner> Owner;
	int64 FileSize = -1;
	{
		FScopeLock ScopeLock(&Locks->Lock);
		TRefCountPtr<FPackageSource> PackageSource;
		FName PackageName = PackagePath.GetPackageFName();
		if (PackageName.IsNone())
		{
			return Workspace->FileSize(PackagePath, PackageSegment, OutUpdatedPath);
		}

		if (!TryFindOrAddPackageSource(PackageName, PackageSource) || PackageSource->Source == EPackageSource::Workspace)
		{
			return Workspace->FileSize(PackagePath, PackageSegment, OutUpdatedPath);
		}
		PackageSource->SetHasLoaded();

		auto MetaDataGetComplete =
			[&FileSize, &PackageSource, &PackagePath, PackageSegment, Locks=this->Locks, OutUpdatedPath]
			(UE::DerivedData::FCacheGetResponse&& Response)
		{
			FScopeLock ScopeLock(&Locks->Lock);
			if ((PackageSource->Source == FEditorDomain::EPackageSource::Undecided || PackageSource->Source == FEditorDomain::EPackageSource::Editor) &&
				Response.Status == UE::DerivedData::EStatus::Ok)
			{
				const FCbObject& MetaData = Response.Record.GetMeta();
				FileSize = MetaData["FileSize"].AsInt64();
				PackageSource->Source = EPackageSource::Editor;
			}
			else
			{
				checkf(PackageSource->Source == EPackageSource::Undecided || PackageSource->Source == EPackageSource::Workspace,
					TEXT("%s was previously loaded from the EditorDomain but now is unavailable."),
					*PackagePath.GetDebugName());
				if (Locks->Owner)
				{
					FEditorDomain& EditorDomain = *Locks->Owner;
					EditorDomain.MarkNeedsLoadFromWorkspace(PackagePath, PackageSource);
					FileSize = EditorDomain.Workspace->FileSize(PackagePath, PackageSegment, OutUpdatedPath);
				}
				else
				{
					UE_LOG(LogEditorDomain, Warning, TEXT("%s size read after EditorDomain shutdown. Returning -1."),
						*PackagePath.GetDebugName());
					FileSize = -1;
				}
			}
		};
		// Fetch meta-data only
		ECachePolicy SkipFlags = ECachePolicy::SkipData & ~ECachePolicy::SkipMeta;
		Owner.Emplace(EPriority::Highest);
		RequestEditorDomainPackage(PackagePath, PackageSource->Digest.Hash, SkipFlags,
			*Owner, MoveTemp(MetaDataGetComplete));
	}
	Owner->Wait();
	return FileSize;
}

FOpenPackageResult FEditorDomain::OpenReadPackage(const FPackagePath& PackagePath, EPackageSegment PackageSegment,
	FPackagePath* OutUpdatedPath)
{
	using namespace UE::EditorDomain;
	using namespace UE::DerivedData;

	FScopeLock ScopeLock(&Locks->Lock);
	if (PackageSegment != EPackageSegment::Header)
	{
		return Workspace->OpenReadPackage(PackagePath, PackageSegment, OutUpdatedPath);
	}
	FName PackageName = PackagePath.GetPackageFName();
	if (PackageName.IsNone())
	{
		return Workspace->OpenReadPackage(PackagePath, PackageSegment, OutUpdatedPath);
	}
	TRefCountPtr<FPackageSource> PackageSource;
	if (!TryFindOrAddPackageSource(PackageName, PackageSource) || (PackageSource->Source == EPackageSource::Workspace))
	{
		return Workspace->OpenReadPackage(PackagePath, PackageSegment, OutUpdatedPath);
	}
	PackageSource->SetHasLoaded();

	// TODO: Change priority to High instead of Blocking once we have removed the GetPackageFormat below
	// and don't need to block on the result before exiting this function
	EPriority Priority = EPriority::Blocking;
	FEditorDomainReadArchive* Result = new FEditorDomainReadArchive(Locks, PackagePath, PackageSource, Priority);
	const FIoHash PackageEditorHash = PackageSource->Digest.Hash;
	const bool bHasEditorSource = (PackageSource->Source == EPackageSource::Editor);

	// Unlock before requesting the package because the completion callback takes the lock.
	ScopeLock.Unlock();

	// Fetch only meta-data in the initial request
	ECachePolicy SkipFlags = ECachePolicy::SkipData & ~ECachePolicy::SkipMeta;
	RequestEditorDomainPackage(PackagePath, PackageEditorHash,
		SkipFlags, Result->GetRequestOwner(),
		[Result](FCacheGetResponse&& Response)
		{
			// Note that ~FEditorDomainReadArchive waits for this callback to be called, so Result cannot dangle
			Result->OnRecordRequestComplete(MoveTemp(Response));
		});

	// Precache the exports segment
	// EDITOR_DOMAIN_TODO: Skip doing this for OpenReadPackage calls from bulk data
	Result->Precache(0, 0);

	if (OutUpdatedPath)
	{
		*OutUpdatedPath = PackagePath;
	}

	const EPackageFormat Format = bHasEditorSource ? EPackageFormat::Binary : Result->GetPackageFormat();
	bool bNeedsEngineVersionChecks = bHasEditorSource ? false : (Result->GetPackageSource() != EPackageSource::Editor);
	return FOpenPackageResult{ TUniquePtr<FArchive>(Result), Format, bNeedsEngineVersionChecks};
}

FOpenAsyncPackageResult FEditorDomain::OpenAsyncReadPackage(const FPackagePath& PackagePath, EPackageSegment PackageSegment)
{
	using namespace UE::EditorDomain;
	using namespace UE::DerivedData;

	FScopeLock ScopeLock(&Locks->Lock);
	if (PackageSegment != EPackageSegment::Header)
	{
		return Workspace->OpenAsyncReadPackage(PackagePath, PackageSegment);
	}

	FName PackageName = PackagePath.GetPackageFName();
	if (PackageName.IsNone())
	{
		return Workspace->OpenAsyncReadPackage(PackagePath, PackageSegment);
	}
	TRefCountPtr<FPackageSource> PackageSource;
	if (!TryFindOrAddPackageSource(PackageName, PackageSource) ||
		(PackageSource->Source == EPackageSource::Workspace))
	{
		return Workspace->OpenAsyncReadPackage(PackagePath, PackageSegment);
	}
	PackageSource->SetHasLoaded();

	// TODO: Change priority to Normal instead of Blocking once we have removed the GetPackageFormat below
	// and don't need to block on the result before exiting this function
	EPriority Priority = EPriority::Blocking;
	FEditorDomainAsyncReadFileHandle* Result =
		new FEditorDomainAsyncReadFileHandle(Locks, PackagePath, PackageSource, Priority);
	const bool bHasEditorSource = (PackageSource->Source == EPackageSource::Editor);
	FIoHash EditorDomainHash = PackageSource->Digest.Hash;

	// Unlock before requesting the package because the completion callback takes the lock.
	ScopeLock.Unlock();

	// Fetch meta-data only in the initial request
	ECachePolicy SkipFlags = ECachePolicy::SkipData & ~ECachePolicy::SkipMeta;
	RequestEditorDomainPackage(PackagePath, EditorDomainHash, SkipFlags, Result->GetRequestOwner(),
		[Result](FCacheGetResponse&& Response)
		{
			// Note that ~FEditorDomainAsyncReadFileHandle waits for this callback to be called, so Result cannot dangle
			Result->OnRecordRequestComplete(MoveTemp(Response));
		});

	const EPackageFormat Format = bHasEditorSource ? EPackageFormat::Binary : Result->GetPackageFormat();
	bool bNeedsEngineVersionChecks = bHasEditorSource ? false : (Result->GetPackageSource() != EPackageSource::Editor);
	return FOpenAsyncPackageResult{ TUniquePtr<IAsyncReadFileHandle>(Result), Format, bNeedsEngineVersionChecks };
}

IMappedFileHandle* FEditorDomain::OpenMappedHandleToPackage(const FPackagePath& PackagePath,
	EPackageSegment PackageSegment, FPackagePath* OutUpdatedPath)
{
	// No need to implement this runtime feature in the editor domain.
	return nullptr;
}

bool FEditorDomain::TryMatchCaseOnDisk(const FPackagePath& PackagePath, FPackagePath* OutNormalizedPath)
{
	return Workspace->TryMatchCaseOnDisk(PackagePath, OutNormalizedPath);
}

TUniquePtr<FArchive> FEditorDomain::OpenReadExternalResource(EPackageExternalResource ResourceType, FStringView Identifier)
{
	return Workspace->OpenReadExternalResource(ResourceType, Identifier);
}

bool FEditorDomain::DoesExternalResourceExist(EPackageExternalResource ResourceType, FStringView Identifier)
{
	return Workspace->DoesExternalResourceExist(ResourceType, Identifier);
}

FOpenAsyncPackageResult FEditorDomain::OpenAsyncReadExternalResource(
	EPackageExternalResource ResourceType, FStringView Identifier)
{
	return Workspace->OpenAsyncReadExternalResource(ResourceType, Identifier);
}

void FEditorDomain::FindPackagesRecursive(TArray<TPair<FPackagePath, EPackageSegment>>& OutPackages,
	FStringView PackageMount, FStringView FileMount, FStringView RootRelPath, FStringView BasenameWildcard)
{
	return Workspace->FindPackagesRecursive(OutPackages, PackageMount, FileMount, RootRelPath, BasenameWildcard);
}

void FEditorDomain::IteratePackagesInPath(FStringView PackageMount, FStringView FileMount, FStringView RootRelPath,
	FPackageSegmentVisitor Callback)
{
	Workspace->IteratePackagesInPath(PackageMount, FileMount, RootRelPath, Callback);

}
void FEditorDomain::IteratePackagesInLocalOnlyDirectory(FStringView RootDir, FPackageSegmentVisitor Callback)
{
	Workspace->IteratePackagesInLocalOnlyDirectory(RootDir, Callback);
}

void FEditorDomain::IteratePackagesStatInPath(FStringView PackageMount, FStringView FileMount,
	FStringView RootRelPath, FPackageSegmentStatVisitor Callback)
{
	Workspace->IteratePackagesStatInPath(PackageMount, FileMount, RootRelPath, Callback);
}

void FEditorDomain::IteratePackagesStatInLocalOnlyDirectory(FStringView RootDir, FPackageSegmentStatVisitor Callback)
{
	Workspace->IteratePackagesStatInLocalOnlyDirectory(RootDir, Callback);
}

void FEditorDomain::Tick(float DeltaTime)
{
	if (bExternalSave)
	{
		SaveClient->Tick(DeltaTime);
	}
}

void FEditorDomain::OnEndLoadPackage(TConstArrayView<UPackage*> LoadedPackages)
{
	if (bExternalSave)
	{
		return;
	}
	TArray<UPackage*> PackagesToSave;
	{
		FScopeLock ScopeLock(&Locks->Lock);
		if (!bHasPassedPostEngineInit)
		{
			return;
		}
		PackagesToSave.Reserve(LoadedPackages.Num());
		for (UPackage* Package : LoadedPackages)
		{
			PackagesToSave.Add(Package);
		}
		FilterKeepPackagesToSave(PackagesToSave);
	}

	for (UPackage* Package : PackagesToSave)
	{
		UE::EditorDomain::TrySavePackage(Package);
	}
}

void FEditorDomain::OnPostEngineInit()
{
	{
		FScopeLock ScopeLock(&Locks->Lock);
		bHasPassedPostEngineInit = true;
		if (bExternalSave)
		{
			return;
		}
	}

	TArray<UPackage*> PackagesToSave;
	FString PackageName;
	for (TObjectIterator<UPackage> It; It; ++It)
	{
		UPackage* Package = *It;
		Package->GetName(PackageName);
		if (Package->IsFullyLoaded() && !FPackageName::IsScriptPackage(PackageName))
		{
			PackagesToSave.Add(Package);
		}
	}

	{
		FScopeLock ScopeLock(&Locks->Lock);
		FilterKeepPackagesToSave(PackagesToSave);
	}

	for (UPackage* Package : PackagesToSave)
	{
		UE::EditorDomain::TrySavePackage(Package);
	}
}

void FEditorDomain::FilterKeepPackagesToSave(TArray<UPackage*>& InOutPackagesToSave)
{
	FPackagePath PackagePath;
	for (int32 Index = 0; Index < InOutPackagesToSave.Num(); )
	{
		UPackage* Package = InOutPackagesToSave[Index];
		bool bKeep = false;
		if (FPackagePath::TryFromPackageName(Package->GetFName(), PackagePath))
		{
			TRefCountPtr<FPackageSource> PackageSource = FindPackageSource(PackagePath);
			if (PackageSource && PackageSource->NeedsEditorDomainSave(*this))
			{
				PackageSource->bHasSaved = true;
				bKeep = true;
			}
		}
		if (bKeep)
		{
			++Index;
		}
		else
		{
			InOutPackagesToSave.RemoveAtSwap(Index);
		}
	}
}

bool FEditorDomain::FPackageSource::NeedsEditorDomainSave(FEditorDomain& EditorDomain) const
{
	return !bHasSaved && Source == EPackageSource::Workspace &&
		(!EditorDomain.bSkipSavesUntilCatalogLoaded || bLoadedAfterCatalogLoaded);
}

void FEditorDomain::FPackageSource::SetHasLoaded()
{
	if (bHasLoaded)
	{
		return;
	}
	bHasLoaded = true;
	bLoadedAfterCatalogLoaded = bHasQueriedCatalog;
}

void FEditorDomain::BatchDownload(TArrayView<FName> PackageNames)
{
	using namespace UE::EditorDomain;
	using namespace UE::DerivedData;

	FScopeLock ScopeLock(&Locks->Lock);
	if (!BatchDownloadOwner)
	{
		BatchDownloadOwner = MakeUnique<FRequestOwner>(EPriority::Highest);
	}

	TArray<FCacheGetRequest> CacheRequests;
	CacheRequests.Reserve(PackageNames.Num());
	ECachePolicy CachePolicy = ECachePolicy::Default | ECachePolicy::SkipData;
	for (FName PackageName : PackageNames)
	{
		FPackageDigest PackageDigest = GetPackageDigest_WithinLock(PackageName);
		if (PackageDigest.IsSuccessful() && EnumHasAnyFlags(PackageDigest.DomainUse, EDomainUse::LoadEnabled))
		{
			CacheRequests.Add({ { WriteToString<256>(PackageName) }, GetEditorDomainPackageKey(PackageDigest.Hash),
				CachePolicy });
		}
	}

	if (!CacheRequests.IsEmpty())
	{
		FRequestBarrier Barrier(*BatchDownloadOwner);
		GetCache().Get(CacheRequests, *BatchDownloadOwner, [this](FCacheGetResponse&& Response)
			{
				FScopeLock ScopeLock(&Locks->Lock);
				TRefCountPtr<FPackageSource> PackageSource;
				FPackageDigest ErrorDigest;
				FName PackageName = FName(*Response.Name);
				if (TryFindOrAddPackageSource(PackageName, PackageSource, &ErrorDigest))
				{
					PackageSource->bHasQueriedCatalog = true;
				}
			});
	}
}

void FEditorDomain::OnPackageSavedWithContext(const FString& PackageFileName, UPackage* Package,
	FObjectPostSaveContext ObjectSaveContext)
{
	if (!ObjectSaveContext.IsUpdatingLoadedPath())
	{
		return;
	}
	FName PackageName = Package->GetFName();
	FScopeLock ScopeLock(&Locks->Lock);
	PackageSources.Remove(PackageName);
}

void FEditorDomain::OnAssetUpdatedOnDisk(const FAssetData& AssetData)
{
	FName PackageName = AssetData.PackageName;
	if (PackageName.IsNone())
	{
		return;
	}
	FScopeLock ScopeLock(&Locks->Lock);
	PackageSources.Remove(PackageName);
}

namespace UE::EditorDomain
{

FPackageDigest::FPackageDigest(EStatus InStatus, FName InStatusArg)
 : Status(InStatus)
 , StatusArg(InStatusArg)
{
}

bool FPackageDigest::IsSuccessful() const
{
	return Status == EStatus::Successful;
}

FString FPackageDigest::GetStatusString() const
{
	return LexToString(Status, StatusArg);
}

}

FString LexToString(UE::EditorDomain::FPackageDigest::EStatus Status, FName StatusArg)
{
	using namespace UE::EditorDomain;

	switch (Status)
	{
	case FPackageDigest::EStatus::NotYetRequested:
		return TEXT("Has not been requested.");
	case FPackageDigest::EStatus::Successful:
		return TEXT("Successful.");
	case FPackageDigest::EStatus::InvalidPackageName:
		return TEXT("PackageName is not a valid LongPackageName.");
	case FPackageDigest::EStatus::DoesNotExistInAssetRegistry:
		return TEXT("Does not exist in AssetRegistry.");
	case FPackageDigest::EStatus::MissingClass:
		return FString::Printf(TEXT("Uses class %s that is not loaded."), *StatusArg.ToString());
	case FPackageDigest::EStatus::MissingCustomVersion:
		return FString::Printf(
			TEXT("Uses CustomVersion guid %s but that guid is not available in FCurrentCustomVersions."),
			*StatusArg.ToString());
	default:
		return TEXT("Unknown result code.");
	}
}
