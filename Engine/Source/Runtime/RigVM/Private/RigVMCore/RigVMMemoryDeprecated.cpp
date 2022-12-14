// Copyright Epic Games, Inc. All Rights Reserved.

#include "RigVMCore/RigVMMemoryDeprecated.h"
#include "UObject/AnimObjectVersion.h"
#include "UObject/ReleaseObjectVersion.h"
#include "UObject/PropertyPortFlags.h"
#include "UObject/Package.h"
#include "RigVMModule.h"

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void FRigVMRegister::Serialize(FArchive& Ar)
{
	Ar.UsingCustomVersion(FAnimObjectVersion::GUID);

	if (Ar.CustomVer(FAnimObjectVersion::GUID) < FAnimObjectVersion::StoreMarkerNamesOnSkeleton)
	{
		return;
	}

	uint16 SliceIndex = 0;

	Ar << Type;
	Ar << ByteIndex;
	Ar << ElementSize;
	Ar << ElementCount;
	Ar << SliceIndex;
	Ar << SliceCount;
	Ar << AlignmentBytes;
	Ar << TrailingBytes;
	Ar << Name;
	Ar << ScriptStructIndex;

	if (Ar.IsSaving() || Ar.IsObjectReferenceCollector() || Ar.IsCountingMemory())
	{
		Save(Ar);
	}
	else if (Ar.IsLoading())
	{
		Load(Ar);
	}
	else
	{
		checkNoEntry();
	}
}

void FRigVMRegister::Save(FArchive& Ar)
{
	Ar << bIsArray;
	Ar << bIsDynamic;
}

void FRigVMRegister::Load(FArchive& Ar)
{
	if (Ar.CustomVer(FAnimObjectVersion::GUID) >= FAnimObjectVersion::SerializeRigVMRegisterArrayState)
	{
		Ar << bIsArray;
	}
	else
	{
		bIsArray = false;
	}

	if (Ar.CustomVer(FAnimObjectVersion::GUID) >= FAnimObjectVersion::SerializeRigVMRegisterDynamicState)
	{
		Ar << bIsDynamic;
	}
	else
	{
		bIsDynamic = false;
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void FRigVMRegisterOffset::Serialize(FArchive& Ar)
{
	Ar.UsingCustomVersion(FAnimObjectVersion::GUID);
	Ar.UsingCustomVersion(FReleaseObjectVersion::GUID);

	if (Ar.CustomVer(FAnimObjectVersion::GUID) < FAnimObjectVersion::StoreMarkerNamesOnSkeleton)
	{
		return;
	}

	if (Ar.IsSaving() || Ar.IsObjectReferenceCollector() || Ar.IsCountingMemory())
	{
		Save(Ar);
	}
	else if (Ar.IsLoading())
	{
		Load(Ar);
	}
	else
	{
		checkNoEntry();
	}
}

void FRigVMRegisterOffset::Save(FArchive& Ar)
{
	Ar << Segments;
	Ar << Type;
	Ar << CPPType;

	Ar << ScriptStruct;
	
	Ar << ElementSize;
	Ar << ParentScriptStruct;
	Ar << CachedSegmentPath;
	Ar << ArrayIndex;
}

void FRigVMRegisterOffset::Load(FArchive& Ar)
{
	Ar << Segments;
	Ar << Type;
	Ar << CPPType;

	if (Ar.CustomVer(FReleaseObjectVersion::GUID) < FReleaseObjectVersion::SerializeRigVMOffsetSegmentPaths)
	{
		FName ScriptStructPath;
		Ar << ScriptStructPath;

		ScriptStruct = FindObject<UScriptStruct>(ANY_PACKAGE, *ScriptStructPath.ToString());
	}
	else
	{
		Ar << ScriptStruct;
	}

	Ar << ElementSize;

	if (Ar.CustomVer(FReleaseObjectVersion::GUID) >= FReleaseObjectVersion::SerializeRigVMOffsetSegmentPaths)
	{
		FString SegmentPath;
		Ar << ParentScriptStruct;
		Ar << SegmentPath;
		Ar << ArrayIndex;

		if (Ar.IsTransacting())
		{
			CachedSegmentPath = SegmentPath;
		}
		else if (ParentScriptStruct != nullptr)
		{
			// if segment path is empty, it implies that the register offset refers to an element in a struct array
			// so segments also need to be recalculated
			int32 InitialOffset = ArrayIndex * ParentScriptStruct->GetStructureSize();
			FRigVMRegisterOffset TempOffset(ParentScriptStruct, SegmentPath, InitialOffset, ElementSize);
			if(TempOffset.Type == ERigVMRegisterType::Invalid)
			{
				Type = ERigVMRegisterType::Invalid;
				CachedSegmentPath = SegmentPath;
				return;
			}
			
			if (TempOffset.GetSegments().Num() == Segments.Num())
			{
				Segments = TempOffset.GetSegments();
				CachedSegmentPath = SegmentPath;
			}
			else
			{
				checkNoEntry();
			}
		}
	}
}

FRigVMRegisterOffset::FRigVMRegisterOffset(UScriptStruct* InScriptStruct, const FString& InSegmentPath, int32 InInitialOffset, uint16 InElementSize, const FName& InCPPType)
	: Segments()
	, Type(ERigVMRegisterType::Plain)
	, CPPType(InCPPType)
	, ScriptStruct(nullptr)
	, ParentScriptStruct(nullptr)
	, ArrayIndex(0)
	, ElementSize(InElementSize)
	, CachedSegmentPath(InSegmentPath)
{
	ParentScriptStruct = InScriptStruct;

	if (ParentScriptStruct)
	{
		ArrayIndex = InInitialOffset / InScriptStruct->GetStructureSize();
	}
	else
	{
		ArrayIndex = InInitialOffset / InElementSize;
	}

	struct FRigVMRegisterOffsetBuilder
	{
		static bool WalkStruct(UStruct* InStruct, const FString& InPath, FRigVMRegisterOffset& Offset)
		{
			FString Left, Right;
			if (!InPath.Split(TEXT("."), &Left, &Right))
			{
				Left = InPath;
			}

			FProperty* Property = InStruct->FindPropertyByName(*Left);
			if(Property == nullptr)
			{
				return false;
			}

			int32 SegmentIndex = Property->GetOffset_ReplaceWith_ContainerPtrToValuePtr();
			if (Offset.Segments.Num() > 0)
			{
				if (Offset.Segments.Last() >= 0)
				{
					Offset.Segments[Offset.Segments.Num() - 1] += SegmentIndex;
				}
				else
				{
					Offset.Segments.Add(SegmentIndex);
				}
			}
			else
			{
				Offset.Segments.Add(SegmentIndex);
			}

			if (!Right.IsEmpty())
			{
				if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
				{
					return WalkStruct(StructProperty->Struct, Right, Offset);
				}
				else if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
				{
					return WalkArray(ArrayProperty, Right, Offset);
				}
			}
			else
			{
				Offset.CPPType = *Property->GetCPPType();
				Offset.ElementSize = Property->ElementSize;

				if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
				{
					Offset.Segments.Add(-1);
					Property = ArrayProperty->Inner;

					Offset.CPPType = *ArrayProperty->Inner->GetCPPType();
					Offset.ElementSize = ArrayProperty->Inner->ElementSize;
				}

				if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
				{
					Offset.ScriptStruct = StructProperty->Struct;
					Offset.Type = ERigVMRegisterType::Struct;
				}
				else
				{
					Offset.Type = ERigVMRegisterType::Plain;
				}
			}

			return true;
		}

		static bool WalkArray(FArrayProperty* InArrayProperty, const FString& InPath, FRigVMRegisterOffset& Offset)
		{
			FString Left, Right;
			if (!InPath.Split(TEXT("."), &Left, &Right))
			{
				Left = InPath;
			}

			int32 ArrayIndex = FCString::Atoi(*Left);
			int32 SegmentIndex = -1 - InArrayProperty->Inner->ElementSize * ArrayIndex;

			if (Offset.Segments.Num() > 0)
			{
				if (Offset.Segments.Last() == 0)
				{
					Offset.Segments[Offset.Segments.Num() - 1] = SegmentIndex;
				}
				else
				{
					Offset.Segments.Add(SegmentIndex);
				}
			}
			else
			{
				Offset.Segments.Add(SegmentIndex);
			}

			if (!Right.IsEmpty())
			{
				if (FStructProperty* StructProperty = CastField<FStructProperty>(InArrayProperty->Inner))
				{
					return WalkStruct(StructProperty->Struct, Right, Offset);
				}
				else if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(InArrayProperty->Inner))
				{
					return WalkArray(ArrayProperty, Right, Offset);
				}
			}
			else
			{
				Offset.CPPType = *InArrayProperty->Inner->GetCPPType();
				Offset.ElementSize = InArrayProperty->Inner->ElementSize;

				if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(InArrayProperty->Inner))
				{
					Offset.Segments.Add(-1);
					InArrayProperty = ArrayProperty;
				}

				if (FStructProperty* StructProperty = CastField<FStructProperty>(InArrayProperty->Inner))
				{
					Offset.ScriptStruct = StructProperty->Struct;
					Offset.Type = ERigVMRegisterType::Struct;
				}
				else
				{
					Offset.Type = ERigVMRegisterType::Plain;
				}
			}

			return true;
		}
	};

	Segments.Add(InInitialOffset);

	// if segment path is not empty, it implies that the register offset refers to a sub-property in a struct pin
	if (!InSegmentPath.IsEmpty())
	{
		ensure(!InSegmentPath.IsEmpty());
		check(InScriptStruct)
		FString SegmentPath = InSegmentPath;
		SegmentPath = SegmentPath.Replace(TEXT("["), TEXT("."));
		SegmentPath = SegmentPath.Replace(TEXT("]"), TEXT("."));
		if(!FRigVMRegisterOffsetBuilder::WalkStruct(InScriptStruct, SegmentPath, *this))
		{
			Type = ERigVMRegisterType::Invalid;
			return;
		}
		
		if (Type == ERigVMRegisterType::Plain)
		{
			if (CPPType == TEXT("FName"))
			{
				Type = ERigVMRegisterType::Name;
			}
			else if (CPPType == TEXT("FString"))
			{
				Type = ERigVMRegisterType::String;
			}
		}
	}
	else
	{
		// if segment path is empty, it implies that the register offset refers to an element in a struct array
		if (ParentScriptStruct)
		{
			ScriptStruct = ParentScriptStruct;
			Type = ERigVMRegisterType::Struct;
			CPPType = *ScriptStruct->GetStructCPPName();
		}
	}

	ensure(ElementSize > 0);
}

