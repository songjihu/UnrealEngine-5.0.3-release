// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreTypes.h"
#include "Containers/StringFwd.h"
#include "Containers/StringView.h"
#include "IO/IoHash.h"
#include "Templates/TypeHash.h"

#define UE_API DERIVEDDATACACHE_API

class FCbWriter;

namespace UE::DerivedData
{

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * An alphanumeric identifier that groups related cache records.
 *
 * A cache bucket name must be alphanumeric, non-empty, and contain fewer than 256 code units.
 */
class FCacheBucket
{
public:
	/** Construct a null cache bucket. */
	FCacheBucket() = default;

	/** Create a cache bucket from a name. */
	UE_API explicit FCacheBucket(FUtf8StringView Name);
	UE_API explicit FCacheBucket(FWideStringView Name);

	/** Whether this is null. */
	inline bool IsNull() const { return !Name; }
	/** Whether this is not null. */
	inline bool IsValid() const { return !IsNull(); }

	/** Reset this to null. */
	inline void Reset() { *this = FCacheBucket(); }

	inline bool operator==(FCacheBucket Other) const { return Name == Other.Name; }
	inline bool operator!=(FCacheBucket Other) const { return Name != Other.Name; }

	inline bool operator<(FCacheBucket Other) const
	{
		return Name != Other.Name && ToString().Compare(Other.ToString(), ESearchCase::IgnoreCase) < 0;
	}

	friend inline uint32 GetTypeHash(FCacheBucket Bucket)
	{
		return ::GetTypeHash(reinterpret_cast<UPTRINT>(Bucket.Name));
	}

	/** Get the name of the cache bucket as a string. */
	inline FAnsiStringView ToString() const;

	/** Get the name of the cache bucket as a null-terminated string. */
	inline const ANSICHAR* ToCString() const { return Name; }

protected:
	static constexpr int32 LengthOffset = -1;

	/** Name stored as a null-terminated string preceded by one byte containing its length. */
	const ANSICHAR* Name = nullptr;
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

/** A key that uniquely identifies a cache record. */
struct FCacheKey
{
	FCacheBucket Bucket;
	FIoHash Hash;

	/** A key with a null bucket and a zero hash. */
	static const FCacheKey Empty;
};

inline const FCacheKey FCacheKey::Empty;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

inline FAnsiStringView FCacheBucket::ToString() const
{
	return Name ? FAnsiStringView(Name, reinterpret_cast<const uint8*>(Name)[LengthOffset]) : FAnsiStringView();
}

template <typename CharType>
inline TStringBuilderBase<CharType>& operator<<(TStringBuilderBase<CharType>& Builder, const FCacheBucket& Bucket)
{
	return Builder << Bucket.ToString();
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

inline bool operator==(const FCacheKey& A, const FCacheKey& B)
{
	return A.Bucket == B.Bucket && A.Hash == B.Hash;
}

inline bool operator!=(const FCacheKey& A, const FCacheKey& B)
{
	return A.Bucket != B.Bucket || A.Hash != B.Hash;
}

inline bool operator<(const FCacheKey& A, const FCacheKey& B)
{
	const FCacheBucket& BucketA = A.Bucket;
	const FCacheBucket& BucketB = B.Bucket;
	return BucketA == BucketB ? A.Hash < B.Hash : BucketA < BucketB;
}

inline uint32 GetTypeHash(const FCacheKey& Key)
{
	return HashCombine(GetTypeHash(Key.Bucket), GetTypeHash(Key.Hash));
}

template <typename CharType>
inline TStringBuilderBase<CharType>& operator<<(TStringBuilderBase<CharType>& Builder, const FCacheKey& Key)
{
	return Builder << Key.Bucket << CharType('/') << Key.Hash;
}

UE_API FCbWriter& operator<<(FCbWriter& Writer, const FCacheKey& Key);

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

} // UE::DerivedData

#undef UE_API
