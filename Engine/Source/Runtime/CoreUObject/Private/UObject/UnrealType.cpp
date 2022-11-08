// Copyright Epic Games, Inc. All Rights Reserved.

#include "UObject/UnrealType.h"
#include "Serialization/ArchiveUObjectFromStructuredArchive.h"

DEFINE_LOG_CATEGORY(LogType);

FPropertyValueIterator::FPropertyValueIterator(
	FFieldClass* InPropertyClass,
	const UStruct* InStruct,
	const void* InStructValue,
	EPropertyValueIteratorFlags InRecursionFlags,
	EFieldIteratorFlags::DeprecatedPropertyFlags InDeprecatedPropertyFlags)
	: PropertyClass(InPropertyClass)
	, RecursionFlags(InRecursionFlags)
	, DeprecatedPropertyFlags(InDeprecatedPropertyFlags)
	, bSkipRecursionOnce(false)
	, bMatchAll(InPropertyClass == FProperty::StaticClass())
{
	FPropertyValueStackEntry Entry(InStructValue);
	FillStructProperties(InStruct, Entry);
	if (Entry.ValueArray.Num() > 0)
	{
		PropertyIteratorStack.Emplace(MoveTemp(Entry));
		IterateToNext();
	}
}

uint8 FPropertyValueIterator::GetPropertyValueFlags(const FProperty* Property)
{
	uint8 Flags = 0;
	if (RecursionFlags == EPropertyValueIteratorFlags::FullRecursion)
	{
		if (Property->IsA(FArrayProperty::StaticClass()))
		{
			Flags = (uint8)EPropertyValueFlags::IsArray;
		}
		else if (Property->IsA(FMapProperty::StaticClass()))
		{
			Flags = (uint8)EPropertyValueFlags::IsMap;
		}
		else if (Property->IsA(FSetProperty::StaticClass()))
		{
			Flags = (uint8)EPropertyValueFlags::IsSet;
		}
		else if (Property->IsA(FStructProperty::StaticClass()))
		{
			Flags = (uint8)EPropertyValueFlags::IsStruct;
		}
	}
	if (bMatchAll || Property->IsA(PropertyClass))
	{
		Flags |= (uint8)EPropertyValueFlags::IsMatch;
	}
	return Flags;
}

void FPropertyValueIterator::FillStructProperties(const UStruct* Struct, FPropertyValueStackEntry& Entry)
{
	FPropertyValueStackEntry::FValueArrayType& ValueArray = Entry.ValueArray;
	for (TFieldIterator<FProperty> It(Struct, EFieldIteratorFlags::IncludeSuper, DeprecatedPropertyFlags, EFieldIteratorFlags::ExcludeInterfaces); It; ++It)
	{
		const FProperty* Property  = *It;
		uint8 PropertyValueFlags = GetPropertyValueFlags(Property);
		if (PropertyValueFlags)
		{
			int32 Num = Property->ArrayDim;
			ValueArray.Reserve(ValueArray.Num() + Num);
			for (int32 StaticIndex = 0; StaticIndex < Num; ++StaticIndex)
			{
				const void* PropertyValue = Property->ContainerPtrToValuePtr<void>(Entry.Owner, StaticIndex);
				ValueArray.Emplace(BasePairType(Property, PropertyValue), PropertyValueFlags);
			}
		}
	}
}

