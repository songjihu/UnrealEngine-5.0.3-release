// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/IndirectArray.h"
#include "UObject/ObjectMacros.h"
#include "UObject/Object.h"
#include "Misc/Guid.h"
#include "Templates/SubclassOf.h"
#include "Interfaces/Interface_AssetUserData.h"
#include "Interfaces/Interface_AsyncCompilation.h"
#include "RenderCommandFence.h"
#include "RenderResource.h"
#include "Serialization/EditorBulkData.h"
#include "Engine/TextureDefines.h"
#include "MaterialShared.h"
#include "TextureResource.h"
#include "Engine/StreamableRenderAsset.h"
#include "PerPlatformProperties.h"
#include "Misc/FieldAccessor.h"
#if WITH_EDITORONLY_DATA
#include "Misc/TVariant.h"
#include "DerivedDataCacheKeyProxy.h"
#endif

#ifndef WITH_TEXTURE_RESOURCE_DEPRECATIONS
#define WITH_TEXTURE_RESOURCE_DEPRECATIONS 1
#endif

#ifndef WITH_TEXTURE_PLATFORMDATA_DEPRECATIONS
#define WITH_TEXTURE_PLATFORMDATA_DEPRECATIONS 1
#endif

#if WITH_EDITOR
#include "Templates/DontCopy.h"
#endif

#include "Texture.generated.h"

namespace FOodleDataCompression {enum class ECompressor : uint8; enum class ECompressionLevel : int8; }


class ITargetPlatform;
class UAssetUserData;
struct FPropertyChangedEvent;

#if WITH_EDITORONLY_DATA
namespace UE::DerivedData { struct FValueId; }
#endif

UENUM()
enum TextureFilter
{
	TF_Nearest UMETA(DisplayName="Nearest"),
	TF_Bilinear UMETA(DisplayName="Bi-linear"),
	TF_Trilinear UMETA(DisplayName="Tri-linear"),
	/** Use setting from the Texture Group. */
	TF_Default UMETA(DisplayName="Default (from Texture Group)"),
	TF_MAX,
};

UENUM()
enum TextureAddress
{
	TA_Wrap UMETA(DisplayName="Wrap"),
	TA_Clamp UMETA(DisplayName="Clamp"),
	TA_Mirror UMETA(DisplayName="Mirror"),
	TA_MAX,
};

UENUM()
enum ETextureMipCount
{
	TMC_ResidentMips,
	TMC_AllMips,
	TMC_AllMipsBiased,
	TMC_MAX,
};

UENUM()
enum ETextureSourceArtType
{
	/** FColor Data[SrcWidth * SrcHeight]. */
	TSAT_Uncompressed,
	/** PNG compresed version of FColor Data[SrcWidth * SrcHeight]. */
	TSAT_PNGCompressed,
	/** DDS file with header. */
	TSAT_DDSFile,
	TSAT_MAX,
};

UENUM()
enum ETextureCompressionQuality
{
	TCQ_Default = 0		UMETA(DisplayName="Default"),
	TCQ_Lowest = 1		UMETA(DisplayName="Lowest"),
	TCQ_Low = 2			UMETA(DisplayName="Low"),
	TCQ_Medium = 3		UMETA(DisplayName="Medium"),
	TCQ_High= 4			UMETA(DisplayName="High"),
	TCQ_Highest = 5		UMETA(DisplayName="Highest"),
	TCQ_MAX,
};

USTRUCT()
struct FTextureSourceBlock
{
	GENERATED_USTRUCT_BODY()

	ENGINE_API FTextureSourceBlock();

	UPROPERTY(VisibleAnywhere, Category = TextureSource)
	int32 BlockX;

	UPROPERTY(VisibleAnywhere, Category = TextureSource)
	int32 BlockY;

	UPROPERTY(VisibleAnywhere, Category = TextureSource)
	int32 SizeX;

	UPROPERTY(VisibleAnywhere, Category = TextureSource)
	int32 SizeY;

	UPROPERTY(VisibleAnywhere, Category = TextureSource)
	int32 NumSlices;

	UPROPERTY(VisibleAnywhere, Category = TextureSource)
	int32 NumMips;
};

/**
 * Texture source data management.
 */
USTRUCT()
struct FTextureSource
{
	GENERATED_USTRUCT_BODY()

	/** Default constructor. */
	ENGINE_API FTextureSource();

	ENGINE_API static int32 GetBytesPerPixel(ETextureSourceFormat Format);
	FORCEINLINE static bool IsHDR(ETextureSourceFormat Format) { return (Format == TSF_BGRE8 || Format == TSF_RGBA16F); }

#if WITH_EDITOR
	// FwdDecl for member structs
	struct FMipData;

	ENGINE_API void InitBlocked(const ETextureSourceFormat* InLayerFormats,
		const FTextureSourceBlock* InBlocks,
		int32 InNumLayers,
		int32 InNumBlocks,
		const uint8** InDataPerBlock);

	ENGINE_API void InitBlocked(const ETextureSourceFormat* InLayerFormats,
		const FTextureSourceBlock* InBlocks,
		int32 InNumLayers,
		int32 InNumBlocks,
		UE::Serialization::FEditorBulkData::FSharedBufferWithID NewData);

	ENGINE_API void InitLayered(
		int32 NewSizeX,
		int32 NewSizeY,
		int32 NewNumSlices,
		int32 NewNumLayers,
		int32 NewNumMips,
		const ETextureSourceFormat* NewLayerFormat,
		const uint8* NewData = NULL
		);

	ENGINE_API void InitLayered(
		int32 NewSizeX,
		int32 NewSizeY,
		int32 NewNumSlices,
		int32 NewNumLayers,
		int32 NewNumMips,
		const ETextureSourceFormat* NewLayerFormat,
		UE::Serialization::FEditorBulkData::FSharedBufferWithID NewData
	);

	/**
	 * Initialize the source data with the given size, number of mips, and format.
	 * @param NewSizeX - Width of the texture source data.
	 * @param NewSizeY - Height of the texture source data.
	 * @param NewNumSlices - The number of slices in the texture source data.
	 * @param NewNumMips - The number of mips in the texture source data.
	 * @param NewFormat - The format in which source data is stored.
	 * @param NewData - [optional] The new source data.
	 */
	ENGINE_API void Init(
		int32 NewSizeX,
		int32 NewSizeY,
		int32 NewNumSlices,
		int32 NewNumMips,
		ETextureSourceFormat NewFormat,
		const uint8* NewData = NULL
		);

	/**
	 * Initialize the source data with the given size, number of mips, and format.
	 * @param NewSizeX - Width of the texture source data.
	 * @param NewSizeY - Height of the texture source data.
	 * @param NewNumSlices - The number of slices in the texture source data.
	 * @param NewNumMips - The number of mips in the texture source data.
	 * @param NewFormat - The format in which source data is stored.
	 * @param NewData - The new source data.
	 */
	ENGINE_API void Init(
		int32 NewSizeX,
		int32 NewSizeY,
		int32 NewNumSlices,
		int32 NewNumMips,
		ETextureSourceFormat NewFormat,
		UE::Serialization::FEditorBulkData::FSharedBufferWithID NewData
	);

	/**
	 * Initializes the source data for a 2D texture with a full mip chain.
	 * @param NewSizeX - Width of the texture source data.
	 * @param NewSizeY - Height of the texture source data.
	 * @param NewFormat - Format of the texture source data.
	 */
	ENGINE_API void Init2DWithMipChain(
		int32 NewSizeX,
		int32 NewSizeY,
		ETextureSourceFormat NewFormat
		);

	ENGINE_API void InitLayered2DWithMipChain(
		int32 NewSizeX,
		int32 NewSizeY,
		int32 NewNumLayers,
		const ETextureSourceFormat* NewFormat
	);

	/**
	 * Initializes the source data for a cubemap with a full mip chain.
	 * @param NewSizeX - Width of each cube map face.
	 * @param NewSizeY - Height of each cube map face.
	 * @param NewFormat - Format of the cube map source data.
	 */
	ENGINE_API void InitCubeWithMipChain(
		int32 NewSizeX,
		int32 NewSizeY,
		ETextureSourceFormat NewFormat
		);

	/**
	 * Initialize the source data with the given size, number of mips, and format.
	 * @param NewSizeX - Width of the texture source data.
	 * @param NewSizeY - Height of the texture source data.
	 * @param NewNumMips - The number of mips in the texture source data.
	 * @param NewFormat - The format in which source data is stored.
	 * @param NewSourceData -The new source data.
	 * @param NewSourceFormat -The compression format of the new source data.
	 */
	ENGINE_API void InitWithCompressedSourceData(
		int32 NewSizeX,
		int32 NewSizeY,
		int32 NewNumMips,
		ETextureSourceFormat NewFormat,
		const TArrayView64<uint8> NewSourceData,
		ETextureSourceCompressionFormat NewSourceFormat
	);

