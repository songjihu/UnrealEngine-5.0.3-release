// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "HAL/Platform.h"

namespace UE { namespace Color {

/** Increment upon breaking changes to the EEncoding enum. */
constexpr uint32 ENCODING_TYPES_VER = 3;

/** Increment upon breaking changes to the EColorSpace and EChromaticAdaptationMethod enums. */
constexpr uint32 COLORSPACE_VER = 1;

/** List of available encodings/transfer functions.
* 
* NOTE: This list is replicated as a UENUM in TextureDefines.h, and both should always match.
*/
enum class EEncoding : uint8
{
	None = 0,
	Linear = 1,
	sRGB,
	ST2084,
	Gamma22,
	BT1886,
	Gamma26,
	Cineon,
	REDLog,
	REDLog3G10,
	SLog1,
	SLog2,
	SLog3,
	AlexaV3LogC,
	CanonLog,
	ProTune,
	VLog,
	Max,
};

/** List of available color spaces. (Increment COLORSPACE_VER upon breaking changes to the list.)
* 
* NOTE: This list is replicated as a UENUM in TextureDefines.h, and both should always match.
*/
enum class EColorSpace : uint8
{
	None = 0,
	sRGB = 1,
	Rec2020,
	ACESAP0,
	ACESAP1,
	P3DCI,
	P3D65,
	REDWideGamut,
	SonySGamut3,
	SonySGamut3Cine,
	AlexaWideGamut,
	CanonCinemaGamut,
	GoProProtuneNative,
	PanasonicVGamut,
};


/** List of available chromatic adaptation methods.
* 
* NOTE: This list is replicated as a UENUM in TextureDefines.h, and both should always match.
*/
enum class EChromaticAdaptationMethod : uint8
{
	None = 0,
	Bradford = 1,
	CAT02 = 2,
};

} } // end namespace UE::Color