uint8* FRigVMRegisterOffset::GetData(uint8* InContainer) const
{
	uint8* Data = InContainer;
	for(int32 SegmentIndex : Segments)
	{
		if (SegmentIndex < 0)
		{
			int32 ArrayOffset = (-SegmentIndex) - 1;
			FRigVMByteArray* ArrayPtr = (FRigVMByteArray*)Data;
			Data = ArrayPtr->GetData() + ArrayOffset;
		}
		else
		{
			Data = Data + SegmentIndex;
		}
	}
	return Data;
}

bool FRigVMRegisterOffset::ContainsArraySegment() const
{
	for (int32 SegmentIndex : Segments)
	{
		if (SegmentIndex < 0) // Segement indices less than zero represent array element offsets.
		{
			return true;
		}
	}
	return false;
}

bool FRigVMRegisterOffset::operator == (const FRigVMRegisterOffset& InOther) const
{
	if (Segments.Num() != InOther.Segments.Num())
	{
		return false;
	}
	if (ElementSize != InOther.ElementSize)
	{
		return false;
	}
	if (GetScriptStruct() != InOther.GetScriptStruct())
	{
		return false;
	}
	if (ParentScriptStruct != InOther.ParentScriptStruct)
	{
		return false;
	}
	if (CachedSegmentPath != InOther.CachedSegmentPath)
	{
		return false;
	}
	for (int32 Index = 0; Index < Segments.Num(); Index++)
	{
		if (Segments[Index] != InOther.Segments[Index])
		{
			return false;
		}
	}
	return true;
}

UScriptStruct* FRigVMRegisterOffset::GetScriptStruct() const
{
	return ScriptStruct;
}