	/** Make a copy with a torn-off BulkData that has the same Guid used for DDC as this->BulkData */
	FTextureSource CopyTornOff() const;

	/** PNG Compresses the source art if possible or tells the bulk data to zlib compress when it saves out to disk. */
	ENGINE_API void Compress();

	/** Force the GUID to change even if mip data has not been modified. */
	ENGINE_API void ForceGenerateGuid();

	/** Lock a mip for reading. */
	ENGINE_API const uint8* LockMipReadOnly(int32 BlockIndex, int32 LayerIndex, int32 MipIndex);

	/** Lock a mip for editing. */
	ENGINE_API uint8* LockMip(int32 BlockIndex, int32 LayerIndex, int32 MipIndex);

	/** Unlock a mip. */
	ENGINE_API void UnlockMip(int32 BlockIndex, int32 LayerIndex, int32 MipIndex);

	/** Retrieve a copy of the data for a particular mip. */
	ENGINE_API bool GetMipData(TArray64<uint8>& OutMipData, int32 BlockIndex, int32 LayerIndex, int32 MipIndex, class IImageWrapperModule* ImageWrapperModule = nullptr);

	/** Returns a FMipData structure that wraps around the entire mip chain for read only operations. This is more efficient than calling the above method once per mip. */
	ENGINE_API FMipData GetMipData(class IImageWrapperModule* ImageWrapperModule);

	/** Computes the size of a single mip. */
	ENGINE_API int64 CalcMipSize(int32 BlockIndex, int32 LayerIndex, int32 MipIndex) const;

	/** Computes the number of bytes per-pixel. */
	ENGINE_API int32 GetBytesPerPixel(int32 LayerIndex = 0) const;

	/** Return true if the source data is power-of-2. */
	ENGINE_API bool IsPowerOfTwo(int32 BlockIndex = 0) const;

	/** Returns true if source art is available. */
	ENGINE_API bool IsValid() const;

	/** Access the given block */
	ENGINE_API void GetBlock(int32 Index, FTextureSourceBlock& OutBlock) const;

	/** Logical size of the texture includes all blocks */
	ENGINE_API FIntPoint GetLogicalSize() const;

	/** Size of texture in blocks */
	ENGINE_API FIntPoint GetSizeInBlocks() const;

	/** Returns the unique ID string for this source art. */
	FString GetIdString() const;

	/** Returns the compression format of the source data in string format for use with the UI. */
	FString GetSourceCompressionAsString() const;

	/** Returns the compression format of the source data in enum format. */
	ETextureSourceCompressionFormat GetSourceCompression() const;

	/** Support for copy/paste */
	void ExportCustomProperties(FOutputDevice& Out, uint32 Indent);
	void ImportCustomProperties(const TCHAR* SourceText, FFeedbackContext* Warn);

	/** Trivial accessors. These will only give values for Block0 so may not be correct for UDIM/multi-block textures, use GetBlock() for this case. */
	FGuid GetPersistentId() const { return BulkData.GetIdentifier(); }
	ENGINE_API FGuid GetId() const;
	FORCEINLINE int32 GetSizeX() const { return SizeX; }
	FORCEINLINE int32 GetSizeY() const { return SizeY; }
	FORCEINLINE int32 GetNumSlices() const { return NumSlices; }
	FORCEINLINE int32 GetNumMips() const { return NumMips; }
	FORCEINLINE int32 GetNumLayers() const { return NumLayers; }
	FORCEINLINE int32 GetNumBlocks() const { return Blocks.Num() + 1; }
	FORCEINLINE ETextureSourceFormat GetFormat(int32 LayerIndex = 0) const { return (LayerIndex == 0) ? Format : LayerFormat[LayerIndex]; }
	FORCEINLINE bool IsPNGCompressed() const { return bPNGCompressed; }
	FORCEINLINE bool IsLongLatCubemap() const { return bLongLatCubemap; }
	FORCEINLINE int64 GetSizeOnDisk() const { return BulkData.GetPayloadSize(); }
	inline bool HasPayloadData() const { return BulkData.HasPayloadData(); }
	/** Returns true if the texture's bulkdata payload is either already in memory or if the payload is 0 bytes in length. It will return false if the payload needs to load from disk */
	FORCEINLINE bool IsBulkDataLoaded() const { return BulkData.DoesPayloadNeedLoading(); }

	ENGINE_API void OperateOnLoadedBulkData(TFunctionRef<void (const FSharedBuffer& BulkDataBuffer)> Operation);

	UE_DEPRECATED(5.00, "There is no longer a need to call LoadBulkDataWithFileReader, FTextureSource::BulkData can now load the data on demand without it.")
	FORCEINLINE bool LoadBulkDataWithFileReader() { return true; }

	FORCEINLINE void RemoveBulkData() { BulkData.UnloadData(); }
	
	/** Sets the GUID to use, and whether that GUID is actually a hash of some data. */
	ENGINE_API void SetId(const FGuid& InId, bool bInGuidIsHash);

	/** Legacy API that defaults to LayerIndex 0 */
	FORCEINLINE bool GetMipData(TArray64<uint8>& OutMipData, int32 MipIndex, class IImageWrapperModule* ImageWrapperModule = nullptr)
	{
		return GetMipData(OutMipData, 0, 0, MipIndex, ImageWrapperModule);
	}

	FORCEINLINE int64 CalcMipSize(int32 MipIndex) const { return CalcMipSize(0, 0, MipIndex); }
	/** Lock a mip for reading. */
	FORCEINLINE const uint8* LockMipReadOnly(int32 MipIndex) { return LockMipReadOnly(0, 0, MipIndex); }
	/** Lock a mip for editing. */
	FORCEINLINE uint8* LockMip(int32 MipIndex) { return LockMip(0, 0, MipIndex); }
	FORCEINLINE void UnlockMip(int32 MipIndex) { UnlockMip(0, 0, MipIndex); }

	struct FMipAllocation
	{
		/** Create an empty object */
		FMipAllocation() = default;
		/** Take a read only FSharedBuffer, will allocate a new buffer and copy from this if Read/Write access is requested */
		FMipAllocation(FSharedBuffer SrcData);

		// Do not actually do anything for copy constructor or assignments, this is required for as long as
		// we need to support the old bulkdata code path (although previously storing these allocations as
		// raw pointers would allow it to be assigned, this would most likely cause a mismatch in lock counts,
		// either in FTextureSource or the underlying bulkdata and was never actually safe)
		FMipAllocation(const FMipAllocation&) {}
		FMipAllocation& operator =(const FMipAllocation&) { return *this; }

		// We do allow rvalue assignment
		FMipAllocation(FMipAllocation&&);
		FMipAllocation& operator =(FMipAllocation&&);

		~FMipAllocation() = default;

		/** Release all currently owned data and return the object to the default state */
		void Reset();

		/** Returns true if the object contains no data */
		bool IsNull() const { return ReadOnlyReference.IsNull(); }

		/** Returns the overall size of the data in bytes */
		int64 GetSize() const { return ReadOnlyReference.GetSize(); }

		/** Returns a FSharedBuffer that contains the current texture data but cannot be directly modified */
		const FSharedBuffer& GetDataReadOnly() const { return ReadOnlyReference; }

		/** Returns a pointer that contains the current texture data and can be written to */
		uint8* GetDataReadWrite();

		/** Returns the internal FSharedBuffer and relinquish ownership, used to transfer the data to virtualized bulkdata */
		FSharedBuffer Release();

	private:
		void CreateReadWriteBuffer(const void* SrcData, int64 DataLength);

		struct FDeleterFree
		{
			void operator()(uint8* Ptr) const
			{
				if (Ptr)
				{
					FMemory::Free(Ptr);
				}
			}
		};

		FSharedBuffer					ReadOnlyReference;
		TUniquePtr<uint8, FDeleterFree>	ReadWriteBuffer;
	};

	/** Structure that encapsulates the decompressed texture data and can be accessed per mip */
	struct ENGINE_API FMipData
	{
		/** Allow the copy constructor by rvalue*/
		FMipData(FMipData&& Other)
			: TextureSource(Other.TextureSource)
		{
			MipData = MoveTemp(Other.MipData);
		}

		~FMipData() = default;

