// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/NamePermissionList.h"
#include "Misc/StringBuilder.h"


bool FPathPermissionList::PassesFilter(const FStringView Item) const
{
	if (DenyListAll.Num() > 0)
	{
		return false;
	}

	if (AllowList.Num() > 0 || DenyList.Num() > 0)
	{
		const uint32 ItemHash = GetTypeHash(Item);

		if (AllowList.Num() > 0 && !AllowList.ContainsByHash(ItemHash, Item))
		{
			return false;
		}

		if (DenyList.ContainsByHash(ItemHash, Item))
		{
			return false;
		}
	}

	return true;
}

bool FPathPermissionList::PassesFilter(const FName Item) const
{
	return PassesFilter(FNameBuilder(Item));
}

bool FPathPermissionList::PassesFilter(const TCHAR* Item) const
{
	return PassesFilter(FStringView(Item));
}

bool FPathPermissionList::PassesStartsWithFilter(const FStringView Item, const bool bAllowParentPaths) const
{
	if (AllowList.Num() > 0)
	{
		bool bPassedAllowList = false;
		for (const auto& Other : AllowList)
		{
			if (Item.StartsWith(Other.Key) && (Item.Len() <= Other.Key.Len() || Item[Other.Key.Len()] == TEXT('/')))
			{
				bPassedAllowList = true;
				break;
			}

			if (bAllowParentPaths)
			{
				// If allowing parent paths (eg, when filtering folders), then we must also check if the item has a AllowList child path
				if (FStringView(Other.Key).StartsWith(Item) && (Other.Key.Len() <= Item.Len() || Other.Key[Item.Len()] == TEXT('/')))
				{
					bPassedAllowList = true;
					break;
				}
			}
		}

		if (!bPassedAllowList)
		{
			return false;
		}
	}

	if (DenyList.Num() > 0)
	{
		for (const auto& Other : DenyList)
		{
			if (Item.StartsWith(Other.Key) && (Item.Len() <= Other.Key.Len() || Item[Other.Key.Len()] == TEXT('/')))
			{
				return false;
			}
		}
	}

	if (DenyListAll.Num() > 0)
	{
		return false;
	}

	return true;
}

bool FPathPermissionList::PassesStartsWithFilter(const FName Item, const bool bAllowParentPaths) const
{
	return PassesStartsWithFilter(FNameBuilder(Item), bAllowParentPaths);
}

bool FPathPermissionList::PassesStartsWithFilter(const TCHAR* Item, const bool bAllowParentPaths) const
{
	return PassesStartsWithFilter(FStringView(Item), bAllowParentPaths);
}

bool FPathPermissionList::AddDenyListItem(const FName OwnerName, const FStringView Item)
{
	const uint32 ItemHash = GetTypeHash(Item);

	FPermissionListOwners* Owners = DenyList.FindByHash(ItemHash, Item);
	const bool bFilterChanged = (Owners == nullptr);
	if (!Owners)
	{
		Owners = &DenyList.AddByHash(ItemHash, FString(Item));
	}

	Owners->AddUnique(OwnerName);
	
	if (bFilterChanged && !bSuppressOnFilterChanged)
	{
		OnFilterChanged().Broadcast();
	}

	return bFilterChanged;
}

bool FPathPermissionList::AddDenyListItem(const FName OwnerName, const FName Item)
{
	return AddDenyListItem(OwnerName, FNameBuilder(Item));
}

bool FPathPermissionList::AddDenyListItem(const FName OwnerName, const TCHAR* Item)
{
	return AddDenyListItem(OwnerName, FStringView(Item));
}

bool FPathPermissionList::AddAllowListItem(const FName OwnerName, const FStringView Item)
{
	const uint32 ItemHash = GetTypeHash(Item);

	FPermissionListOwners* Owners = AllowList.FindByHash(ItemHash, Item);
	const bool bFilterChanged = (Owners == nullptr);
	if (!Owners)
	{
		Owners = &AllowList.AddByHash(ItemHash, FString(Item));
	}

	Owners->AddUnique(OwnerName);

	if (bFilterChanged && !bSuppressOnFilterChanged)
	{
		OnFilterChanged().Broadcast();
	}

	return bFilterChanged;
}

bool FPathPermissionList::AddAllowListItem(const FName OwnerName, const FName Item)
{
	return AddAllowListItem(OwnerName, FNameBuilder(Item));
}

bool FPathPermissionList::AddAllowListItem(const FName OwnerName, const TCHAR* Item)
{
	return AddAllowListItem(OwnerName, FStringView(Item));
}

bool FPathPermissionList::AddDenyListAll(const FName OwnerName)
{
	const int32 OldNum = DenyListAll.Num();
	DenyListAll.AddUnique(OwnerName);

	const bool bFilterChanged = OldNum != DenyListAll.Num();
	if (bFilterChanged && !bSuppressOnFilterChanged)
	{
		OnFilterChanged().Broadcast();
	}

	return bFilterChanged;
}

bool FPathPermissionList::HasFiltering() const
{
	return DenyList.Num() > 0 || AllowList.Num() > 0 || DenyListAll.Num() > 0;
}

