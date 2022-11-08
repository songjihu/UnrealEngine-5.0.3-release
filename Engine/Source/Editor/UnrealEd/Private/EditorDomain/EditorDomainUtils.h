// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Containers/Array.h"
#include "Containers/ArrayView.h"
#include "Containers/Map.h"
#include "Containers/Set.h"
#include "Containers/UnrealString.h"
#include "DerivedDataCache.h"
#include "EditorDomain/EditorDomain.h"
#include "HAL/CriticalSection.h"
#include "Memory/MemoryFwd.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/EnumClassFlags.h"
#include "Serialization/CustomVersion.h"
#include "Templates/Function.h"
#include "UObject/NameTypes.h"

DECLARE_LOG_CATEGORY_EXTERN(LogEditorDomainSave, Log, All);

class FAssetPackageData;
class FBlake3;
class FPackagePath;
class IAssetRegistry;
class UPackage;

namespace UE::DerivedData { struct FCacheKey; }

namespace UE::EditorDomain
{

/** A UClass's data that is used in the EditorDomain Digest, and holds other information about Classes the EditorDomain needs. */
struct FClassDigestData
{
public:
	FBlake3Hash SchemaHash;
	/** CustomVersions used by the class */
	TArray<int32> CustomVersionHandles;
	/** Classes that can be created by the class during PostLoad/PreSave (parsed from ini) */
	TArray<FName> ConstructClasses;
	/** The closest native parent of (the possibly CoreRedirected target of) the class */
	FName ResolvedClosestNative;
	/** EditorDomainEnabled allows everything and uses only a blocklist, so DomainUse by default is enabled. */
	EDomainUse EditorDomainUse = EDomainUse::LoadEnabled | EDomainUse::SaveEnabled;

	bool bNative = false;
	/** bTargetIterativeEnabled uses an allowlist (with a blocklist override), so defaults to false. */
	bool bTargetIterativeEnabled = false;

	bool bConstructed = false;
	bool bConstructionComplete = false;
};

/** Threadsafe cache of ClassName -> Digest data for calculating EditorDomain Digests */
struct FClassDigestMap
{
	TMap<FName, FClassDigestData> Map;
	FRWLock Lock;
};

/**
 * Calculate the PackageDigest for the given packagePath.
 * Reads information from the AssetRegistry to compute the digest.
 */
FPackageDigest CalculatePackageDigest(IAssetRegistry& AssetRegistry, FName PackageName);

/** For any ClassNames not already in ClassDigests, look up their UStruct and add them. */
void PrecacheClassDigests(TConstArrayView<FName> ClassNames);

/** Get the CacheRequest for the given package from the EditorDomain cache bucket. */
void RequestEditorDomainPackage(const FPackagePath& PackagePath,
	const FIoHash& EditorDomainHash, UE::DerivedData::ECachePolicy SkipFlags,
	UE::DerivedData::IRequestOwner& Owner, UE::DerivedData::FOnCacheGetComplete&& Callback);
UE::DerivedData::FCacheKey GetEditorDomainPackageKey(const FIoHash& EditorDomainHash);

/** Save the given package into the EditorDomain. */
bool TrySavePackage(UPackage* Package);

/** Get the CacheRequest for the BulkDataList of the given package. */
void GetBulkDataList(FName PackageName, UE::DerivedData::IRequestOwner& Owner,
	TUniqueFunction<void(FSharedBuffer Buffer)>&& Callback);

/** Write the data for the BulkDataList of the given package to the cache. */
void PutBulkDataList(FName PackageName, FSharedBuffer Buffer);

void GetBulkDataPayloadId(FName PackageName, const FGuid& BulkDataId, UE::DerivedData::IRequestOwner& Owner,
	TUniqueFunction<void(FSharedBuffer Buffer)> && Callback);

void PutBulkDataPayloadId(FName PackageName, const FGuid& BulkDataId, FSharedBuffer Buffer);

/** Accessor for the global ClassDigest map shared by systems needing to calculate PackageDigests. */
FClassDigestMap& GetClassDigests();

/** Initializes some global config-driven values used by the EditorDomain and TargetDomain. */
void UtilsInitialize();

}