		/** Disallow everything else so we don't get duplicates */
		FMipData() = delete;
		FMipData(const FMipData&) = delete;
		FMipData& operator=(const FMipData&) = delete;
		FMipData& operator=(FMipData&& Other) = delete;

		/** Get a copy of a given texture mip, to be stored in OutMipData */
		bool GetMipData(TArray64<uint8>& OutMipData, int32 BlockIndex, int32 LayerIndex, int32 MipIndex) const;

	private:
		// We only want to allow FTextureSource to create FMipData objects
		friend struct FTextureSource;

		FMipData(const FTextureSource& InSource, FSharedBuffer InData);

		const FTextureSource& TextureSource;
		FSharedBuffer MipData;
	};
#endif // WITH_EDITOR

private:
	/** Allow UTexture access to internals. */
	friend class UTexture;
	friend class UTexture2D;
	friend class UTextureCube;
	friend class UVolumeTexture;
	friend class UTexture2DArray;
	friend class UTextureCubeArray;

#if WITH_EDITOR
	/** Protects simultaneous access to BulkData */
	TDontCopy<FRWLock> BulkDataLock;
#endif
	/** The bulk source data. */
	UE::Serialization::FEditorBulkData BulkData;
	
	/** Number of mips that are locked. */
	uint32 NumLockedMips;

	enum class ELockState : uint8
	{
		None,
		ReadOnly,
		ReadWrite
	};
	/** The state of any lock being held on the mip data */
	ELockState LockState;

#if WITH_EDITOR
	/** Pointer to locked mip data, if any. */
	FMipAllocation LockedMipData;

	// Internal implementation for locking the mip data, called by LockMipReadOnly or LockMip */
	uint8* LockMipInternal(int32 BlockIndex, int32 LayerIndex, int32 MipIndex, ELockState RequestedLockState);
	
	/** Returns the source data fully decompressed */
	FSharedBuffer Decompress(class IImageWrapperModule* ImageWrapperModule) const;
	/** Attempt to decompress the source data from a compressed png format. All failures will be logged and result in the method returning false */
	FSharedBuffer TryDecompressPngData(IImageWrapperModule* ImageWrapperModule) const;
	/** Attempt to decompress the source data from Jpeg format. All failures will be logged and result in the method returning false */
	FSharedBuffer TryDecompressJpegData(IImageWrapperModule* ImageWrapperModule) const;

	/** Return true if the source art is not png compressed but could be. */
	bool CanPNGCompress() const;
	/** Removes source data. */
	void RemoveSourceData();
	/** Retrieve the size and offset for a source mip. The size includes all slices. */
	int64 CalcMipOffset(int32 BlockIndex, int32 LayerIndex, int32 MipIndex) const;

	int64 CalcBlockSize(int32 BlockIndex) const;
	int64 CalcLayerSize(int32 BlockIndex, int32 LayerIndex) const;
	int64 CalcBlockSize(const FTextureSourceBlock& Block) const;
	int64 CalcLayerSize(const FTextureSourceBlock& Block, int32 LayerIndex) const;

	void InitLayeredImpl(
		int32 NewSizeX,
		int32 NewSizeY,
		int32 NewNumSlices,
		int32 NewNumLayers,
		int32 NewNumMips,
		const ETextureSourceFormat* NewLayerFormat);

	void InitBlockedImpl(const ETextureSourceFormat* InLayerFormats,
		const FTextureSourceBlock* InBlocks,
		int32 InNumLayers,
		int32 InNumBlocks);

	bool EnsureBlocksAreSorted();

public:
	/** Uses a hash as the GUID, useful to prevent creating new GUIDs on load for legacy assets. */
	ENGINE_API void UseHashAsGuid();

	void ReleaseSourceMemory(); // release the memory from the mips (does almost the same as remove source data except doesn't rebuild the guid)
	FORCEINLINE bool HasHadBulkDataCleared() const { return bHasHadBulkDataCleared; }
private:
	/** Used while cooking to clear out unneeded memory after compression */
	bool bHasHadBulkDataCleared;
#endif

#if WITH_EDITORONLY_DATA
	/** GUID used to track changes to the source data. */
	UPROPERTY(VisibleAnywhere, Category=TextureSource)
	FGuid Id;

	/** Position of texture block0, only relevant if source has multiple blocks */
	UPROPERTY(VisibleAnywhere, Category = TextureSource)
	int32 BaseBlockX;

	UPROPERTY(VisibleAnywhere, Category = TextureSource)
	int32 BaseBlockY;

	/** Width of the texture. */
	UPROPERTY(VisibleAnywhere, Category=TextureSource)
	int32 SizeX;

	/** Height of the texture. */
	UPROPERTY(VisibleAnywhere, Category=TextureSource)
	int32 SizeY;

	/** Depth (volume textures) or faces (cube maps). */
	UPROPERTY(VisibleAnywhere, Category=TextureSource)
	int32 NumSlices;

	/** Number of mips provided as source data for the texture. */
	UPROPERTY(VisibleAnywhere, Category=TextureSource)
	int32 NumMips;

	/** Number of layers (for multi-layered virtual textures) provided as source data for the texture. */
	UPROPERTY(VisibleAnywhere, Category = TextureSource)
	int32 NumLayers;

	/** RGBA8 source data is optionally compressed as PNG. */
	UPROPERTY(VisibleAnywhere, Category=TextureSource)
	bool bPNGCompressed;

	/**
	 * Source represents a cubemap in long/lat format, will have only 1 slice per cube, rather than 6 slices.
	 * Not needed for non-array cubemaps, since we can just look at NumSlices == 1 or 6
	 * But for cube arrays, no way of determining whether NumSlices=6 means 1 cubemap, or 6 long/lat cubemaps
	 */
	UPROPERTY(VisibleAnywhere, Category = TextureSource)
	bool bLongLatCubemap;

	/** Compression format that source data is stored as. */
	UPROPERTY(VisibleAnywhere, Category = TextureSource)
	TEnumAsByte<enum ETextureSourceCompressionFormat> CompressionFormat;

	/** Uses hash instead of guid to identify content to improve DDC cache hit. */
	UPROPERTY(VisibleAnywhere, Category=TextureSource)
	bool bGuidIsHash;

	/** Format in which the source data is stored. */
	UPROPERTY(VisibleAnywhere, Category=TextureSource)
	TEnumAsByte<enum ETextureSourceFormat> Format;

	/** For multi-layered sources, each layer may have a different format (in this case LayerFormat[0] == Format) . */
	UPROPERTY(VisibleAnywhere, Category = TextureSource)
	TArray< TEnumAsByte<enum ETextureSourceFormat> > LayerFormat;

	/**
	 * All sources have 1 implicit block defined by BaseBlockXY/SizeXY members.  Textures imported as UDIM may have additional blocks defined here.
	 * These are stored sequentially in the source's bulk data.
	 */
	UPROPERTY(VisibleAnywhere, Category = TextureSource)
	TArray<FTextureSourceBlock> Blocks;

	/**
	 * Offsets of each block (including Block0) in the bulk data.
	 * Blocks are not necessarily stored in order, since block indices are sorted by X/Y location.
	 * For non-UDIM textures, this will always have a single entry equal to 0
	 */
	UPROPERTY()
	TArray<int64> BlockDataOffsets;

#endif // WITH_EDITORONLY_DATA
};

/**
 * Optional extra fields for texture platform data required by some platforms.
 * Data in this struct is only serialized if the struct's value is non-default.
 */
struct FOptTexturePlatformData
{
	/** Arbitrary extra data that the runtime may need. */
	uint32 ExtData;
	/** Number of mips making up the mip tail, which must always be resident */
	uint32 NumMipsInTail;

	FOptTexturePlatformData()
		: ExtData(0)
		, NumMipsInTail(0)
	{}

	inline bool operator == (FOptTexturePlatformData const& RHS) const 
	{
		return ExtData == RHS.ExtData
			&& NumMipsInTail == RHS.NumMipsInTail;
	}

	inline bool operator != (FOptTexturePlatformData const& RHS) const
	{
		return !(*this == RHS);
	}

	friend inline FArchive& operator << (FArchive& Ar, FOptTexturePlatformData& Data)
	{
		Ar << Data.ExtData;
		Ar << Data.NumMipsInTail;
		return Ar;
	}
};

/**
 * Platform-specific data used by the texture resource at runtime.
 */
USTRUCT()
struct FTexturePlatformData
{
	GENERATED_USTRUCT_BODY()