bool FPropertyValueIterator::NextValue(EPropertyValueIteratorFlags InRecursionFlags)
{
	check(PropertyIteratorStack.Num() > 0)
	FPropertyValueStackEntry& Entry = PropertyIteratorStack.Last();

	// If we have pending values, deal with them
	if (Entry.NextValueIndex < Entry.ValueArray.Num())
	{
		const bool bIsPropertyMatchProcessed = Entry.ValueIndex == Entry.NextValueIndex;
		Entry.ValueIndex = Entry.NextValueIndex;
		Entry.NextValueIndex = Entry.ValueIndex + 1;

		const FProperty* Property = Entry.ValueArray[Entry.ValueIndex].Key.Key;
		const void* PropertyValue = Entry.ValueArray[Entry.ValueIndex].Key.Value;
		const uint8 PropertyValueFlags = Entry.ValueArray[Entry.ValueIndex].Value;
		check(PropertyValueFlags);

		// Handle matching properties
		if (!bIsPropertyMatchProcessed && (PropertyValueFlags & EPropertyValueFlags::IsMatch))
		{
			if (PropertyValueFlags & EPropertyValueFlags::ContainerMask)
			{
				// this match is also a container/struct, so recurse into it next time
				Entry.NextValueIndex = Entry.ValueIndex;
			}
			return false; // Break at this matching property
		}

		// Handle container properties
		check(PropertyValueFlags & EPropertyValueFlags::ContainerMask);
		if (InRecursionFlags == EPropertyValueIteratorFlags::FullRecursion)
		{
			FPropertyValueStackEntry NewEntry(PropertyValue);

			if (PropertyValueFlags & EPropertyValueFlags::IsArray)
			{
				const FArrayProperty* ArrayProperty = CastFieldChecked<FArrayProperty>(Property);
				const FProperty* InnerProperty = ArrayProperty->Inner;
				uint8 InnerFlags = GetPropertyValueFlags(InnerProperty);
				if (InnerFlags)
				{
					FScriptArrayHelper Helper(ArrayProperty, PropertyValue);
					const int32 Num = Helper.Num();
					NewEntry.ValueArray.Reserve(Num);
					for (int32 DynamicIndex = 0; DynamicIndex < Num; ++DynamicIndex)
					{
						NewEntry.ValueArray.Emplace(
							BasePairType(InnerProperty, Helper.GetRawPtr(DynamicIndex)), InnerFlags);
					}
				}
			}
			else if (PropertyValueFlags & EPropertyValueFlags::IsMap)
			{
				const FMapProperty* MapProperty = CastFieldChecked<FMapProperty>(Property);
				const FProperty* KeyProperty = MapProperty->KeyProp;
				const FProperty* ValueProperty = MapProperty->ValueProp;
				uint8 KeyFlags = GetPropertyValueFlags(KeyProperty);
				uint8 ValueFlags = GetPropertyValueFlags(ValueProperty);
				if (KeyFlags | ValueFlags)
				{
					FScriptMapHelper Helper(MapProperty, PropertyValue);
					const int32 Num = Helper.Num();
					for (int32 DynamicIndex = 0; DynamicIndex < Num; ++DynamicIndex)
					{
						if (Helper.IsValidIndex(DynamicIndex))
						{
							if (KeyFlags)
							{
								NewEntry.ValueArray.Emplace(
									BasePairType(KeyProperty, Helper.GetKeyPtr(DynamicIndex)), KeyFlags);
							}
							if (ValueFlags)
							{
								NewEntry.ValueArray.Emplace(
									BasePairType(ValueProperty, Helper.GetValuePtr(DynamicIndex)), ValueFlags);
							}
						}
					}
				}
			}
			else if (PropertyValueFlags & EPropertyValueFlags::IsSet)
			{
				const FSetProperty* SetProperty = CastFieldChecked<FSetProperty>(Property);
				const FProperty* InnerProperty = SetProperty->ElementProp;
				uint8 InnerFlags = GetPropertyValueFlags(InnerProperty);
				if (InnerFlags)
				{
					FScriptSetHelper Helper(SetProperty, PropertyValue);
					const int32 Num = Helper.Num();
					for (int32 DynamicIndex = 0; DynamicIndex < Num; ++DynamicIndex)
					{
						if (Helper.IsValidIndex(DynamicIndex))
						{
							NewEntry.ValueArray.Emplace(
								BasePairType(InnerProperty, Helper.GetElementPtr(DynamicIndex)), InnerFlags);
						}
					}
				}
			}
			else if (PropertyValueFlags & EPropertyValueFlags::IsStruct)
			{
				const FStructProperty* StructProperty = CastFieldChecked<FStructProperty>(Property);
				FillStructProperties(StructProperty->Struct, NewEntry);
			}
			if (NewEntry.ValueArray.Num() > 0)
			{
				PropertyIteratorStack.Emplace(MoveTemp(NewEntry));
				return true; // NextValue should be called again to move to the top of the stack
			}
		}
	}

	if (Entry.NextValueIndex == Entry.ValueArray.Num())
	{
		PropertyIteratorStack.Pop();
	}

	// NextValue should be called again to continue iteration
	return PropertyIteratorStack.Num() > 0;
}

void FPropertyValueIterator::IterateToNext()
{
	EPropertyValueIteratorFlags LocalRecursionFlags = RecursionFlags;

	if (bSkipRecursionOnce)
	{
		LocalRecursionFlags = EPropertyValueIteratorFlags::NoRecursion;
		bSkipRecursionOnce = false;
	}

	while (NextValue(LocalRecursionFlags))
	{
		// Reset recursion override as we've skipped the first property
		LocalRecursionFlags = RecursionFlags;
	}
}

void FPropertyValueIterator::GetPropertyChain(TArray<const FProperty*>& PropertyChain) const
{
	PropertyChain.Reserve(PropertyIteratorStack.Num());
	// Iterate over struct/container property stack, starting at the inner most property
	for (int32 StackIndex = PropertyIteratorStack.Num() - 1; StackIndex >= 0; StackIndex--)
	{
		const FPropertyValueStackEntry& Entry = PropertyIteratorStack[StackIndex];

		// Index should always be valid
		const FProperty* Property = Entry.ValueArray[Entry.ValueIndex].Key.Key;
		PropertyChain.Add(Property);
	}
}