uint16 FRigVMRegisterOffset::GetElementSize() const
{
	return ElementSize;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class FRigVMMemoryContainerImportErrorContext : public FOutputDevice
{
public:

	int32 NumErrors;

	FRigVMMemoryContainerImportErrorContext()
		: FOutputDevice()
		, NumErrors(0)
	{
	}

	FORCEINLINE_DEBUGGABLE void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const class FName& Category) override
	{
#if WITH_EDITOR
		UE_LOG(LogRigVM, Display, TEXT("Skipping Importing To MemoryContainer: %s"), V);
#else
		UE_LOG(LogRigVM, Error, TEXT("Error Importing To MemoryContainer: %s"), V);
#endif
		NumErrors++;
	}
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED

FRigVMByteArray FRigVMMemoryContainer::DefaultByteArray;
FRigVMRegisterOffset FRigVMMemoryContainer::InvalidRegisterOffset;

#endif

FRigVMMemoryContainer::FRigVMMemoryContainer(bool bInUseNames)
	: bUseNameMap(bInUseNames)
	, MemoryType(ERigVMMemoryType::Work)
	, bEncounteredErrorDuringLoad(false)
{
}

FRigVMMemoryContainer::FRigVMMemoryContainer(const FRigVMMemoryContainer& Other)
{
	*this = Other;
}

FRigVMMemoryContainer::~FRigVMMemoryContainer()
{
	Reset();
}

bool FRigVMMemoryContainer::CopyRegisters(const FRigVMMemoryContainer &InOther)
{
	ensure(Registers.Num() == InOther.Registers.Num());
	for (int32 Index = 0; Index < Registers.Num(); ++Index)
	{
#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
		
		if (!Copy(Index, Index, &InOther))
		{
			return false;
		}

#endif
	}
	return true;
}

FRigVMMemoryContainer& FRigVMMemoryContainer::operator= (const FRigVMMemoryContainer &InOther)
{
	Reset();

	MemoryType = InOther.MemoryType;
	bUseNameMap = InOther.bUseNameMap;
	bEncounteredErrorDuringLoad = false;
	Data.Append(InOther.Data);
	Registers.Append(InOther.Registers);
	RegisterOffsets.Append(InOther.RegisterOffsets);
	ScriptStructs.Append(InOther.ScriptStructs);

	UpdateRegisters();

	for (int32 Index = 0; Index < Registers.Num(); Index++)
	{
		const FRigVMRegister& Register = Registers[Index];
		if (Register.IsNestedDynamic())
		{
			FMemory::Memzero(&Data[Register.GetWorkByteIndex()], sizeof(FRigVMNestedByteArray));
			*(FRigVMNestedByteArray*)&Data[Register.GetWorkByteIndex()] = FRigVMNestedByteArray();
		}
		else if(Register.IsDynamic())
		{
			FMemory::Memzero(&Data[Register.GetWorkByteIndex()], sizeof(FRigVMByteArray));
			*(FRigVMByteArray*)&Data[Register.GetWorkByteIndex()] = FRigVMByteArray();
		}

		Construct(Index);

		// Why was this introduced? Are we trying to save time here?
		// An assignment should be copying everything.
#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED

		if (MemoryType == ERigVMMemoryType::Literal)
		{
			Copy(Index, Index, &InOther);
		}

#endif
	}

	return *this;
}

void FRigVMMemoryContainer::Serialize(FArchive& Ar)
{
	Ar.UsingCustomVersion(FAnimObjectVersion::GUID);

 	if (Ar.CustomVer(FAnimObjectVersion::GUID) < FAnimObjectVersion::StoreMarkerNamesOnSkeleton)
 	{
 		return;
 	}

	if (Ar.IsSaving() || Ar.IsObjectReferenceCollector() || Ar.IsCountingMemory())
	{
		Save(Ar);
	}
	else if (Ar.IsLoading())
	{
		Load(Ar);
	}
	else
	{
		checkNoEntry();
	}
}

void FRigVMMemoryContainer::Save(FArchive& Ar)
{
#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED

	for (FRigVMRegister& Register : Registers)
	{
		if (Register.IsNestedDynamic())
		{
			FRigVMNestedByteArray* NestedArrayStorage = (FRigVMNestedByteArray*)&Data[Register.GetWorkByteIndex()];
			Register.SliceCount = NestedArrayStorage->Num();
		}
		else if (Register.IsDynamic())
		{
			FRigVMByteArray* ArrayStorage = (FRigVMByteArray*)&Data[Register.GetWorkByteIndex()];
			Register.SliceCount = ArrayStorage->Num();
		}
	}

	Ar << bUseNameMap;
	Ar << MemoryType;
	Ar << Registers;
	Ar << RegisterOffsets;
	
	TArray<FString> ScriptStructPaths;
	for (UScriptStruct* ScriptStruct : ScriptStructs)
	{
		ScriptStructPaths.Add(ScriptStruct->GetPathName());
	}
	Ar << ScriptStructPaths;

	uint64 TotalBytes = Data.Num();
	Ar << TotalBytes;

	// if we are running a memory count
	// there's no need to do a deep serialize
	// since we know the memory needed
	if(Ar.IsCountingMemory())
	{
		Ar << Data;
		return;
	}

	for (FRigVMRegister& Register : Registers)
	{
		if (Register.ElementCount == 0 && !Register.IsDynamic())
		{
			continue;
		}

		// literal mem container should not have any dynamic registers,
		// if we ever added them, we might need to re-consider if we need to serialize them
		// by default dynamic registers' memory are not serialized
		if (MemoryType == ERigVMMemoryType::Literal)
		{
			check(!Register.IsDynamic());
		}

		if (!Register.IsDynamic())
		{
			switch (Register.Type)
			{
				case ERigVMRegisterType::Plain:
				{
					FRigVMByteArray View;
					View.Append(&Data[Register.GetWorkByteIndex()], Register.GetAllocatedBytes() - Register.GetAlignmentBytes());
					Ar << View;
					break;
				}
				case ERigVMRegisterType::Name:
				{
					TArray<FName> View;
					View.Append((FName*)&Data[Register.GetWorkByteIndex()], Register.GetTotalElementCount());
					Ar << View;
					break;
				}
				case ERigVMRegisterType::String:
				{
					TArray<FString> View;
					View.Append((FString*)&Data[Register.GetWorkByteIndex()], Register.GetTotalElementCount());
					Ar << View;
					break;
				}
				case ERigVMRegisterType::Struct:
				{
					uint8* DataPtr = &Data[Register.GetWorkByteIndex()];
					UScriptStruct* ScriptStruct = GetScriptStruct(Register);

					TArray<uint8, TAlignedHeapAllocator<16>> DefaultStructData;
					DefaultStructData.AddZeroed(ScriptStruct->GetStructureSize());
					ScriptStruct->InitializeDefaultValue(DefaultStructData.GetData());

					TArray<FString> View;
					for (uint16 ElementIndex = 0; ElementIndex < Register.GetTotalElementCount(); ElementIndex++)
					{
						FString Value;
						ScriptStruct->ExportText(Value, DataPtr, DefaultStructData.GetData(), nullptr, PPF_None, nullptr);
						View.Add(Value);
						DataPtr += Register.ElementSize;
					}

					ScriptStruct->DestroyStruct(DefaultStructData.GetData(), 1);

					Ar << View;
					break;
				}
			}
		}
		else if (!Register.IsNestedDynamic())
		{
			FRigVMByteArray* ArrayStorage = (FRigVMByteArray*)&Data[Register.GetWorkByteIndex()];
			
			// notice that all dynamic registers' memory is not really serialized
			// serialize empty arrays here just to avoid having to increment serialization version
			// it does not need to be saved since it will be recomputed during execution
			switch (Register.Type)
			{
				case ERigVMRegisterType::Plain:
				{
					FRigVMByteArray DummyStorage;
					Ar << DummyStorage;
					break;
				}
				case ERigVMRegisterType::Name:
				{
					TArray<FName> View;
					Ar << View;
					break;
				}
				case ERigVMRegisterType::String:
				{
					TArray<FString> View;
					Ar << View;
					break;
				}
				case ERigVMRegisterType::Struct:
				{
					TArray<FString> View;
					Ar << View;
					break;
				}
			}
		}
		else
		{
			FRigVMNestedByteArray* NestedArrayStorage = (FRigVMNestedByteArray*)&Data[Register.GetWorkByteIndex()];

			for (int32 SliceIndex = 0; SliceIndex < Register.SliceCount; SliceIndex++)
			{
				if (NestedArrayStorage->Num() <= SliceIndex)
				{
					FRigVMByteArray EmptyStorage;
					Ar << EmptyStorage;
					continue;
				}

				FRigVMByteArray& ArrayStorage = (*NestedArrayStorage)[SliceIndex];

				// notice that all dynamic registers' memory is not really serialized
				// serialize empty arrays here just to avoid having to increment serialization version
				// it does not need to be saved since it will be recomputed during execution
				switch (Register.Type)
				{
					case ERigVMRegisterType::Plain:
					{
						FRigVMByteArray DummyStorage;
						Ar << DummyStorage;
						break;
					}
					case ERigVMRegisterType::Name:
					{
						TArray<FName> View;
						Ar << View;
						break;
					}
					case ERigVMRegisterType::String:
					{
						TArray<FString> View;
						Ar << View;
						break;
					}
					case ERigVMRegisterType::Struct:
					{
						TArray<FString> View;
						Ar << View;
						break;
					}
				}
			}
		}
	}
#endif
}

void FRigVMMemoryContainer::Load(FArchive& Ar)
{
	for (int32 RegisterIndex = 0; RegisterIndex < Registers.Num(); RegisterIndex++)
	{
		Destroy(RegisterIndex);
	}
	
	Ar << bUseNameMap;
	Ar << MemoryType;
	Ar << Registers;
	Ar << RegisterOffsets;

#if DEBUG_RIGVMMEMORY
	UE_LOG_RIGVMMEMORY(TEXT("%d Memory - Begin Loading..."), (int32)GetMemoryType());
#endif

	bEncounteredErrorDuringLoad = false;

	for(const FRigVMRegisterOffset& RegisterOffset : RegisterOffsets)
	{
		if(RegisterOffset.GetType() == ERigVMRegisterType::Invalid)
		{
			FString PackagePath = Ar.GetArchiveName();
			UE_LOG(LogRigVM, Error, TEXT("RegisterOffset '%s' cannot be found. Asset '%s' no longer functional."), *RegisterOffset.GetCachedSegmentPath(), *PackagePath);
			bEncounteredErrorDuringLoad = true;
		}
	}

	ScriptStructs.Reset();
	TArray<FString> ScriptStructPaths;
	Ar << ScriptStructPaths;

	for (const FString& ScriptStructPath : ScriptStructPaths)
	{
		UScriptStruct* ScriptStruct = FindObject<UScriptStruct>(nullptr, *ScriptStructPath);

		// this might have happened if a given script struct no longer 
		// exists or cannot be loaded.
		if (ScriptStruct == nullptr)
		{
			FString PackagePath = Ar.GetArchiveName();
			UE_LOG(LogRigVM, Error, TEXT("Struct '%s' cannot be found. Asset '%s' no longer functional."), *ScriptStructPath, *PackagePath);
			bEncounteredErrorDuringLoad = true;
		}

		ScriptStructs.Add(ScriptStruct);
	}

	uint64 TotalBytes = 0;
	Ar << TotalBytes;

	Data.Empty();

	if (!bEncounteredErrorDuringLoad)
	{

		// during load we'll recreate the memory for all registers.
		// the size for structs might have changed, so we need to reallocate.
		for (int32 RegisterIndex = 0; RegisterIndex < Registers.Num(); RegisterIndex++)
		{
			FRigVMRegister& Register = Registers[RegisterIndex];

			UScriptStruct* ScriptStruct = GetScriptStruct(Register);
			if (ScriptStruct)
			{
				Register.ElementSize = ScriptStruct->GetStructureSize();
			}
			else if (Register.Type == ERigVMRegisterType::Name)
			{
				Register.ElementSize = sizeof(FName);
			}
			else if (Register.Type == ERigVMRegisterType::String)
			{
				Register.ElementSize = sizeof(FString);
			}

			Register.AlignmentBytes = 0;
			Register.TrailingBytes = 0;

			if (Registers[RegisterIndex].IsDynamic())
			{
				Register.ByteIndex = Data.AddZeroed(sizeof(FRigVMByteArray));
			}
			else
			{
				Register.ByteIndex = Data.AddZeroed(Register.GetNumBytesAllSlices());
			}
		}

		UpdateRegisters();

		for (int32 RegisterOffsetIndex = 0; RegisterOffsetIndex < RegisterOffsets.Num(); RegisterOffsetIndex++)
		{
			FRigVMRegisterOffset& RegisterOffset = RegisterOffsets[RegisterOffsetIndex];

			UScriptStruct* ScriptStruct = RegisterOffset.GetScriptStruct();
			if (ScriptStruct)
			{
				RegisterOffset.SetElementSize(ScriptStruct->GetStructureSize());
			}
			if (RegisterOffset.GetType() == ERigVMRegisterType::Name)
			{
				RegisterOffset.SetElementSize(sizeof(FName));
			}
			else if (RegisterOffset.GetType() == ERigVMRegisterType::String)
			{
				RegisterOffset.SetElementSize(sizeof(FString));
			}
		}

		// once the register memory is allocated we can construt its contents.
		for (int32 RegisterIndex = 0; RegisterIndex < Registers.Num(); RegisterIndex++)
		{
			const FRigVMRegister& Register = Registers[RegisterIndex];
			if (!Registers[RegisterIndex].IsDynamic())
			{
				Construct(RegisterIndex);
			}
		}
	}

	for (int32 RegisterIndex = 0; RegisterIndex < Registers.Num(); RegisterIndex++)
	{
		FRigVMRegister& Register = Registers[RegisterIndex];

		if (Register.ElementCount == 0 && !Register.IsDynamic())
		{
			continue;
		}

		// literal mem container should not have any dynamic registers,
		// if we ever added them, we might need to re-consider if we need to serialize them
		// by default dynamic registers' memory are not serialized
		if (MemoryType == ERigVMMemoryType::Literal)
		{
			check(!Register.IsDynamic());
		}
		
		if (!Register.IsDynamic())
		{
			switch (Register.Type)
			{
				case ERigVMRegisterType::Plain:
				{
					FRigVMByteArray View;
					Ar << View;

					if (!bEncounteredErrorDuringLoad)
					{
						ensure(View.Num() <= Register.GetAllocatedBytes());
						FMemory::Memcpy(&Data[Register.GetWorkByteIndex()], View.GetData(), View.Num());
					}
					break;
				}
				case ERigVMRegisterType::Name:
				{
					TArray<FName> View;
					Ar << View;

					if (!bEncounteredErrorDuringLoad)
					{
						ensure(View.Num() == Register.GetTotalElementCount());
						RigVMCopy<FName>(&Data[Register.GetWorkByteIndex()], View.GetData(), View.Num());
					}
					break;
				}
				case ERigVMRegisterType::String:
				{
					TArray<FString> View;
					Ar << View;

					if (!bEncounteredErrorDuringLoad)
					{
						ensure(View.Num() == Register.GetTotalElementCount());
						RigVMCopy<FString>(&Data[Register.GetWorkByteIndex()], View.GetData(), View.Num());
					}
					break;
				}
				case ERigVMRegisterType::Struct:
				{
					TArray<FString> View;
					Ar << View;

					if (!bEncounteredErrorDuringLoad)
					{
						ensure(View.Num() == Register.GetTotalElementCount());

						uint8* DataPtr = &Data[Register.GetWorkByteIndex()];
						UScriptStruct* ScriptStruct = GetScriptStruct(Register);
						if (ScriptStruct && !bEncounteredErrorDuringLoad)
						{
							ensure(ScriptStruct->GetStructureSize() == Register.ElementSize);

							for (uint16 ElementIndex = 0; ElementIndex < Register.GetTotalElementCount(); ElementIndex++)
							{
								FRigVMMemoryContainerImportErrorContext ErrorPipe;
								ScriptStruct->ImportText(*View[ElementIndex], DataPtr, nullptr, PPF_None, &ErrorPipe, ScriptStruct->GetName());
								if (ErrorPipe.NumErrors > 0)
								{
									bEncounteredErrorDuringLoad = true;
									break;
								}
								DataPtr += Register.ElementSize;
							}
						}
					}
					break;
				}
			}
		}
		else if (!Register.IsNestedDynamic())
		{

			FRigVMByteArray* ArrayStorage = nullptr;

			if (!bEncounteredErrorDuringLoad)
			{
				ArrayStorage = (FRigVMByteArray*)&Data[Register.GetWorkByteIndex()];
			}
			
			// notice that all dynamic registers' memory is not really serialized
			// serialize empty arrays here just to avoid having to increment serialization version
			// the loaded data is not getting used at all, it will be recomputed at execution time
			switch (Register.Type)
			{
				case ERigVMRegisterType::Plain:
				{
					FRigVMByteArray View;
					Ar << View;
					break;
				}
				case ERigVMRegisterType::Name:
				{
					TArray<FName> View;
					Ar << View;
					break;
				}
				case ERigVMRegisterType::String:
				{
					TArray<FString> View;
					Ar << View;
					break;
				}
				case ERigVMRegisterType::Struct:
				{
					TArray<FString> View;
					Ar << View;
					break;
				}
			}
		}
		else
		{
			
			FRigVMNestedByteArray* NestedArrayStorage = nullptr;
			if (!bEncounteredErrorDuringLoad)
			{
				NestedArrayStorage = (FRigVMNestedByteArray*)&Data[Register.GetWorkByteIndex()];
				NestedArrayStorage->Reset();
				NestedArrayStorage->SetNumZeroed(Register.SliceCount);
			}
			
			for (int32 SliceIndex = 0; SliceIndex < Register.SliceCount; SliceIndex++)
			{
				FRigVMByteArray* ArrayStorage = nullptr;
				if (NestedArrayStorage)
				{
					ArrayStorage = &(*NestedArrayStorage)[SliceIndex];
				}

				// notice that all dynamic registers' memory is not really serialized
				// serialize empty arrays here just to avoid having to increment serialization version
				// the loaded data is not getting used at all, it will be recomputed at execution time
				switch (Register.Type)
				{
					case ERigVMRegisterType::Plain:
					{
						FRigVMByteArray View;
						Ar << View;
						break;
					}
					case ERigVMRegisterType::Name:
					{
						TArray<FName> View;
						Ar << View;
						break;
					}
					case ERigVMRegisterType::String:
					{
						TArray<FString> View;
						Ar << View;
						break;
					}
					case ERigVMRegisterType::Struct:
					{
						TArray<FString> View;
						Ar << View;
						break;
					}
				}
			}
		}
	}


#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED

	if (bEncounteredErrorDuringLoad)
	{
#if DEBUG_RIGVMMEMORY
		UE_LOG_RIGVMMEMORY(TEXT("%d Memory - Encountered errors during load."), (int32)GetMemoryType());
#endif
		Reset();
	}
	else
	{
		UpdateRegisters();
	}

#if DEBUG_RIGVMMEMORY
	UE_LOG_RIGVMMEMORY(TEXT("%d Memory - Finished Loading."), (int32)GetMemoryType());
#endif

#endif
}

void FRigVMMemoryContainer::Reset()
{
	if (Data.Num() > 0)
	{
		for (int32 Index = 0; Index < Registers.Num(); Index++)
		{
			Destroy(Index);
		}
	}

	Data.Reset();
	Registers.Reset();
	RegisterOffsets.Reset();
	ScriptStructs.Reset();
	NameMap.Reset();
}

void FRigVMMemoryContainer::Empty()
{
	Data.Empty();
	Registers.Empty();
	RegisterOffsets.Empty();
	ScriptStructs.Empty();
	NameMap.Empty();
}

#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED

bool FRigVMMemoryContainer::Copy(
	int32 InTargetRegisterIndex,
	int32 InTargetRegisterOffset,
	ERigVMRegisterType InTargetType,
	const uint8* InSourcePtr,
	uint8* InTargetPtr,
	uint16 InNumBytes)
{
	switch (InTargetType)
	{
		case ERigVMRegisterType::Plain:
		{
			FMemory::Memcpy(InTargetPtr, InSourcePtr, InNumBytes);
			break;
		}
		case ERigVMRegisterType::Struct:
		{
			UScriptStruct* ScriptStruct = GetScriptStruct(InTargetRegisterIndex, InTargetRegisterOffset);
			int32 NumStructs = InNumBytes / ScriptStruct->GetStructureSize();
			ensure(NumStructs * ScriptStruct->GetStructureSize() == InNumBytes);
			if (NumStructs > 0 && InTargetPtr)
			{
				ScriptStruct->CopyScriptStruct(InTargetPtr, InSourcePtr, NumStructs);
			}
			break;
		}
		case ERigVMRegisterType::Name:
		{
			int32 NumNames = InNumBytes / sizeof(FName);
			ensure(NumNames * sizeof(FName) == InNumBytes);
			RigVMCopy<FName>(InTargetPtr, InSourcePtr, NumNames);
			break;
		}
		case ERigVMRegisterType::String:
		{
			int32 NumStrings = InNumBytes / sizeof(FString);
			ensure(NumStrings * sizeof(FString) == InNumBytes);
			RigVMCopy<FString>(InTargetPtr, InSourcePtr, NumStrings);
			break;
		}
		case ERigVMRegisterType::Invalid:
		{
			return false;
		}
	}

	return true;
}

bool FRigVMMemoryContainer::Copy(
	int32 InSourceRegisterIndex,
	int32 InTargetRegisterIndex,
	const FRigVMMemoryContainer* InSourceMemory,
	int32 InSourceRegisterOffset,
	int32 InTargetRegisterOffset,
	int32 InSourceSliceIndex,
	int32 InTargetSliceIndex)
{
	if (InSourceMemory == nullptr)
	{
		InSourceMemory = this;
	}

	ensure(InSourceMemory->Registers.IsValidIndex(InSourceRegisterIndex));
	ensure(Registers.IsValidIndex(InTargetRegisterIndex));

	if (InSourceRegisterIndex == InTargetRegisterIndex && InSourceRegisterOffset == InTargetRegisterOffset && this == InSourceMemory)
	{
		return false;
	}

	const FRigVMRegister& Source = InSourceMemory->Registers[InSourceRegisterIndex];
	const FRigVMRegister& Target = Registers[InTargetRegisterIndex];

	// todo: copy between slices!
	int32 SourceElementCount = Source.ElementCount;
	const uint8* SourcePtr = InSourceMemory->GetDataPtr(Source, InSourceRegisterOffset, InSourceSliceIndex, false);
	uint8* TargetPtr = GetDataPtr(Target, InTargetRegisterOffset, InTargetSliceIndex, false);
	uint16 NumBytes = Target.GetNumBytesPerSlice();

	// TODO: preventing crash when SourcePtr is null
	if (SourcePtr == nullptr)
	{
		return false;
	}
	
	if (Source.IsNestedDynamic())
	{
#if WITH_EDITOR
		ensure(InSourceRegisterOffset == INDEX_NONE);
#else
		if (InSourceRegisterOffset != INDEX_NONE)
		{
			return true;
		}
#endif
		FRigVMByteArray* ArrayStorage = (FRigVMByteArray*)SourcePtr;
		SourceElementCount = ArrayStorage->Num() / Source.ElementSize;
		SourcePtr = ArrayStorage->GetData();
	}
	else if (Source.IsDynamic())
	{
		FRigVMByteArray* ArrayStorage = (FRigVMByteArray*)SourcePtr;
		SourcePtr = ArrayStorage->GetData();
	}

	// TODO: preventing crash when TargetPtr has not been allocated 
	if (TargetPtr == nullptr && Target.IsNestedDynamic())
	{
		uint8* Ptr = (uint8*)&Data[Target.GetWorkByteIndex()];
		FRigVMNestedByteArray& Storage = *(FRigVMNestedByteArray*)Ptr;
		Storage.SetNum(Target.SliceCount);
		Storage[InTargetSliceIndex].SetNumZeroed(SourceElementCount * Source.ElementSize);
		Copy(InTargetRegisterIndex, INDEX_NONE, Source.Type, SourcePtr, Storage[InTargetSliceIndex].GetData(), Source.ElementSize * SourceElementCount);
		return true;
	}

	ERigVMRegisterType TargetType = Target.Type;
	if (Target.IsDynamic() && !Target.bIsArray)
	{
		FRigVMByteArray* ArrayStorage = (FRigVMByteArray*)TargetPtr;
		TargetPtr = ArrayStorage->GetData();
	}
	else if (TargetPtr && Target.IsNestedDynamic())
	{
#if WITH_EDITOR
		ensure(InTargetRegisterOffset == INDEX_NONE);
#else
		if (InTargetRegisterOffset != INDEX_NONE)
		{
			return true;
		}
		return true;
#endif

		NumBytes = Source.ElementSize * SourceElementCount;

		FRigVMByteArray* ArrayStorage = (FRigVMByteArray*)TargetPtr;
		if (ArrayStorage->Num() != NumBytes)
		{
			Destroy(InTargetRegisterIndex, INDEX_NONE, InTargetSliceIndex);
			ArrayStorage->SetNumZeroed(NumBytes);
			Construct(InTargetRegisterIndex, INDEX_NONE, InTargetSliceIndex);

			TargetPtr = ArrayStorage->GetData();
		}
	}
	else if (InTargetRegisterOffset != INDEX_NONE)
	{
		if (Target.GetNumBytesPerSlice() == 0)
		{
			return true;
		}

		TargetType = RegisterOffsets[InTargetRegisterOffset].GetType();
		NumBytes = RegisterOffsets[InTargetRegisterOffset].GetElementSize();
	}
	else if (Target.GetNumBytesPerSlice() == 0)
	{
		return true;
	}

	return Copy(InTargetRegisterIndex, InTargetRegisterOffset, TargetType, SourcePtr, TargetPtr, NumBytes);
}

bool FRigVMMemoryContainer::Copy(
	const FName& InSourceName,
	const FName& InTargetName,
	const FRigVMMemoryContainer* InSourceMemory,
	int32 InSourceRegisterOffset,
	int32 InTargetRegisterOffset,
	int32 InSourceSliceIndex,
	int32 InTargetSliceIndex)
{
	ensure(bUseNameMap);

	int32 SourceRegisterIndex = GetIndex(InSourceName);
	int32 TargetRegisterIndex = GetIndex(InTargetName);

	if(SourceRegisterIndex == INDEX_NONE || TargetRegisterIndex == INDEX_NONE)
	{
		return false;
	}

	return Copy(SourceRegisterIndex, TargetRegisterIndex, InSourceMemory, InSourceRegisterOffset, InTargetRegisterOffset);
}

bool FRigVMMemoryContainer::Copy(
	const FRigVMOperand& InSourceOperand,
	const FRigVMOperand& InTargetOperand,
	const FRigVMMemoryContainer* InSourceMemory,
	int32 InSourceSliceIndex,
	int32 InTargetSliceIndex)
{
	return Copy(InSourceOperand.GetRegisterIndex(), InTargetOperand.GetRegisterIndex(), InSourceMemory, InSourceOperand.GetRegisterOffset(), InTargetOperand.GetRegisterOffset(), InSourceSliceIndex, InTargetSliceIndex);
}

#endif

int32 FRigVMMemoryContainer::Allocate(const FName& InNewName, int32 InElementSize, int32 InElementCount, int32 InSliceCount, const uint8* InDataPtr, bool bUpdateRegisters)
{
	FName Name = InNewName;
	if (bUseNameMap && InNewName == NAME_None)
	{
		const TCHAR* RegisterPrefix = TEXT("Register");
		int32 RegisterSuffix = 0;
		do
		{
			Name = FName(*FString::Printf(TEXT("%s_%d"), RegisterPrefix, RegisterSuffix++));
		} while (!IsNameAvailable(Name));
	}

	ensure(InElementSize > 0 && InElementCount >= 0 && InSliceCount > 0);

	if (bUseNameMap)
	{
		if (!IsNameAvailable(Name))
		{
			return INDEX_NONE;
		}
	}

	FRigVMRegister NewRegister;
	NewRegister.ByteIndex = Data.Num();
	if (bUseNameMap)
	{
		NewRegister.Name = Name;
	}
	NewRegister.ElementSize = (uint16)InElementSize;
	NewRegister.ElementCount = (uint16)InElementCount;
	NewRegister.SliceCount = (uint16)InSliceCount;
	NewRegister.Type = ERigVMRegisterType::Plain;

	Data.AddZeroed(NewRegister.GetAllocatedBytes());

#if DEBUG_RIGVMMEMORY
	if (NewRegister.ElementCount > 0)
	{
		UE_LOG_RIGVMMEMORY(TEXT("%d.%04d: Allocated %04d bytes at %04d (%s)."), (int32)GetMemoryType(), Registers.Num(), NewRegister.GetAllocatedBytes(), (int32)reinterpret_cast<long long>(&Data[NewRegister.ByteIndex]), *NewRegister.Name.ToString());
	}
#endif

	if (InDataPtr != nullptr)
	{
		for (uint16 SliceIndex = 0; SliceIndex < NewRegister.SliceCount; SliceIndex++)
		{
			FMemory::Memcpy(&Data[NewRegister.GetWorkByteIndex(SliceIndex)], InDataPtr, NewRegister.GetNumBytesPerSlice());
		}
	}

	int32 RegisterIndex = Registers.Num();
	Registers.Add(NewRegister);

	if (bUpdateRegisters)
	{
		UpdateRegisters();
	}
	return RegisterIndex;
}

int32 FRigVMMemoryContainer::Allocate(int32 InElementSize, int32 InElementCount, int32 InSliceCount, const uint8* InDataPtr, bool bUpdateRegisters)
{
	return Allocate(NAME_None, InElementSize, InElementCount, InSliceCount, InDataPtr, bUpdateRegisters);
}

bool FRigVMMemoryContainer::Construct(int32 InRegisterIndex, int32 InElementIndex, int32 InSliceIndex)
{
	ensure(Registers.IsValidIndex(InRegisterIndex));

	const FRigVMRegister& Register = Registers[InRegisterIndex];

	if (Register.ElementCount == 0 || (Register.Type == ERigVMRegisterType::Plain && !Register.IsDynamic()))
	{
		return true;
	}

	int32 ElementIndex = InElementIndex == INDEX_NONE ? 0 : InElementIndex;
	int32 ElementCount = InElementIndex == INDEX_NONE ? Register.GetTotalElementCount() : 1;
	int32 NumSlices = 1;

	uint8* TopDataPtr = &Data[Register.GetWorkByteIndex()];
	if (Register.IsDynamic())
	{
		if (Register.IsNestedDynamic())
		{
			FRigVMNestedByteArray& Storage = *(FRigVMNestedByteArray*)TopDataPtr;
			NumSlices = Storage.Num();
			TopDataPtr = (uint8*)Storage.GetData();
		}
		else
		{
			FRigVMByteArray& Storage = *(FRigVMByteArray*)TopDataPtr;
			ElementCount = Storage.Num() / Register.ElementSize;
			if (ElementCount == 0)
			{
				Storage.AddZeroed(Register.ElementSize);
				ElementCount = 1;
			}
			ensure(ElementCount * Register.ElementSize == Storage.Num());
			TopDataPtr = Storage.GetData();
		}
	}

	for (int32 SliceIndex = 0; SliceIndex < NumSlices; SliceIndex++)
	{
		if (InSliceIndex != INDEX_NONE)
		{
			SliceIndex = InSliceIndex;

			if (Register.IsNestedDynamic())
			{
				TopDataPtr = TopDataPtr + SliceIndex * sizeof(FRigVMByteArray);
			}
		}

		uint8* NestedDataPtr = TopDataPtr;
		if (Register.IsNestedDynamic())
		{
			FRigVMByteArray& Storage = *(FRigVMByteArray*)TopDataPtr;
			ElementCount = Storage.Num() / Register.ElementSize;
			ensure(ElementCount * Register.ElementSize == Storage.Num());
			NestedDataPtr = Storage.GetData();
		}

		switch (Register.Type)
		{
			case ERigVMRegisterType::Struct:
			{
				uint8* DataPtr = NestedDataPtr + ElementIndex * Register.ElementSize;
				UScriptStruct* ScriptStruct = GetScriptStruct(InRegisterIndex);

				if (ScriptStruct)
				{
					if (Register.ElementSize == ScriptStruct->GetStructureSize())
					{
						ScriptStruct->InitializeStruct(DataPtr, ElementCount);
#if DEBUG_RIGVMMEMORY
						UE_LOG_RIGVMMEMORY(TEXT("%d.%04d: Initialized struct, %04d bytes at %04d (%s)."), (int32)GetMemoryType(), InRegisterIndex, ElementCount * ScriptStruct->GetStructureSize(), (int32)reinterpret_cast<long long>(DataPtr), *Register.Name.ToString());
#endif
					}
				}
				break;
			}
			case ERigVMRegisterType::String:
			{
				FString* DataPtr = (FString*)(NestedDataPtr + ElementIndex * Register.ElementSize);
				RigVMInitialize<FString>(DataPtr, ElementCount);
#if DEBUG_RIGVMMEMORY
				UE_LOG_RIGVMMEMORY(TEXT("%d.%04d: Initialized string, %04d bytes at %04d (%s)."), (int32)GetMemoryType(), InRegisterIndex, ElementCount * Register.ElementSize, (int32)reinterpret_cast<long long>(DataPtr), *Register.Name.ToString());
#endif
				break;
			}
			case ERigVMRegisterType::Name:
			{
				FName* DataPtr = (FName*)(NestedDataPtr + ElementIndex * Register.ElementSize);
				RigVMInitialize<FName>(DataPtr, ElementCount);
#if DEBUG_RIGVMMEMORY
				UE_LOG_RIGVMMEMORY(TEXT("%d.%04d: Initialized name, %04d bytes at %04d (%s)."), (int32)GetMemoryType(), InRegisterIndex, ElementCount * Register.ElementSize, (int32)reinterpret_cast<long long>(DataPtr), *Register.Name.ToString());
#endif
				break;
			}
			default:
			{
				return false;
			}
		}

		if (Register.IsNestedDynamic())
		{
			TopDataPtr = TopDataPtr + sizeof(FRigVMByteArray);
		}

		if (InSliceIndex != INDEX_NONE)
		{
			break;
		}
	}

	return true;
}

bool FRigVMMemoryContainer::Destroy(int32 InRegisterIndex, int32 InElementIndex, int32 InSliceIndex)
{
	FRigVMRegister& Register = Registers[InRegisterIndex];

	if (Register.ElementCount == 0 || (Register.Type == ERigVMRegisterType::Plain && !Register.IsDynamic()))
	{
		return true;
	}

	int32 ElementIndex = InElementIndex == INDEX_NONE ? 0 : InElementIndex;
	int32 ElementCount = InElementIndex == INDEX_NONE ? Register.GetTotalElementCount() : 1;
	int32 NumSlices = 1;

	uint8* TopDataPtr = &Data[Register.GetWorkByteIndex()];
	if (Register.IsDynamic())
	{
		if (Register.IsNestedDynamic())
		{
			FRigVMNestedByteArray& Storage = *(FRigVMNestedByteArray*)TopDataPtr;
			NumSlices = Storage.Num();
			TopDataPtr = (uint8*)Storage.GetData();
		}
		else
		{
			FRigVMByteArray& Storage = *(FRigVMByteArray*)TopDataPtr;
			ElementCount = Storage.Num() / Register.ElementSize;
			ensure(ElementCount * Register.ElementSize == Storage.Num());
			TopDataPtr = Storage.GetData();
		}
	}

	for (int32 SliceIndex = 0; SliceIndex < NumSlices; SliceIndex++)
	{
		if (InSliceIndex != INDEX_NONE) 
		{
			SliceIndex = InSliceIndex;

			if (Register.IsNestedDynamic())
			{
				TopDataPtr = TopDataPtr + SliceIndex * sizeof(FRigVMByteArray);
			}
		}

		uint8* NestedDataPtr = TopDataPtr;
		if (Register.IsNestedDynamic())
		{
			FRigVMByteArray& Storage = *(FRigVMByteArray*)TopDataPtr;
			ElementCount = Storage.Num() / Register.ElementSize;
			ensure(ElementCount * Register.ElementSize == Storage.Num());
			NestedDataPtr = Storage.GetData();
		}

		switch (Register.Type)
		{
			case ERigVMRegisterType::Struct:
			{
				uint8* DataPtr = NestedDataPtr + ElementIndex * Register.ElementSize;

				UScriptStruct* ScriptStruct = GetScriptStruct(InRegisterIndex);
				if (ScriptStruct)
				{
					if (Register.ElementSize != ScriptStruct->GetStructureSize())
					{
						FMemory::Memzero(DataPtr, Register.ElementSize * ElementCount);
#if DEBUG_RIGVMMEMORY
						UE_LOG_RIGVMMEMORY(TEXT("%d.%04d: Zeroed struct, %04d bytes at %04d (%s)."), (int32)GetMemoryType(), InRegisterIndex, Register.ElementSize * ElementCount, (int32)reinterpret_cast<long long>(DataPtr), *Register.Name.ToString());
#endif
					}
					else
					{
						ScriptStruct->DestroyStruct(DataPtr, ElementCount);
#if DEBUG_RIGVMMEMORY
						UE_LOG_RIGVMMEMORY(TEXT("%d.%04d: Destroyed struct, %04d bytes at %04d (%s)."), (int32)GetMemoryType(), InRegisterIndex, ScriptStruct->GetStructureSize() * ElementCount, (int32)reinterpret_cast<long long>(DataPtr), *Register.Name.ToString());
#endif
					}
				}
				else
				{
					FMemory::Memzero(DataPtr, Register.ElementSize * ElementCount);
#if DEBUG_RIGVMMEMORY
					UE_LOG_RIGVMMEMORY(TEXT("%d.%04d: Zeroed struct, %04d bytes at %04d (%s)."), (int32)GetMemoryType(), InRegisterIndex, Register.ElementSize * ElementCount, (int32)reinterpret_cast<long long>(DataPtr), *Register.Name.ToString());
#endif
				}
				break;
			}
			case ERigVMRegisterType::String:
			{
				FString* DataPtr = (FString*)(NestedDataPtr + ElementIndex * Register.ElementSize);
				RigVMDestroy<FString>(DataPtr, ElementCount);
#if DEBUG_RIGVMMEMORY
				UE_LOG_RIGVMMEMORY(TEXT("%d.%04d: Destroyed string, %04d bytes at %04d (%s)."), (int32)GetMemoryType(), InRegisterIndex, ElementCount * Register.ElementSize, (int32)reinterpret_cast<long long>(DataPtr), *Register.Name.ToString());
#endif
				break;
			}
			case ERigVMRegisterType::Name:
			{
				FName* DataPtr = (FName*)(NestedDataPtr + ElementIndex * Register.ElementSize);
				RigVMDestroy<FName>(DataPtr, ElementCount);
#if DEBUG_RIGVMMEMORY
				UE_LOG_RIGVMMEMORY(TEXT("%d.%04d: Destroyed name, %04d bytes at %04d (%s)."), (int32)GetMemoryType(), InRegisterIndex, ElementCount * Register.ElementSize, (int32)reinterpret_cast<long long>(DataPtr), *Register.Name.ToString());
#endif
				break;
			}
			default:
			{
				if (!Register.IsDynamic())
				{
					return false;
				}
			}
		}

		if (Register.IsNestedDynamic())
		{
			((FRigVMByteArray*)TopDataPtr)->Empty();
			TopDataPtr = TopDataPtr + sizeof(FRigVMByteArray);
		}

		if (InSliceIndex != INDEX_NONE)
		{
			break;
		}
	}

	if (Register.IsDynamic())
	{
		TopDataPtr = &Data[Register.GetWorkByteIndex()];

		if (Register.IsNestedDynamic())
		{
			if (InSliceIndex != INDEX_NONE)
			{
				((FRigVMNestedByteArray*)TopDataPtr)->Empty();
			}
		}
		else
		{
			((FRigVMByteArray*)TopDataPtr)->Empty();
		}
	}

	return true;
}

int32 FRigVMMemoryContainer::GetOrAddRegisterOffset(int32 InRegisterIndex, int32 InArrayElement)
{
	return GetOrAddRegisterOffset(InRegisterIndex, FString(), InArrayElement);
}

int32 FRigVMMemoryContainer::GetOrAddRegisterOffset(int32 InRegisterIndex, const FString& InSegmentPath, int32 InArrayElement)
{
	ensure(Registers.IsValidIndex(InRegisterIndex));

	UScriptStruct* ScriptStruct = GetScriptStruct(InRegisterIndex);
	int32 ElementSize = (int32)Registers[InRegisterIndex].ElementSize;
	int32 InitialOffset = InArrayElement * ElementSize;
	return GetOrAddRegisterOffset(InRegisterIndex, ScriptStruct, InSegmentPath, InitialOffset);
}

int32 FRigVMMemoryContainer::GetOrAddRegisterOffset(int32 InRegisterIndex, UScriptStruct* InScriptStruct, const FString& InSegmentPath, int32 InInitialOffset, int32 InElementSize)
{
	if ((InScriptStruct == nullptr || InSegmentPath.IsEmpty()) && InInitialOffset == 0)
	{
		return INDEX_NONE;
	}

	// if this is a register offset for a external variable
	// the register index is expected to be INDEX_NONE
	// and it is also expected that InElementSize != 0
	if (InElementSize == 0)
	{
		ensure(Registers.IsValidIndex(InRegisterIndex)); 
		InElementSize = (int32)Registers[InRegisterIndex].ElementSize;
	}
	
	FName BaseCPPType = NAME_None;
	if(Registers.IsValidIndex(InRegisterIndex))
	{
		if(InElementSize == 0)
		{
			InElementSize = (int32)Registers[InRegisterIndex].ElementSize;
		}
#if WITH_EDITORONLY_DATA
		BaseCPPType = Registers[InRegisterIndex].BaseCPPType;
#endif
	}

	FRigVMRegisterOffset Offset(InScriptStruct, InSegmentPath, InInitialOffset, InElementSize, BaseCPPType);
	int32 ExistingIndex = RegisterOffsets.Find(Offset);
	if (ExistingIndex == INDEX_NONE)
	{
		return RegisterOffsets.Add(Offset);
	}
	return ExistingIndex;
}

#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED

void FRigVMMemoryContainer::SetRegisterValueFromString(const FRigVMOperand& InOperand, const FString& InCPPType, const UObject* InCPPTypeObject, const TArray<FString>& InDefaultValues)
{
	if (InOperand.GetRegisterIndex() < 0 || InOperand.GetRegisterIndex() >= Registers.Num())
	{
		return;
	}

	FRigVMRegister& Register = Registers[InOperand.GetRegisterIndex()];

	if (Register.ElementCount != InDefaultValues.Num())
	{
		return;
	}

	FString CPPType = InCPPType;
	if (RigVMTypeUtils::IsArrayType(CPPType))
	{
		CPPType = CPPType.Mid(7, CPPType.Len() - 8);
	}

	for (int32 Index = 0; Index < InDefaultValues.Num(); Index++)
	{
		FString DefaultValue = InDefaultValues[Index];

		if (const UScriptStruct* ScriptStruct = Cast<UScriptStruct>(InCPPTypeObject))
		{
			uint8* DataPtr = (uint8*)GetData(Register);
			DataPtr += Index * ScriptStruct->GetStructureSize();
			FRigVMMemoryContainerImportErrorContext ErrorPipe;
			((UScriptStruct*)ScriptStruct)->ImportText(*DefaultValue, DataPtr, nullptr, PPF_None, &ErrorPipe, ScriptStruct->GetName());
		}
		else if (const UEnum* Enum = Cast<const UEnum>(InCPPTypeObject))
		{
			if (FCString::IsNumeric(*DefaultValue))
			{
				GetFixedArray<uint8>(Register)[Index] = (uint8)FCString::Atoi(*DefaultValue);
			}
			else
			{
				GetFixedArray<uint8>(Register)[Index] = (uint8)Enum->GetValueByNameString(DefaultValue);
			}
		}
		else if (CPPType == TEXT("bool") && Register.Type == ERigVMRegisterType::Plain && Register.ElementSize == sizeof(bool))
		{
			GetFixedArray<bool>(Register)[Index] = (DefaultValue == TEXT("True")) || (DefaultValue == TEXT("true")) || (DefaultValue == TEXT("1"));
		}
		else if (CPPType == TEXT("int32") && Register.Type == ERigVMRegisterType::Plain && Register.ElementSize == sizeof(int32))
		{
			GetFixedArray<int32>(Register)[Index] = FCString::Atoi(*DefaultValue);
		}
		else if (CPPType == TEXT("float") && Register.Type == ERigVMRegisterType::Plain && Register.ElementSize == sizeof(float))
		{
			GetFixedArray<float>(Register)[Index] = FCString::Atof(*DefaultValue);
		}
		else if (CPPType == TEXT("double") && Register.Type == ERigVMRegisterType::Plain && Register.ElementSize == sizeof(double))
		{
			GetFixedArray<double>(Register)[Index] = FCString::Atod(*DefaultValue);
		}
		else if (CPPType == TEXT("FName") && Register.Type == ERigVMRegisterType::Name)
		{
			GetFixedArray<FName>(Register)[Index] = *DefaultValue;
		}
		else if (CPPType == TEXT("FString") && Register.Type == ERigVMRegisterType::String)
		{
			GetFixedArray<FString>(Register)[Index] = *DefaultValue;
		}
	}
}

TArray<FString> FRigVMMemoryContainer::GetRegisterValueAsString(const FRigVMOperand& InOperand, const FString& InCPPType, const UObject* InCPPTypeObject)
{
	TArray<FString> DefaultValues;

	if (InOperand.GetRegisterIndex() < 0 || InOperand.GetRegisterIndex() >= Registers.Num())
	{
		return DefaultValues;
	}

	FRigVMRegister& Register = Registers[InOperand.GetRegisterIndex()];

	int32 SliceCount = Register.SliceCount;
	
	if (Register.IsNestedDynamic())
	{
		FRigVMNestedByteArray* Storage = (FRigVMNestedByteArray*)&Data[Register.GetWorkByteIndex()];
		SliceCount = Storage->Num();
	}
	else if (Register.IsDynamic())
	{
		FRigVMByteArray* Storage = (FRigVMByteArray*)&Data[Register.GetWorkByteIndex()];
		SliceCount = Storage->Num() / Register.ElementSize;
	}

	for (int32 SliceIndex = 0; SliceIndex < SliceCount; SliceIndex++)
	{
		int32 ElementCount = Register.ElementCount;
		if (Register.IsNestedDynamic())
		{
			FRigVMNestedByteArray* Storage = (FRigVMNestedByteArray*)&Data[Register.GetWorkByteIndex()];
			ElementCount = (*Storage)[SliceIndex].Num() / Register.ElementSize;
		}

		for (int32 ElementIndex = 0; ElementIndex < ElementCount; ElementIndex++)
		{
			FString DefaultValue;

			if (Register.ScriptStructIndex != INDEX_NONE)
			{
				UScriptStruct* ScriptStruct = GetScriptStruct(Register);
				if (ScriptStruct == InCPPTypeObject)
				{
					uint8* DataPtr = (uint8*)GetData(Register, INDEX_NONE, SliceIndex);
					DataPtr += ElementIndex * ScriptStruct->GetStructureSize(); 

					//use DataPtr for both value and default value to force exporting complete string
					ScriptStruct->ExportText(DefaultValue, DataPtr, DataPtr, nullptr, PPF_None, nullptr);
				}
			}
			else if (const UEnum* Enum = Cast<const UEnum>(InCPPTypeObject))
			{
				DefaultValue = Enum->GetNameStringByValue((int64)GetFixedArray<uint8>(Register, INDEX_NONE, SliceIndex)[ElementIndex]);
			}
			else if (InCPPType == TEXT("bool") && Register.GetNumBytesPerSlice() == sizeof(bool))
			{
				DefaultValue = GetFixedArray<bool>(Register, INDEX_NONE, SliceIndex)[ElementIndex] ? TEXT("True") : TEXT("False");
			}
			else if (InCPPType == TEXT("int32") && Register.GetNumBytesPerSlice() == sizeof(int32))
			{
				DefaultValue = FString::FromInt(GetFixedArray<int32>(Register, INDEX_NONE, SliceIndex)[ElementIndex]);
			}
			else if (InCPPType == TEXT("float") && Register.GetNumBytesPerSlice() == sizeof(float))
			{
				float Value = GetFixedArray<float>(Register, INDEX_NONE, SliceIndex)[ElementIndex];
				DefaultValue = FString::Printf(TEXT("%f"), Value);
			}
			else if (InCPPType == TEXT("double") && Register.GetNumBytesPerSlice() == sizeof(double))
			{
				double Value = GetFixedArray<double>(Register, INDEX_NONE, SliceIndex)[ElementIndex];
				DefaultValue = FString::Printf(TEXT("%f"), (float)Value);
			}
			else if (InCPPType == TEXT("FName") && Register.GetNumBytesPerSlice() == sizeof(FName))
			{
				DefaultValue = GetFixedArray<FName>(Register, INDEX_NONE, SliceIndex)[ElementIndex].ToString();
			}
			else if (InCPPType == TEXT("FString") && Register.GetNumBytesPerSlice() == sizeof(FString))
			{
				DefaultValue = GetFixedArray<FString>(Register, INDEX_NONE, SliceIndex)[ElementIndex];
			}
			else
			{
				continue;
			}

			DefaultValues.Add(DefaultValue);
		}
	}

	return DefaultValues;
}

const FRigVMRegisterOffset& FRigVMMemoryContainer::GetRegisterOffset(int32 InRegisterOffsetIndex) const
{
	if (RegisterOffsets.IsValidIndex(InRegisterOffsetIndex))
	{
		return RegisterOffsets[InRegisterOffsetIndex];
	}

	return InvalidRegisterOffset;
}

const FRigVMRegisterOffset& FRigVMMemoryContainer::GetRegisterOffsetForOperand(const FRigVMOperand& InOperand) const
{
	ensure(InOperand.GetMemoryType() == MemoryType || 
		(InOperand.GetMemoryType() == ERigVMMemoryType::External && MemoryType == ERigVMMemoryType::Work));
	
	return GetRegisterOffset(InOperand.GetRegisterOffset());
}

#endif

void FRigVMMemoryContainer::UpdateRegisters()
{
	int32 AlignmentShift = 0;
	for (int32 RegisterIndex = 0; RegisterIndex < Registers.Num(); RegisterIndex++)
	{
		FRigVMRegister& Register = Registers[RegisterIndex];
		Register.ByteIndex += AlignmentShift;

		int32 Alignment = 4;

		if (Register.IsDynamic() ||
			Register.Type == ERigVMRegisterType::Name ||
			Register.Type == ERigVMRegisterType::String)
		{
			Alignment = 8;
		}
		else if (UScriptStruct* ScriptStruct = GetScriptStruct(RegisterIndex))
		{
			if (UScriptStruct::ICppStructOps* TheCppStructOps = ScriptStruct->GetCppStructOps())
			{
				Alignment = TheCppStructOps->GetAlignment();
			}
		}

		if (Alignment != 0)
		{
			// no need to adjust for alignment if nothing is allocated
			if (!Register.IsDynamic() && Register.ElementCount == 0)
			{
				continue;
			}

			ensure(Data.IsValidIndex(Register.GetWorkByteIndex()));


			if (ensure(Data.IsValidIndex(Register.GetWorkByteIndex())))
			{
				uint8* Pointer = (uint8*)(&(Data[Register.GetWorkByteIndex()]));
			
				if (Register.AlignmentBytes > 0)
				{
					if (!IsAligned(Pointer, Alignment))
					{
						Data.RemoveAt(Register.GetFirstAllocatedByte(), Register.AlignmentBytes);
						AlignmentShift -= Register.AlignmentBytes;
						Register.ByteIndex -= Register.AlignmentBytes;
						Register.AlignmentBytes = 0;
						Pointer = (uint8*)(&(Data[Register.GetWorkByteIndex()]));
					}
				}

				while (!IsAligned(Pointer, Alignment))
				{
					Data.InsertZeroed(Register.GetFirstAllocatedByte(), 1);
					Register.AlignmentBytes++;
					Register.ByteIndex++;
					AlignmentShift++;
					Pointer = (uint8*)(&(Data[Register.GetWorkByteIndex()]));
				}
			}
		}
	}

	if (bUseNameMap)
	{
		NameMap.Reset();
		for (int32 Index = 0; Index < Registers.Num(); Index++)
		{
			NameMap.Add(Registers[Index].Name, Index);
		}
	}
}

#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED

void FRigVMMemoryContainer::FillWithZeroes(int32 InRegisterIndex)
{
	ensure(Registers.IsValidIndex(InRegisterIndex));
	FMemory::Memzero(GetData(InRegisterIndex), Registers[InRegisterIndex].GetNumBytesAllSlices());
}

#endif

int32 FRigVMMemoryContainer::FindOrAddScriptStruct(UScriptStruct* InScriptStruct)
{
	if (InScriptStruct == nullptr)
	{
		return INDEX_NONE;
	}

	int32 StructIndex = INDEX_NONE;
	if (ScriptStructs.Find(InScriptStruct, StructIndex))
	{
		return StructIndex;
	}
	return ScriptStructs.Add(InScriptStruct);
}