	/** Width of the texture. */
	int32 SizeX;
	/** Height of the texture. */
	int32 SizeY;
	/** Packed bits [b31: CubeMap], [b30: HasOptData], [b29-0: NumSlices]. See bit masks below. */
	uint32 PackedData;
	/** Format in which mip data is stored. */
	EPixelFormat PixelFormat;
	/** Additional data required by some platforms.*/
	FOptTexturePlatformData OptData;
	/** Mip data or VT data. one or the other. */
	TIndirectArray<struct FTexture2DMipMap> Mips;
	struct FVirtualTextureBuiltData* VTData;

#if WITH_EDITORONLY_DATA
	/** The key associated with this derived data. */
	TVariant<FString, UE::DerivedData::FCacheKeyProxy> DerivedDataKey;

	// Stores information about how we generated this encoded texture.
	// Mostly relevant to Oodle, however notably does actually tell
	// you _which_ encoder was used.
	struct FTextureEncodeResultMetadata
	{
		// Returned from ITextureFormat
		FName Encoder;

		// This struct is not always filled out, allow us to check for invalid data.
		bool bIsValid = false;

		// If this is false, the remaining fields are invalid (as encode speed governs
		// the various Oodle specific values right now.)
		bool bSupportsEncodeSpeed = false;

		// If true, then the encoding settings were overridden in the texture editor
		// for encoding experimentation, and thus RDOSource and EncodeSpeed should 
		// be ignored.
		bool bWasEditorCustomEncoding = false;

		enum class OodleRDOSource : uint8
		{
			Default,	// We defaulted back to the project settings
			LODGroup,	// We used the LCA off the LOD group to generate a lambda
			Texture,	// We used the LCA off the texture to generate a lambda.
		};

		OodleRDOSource RDOSource = OodleRDOSource::Default;

		// The resulting RDO lambda, 0 means no RDO.
		uint8 OodleRDO = 0;

		// enum ETextureEncodeEffort
		uint8 OodleEncodeEffort = 0;

		// enum ETextureUniversalTiling
		uint8 OodleUniversalTiling = 0;

		// Which encode speed we ended up using. Must be either ETextureEncodeSpeed::Final or ETextureEncodeSpeed::Fast.
		uint8 EncodeSpeed = 0;
	};

	FTextureEncodeResultMetadata ResultMetadata;

	struct FStructuredDerivedDataKey
	{
		FIoHash BuildDefinitionKey;
		FGuid SourceGuid;
		FGuid CompositeSourceGuid;

		bool operator==(const FStructuredDerivedDataKey& Other) const
		{
			return BuildDefinitionKey == Other.BuildDefinitionKey && SourceGuid == Other.SourceGuid && CompositeSourceGuid == Other.CompositeSourceGuid;
		}

		bool operator!=(const FStructuredDerivedDataKey& Other) const
		{
			return BuildDefinitionKey != Other.BuildDefinitionKey || SourceGuid != Other.SourceGuid || CompositeSourceGuid != Other.CompositeSourceGuid;
		}
	};

	/** This is the key for the FetchOrBuild variant of our Cache. We assume that uniqueness
	*	for that is equivalent to uniqueness if we use both FetchFirst and FetchOrBuild. This
	*	is used as the key in to CookedPlatformData, as well as to determine if we are already
	*	cooking the data the editor needs in CachePlatformData. 
	*	Note that since this is read on the game thread constantly in CachePlatformData, it
	*	must be written to on the game thread to avoid false recaches.
	*/
	TVariant<FString, FStructuredDerivedDataKey> FetchOrBuildDerivedDataKey;

	/** Async cache task if one is outstanding. */
	struct FTextureAsyncCacheDerivedDataTask* AsyncTask;

#endif

	/** Default constructor. */
	ENGINE_API FTexturePlatformData();

	/** Destructor. */
	ENGINE_API ~FTexturePlatformData();

private:
	static constexpr uint32 BitMask_CubeMap    = 1u << 31u;
	static constexpr uint32 BitMask_HasOptData = 1u << 30u;
	static constexpr uint32 BitMask_NumSlices  = BitMask_HasOptData - 1u;

public:
	/** Return whether TryLoadMips() would stall because async loaded mips are not yet available. */
	bool IsReadyForAsyncPostLoad() const;

	/**
	 * Try to load mips from the derived data cache.
	 * @param FirstMipToLoad - The first mip index to load.
	 * @param OutMipData -	Must point to an array of pointers with at least
	 *						Texture.Mips.Num() - FirstMipToLoad + 1 entries. Upon
	 *						return those pointers will contain mip data.
	 * @param DebugContext - A string used for debug tracking and logging. Usually Texture->GetPathName()
	 * @returns true if all requested mips have been loaded.
	 */
	bool TryLoadMips(int32 FirstMipToLoad, void** OutMipData, FStringView DebugContext);

	/** Serialization. */
	void Serialize(FArchive& Ar, class UTexture* Owner);

#if WITH_EDITORONLY_DATA
	FString GetDerivedDataMipKeyString(int32 MipIndex, const FTexture2DMipMap& Mip) const;
	static UE::DerivedData::FValueId MakeMipId(int32 MipIndex);
#endif // WITH_EDITORONLY_DATA

	/** 
	 * Serialization for cooked builds.
	 *
	 * @param Ar Archive to serialize with
	 * @param Owner Owner texture
	 * @param bStreamable Store some mips inline, only used during cooking
	 */
	void SerializeCooked(FArchive& Ar, class UTexture* Owner, bool bStreamable);
	
	inline bool GetHasOptData() const
	{
		return (PackedData & BitMask_HasOptData) == BitMask_HasOptData;
	}

	inline void SetOptData(FOptTexturePlatformData Data)
	{
		// Set the opt data flag to true if the specified data is non-default.
		bool bHasOptData = Data != FOptTexturePlatformData();
		PackedData = (bHasOptData ? BitMask_HasOptData : 0) | (PackedData & (~BitMask_HasOptData));

		OptData = Data;
	}

	inline bool IsCubemap() const
	{
		return (PackedData & BitMask_CubeMap) == BitMask_CubeMap; 
	}

	inline void SetIsCubemap(bool bCubemap)
	{
		PackedData = (bCubemap ? BitMask_CubeMap : 0) | (PackedData & (~BitMask_CubeMap));
	}
	
	inline int32 GetNumSlices() const 
	{
		return (int32)(PackedData & BitMask_NumSlices);
	}

	inline void SetNumSlices(int32 NumSlices)
	{
		PackedData = (NumSlices & BitMask_NumSlices) | (PackedData & (~BitMask_NumSlices));
	}

	inline int32 GetNumMipsInTail() const
	{
		return OptData.NumMipsInTail;
	}

	inline int32 GetExtData() const
	{
		return OptData.ExtData;
	}

#if WITH_EDITOR
	static bool IsUsingNewDerivedData();
	bool IsAsyncWorkComplete() const;

	// Compresses the texture using the given compressor and adds the result to the DDC.
	// This might not be synchronous, and might be called from a worker thread!
	//
	// If Compressor is 0, uses the default texture compressor module. Must be nonzero
	// if called from a worker thread.
	//
	// InFlags are ETextureCacheFlags.
	// InSettingsPerLayerFetchFirst can be nullptr - if not, then the caceh will check if
	// the corresponding texture exists in the DDC before trying the FetchOrBuild settings.
	// FetchFirst is ignored if forcerebuild is passed as a flag.
	// InSettingsPerLayerFetchOrBuild is required. If a texture matching the settings exists
	// in the ddc, it is used, otherwise it is built.
	void Cache(
		class UTexture& InTexture,
		const struct FTextureBuildSettings* InSettingsPerLayerFetchFirst,
		const struct FTextureBuildSettings* InSettingsPerLayerFetchOrBuild,
		const FTexturePlatformData::FTextureEncodeResultMetadata* OutResultMetadataPerLayerFetchFirst,
		const FTexturePlatformData::FTextureEncodeResultMetadata* OutResultMetadataPerLayerFetchOrBuild,
		uint32 InFlags,
		class ITextureCompressorModule* Compressor);
	void FinishCache();
	bool TryCancelCache();
	void CancelCache();
	ENGINE_API bool TryInlineMipData(int32 FirstMipToLoad = 0, FStringView DebugContext=FStringView());
	ENGINE_API TFuture<TTuple<uint64, uint64>> LaunchEstimateOnDiskSizeTask(
		FOodleDataCompression::ECompressor InOodleCompressor,
		FOodleDataCompression::ECompressionLevel InOodleCompressionLevel,
		uint32 InCompressionBlockSize,
		FStringView InDebugContext);
	bool AreDerivedMipsAvailable(FStringView Context) const;
	bool AreDerivedVTChunksAvailable(FStringView Context) const;
	UE_DEPRECATED(5.00, "Use AreDerivedMipsAvailable with the context instead.")
	bool AreDerivedMipsAvailable() const;
	UE_DEPRECATED(5.00, "Use AreDerivedVTChunksAvailable with the context instead.")
	bool AreDerivedVTChunksAvailable() const;
#endif