TArray<FName> FPathPermissionList::GetOwnerNames() const
{
	TArray<FName> OwnerNames;

	for (const auto& It : DenyList)
	{
		for (const auto& OwnerName : It.Value)
		{
			OwnerNames.AddUnique(OwnerName);
		}
	}

	for (const auto& It : AllowList)
	{
		for (const auto& OwnerName : It.Value)
		{
			OwnerNames.AddUnique(OwnerName);
		}
	}

	for (const auto& OwnerName : DenyListAll)
	{
		OwnerNames.AddUnique(OwnerName);
	}

	return OwnerNames;
}

bool FPathPermissionList::UnregisterOwner(const FName OwnerName)
{
	bool bFilterChanged = false;

	for (auto It = DenyList.CreateIterator(); It; ++It)
	{
		It->Value.Remove(OwnerName);
		if (It->Value.Num() == 0)
		{
			It.RemoveCurrent();
			bFilterChanged = true;
		}
	}

	for (auto It = AllowList.CreateIterator(); It; ++It)
	{
		It->Value.Remove(OwnerName);
		if (It->Value.Num() == 0)
		{
			It.RemoveCurrent();
			bFilterChanged = true;
		}
	}

	bFilterChanged |= (DenyListAll.Remove(OwnerName) > 0);

	if (bFilterChanged && !bSuppressOnFilterChanged)
	{
		OnFilterChanged().Broadcast();
	}

	return bFilterChanged;
}

bool FPathPermissionList::UnregisterOwners(const TArray<FName>& OwnerNames)
{
	bool bFilterChanged = false;
	{
		TGuardValue<bool> Guard(bSuppressOnFilterChanged, true);

		for (const FName& OwnerName : OwnerNames)
		{
			bFilterChanged |= UnregisterOwner(OwnerName);
		}
	}

	if (bFilterChanged && !bSuppressOnFilterChanged)
	{
		OnFilterChanged().Broadcast();
	}

	return bFilterChanged;
}

bool FPathPermissionList::Append(const FPathPermissionList& Other)
{
	bool bFilterChanged = false;
	{
		TGuardValue<bool> Guard(bSuppressOnFilterChanged, true);

		for (const auto& It : Other.DenyList)
		{
			for (const auto& OwnerName : It.Value)
			{
				bFilterChanged |= AddDenyListItem(OwnerName, It.Key);
			}
		}

		for (const auto& It : Other.AllowList)
		{
			for (const auto& OwnerName : It.Value)
			{
				bFilterChanged |= AddAllowListItem(OwnerName, It.Key);
			}
		}

		for (const auto& OwnerName : Other.DenyListAll)
		{
			bFilterChanged |= AddDenyListAll(OwnerName);
		}
	}

	if (bFilterChanged && !bSuppressOnFilterChanged)
	{
		OnFilterChanged().Broadcast();
	}

	return bFilterChanged;
}

FPathPermissionList FPathPermissionList::CombinePathFilters(const FPathPermissionList& OtherFilter) const
{
	FPathPermissionList Result;

	if (IsDenyListAll() || OtherFilter.IsDenyListAll())
	{
		Result.AddDenyListAll(NAME_None);
	}

	for (const TPair<FString, FPermissionListOwners>& It : GetDenyList())
	{
		for (const FName& OwnerName : It.Value)
		{
			Result.AddDenyListItem(OwnerName, It.Key);
		}
	}

	for (const TPair<FString, FPermissionListOwners>& It : OtherFilter.GetDenyList())
	{
		for (const FName& OwnerName : It.Value)
		{
			Result.AddDenyListItem(OwnerName, It.Key);
		}
	}

	if (GetAllowList().Num() > 0 || OtherFilter.GetAllowList().Num() > 0)
	{
		for (const TPair<FString, FPermissionListOwners>& It : GetAllowList())
		{
			const FString& Path = It.Key;
			if (OtherFilter.PassesStartsWithFilter(Path, true))
			{
				for (const FName& OwnerName : It.Value)
				{
					Result.AddAllowListItem(OwnerName, Path);
				}
			}
		}

		for (const TPair<FString, FPermissionListOwners>& It : OtherFilter.GetAllowList())
		{
			const FString& Path = It.Key;
			if (PassesStartsWithFilter(Path, true))
			{
				for (const FName& OwnerName : It.Value)
				{
					Result.AddAllowListItem(OwnerName, Path);
				}
			}
		}

		// Block everything if none of the AllowList paths passed
		if (Result.GetAllowList().Num() == 0)
		{
			Result.AddDenyListAll(NAME_None);
		}
	}

	return Result;
}

bool FPathPermissionList::UnregisterOwnersAndAppend(const TArray<FName>& OwnerNamesToRemove, const FPathPermissionList& FiltersToAdd)
{
	bool bFilterChanged = false;
	{
		TGuardValue<bool> Guard(bSuppressOnFilterChanged, true);

		bFilterChanged |= UnregisterOwners(OwnerNamesToRemove);
		bFilterChanged |= Append(FiltersToAdd);
	}

	if (bFilterChanged && !bSuppressOnFilterChanged)
	{
		OnFilterChanged().Broadcast();
	}

	return bFilterChanged;
}