	/** Return the number of mips that are not streamable. */
	int32 GetNumNonStreamingMips() const;
	/** Return the number of mips that streamable but not optional. */
	int32 GetNumNonOptionalMips() const;
	/** Return true if at least one mip can be loaded either from DDC or disk. */
	bool CanBeLoaded() const;

	// Only because we don't want to expose FVirtualTextureBuiltData
	ENGINE_API int32 GetNumVTMips() const;
	ENGINE_API EPixelFormat GetLayerPixelFormat(uint32 LayerIndex) const;

private:

	bool CanUseCookedDataPath() const;
};

/**
 * Collection of values that contribute to pixel format chosen for texture
 */
USTRUCT()
struct FTextureFormatSettings
{
	GENERATED_USTRUCT_BODY()

	FTextureFormatSettings()
		: CompressionSettings(TC_Default)
		, CompressionNoAlpha(false)
		, CompressionForceAlpha(false)
		, CompressionNone(false)
		, CompressionYCoCg(false)
		, SRGB(false)
	{}

	UPROPERTY()
	TEnumAsByte<enum TextureCompressionSettings> CompressionSettings;

	UPROPERTY()
	uint8 CompressionNoAlpha : 1;

	UPROPERTY()
	uint8 CompressionForceAlpha : 1;

	UPROPERTY()
	uint8 CompressionNone : 1;

	UPROPERTY()
	uint8 CompressionYCoCg : 1;

	UPROPERTY()
	uint8 SRGB : 1;
};


USTRUCT(BlueprintType)
struct FTextureSourceColorSettings
{
	GENERATED_USTRUCT_BODY()

	FTextureSourceColorSettings()
		: EncodingOverride(ETextureSourceEncoding::TSE_None)
		, ColorSpace(ETextureColorSpace::TCS_None)
		, RedChromaticityCoordinate(FVector2D::ZeroVector)
		, GreenChromaticityCoordinate(FVector2D::ZeroVector)
		, BlueChromaticityCoordinate(FVector2D::ZeroVector)
		, WhiteChromaticityCoordinate(FVector2D::ZeroVector)
		, ChromaticAdaptationMethod(ETextureChromaticAdaptationMethod::TCAM_Bradford)
	{}

	/** Source encoding of the texture, exposing more options than just sRGB. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = ColorManagement)
	ETextureSourceEncoding EncodingOverride;

	/** Source color space of the texture. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = ColorManagement)
	ETextureColorSpace ColorSpace;

	/** Red chromaticity coordinate of the source color space. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = ColorManagement, meta = (EditCondition = "ColorSpace == ETextureColorSpace::TCS_Custom"))
	FVector2D RedChromaticityCoordinate;

	/** Green chromaticity coordinate of the source color space. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = ColorManagement, meta = (EditCondition = "ColorSpace == ETextureColorSpace::TCS_Custom"))
	FVector2D GreenChromaticityCoordinate;

	/** Blue chromaticity coordinate of the source color space. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = ColorManagement, meta = (EditCondition = "ColorSpace == ETextureColorSpace::TCS_Custom"))
	FVector2D BlueChromaticityCoordinate;

	/** White chromaticity coordinate of the source color space. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = ColorManagement, meta = (EditCondition = "ColorSpace == ETextureColorSpace::TCS_Custom"))
	FVector2D WhiteChromaticityCoordinate;

	/** Chromatic adaption method applied if the source white point differs from the working color space white point. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = ColorManagement)
	ETextureChromaticAdaptationMethod ChromaticAdaptationMethod;
};

UCLASS(abstract, MinimalAPI, BlueprintType)
class UTexture : public UStreamableRenderAsset, public IInterface_AssetUserData, public IInterface_AsyncCompilation
{
	GENERATED_UCLASS_BODY()

	/*--------------------------------------------------------------------------
		Editor only properties used to build the runtime texture data.
	--------------------------------------------------------------------------*/

#if WITH_EDITORONLY_DATA
	UPROPERTY()
	FTextureSource Source;
#endif

private:
	/** Unique ID for this material, used for caching during distributed lighting */
	UPROPERTY()
	FGuid LightingGuid;

public:
#if WITH_EDITORONLY_DATA
	UPROPERTY()
	FString SourceFilePath_DEPRECATED;

	UPROPERTY(VisibleAnywhere, Instanced, Category=ImportSettings)
	TObjectPtr<class UAssetImportData> AssetImportData;

public:

	/** Static texture brightness adjustment (scales HSV value.)  (Non-destructive; Requires texture source art to be available.) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Adjustments, meta=(DisplayName = "Brightness"))
	float AdjustBrightness;

	/** Static texture curve adjustment (raises HSV value to the specified power.)  (Non-destructive; Requires texture source art to be available.)  */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Adjustments, meta=(DisplayName = "Brightness Curve"))
	float AdjustBrightnessCurve;

	/** Static texture "vibrance" adjustment (0 - 1) (HSV saturation algorithm adjustment.)  (Non-destructive; Requires texture source art to be available.)  */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Adjustments, meta=(DisplayName = "Vibrance", ClampMin = "0.0", ClampMax = "1.0"))
	float AdjustVibrance;

	/** Static texture saturation adjustment (scales HSV saturation.)  (Non-destructive; Requires texture source art to be available.)  */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Adjustments, meta=(DisplayName = "Saturation"))
	float AdjustSaturation;

	/** Static texture RGB curve adjustment (raises linear-space RGB color to the specified power.)  (Non-destructive; Requires texture source art to be available.)  */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Adjustments, meta=(DisplayName = "RGBCurve"))
	float AdjustRGBCurve;

	/** Static texture hue adjustment (0 - 360) (offsets HSV hue by value in degrees.)  (Non-destructive; Requires texture source art to be available.)  */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Adjustments, meta=(DisplayName = "Hue", ClampMin = "0.0", ClampMax = "360.0"))
	float AdjustHue;

	/** Remaps the alpha to the specified min/max range, defines the new value of 0 (Non-destructive; Requires texture source art to be available.) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Adjustments, meta=(DisplayName = "Min Alpha"))
	float AdjustMinAlpha;

	/** Remaps the alpha to the specified min/max range, defines the new value of 1 (Non-destructive; Requires texture source art to be available.) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Adjustments, meta=(DisplayName = "Max Alpha"))
	float AdjustMaxAlpha;

	/** If enabled, the texture's alpha channel will be discarded during compression */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Compression, meta=(DisplayName="Compress Without Alpha"))
	uint32 CompressionNoAlpha:1;

	/** If true, force the texture to be uncompressed no matter the format. */
	UPROPERTY()
	uint32 CompressionNone:1;

	/** If enabled, defer compression of the texture until save or manually compressed in the texture editor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Compression, meta=(NoResetToDefault))
	uint32 DeferCompression:1;

	/** How aggressively should any relevant lossy compression be applied. For compressors that support EncodeSpeed (i.e. Oodle), this is only
	*	applied if enabled (see Project Settings -> Texture Encoding). Note that this is *in addition* to any
	*	unavoidable loss due to the target format - selecting "No Lossy Compression" will not result in zero distortion for BCn formats.
	*/
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Compression, AdvancedDisplay)
	TEnumAsByte<ETextureLossyCompressionAmount> LossyCompressionAmount;
	
	/** Oodle Texture SDK Version to encode with.  Enter 'latest' to update; 'None' preserves legacy encoding to avoid patches. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Compression, AdvancedDisplay)
	FName OodleTextureSdkVersion;

	/** The maximum resolution for generated textures. A value of 0 means the maximum size for the format on each platform. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category=Compression, meta=(DisplayName="Maximum Texture Size", ClampMin = "0.0"), AdvancedDisplay)
	int32 MaxTextureSize;

	/** The compression quality for generated ASTC textures (i.e. mobile platform textures). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Compression, meta = (DisplayName = "ASTC Compression Quality"), AdvancedDisplay)
	TEnumAsByte<enum ETextureCompressionQuality> CompressionQuality;

	/** When true, the alpha channel of mip-maps and the base image are dithered for smooth LOD transitions. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Texture, AdvancedDisplay)
	uint32 bDitherMipMapAlpha:1;

	/** Whether mip RGBA should be scaled to preserve the number of pixels with Value >= AlphaCoverageThresholds.  AlphaCoverageThresholds are ignored if this is off. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Texture, AdvancedDisplay)
	bool bDoScaleMipsForAlphaCoverage = false;
	
	/** Alpha values per channel to compare to when preserving alpha coverage. 0 means disable channel.  Typical good values in 0.5 - 0.9, not 1.0 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Texture, meta=(ClampMin = "0", ClampMax = "1.0"), AdvancedDisplay)
	FVector4 AlphaCoverageThresholds = FVector4(0,0,0,0);

	/** When true the texture's border will be preserved during mipmap generation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=LevelOfDetail, AdvancedDisplay)
	uint32 bPreserveBorder:1;

	/** When true the texture's green channel will be inverted. This is useful for some normal maps. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Texture, AdvancedDisplay)
	uint32 bFlipGreenChannel:1;

	/** How to pad the texture to a power of 2 size (if necessary) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Texture)
	TEnumAsByte<enum ETexturePowerOfTwoSetting::Type> PowerOfTwoMode;

	/** The color used to pad the texture out if it is resized due to PowerOfTwoMode */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Texture)
	FColor PaddingColor;

	/** Whether to chroma key the image, replacing any pixels that match ChromaKeyColor with transparent black */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Adjustments)
	bool bChromaKeyTexture;

	/** The threshold that components have to match for the texel to be considered equal to the ChromaKeyColor when chroma keying (<=, set to 0 to require a perfect exact match) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Adjustments, meta=(EditCondition="bChromaKeyTexture", ClampMin="0"))
	float ChromaKeyThreshold;

	/** The color that will be replaced with transparent black if chroma keying is enabled */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Adjustments, meta=(EditCondition="bChromaKeyTexture"))
	FColor ChromaKeyColor;

	/** Per asset specific setting to define the mip-map generation properties like sharpening and kernel size. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=LevelOfDetail)
	TEnumAsByte<enum TextureMipGenSettings> MipGenSettings;
	
	/**
	 * Can be defined to modify the roughness based on the normal map variation (mostly from mip maps).
	 * MaxAlpha comes in handy to define a base roughness if no source alpha was there.
	 * Make sure the normal map has at least as many mips as this texture.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Compositing)
	TObjectPtr<class UTexture> CompositeTexture;

	/* defines how the CompositeTexture is applied, e.g. CTM_RoughnessFromNormalAlpha */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Compositing, AdvancedDisplay)
	TEnumAsByte<enum ECompositeTextureMode> CompositeTextureMode;

	/**
	 * default 1, high values result in a stronger effect e.g 1, 2, 4, 8
	 * this is no slider because the texture update would not be fast enough
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Compositing, AdvancedDisplay)
	float CompositePower;

	/**
	 * Array of settings used to control the format of given layer
	 * If this array doesn't include an entry for a given layer, values from UTexture will be used
	 */
	UPROPERTY()
	TArray<FTextureFormatSettings> LayerFormatSettings;

#endif // WITH_EDITORONLY_DATA

	/*--------------------------------------------------------------------------
		Properties needed at runtime below.
	--------------------------------------------------------------------------*/

	/*
	 * Level scope index of this texture. It is used to reduce the amount of lookup to map a texture to its level index.
	 * Useful when building texture streaming data, as well as when filling the texture streamer with precomputed data.
     * It relates to FStreamingTextureBuildInfo::TextureLevelIndex and also the index in ULevel::StreamingTextureGuids. 
	 * Default value of -1, indicates that the texture has an unknown index (not yet processed). At level load time, 
	 * -2 is also used to indicate that the texture has been processed but no entry were found in the level table.
	 * After any of these processes, the LevelIndex is reset to INDEX_NONE. Making it ready for the next level task.
	 */
	UPROPERTY(transient, duplicatetransient, NonTransactional)
	int32 LevelIndex = INDEX_NONE;

	/** A bias to the index of the top mip level to use. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=LevelOfDetail, meta=(DisplayName="LOD Bias"), AssetRegistrySearchable)
	int32 LODBias;

	/** Compression settings to use when building the texture. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Compression, AssetRegistrySearchable)
	TEnumAsByte<enum TextureCompressionSettings> CompressionSettings;

	/** The texture filtering mode to use when sampling this texture. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Texture, AssetRegistrySearchable, AdvancedDisplay)
	TEnumAsByte<enum TextureFilter> Filter;

	/** The texture mip load options. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Texture, AssetRegistrySearchable, AdvancedDisplay)
	ETextureMipLoadOptions MipLoadOptions;

	/** Texture group this texture belongs to */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=LevelOfDetail, meta=(DisplayName="Texture Group"), AssetRegistrySearchable)
	TEnumAsByte<enum TextureGroup> LODGroup;

	/** Downscale source texture, applied only to textures without mips 
	 * 0.0 - use scale value from texture group
	 * 1.0 - do not scale texture
	 * > 1.0 - scale texure
	 */
	UPROPERTY(EditAnywhere, Category=LevelOfDetail, AdvancedDisplay, meta=(ClampMin="0.0", ClampMax="8.0"))
	FPerPlatformFloat Downscale;

	/** Texture downscaling options */
	UPROPERTY(EditAnywhere, Category=LevelOfDetail, AdvancedDisplay)
	ETextureDownscaleOptions DownscaleOptions;

	
	/** This should be unchecked if using alpha channels individually as masks. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Texture, meta=(DisplayName="sRGB"), AssetRegistrySearchable)
	uint8 SRGB:1;

#if WITH_EDITORONLY_DATA
	/** Texture color management settings: source encoding and color space. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Texture, AdvancedDisplay)
	FTextureSourceColorSettings SourceColorSettings;

	/** A flag for using the simplified legacy gamma space e.g pow(color,1/2.2) for converting from FColor to FLinearColor, if we're doing sRGB. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Texture, AdvancedDisplay)
	uint8 bUseLegacyGamma:1;

	/** Indicates we're currently importing the object (set in PostEditImport, unset in the subsequent PostEditChange) */
	uint8 bIsImporting : 1;
	
	/** Indicates ImportCustomProperties has been called (set in ImportCustomProperties, unset in the subsequent PostEditChange) */
	uint8 bCustomPropertiesImported : 1;

	// When we are open in an asset editor, we have a pointer to a custom encoding
	// object which can optionally cause us to do something other than Fast/Final encode settings.
	TWeakPtr<class FTextureEditorCustomEncode> TextureEditorCustomEncoding;
#endif // WITH_EDITORONLY_DATA

	/** If true, the RHI texture will be created using TexCreate_NoTiling */
	UPROPERTY()
	uint8 bNoTiling:1;

	/** Is this texture streamed in using VT								*/
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = Texture, AssetRegistrySearchable, AdvancedDisplay)
	uint8 VirtualTextureStreaming : 1;

	/** If true the texture stores YCoCg. Blue channel will be filled with a precision scale during compression. */
	UPROPERTY()
	uint8 CompressionYCoCg : 1;

	/** If true, the RHI texture will be created without TexCreate_OfflineProcessed. */
	UPROPERTY(transient)
	uint8 bNotOfflineProcessed : 1;

private:
	/** Whether the async resource release process has already been kicked off or not */
	UPROPERTY(transient)
	uint8 bAsyncResourceReleaseHasBeenStarted : 1;

protected:
	/** Array of user data stored with the asset */
	UPROPERTY(EditAnywhere, AdvancedDisplay, Instanced, Category = Texture)
	TArray<TObjectPtr<UAssetUserData>> AssetUserData;

private:

#if WITH_EDITOR
	/** Used to mark texture streamable state when cooking. */
	TOptional<bool> bCookedIsStreamable;
#endif

	/** The texture's resource, can be NULL */
	class FTextureResource*	PrivateResource;
	/** Value updated and returned by the render-thread to allow
	  * fenceless update from the game-thread without causing
	  * potential crash in the render thread.
	  */
	class FTextureResource* PrivateResourceRenderThread;

public:
#if WITH_TEXTURE_RESOURCE_DEPRECATIONS
	UE_DEPRECATED(5.00, "Use GetResource() / SetResource() accessors instead.")
	TFieldPtrAccessor<FTextureResource> Resource;
#endif

	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	ENGINE_API virtual ~UTexture() {};
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
	
	/** Set texture's resource, can be NULL */
	ENGINE_API void SetResource(FTextureResource* Resource);

	/** Get the texture's resource, can be NULL */
	ENGINE_API FTextureResource* GetResource();

	/** Get the const texture's resource, can be NULL */
	ENGINE_API const FTextureResource* GetResource() const;

	/** Stable RHI texture reference that refers to the current RHI texture. Note this is manually refcounted! */
	FTextureReference TextureReference;

	/** Release fence to know when resources have been freed on the rendering thread. */
	FRenderCommandFence ReleaseFence;

	/** delegate type for texture save events ( Params: UTexture* TextureToSave ) */
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnTextureSaved, class UTexture*);
	/** triggered before a texture is being saved */
	ENGINE_API static FOnTextureSaved PreSaveEvent;

	ENGINE_API virtual void ExportCustomProperties(FOutputDevice& Out, uint32 Indent) override;
	ENGINE_API virtual void ImportCustomProperties(const TCHAR* SourceText, FFeedbackContext* Warn) override;
	ENGINE_API virtual void PostEditImport() override;

	/**
	 * Resets the resource for the texture.
	 */
	ENGINE_API void ReleaseResource();

	/**
	 * Creates a new resource for the texture, and updates any cached references to the resource.
	 */
	ENGINE_API virtual void UpdateResource();

	/**
	 * Implemented by subclasses to create a new resource for the texture.
	 */
	virtual class FTextureResource* CreateResource() PURE_VIRTUAL(UTexture::CreateResource,return NULL;);

	/** Cache the combined LOD bias based on texture LOD group and LOD bias. */
	ENGINE_API void UpdateCachedLODBias();

	/**
	 * @return The material value type of this texture.
	 */
	virtual EMaterialValueType GetMaterialType() const PURE_VIRTUAL(UTexture::GetMaterialType,return MCT_Texture;);

	/**
	 * Returns if the texture is actually being rendered using virtual texturing right now.
	 * Unlike the 'VirtualTextureStreaming' property which reflects the user's desired state
	 * this reflects the actual current state on the renderer depending on the platform, VT
	 * data being built, project settings, ....
	 */
	virtual bool IsCurrentlyVirtualTextured() const
	{
		return false;
	}

	/** Returns the virtual texture build settings. */
	ENGINE_API virtual void GetVirtualTextureBuildSettings(struct FVirtualTextureBuildSettings& OutSettings) const;

	/**
	 * Textures that use the derived data cache must override this function and
	 * provide a pointer to the linked list of platform data.
	 */
	virtual FTexturePlatformData** GetRunningPlatformData() { return NULL; }
	virtual TMap<FString, FTexturePlatformData*>* GetCookedPlatformData() { return NULL; }

	void CleanupCachedRunningPlatformData();

	/**
	 * Serializes cooked platform data.
	 */
	ENGINE_API void SerializeCookedPlatformData(class FArchive& Ar);

	//~ Begin IInterface_AssetUserData Interface
	ENGINE_API virtual void AddAssetUserData(UAssetUserData* InUserData) override;
	ENGINE_API virtual void RemoveUserDataOfClass(TSubclassOf<UAssetUserData> InUserDataClass) override;
	ENGINE_API virtual UAssetUserData* GetAssetUserDataOfClass(TSubclassOf<UAssetUserData> InUserDataClass) override;
	ENGINE_API virtual const TArray<UAssetUserData*>* GetAssetUserDataArray() const override;
	//~ End IInterface_AssetUserData Interface

#if WITH_EDITOR
	/**
	 * Caches platform data for the texture.
	 * 
	 * @param bAsyncCache spawn a thread to cache the platform data 
	 * @param bAllowAsyncBuild allow building the DDC file in the thread if missing.
	 * @param bAllowAsyncLoading allow loading source data in the thread if missing (the data won't be reusable for later use though)
	 * @param Compressor optional compressor as the texture compressor can not be retrieved from an async thread.
	 * 
	 * This is called optionally from worker threads via the FAsyncEncode class (LightMaps, ShadowMaps)
	 */
	void CachePlatformData(bool bAsyncCache = false, bool bAllowAsyncBuild = false, bool bAllowAsyncLoading = false, class ITextureCompressorModule* Compressor = nullptr);

	/**
	 * Begins caching platform data in the background for the platform requested
	 */
	ENGINE_API virtual void BeginCacheForCookedPlatformData(  const ITargetPlatform* TargetPlatform ) override;

	/**
	 * Have we finished loading all the cooked platform data for the target platforms requested in BeginCacheForCookedPlatformData
	 * 
	 * @param	TargetPlatform target platform to check for cooked platform data
	 */
	ENGINE_API virtual bool IsCachedCookedPlatformDataLoaded( const ITargetPlatform* TargetPlatform ) override;

	/**
	 * Clears cached cooked platform data for specific platform
	 * 
	 * @param	TargetPlatform	target platform to cache platform specific data for
	 */
	ENGINE_API virtual void ClearCachedCookedPlatformData( const ITargetPlatform* TargetPlatform ) override;

	/**
	 * Clear all cached cooked platform data
	 * 
	 * @param	TargetPlatform	target platform to cache platform specific data for
	 */
	ENGINE_API virtual void ClearAllCachedCookedPlatformData() override;

	/**
	 * Returns true if the current texture is a default placeholder because compilation is still ongoing.
	 */
	ENGINE_API virtual bool IsDefaultTexture() const;

	/**
	 * Begins caching platform data in the background.
	 */
	ENGINE_API void BeginCachePlatformData();

	/**
	 * Returns true if all async caching has completed.
	 */
	ENGINE_API bool IsAsyncCacheComplete() const;

	/**
	 * Blocks on async cache tasks and prepares platform data for use.
	 */
	ENGINE_API void FinishCachePlatformData();

	/**
	 * Forces platform data to be rebuilt.
	 * @param InEncodeSpeedOverride		Optionally force a specific encode speed 
	 * 									using the ETextureEncodeSpeedOverride enum.
	 * 									Type hidden to keep out of Texture.h
	 */
	ENGINE_API void ForceRebuildPlatformData(uint8 InEncodeSpeedOverride=255);

	/**
	 * Get an estimate of the peak amount of memory required to build this texture.
	 */
	ENGINE_API int64 GetBuildRequiredMemory() const;

	/**
	 * Marks platform data as transient. This optionally removes persistent or cached data associated with the platform.
	 */
	ENGINE_API void MarkPlatformDataTransient();

	/**
	* Return maximum dimension for this texture type.
	*/
	ENGINE_API virtual uint32 GetMaximumDimension() const;

	/**
	 * Gets settings used to choose format for the given layer
	 */
	ENGINE_API void GetLayerFormatSettings(int32 LayerIndex, FTextureFormatSettings& OutSettings) const;
	ENGINE_API void SetLayerFormatSettings(int32 LayerIndex, const FTextureFormatSettings& InSettings);

	ENGINE_API void GetDefaultFormatSettings(FTextureFormatSettings& OutSettings) const;
#endif

	/** @return the width of the surface represented by the texture. */
	virtual float GetSurfaceWidth() const PURE_VIRTUAL(UTexture::GetSurfaceWidth,return 0;);

	/** @return the height of the surface represented by the texture. */
	virtual float GetSurfaceHeight() const PURE_VIRTUAL(UTexture::GetSurfaceHeight,return 0;);

	/** @return the depth of the surface represented by the texture. */
	virtual float GetSurfaceDepth() const PURE_VIRTUAL(UTexture::GetSurfaceDepth, return 0;);

	/** @return the array size of the surface represented by the texture. */
	virtual uint32 GetSurfaceArraySize() const PURE_VIRTUAL(UTexture::GetSurfaceArraySize, return 0;);

	virtual TextureAddress GetTextureAddressX() const { return TA_Wrap; }
	virtual TextureAddress GetTextureAddressY() const { return TA_Wrap; }
	virtual TextureAddress GetTextureAddressZ() const { return TA_Wrap; }

	/**
	 * Access the GUID which defines this texture's resources externally through FExternalTextureRegistry
	 */
	virtual FGuid GetExternalTextureGuid() const
	{
		return FGuid();
	}

#if WITH_EDITOR
	//~ Begin AsyncCompilation Interface
	virtual bool IsCompiling() const override { return IsDefaultTexture(); }
	//~ End AsyncCompilation Interface

	//~ Begin UObject Interface.
	ENGINE_API virtual bool Modify(bool bAlwaysMarkDirty = true) override;
	ENGINE_API virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	ENGINE_API virtual bool CanEditChange(const FProperty* InProperty) const override;
#endif // WITH_EDITOR
	ENGINE_API virtual void Serialize(FArchive& Ar) override;
	ENGINE_API virtual void PostInitProperties() override;
	ENGINE_API virtual void PostLoad() override;
	PRAGMA_DISABLE_DEPRECATION_WARNINGS // Suppress compiler warning on override of deprecated function
	UE_DEPRECATED(5.0, "Use version that takes FObjectPreSaveContext instead.")
	ENGINE_API virtual void PreSave(const class ITargetPlatform* TargetPlatform) override;
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
	ENGINE_API virtual void PreSave(FObjectPreSaveContext ObjectSaveContext) override;
	ENGINE_API virtual void BeginDestroy() override;
	ENGINE_API virtual bool IsReadyForFinishDestroy() override;
	ENGINE_API virtual void FinishDestroy() override;
	ENGINE_API virtual void PostCDOContruct() override;
#if WITH_EDITORONLY_DATA
	ENGINE_API virtual void GetAssetRegistryTags(TArray<FAssetRegistryTag>& OutTags) const override;
#endif
	ENGINE_API virtual bool IsPostLoadThreadSafe() const override;
	//~ End UObject Interface.

	//~ Begin UStreamableRenderAsset Interface
	virtual int32 GetLODGroupForStreaming() const final override { return static_cast<int32>(LODGroup); }
	virtual EStreamableRenderAssetType GetRenderAssetType() const final override { return EStreamableRenderAssetType::Texture; }
	ENGINE_API virtual FIoFilenameHash GetMipIoFilenameHash(const int32 MipIndex) const final override;
	ENGINE_API virtual bool DoesMipDataExist(const int32 MipIndex) const final override;
	ENGINE_API virtual bool HasPendingRenderResourceInitialization() const final override;
	ENGINE_API virtual bool HasPendingLODTransition() const final override;
	ENGINE_API virtual void InvalidateLastRenderTimeForStreaming() final override;
	ENGINE_API virtual float GetLastRenderTimeForStreaming() const final override;
	ENGINE_API virtual bool ShouldMipLevelsBeForcedResident() const final override;
	//~ End UStreamableRenderAsset Interface

	/**
	 * Cancels any pending texture streaming actions if possible.
	 * Returns when no more async loading requests are in flight.
	 */
	ENGINE_API static void CancelPendingTextureStreaming();

	/**
	 *	Gets the average brightness of the texture (in linear space)
	 *
	 *	@param	bIgnoreTrueBlack		If true, then pixels w/ 0,0,0 rgb values do not contribute.
	 *	@param	bUseGrayscale			If true, use gray scale else use the max color component.
	 *
	 *	@return	float					The average brightness of the texture
	 */
	ENGINE_API virtual float GetAverageBrightness(bool bIgnoreTrueBlack, bool bUseGrayscale);
	
	// @todo document
	ENGINE_API static const TCHAR* GetTextureGroupString(TextureGroup InGroup);

	// @todo document
	ENGINE_API static const TCHAR* GetMipGenSettingsString(TextureMipGenSettings InEnum);

	// @param	bTextureGroup	true=TexturGroup, false=Texture otherwise
	ENGINE_API static TextureMipGenSettings GetMipGenSettingsFromString(const TCHAR* InStr, bool bTextureGroup);

	/**
	 * Forces textures to recompute LOD settings and stream as needed.
	 * @returns true if the settings were applied, false if they couldn't be applied immediately.
	 */
	ENGINE_API static bool ForceUpdateTextureStreaming();

	/**
	 * Checks whether this texture has a high dynamic range (HDR) source.
	 *
	 * @return true if the texture has an HDR source, false otherwise.
	 */
	bool HasHDRSource(int32 LayerIndex = 0) const
	{
#if WITH_EDITOR
		return FTextureSource::IsHDR(Source.GetFormat(LayerIndex));
#else
		return false;
#endif // WITH_EDITOR
	}


	/** @return true if the compression type is a normal map compression type */
	bool IsNormalMap() const
	{
		return (CompressionSettings == TC_Normalmap);
	}

	/** @return true if the texture has an uncompressed texture setting */
	bool IsUncompressed() const
	{
		return (CompressionSettings == TC_Grayscale ||
				CompressionSettings == TC_Displacementmap ||
				CompressionSettings == TC_VectorDisplacementmap ||
				CompressionSettings == TC_HDR ||
				CompressionSettings == TC_EditorIcon ||
				CompressionSettings == TC_DistanceFieldFont ||
				CompressionSettings == TC_HalfFloat
			);
	}

	/**
	 * Calculates the size of this texture if it had MipCount miplevels streamed in.
	 *
	 * @param	Enum	Which mips to calculate size for.
	 * @return	Total size of all specified mips, in bytes
	 */
	virtual uint32 CalcTextureMemorySizeEnum( ETextureMipCount Enum ) const
	{
		return 0;
	}

	/** Returns a unique identifier for this texture. Used by the lighting build and texture streamer. */
	const FGuid& GetLightingGuid() const
	{
		return LightingGuid;
	}

	/** 
	 * Assigns a new GUID to a texture. This will be called whenever a texture is created or changes. 
	 * In game, the GUIDs are only used by the texture streamer to link build data to actual textures,
	 * that means new textures don't actually need GUIDs (see FStreamingTextureLevelContext)
	 */
	void SetLightingGuid()
	{
#if WITH_EDITORONLY_DATA
		LightingGuid = FGuid::NewGuid();
#else
		LightingGuid = FGuid(0, 0, 0, 0);
#endif // WITH_EDITORONLY_DATA
	}

	void SetLightingGuid(const FGuid& Guid)
	{
		LightingGuid = Guid;
	}

	/** Generates a deterministic GUID for the texture based on the full name of the object.
	  * Used to ensure that assets created during cook can be deterministic
	  */
	ENGINE_API void SetDeterministicLightingGuid();

	/**
	 * Retrieves the pixel format enum for enum <-> string conversions.
	 */
	ENGINE_API static class UEnum* GetPixelFormatEnum();

	/** Returns the minimum number of mips that must be resident in memory (cannot be streamed). */
	static FORCEINLINE int32 GetStaticMinTextureResidentMipCount()
	{
		return GMinTextureResidentMipCount;
	}

	/** Sets the minimum number of mips that must be resident in memory (cannot be streamed). */
	static void SetMinTextureResidentMipCount(int32 InMinTextureResidentMipCount);

#if WITH_EDITOR
	/** Called by ULevel::MarkNoStreamableTexturesPrimitiveComponents when cooking level. */
	bool IsCandidateForTextureStreaming(const ITargetPlatform* InTargetPlatform) const;
#endif

protected:

	/** The minimum number of mips that must be resident in memory (cannot be streamed). */
	static ENGINE_API int32 GMinTextureResidentMipCount;

#if WITH_EDITOR
	// The Texture compiler might use TryCancelCachePlatformData on shutdown
	friend class FTextureCompilingManager;

	enum class ENotifyMaterialsEffectOnShaders
	{
		Default,
		DoesNotInvalidate
	};
	/** Try to cancel any async tasks on PlatformData. 
	 *  Returns true if there is no more async tasks pending, false otherwise.
	 */
	ENGINE_API bool TryCancelCachePlatformData();

	/** Notify any loaded material instances that the texture has changed. */
	ENGINE_API void NotifyMaterials(const ENotifyMaterialsEffectOnShaders EffectOnShaders = ENotifyMaterialsEffectOnShaders::Default);

	virtual bool GetStreamableRenderResourceState(FTexturePlatformData* InPlatformData, FStreamableRenderResourceState& OutState) const { return false; }
#endif //WITH_EDITOR

	void BeginFinalReleaseResource();

	/**
	 * Calculates the render resource initial state, expected to be used in InitResource() for derived classes implementing streaming.
	 *
	 * @param	PlatformData - the asset platform data.
	 * @param	bAllowStreaming - where streaming is allowed, might still be disabled based on asset settings.
	 * @param	MaxMipCount - optional limitation on the max mip count.
	 * @return  The state to be passed to FStreamableTextureResource.
	 */
	FStreamableRenderResourceState GetResourcePostInitState(const FTexturePlatformData* PlatformData, bool bAllowStreaming, int32 MinRequestMipCount = 0, int32 MaxMipCount = 0, bool bSkipCanBeLoaded = false) const;
};

/** 
* Replaces the RHI reference of one texture with another.
* Allows one texture to be replaced with another at runtime and have all existing references to it remain valid.
*/
struct FTextureReferenceReplacer
{
	FTextureReferenceRHIRef OriginalRef;

	FTextureReferenceReplacer(UTexture* OriginalTexture)
	{
		if (OriginalTexture)
		{
			OriginalTexture->ReleaseResource();
			OriginalRef = OriginalTexture->TextureReference.TextureReferenceRHI;
		}
		else
		{
			OriginalRef = nullptr;
		}
	}

	void Replace(UTexture* NewTexture)
	{
		if (OriginalRef)
		{
			NewTexture->TextureReference.TextureReferenceRHI = OriginalRef;
		}
	}
};
